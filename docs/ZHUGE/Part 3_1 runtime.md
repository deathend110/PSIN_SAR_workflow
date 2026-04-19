# Part 3-1 Icraft运行时
## 1 背景

智能计算平台中的硬件平台是FPAI的架构，其包含CPU、GPU、NPU和PL等计算资源。不同的计算资源对应着不同的后端。**Icraft XRT**是统一前向后的运行时，在XRT中，不再区分仿真和运行，而是把不同的计算资源抽象为不同的后端，使得Icraft中的网络能够在任意的计算资源上完成计算。

### 1.1 XRT数据结构

- `Device` ：抽象了具体的硬件设备，比如一个板卡。一个 `Device` 中包含若干个 `RegRegion` 和 `MemRegion` ，分别表示不同的寄存器区域和内存区域。
- `MemChunk` ：  表示某一个内存区域上分配的一块连续内存块。`MemChunk` 没有构造函数，所有的 `MemChunk` 都是由 `MemRegion` 的 `malloc` 方法分配产生的。
- `Tensor`：表示前向工程中算子的输入和输出，包含 `dtype` 和 `chunk` 等属性分别描述数据类型和和数据。
- `Backend` ：表示某一种计算资源，比如`Host(CPU)`, `Cuda(GPU)`和`Buyi`(加速器)。 `Backend` 包含了其所支持的每一个算子在该计算资源上初始化和前向计算的逻辑。
- `Session`：表示会话（进程），主要的用户接口，用户通过 `Session` 的接口来描述如何把网络放到各个后端上计算。创建 `Session` 的过程就是资源分配的过程，即将网络的每一个算子绑定到合适的计算资源（Backend）上。

### 1.2 XRT支持的后端

- `HostBackend`

- `BuyiBackend`

- `ZG330Backend`：负责处理指定诸葛架构330芯片为计算资源的算子。

> **本教程主要围绕后端为`ZG330Backend`介绍的运行时。**

#### 1.2.1 ZG330Backend功能接口

- `log` ：生成ZG330Backend对应的log，包含部署物理地址、内存复用/ocm优化后中间层信息等，log保存在 ${工作目录}/.icraft/logs/ 路径下
- `precheck` ： 对ZG330Backend进行预检，会检查内存中的指令以及权重数据是否正确上传至etm指定地址
- `userReuseSegment`：用户配置网络权重/指令/中间层数据段的memchunk，用于复用数据或内存空间，减少多网络部署对etm的使用量
- `userConnectNetwork` ：用户配置网络输入/输出数据段的memchunk，用于连接多网络在etm上的输入和输出
- `disableMergeHardOp`：关闭算子连贯执行模式，ZG330Backend默认是使用算子连贯执行模式
- `disableEtmOptimize`：关闭etm内存回收，ZG330Backend默认开启etm内存回收
- `ocmOptimize`：选择ocm优化方案，ZG330Backend默认使用OcmOpt::BEST_SCORE，关闭ocm优化选择OcmOpt::None

> 在实际使用场景中，ZG330Backend的使用依托于 icraft::xrt::Session ， Session的构造过程中会完成ZG330Backend的初始化， Session的apply部署时也会完成ZG330Backend的部署

#### 1.2.2 ZG330Backend工作流程

![_images/workprocess.jpg](./assets/workprocess.jpg)

#### 1.2.3 ZG330Backend数据结构

- `LogicSegment` ：逻辑分段类，在ZG330Backend初始化时生成，表示对应network_view的各分段的逻辑地址相关数据
- `PhySegment` ：物理分段类，在ZG330Backend在apply部署后生成，表示对应network_view的各分段的真实物理地址相关数据
- `ForwardInfo`：ZG330Backend前向所需要的信息，包含：
  - `value_map`：network_view中包含的所有FTMP（即Value）信息
  - `hardop_map`：network_view中包含的所有HardOp的信息
  - `idx_map`：network_view中包含的所有算子的同步信息

> network_view是原网络中所有以ZG330Backend为后端的算子集合.

### 1.3 API接口

Icarft提供了丰富的API接口（包括C++和Python）来帮助用户将编译生成的网络在指定的后端上运行。

下面是python运行时和c++运行时和icraft相关的头文件

**Python**

```Python
from icraft.xir import *   
from icraft.xrt import * 
from icraft.host_backend import *
from icraft.zg330backend import *
```

- xir:Icraft-XIR的python模块
- xrt:Icraft-XIR的python模块(runtime运行时)
- host_backend:Icraft-HostBackend的python模块
- zg330backend:Icraft-zg330backend的python模块

**C++**

```C++
#include <icraft-xrt/core/session.h>
#include <icraft-xrt/dev/host_device.h>
#include <icraft-xrt/dev/zg330_device.h>
#include <icraft-backends/zg330backend/zg330backend.h>
#include <icraft-backends/hostbackend/backend.h>
#include <icraft-backends/hostbackend/utils.h>
```

## 2 运行时整体流程

模型部署/模型仿真可分为以下步骤：

![image-20251218164600766](./assets/runtime.png)

1. **开启设备**，可上板，可仿真。

2. **模型前处理**，是将模型的输入部分与框架下对齐。如果是图片输入，一般情况下框架下的维度为NCHW，而icraft的输入维度是NHWC，这里需要转换；如果是ftmp输入，直接复制框架下全部前处理即可。

3. **构造输入**，为适配icraft，将输入类型转成icraft.Tensor，过程往往是

   `框架下的input->input.numpy().astype(np.float32)->icraft.xrt.Tensor(input,icraft.xir.Layout("NHWC")`

   其中，astype()以及Layout()里面的内容需要根据输入数据自行调整。

4. **网络构造**，有三种方式可以加载网络，还可以对网络进行切分优化。

5. **会话构造**，包括创建session，和一些session的优化。

6. **模型前向推理**，session.forward( [input_tensor] )

7. **输出结果搬运**
   * 需要将数据进行从plddr向psddr上搬移，且需要确认排布转为软件排布，之后可以接自己所需要的模型后处理
    * 如果需要连续推理多个样本，该步骤结束后需要重置设备以供下个样本的正确前向推理。

8. **模型后处理**，得到网络输出结果需要进行后处理以得到整个任务的正确输出结果，该步根据框架下的模型后处理进行对齐即可。

9.  **关闭设备**。

下面具体介绍每个流程。

### 2.1 开启设备

#### 2.1.1 仿真

如果仅用于仿真测试，无需上板，那么仅构造Host_device，在Host_device上创建session即可<br>

**Python**

```c++
Host_device = HostDevice.Default();
```

**C++**

```
Device Host_device = HostDevice::Default();
```

**cuda仿真模式**：

- 运行时仿真功能提供cuda仿真模式，只需要在仿真前向初始化device时候，默认为CudaDevice即可

  - **Python**

    ```
    Device device = CudaDevice.Default();
    ```

  - **C++**

    ```
    Device device = CudaDevice::Default();
    ```

- 另外需要在头文件中引入#include <icraft-backends/hostbackend/cuda/device.h>
- cuda仿真的环境要求：
  - cuda版本：11.7 [cuda11.7 x86 windows10 离线安装包下载](https://gitee.com/link?target=https%3A%2F%2Fdeveloper.download.nvidia.com%2Fcompute%2Fcuda%2F11.7.0%2Flocal_installers%2Fcuda_11.7.0_516.01_windows.exe)
  - 电脑系统： x86
  - 显卡要求：能够支持cuda11.7的显卡

#### 2.1.2 上板

如果需要上板部署，那么需要构造Device对象，并开启对应url的设备<br>

ZG330Device是与ZG330backend绑定的设备，通过使用不同的url可以构造不同通信协议下的device， 主要包括axi、socket、mock三种，其中mock是模拟device，只包含内存分配功能，主要用在不上板生成快照文件的场景， 无法进行前向操作。

**Python**

```Python
# 导入必要的库
import xir
from xrt import *
from zg330backend import *
# 开启设备
# sockek模式url
URL_PATH =  "socket://zg330aiu@192.168.125.184:9981?npu=0x40000000&dma=0x80000000"
# axi模式url
URL_PATH =  "axi://zg330aiu?npu=0x40000000&dma=0x80000000"
device = Device.Open(URL_PATH)
```

**C++**

```C++
// 开启设备
// sockek模式url
std::string URL_PATH = "socket://zg330aiu@192.168.125.184:9981?npu=0x40000000&dma=0x80000000";
// axi模式url
std::string URL_PATH = "axi://zg330aiu?npu=0x40000000&dma=0x80000000";
Device device = Device::Open(URL_PATH);
```

> 注意事项：
>
> 1. 如果仅仿真，不需要开启设备；如果需要获得板级部署结果，需要开启设备；
> 2. npu=0x40000000&dma=0x80000000 地址可配置，可缺省；

### 2.2 模型前处理

icraft的样本输入与框架下的样本输入需要对齐，即保证输入模型的tensor值一致。

在`modelzoo utils`中提供了PicPre.hpp，其中的PicPre类可以方便的完成对于输入数据为图像的大多数前处理，将输入图像根据实际需求进行不同模式下的resize和pad等操作。

**PicPre类使用示例(C++)：**

```c++
// 对图片进行前处理操作得到对应的 cvMat
PicPre img(IMG_PATH, cv::IMREAD_COLOR);

img.Resize({ netinfo.i_cubic[0].h, netinfo.i_cubic[0].w }, PicPre::LONG_SIDE).rPad();
```

> 注意：上述提供的仅供参考，也可自行写前处理代码，对齐即可。
>
> 如果是pyrt，更简单，直接把前处理代码复制过来，如果是图片输入记得将维度换成NHWC即可。

### 2.3 输入构造

模型前向推理时要求输入数据类型是icraft.xrt.Tensor，本章节讲述如何构造输入Tensor。

输入来源可分为**psin**( ps->plddr)和plin(摄像头->plddr)。psin工程模型的输入是从ps侧加载输入，需要将输入图像或者数据根据实际需求进行前处理后，经过cast模块处理后再送入网络后，完成网络前向。这里是介绍基于psin数据流下的输入构造，有两种方式，一种是直接调用hostbackend自带的接口，另一种是调用我们模型库自定义的接口（即函数定义在modelzoo utils中），下面分别介绍。

#### 1) hostbackend自带接口

**c++调用示例：**

```cpp
// 将ftmp文件解析为HostDevice上的Xrt Tensor
xrt::Tensor icraft::hostbackend::utils::Ftmp2Tensor(const std::string &ftmp_path, const xir::Value &value)
```

```cpp
// 将img_path上的图片通过opencv解析为HostDevice上的Xrt Tensor
xrt::Tensor icraft::hostbackend::utils::Image2Tensor(const std::string &img_path, int height = -1, int width = -1)
```

```cpp
// 将图片文件通过opencv解析为HostDevice上的Xrt Tensor
xrt::Tensor icraft::hostbackend::utils::Image2Tensor(const std::string &img_path, const xir::Value &value)
```

> **一般不建议采用后两种通过图片构造的方式，因为接口中对于图片的前处理是直接的resize,既为：cv::resize(img, resized, cv::Size(out_W, out_H), 0, 0, cv::INTER_LINEAR);**

**python调用示例：**

```python
import icraft
from icraft import xrt
# 当输入是图片时，调用
input_tensor = icraft.xrt.Tensor(input_np, Layout("NHWC"))

# 当输入是ftmp时，Layout中的参数与模型输入维度对应，调用
input_tensor = icraft.xrt.Tensor(input_np, icraft.xir.Layout("**C"))
input_tensor = icraft.xrt.Tensor(input_np, icraft.xir.Layout("C"))
input_tensor = icraft.xrt.Tensor(input_np, icraft.xir.Layout("*C"))
```

注意：上述代码中的input_np的类型为numpy，完成前处理后的数据类型一般为torch.tensor，这里需要先转换成numpy再输入到icraft.xrt.Tensor接口中。

#### 2) 模型库自定义接口

**c++ 调用示例**

```cpp
Tensor CvMat2Tensor(cv::Mat& img, const NetworkView& network)
```

该tensor构造函数输入是已经经过所需前处理的mat，network参数则是用于获取网络对应位置输入数据的value的tensortype，通过tensortype中的数据存储类型将输入mat转换为对应的数据类型，且根据value完成输入tensor的形状、layout、数据类型的定义，最后将已经转换类型的mat数据copy给输入tensor，完成tensor的构造。详见`modelzoo utils`中icraft_utils.hpp的CvMat2Tensor函数代码。

```cpp
Tensor Data2Tensor(const T* input_data, const xir::Value& input_value)
```

该tensor构造函数输入是已经经过所需前处理的输入数据的指针，input_value对应输入数据的value的tensortype，通过tensortype中的数据存储类型将输入指针指向的数据转换为对应的数据类型，且根据value完成输入tensor的形状、layout、数据类型的定义，最后将已经转换类型的数据数据copy给输入tensor，完成tensor的构造。详见`modelzoo utils`中icraft_utils.hpp的Data2Tensor函数代码。

**python调用示例**

```python
# img维度为NHWC,类型为numpy
input_tensor = numpy2Tensor(img,network)
```

> **无论如何去构造Tensor，原则是要保证数据是经过正确的网络前处理的，且数据的维度要和实际送入网络中的保持一致。**

### 2.4 网络构造

#### 2.4.1 加载网络

加载网络的方式有两种，基于json&raw和基于快照（可快速加载网络），常规网络加载通常使用json&raw，本章节介绍基于json&raw方式的网络加载。

**C++**

```cpp
# 加载编译得到的网络
std::string JSON_PATH = "./yolov5s_640x640_ZG.json";
std::string RAW_PATH = "./yolov5s_640x640_ZG.raw";
auto network = Network::CreateFromJsonFile(JSON_FILE);// 从json文件创建一个网络
network.lazyLoadParamsFromFile(RAW_FILE);// 从指定文件路径中加载网络参数；
```

**Python**

```Python
# 加载编译得到的网络
JSON_FILE = "./yolov5s_640x640_ZG.json"
RAW_FILE = "./yolov5s_640x640_ZG.raw"
network = Network.CreateFromJsonFile(JSON_FILE) # 从json文件创建一个网络
generated_network.lazyLoadParamsFromFile(RAW_FILE)
```

#### 2.4.2 网络/session切分

对于多线程、多网络相连等应用场景，常常需要对网络进行拆分。**ZG330Backend中默认开启MMU，不提供关闭MMU的接口，用户无需关心MMU模式**。因此既可用view方式也可用sub方式进行切分。

##### 1）View

作用：去除某些特定算子，保留所需的模型结构

使用场景：可用于网络拆分、多网络内存复用、网络衔接、剔除效率低的算子等场景

使用方法：

- 方法1：network.view([op_ids])：只保留列表中指定op_id的算子
- 方法2： network.viewExcept({op_ids})：不保留列表中指定op_id的算子
- 方法3：network.view(start_index, end_index)：通过指定算子在原网络中的start_index(包含)、end_indx（不包含），保留原网络中[start_index， end_index)中间的算子
- 方法4：network.view(start_index)：通过指定算子在原网络中的start_index(包含)，保留start_index之后的网络结构
- 方法5：viewByOpId(start_op_id, end_op_id)：通过指定原网络中的开始算子id（包含），结束算子id（包含），保留原网络中[start_op_id, end_op_id]中间的算子，需考虑算子执行序（可查看json）

注意：

- op_id算子的ID，使用Icraft-show可以在每个算子右上角找出其对应的op_id，也可点击算子查看，op_id大小与算子在计算流中的顺序无关联。 
- 算子的索引值index指该算子在计算流中顺序，索引编号从0开始，因此对于总共有N个算子的网络而言，最后一个算子的索引编号为N-1。

使用举例：

<img src="./assets/input_opid.png" alt="op_id" style="zoom: 67%;" />



![image-20251216142009385](./assets/output_opid.png)

上图是使用Icraftshow工具将yolov5s generate阶段生成的json&raw文件可视化后的部分网络结构

- 示例1：若只保留网络中的677算子
  - 采用方法1，network.view([677])
  - 采用方法3， network.view(2,3)
- 示例2：若网络需去除最后一个算子
  - 采用方法2， network.viewExcept({147})
- 示例3：若网络只保留最后的算子
  - 采用方法3，先计算出总共有N个算子，再倒退其索引编号，network.view(N-2，N-1)
  - 采用方法4， network.view(-1)
- 示例4：若网络只保留第一层卷积后的算子
  - 采用方法4，由于第一层卷积是第4个算子，network.view(3)
- 示例5：若只想保留网络中间结构，比如某两个卷积
  - 采用方法5，可直接指定起始和结束op_id，network.viewByOpId(620,621)

##### 2）sub

作用：直接对session操作，去除某些特定算子，保留所需的模型结构。

使用场景：可用于去除某些特定算子、网络拆分场景。

使用方法：

- ```cpp
  Session sub(int64_t start_index, int64_t end_index) const 
  ```

  - 释义：从该session创建一个新的session, 共享相同的backends, 但是network_view不同。

  - 参数说明：start_index是指NetworkView包含的算子在原网络中的开始索引(包含)，支持负数索引；

    end_index是指NetworkView包含的算子在原网络中的结束索引(不包含)，支持负数索引；

  - 举例：

  ```cpp
  auto sess_icore = net_session.sub(2,-2);
  ```

  - 返回：创建的session；

- ```cpp
  Session sub(int64_t start_index) const 
  ```

  - 释义：从该session创建一个新的session, 共享相同的backends, 但是network_view不同。

  - 参数说明：start_index是指NetworkView包含的算子在原网络视图中的开始索引(包含)，支持负数索引；

  - 返回：创建的Session；

说明：通过sub去直接拆分session，sub的用法不同于view，其直接作用于已经创建的session，通过sub拆分的两个session的backend使用的是同一个物理分段信息，因此不需要使用内存复用将输入段和输出段相连。 天然就是相连的。

**不同的网络在实际拆分时候，需根据实际需求及网络本身决定送入sub的参数(start_index,end_index)，即session切分的位置。**

### 2.5 会话构造

#### 2.5.1 创建session

创建Session的过程就是将算子绑定到后端的过程。创建Session有三种方法：按优先级将算子绑定到指定的后端、直接使用绑定好算子和设备的后端、直接使用序列化Session得到的快照文件。这里介绍按优先级将算子绑定到指定的后端类型。

**c++示例**

```c++
// 创建Session1
// 该方法会根据模板参数的顺序，自动创建Backend对象
// 首先尝试将算子绑定到ZG330Backend上，如果不支持，则绑定到HostBackend上
auto sess1 = Session::Create<ZG330Backend, HostBackend>(network, { zg330_device, host_device });

// 获取sess1所有的Backends
auto& backends = sess1->backends;
auto zg330_backend = backends[0].cast<ZG330Backend>();
```

**python 示例**

```python
# 导入必要的库
import xir
from xrt import *
from host_backend import *
from zg330backend import *

# 创建Session，按顺序传入想要绑定的后端类型
sess1 = Session.Create([ZG330Backend, HostBackend], network.view(0), [ zg330_device, host_device ])
sess1.apply()

zg330_backend = ZG330Backend(sess.backends[0])
```

> 当网络部署完成后，只有通过Session的forward接口才能真正执行前向推理，ZG330Backend中不具备该能力。 如果使用者要搭建真正能前向运行的应用工程，请按照： **构建Session->初始化各个Backend->调用forward执行推理** 的流程进行搭建。

#### 2.5.2 开启内部计时 

`Session` 还支持一些选项，以控制前向的过程。目前支持的选项如下：

- `enableTimeProfile` ，使能时间分析功能

使用示例：

```cpp
// 加载网络

// 创建Session

// 使能时间分析
session.enableTimeProfile(true);
session.apply();
// 前向
auto input_dtype = network.inputOp()[0].dtype();
auto input_tensor = img2Tensor(img_file, input_dtype);
auto output_tensors = sess.forward({ input_tensor });

// 获取时间分析结果
auto profile_results = session.timeProfileResults();
for (auto&& [op_id, result] : profile_results) {
   auto&& [wall, mem, hard, other] = result;
   fmt::print("op_id = {}, wall = {}ms, mem = {}ms, hard = {}ms, other = {}ms\n", op_id, wall, mem, hard, other);
   }
// 获取总的执行时间
// 无论是否开启时间分析功能，总时间都会被统计
fmt::print("total_time = {}s\n", sess.totalTime<seconds>());
```

注意：

- 内部计时结果的获取可通过`session.timeProfileResults()`获得,但必须在`session.forward()`之后调用 
- 在`modelzoo utils/icraft_utils/`中的`calctime_detail`函数可以方便的完成内部计时结果的获取，可参考[Part 4 性能分析及优化](./Part%204%20性能分析及优化.md)进一步进行耗时分析及优化。

#### 2.5.3 session优化

`ZG330Backend` 提供了一些可以优化运行性能、以及调试的功能， 需要注意的是这些接口的配置时机，按照配置时机可分为：

- apply部署前配置：userReuseSegment、userConnectNetwork、disableMergeHardOp、disableEtmOptimize、ocmOptimize
- apply部署后配置：precheck，log

##### 2.5.3.1 userReuseSegment内存复用

内存复用，目的是复用数据或内存空间，减少网络部署对etm的使用量， 相同网络可以复用权重/指令/中间层数据段，不同网络只能复用中间层数据段。

（代码来源：icraft docs）

**c ++ 示例：**

```cpp
// 1.开启设备

// 2.加载网络

// 3.直接构建ZG330Backend
icraft::xrt::Backend backend1 = ZG330Backend::Init();
icraft::xrt::Backend backend2 = ZG330Backend::Init();
backend1.init(network, device);
backend2.init(network, device);

// 4.获得需要的内存空间大小
auto instr_bytesize = backend1->logic_segment_map.at(SegmentType::INSTR)->byte_size;
auto weight_bytesize = backend1->logic_segment_map.at(SegmentType::WEIGHT)->byte_size;
auto ftmp_bytesize = backend1->logic_segment_map.at(SegmentType::FTMP)->byte_size;

// 5.分配内存，起始地址需要4096字节对齐
auto weight_chunk = device.defaultMemRegion().malloc(weight_bytesize, true, 4096);
auto instr_chunk = device.defaultMemRegion().malloc(instr_bytesize, true, 4096);
auto ftmp_chunk = device.defaultMemRegion().malloc(ftmp_bytesize, true, 4096);

// 6.同一个网络可以复用指令、权重
backend1.userReuseSegment(weight_chunk, SegmentType::WEIGHT);
backend2.userReuseSegment(weight_chunk, SegmentType::WEIGHT);
backend1.userReuseSegment(instr_chunk, SegmentType::INSTR);
backend2.userReuseSegment(instr_chunk, SegmentType::INSTR);
backend1.userReuseSegment(ftmp_chunk, SegmentType::FTMP);
backend2.userReuseSegment(ftmp_chunk, SegmentType::FTMP);

// 7. 完成网络在设备上的部署
backend1.apply();
backend2.apply();
```

**python示例：**

```python
# 1.导入必要的库

# 2.打开设备

# 3.加载网络

# 4.构造ZG330Backend
backend1 = Backend.Create(ZG330Backend, network.view(0), device)
backend2 = Backend.Create(ZG330Backend, network.view(0), device)
zg330_backend1 = ZG330Backend(backend1)
zg330_backend2 = ZG330Backend(backend2)

# 5.获得需要的内存空间大小
instr_bytesize = zg330_backend1.logic_segment_map[SegmentType.INSTR].byte_size
weight_bytesize = zg330_backend1.logic_segment_map[SegmentType.WEIGHT].byte_size
ftmp_bytesize = zg330_backend1.logic_segment_map[SegmentType.FTMP].byte_size

# 6.分配内存，起始地址需要4096字节对齐
weight_chunk = device.defaultMemRegion().malloc(weight_bytesize, True, 4096)
instr_chunk = device.defaultMemRegion().malloc(instr_bytesize, True, 4096)
ftmp_chunk = device.defaultMemRegion().malloc(ftmp_bytesize, True, 4096)

# 7.同一个网络可以复用指令、权重
zg330_backend1.userReuseSegment(weight_chunk, SegmentType.WEIGHT)
zg330_backend2.userReuseSegment(weight_chunk, SegmentType.WEIGHT)
zg330_backend1.userReuseSegment(instr_chunk, SegmentType.INSTR)
zg330_backend2.userReuseSegment(instr_chunk, SegmentType.INSTR)
zg330_backend1.userReuseSegment(ftmp_chunk, SegmentType.FTMP)
zg330_backend2.userReuseSegment(ftmp_chunk, SegmentType.FTMP)

# 8. 完成网络在设备上的部署
backend1.apply()
backend2.apply()
```

##### 2.5.3.2 userConnectNetwork多网络输入输出连接

多网络输入输出连接，目的是复用多个网络在 `PL DDR` 上的数据结果，减少PL和PS之间拷贝的耗时，提高网络运行性能。

(代码来源：icraft docs)

**c ++ 示例：**

```cpp
// 1.打开设备

// 2.加载网络

// 3.直接构建ZG330Backend
icraft::xrt::Backend zg330_backend1 = ZG330Backend::Init();
icraft::xrt::Backend zg330_backend2 = ZG330Backend::Init();
zg330_backend1.init(network, device);
zg330_backend2.init(network, device);

// 4.分配需要复用的网络输入/输出的内存空间，进行内存复用
auto output_id = 10;       //网络1输出value的id
auto input_id = 1;         //网络2输入value的id
auto output_size = network.getValueById(output_id).storageBytes();
//memchunk起始地址要求4096对齐
auto memchunk = zg330_device.defaultMemRegion().malloc(output_size, true, 4096);
zg330_backend1.userConnectNetwork(memchunk, output_id);
zg330_backend2.userConnectNetwork(memchunk, input_id);

// 5.完成网络在设备上的部署
zg330_backend1.apply();
zg330_backend2.apply();
```

**python示例：**

```python
# 1.导入必要的库

# 2.打开设备

# 3.加载网络

# 4.构造ZG330Backend
backend1 = Backend.Create(ZG330Backend, network.view(0), device)
backend2 = Backend.Create(ZG330Backend, network.view(0), device)
zg330_backend1 = ZG330Backend(backend1)
zg330_backend2 = ZG330Backend(backend2)

# 5.获得需要的内存空间大小
instr_bytesize = zg330_backend1.logic_segment_map[SegmentType.INSTR].byte_size
weight_bytesize = zg330_backend1.logic_segment_map[SegmentType.WEIGHT].byte_size
ftmp_bytesize = zg330_backend1.logic_segment_map[SegmentType.FTMP].byte_size

# 6.分配内存，起始地址需要4096字节对齐
weight_chunk = device.defaultMemRegion().malloc(weight_bytesize, True, 4096)
instr_chunk = device.defaultMemRegion().malloc(instr_bytesize, True, 4096)
ftmp_chunk = device.defaultMemRegion().malloc(ftmp_bytesize, True, 4096)

# 7.同一个网络可以复用指令、权重
zg330_backend1.userReuseSegment(weight_chunk, SegmentType.WEIGHT)
zg330_backend2.userReuseSegment(weight_chunk, SegmentType.WEIGHT)
zg330_backend1.userReuseSegment(instr_chunk, SegmentType.INSTR)
zg330_backend2.userReuseSegment(instr_chunk, SegmentType.INSTR)
zg330_backend1.userReuseSegment(ftmp_chunk, SegmentType.FTMP)
zg330_backend2.userReuseSegment(ftmp_chunk, SegmentType.FTMP)

# 8. 完成网络在设备上的部署
backend1.apply()
backend2.apply()
```

##### 2.5.3.3 disableMergeHardOp关闭算子连贯执行模式

关闭算子连贯执行模式，ZG330Backend默认是使用算子连贯执行模式，该模式下连续在icore上运行的算子连贯执行，从而减少频繁同步带来的软件开销。对于一些调试场景下，如需关闭，代码示例如下，省略的步骤详见其他章节的代码示例：

(代码来源：icraft docs)

**c ++ 示例：**

```cpp
// 1.打开设备

// 2.加载网络

// 3.构建ZG330Backend

// 4.关闭算子连贯执行
zg_backend.disableMergeHardOp();

// 5.完成网络在设备上的部署
zg_backend.apply();
```

**python示例：**

```python
# 1.导入必要的库

# 2.打开设备

# 3.加载网络

# 4.构造ZG330Backend

# 5.关闭算子连贯执行
zg_backend.disableMergeHardOp()

# 6. 完成网络在设备上的部署
zg_backend.apply()
```

##### 2.5.3.4 disableEtmOptimize关闭etm内存回收

ZG330Backend默认开启etm内存回收，网络中间层特征图的内存可以复用，从而减少对etm的使用量。 对于一些调试场景下，如需关闭，则在apply前调用`zg_backend.disableEtmOptimize();`

(代码来源：icraft docs)

**C++示例：**

```cpp
// 1.打开设备

// 2.加载网络

// 3.构建ZG330Backend

// 4.关闭算子连贯执行
zg_backend.disableEtmOptimize();

// 5.完成网络在设备上的部署
zg_backend.apply()
```

**python示例：**

```python
# 1.导入必要的库

# 2.打开设备

# 3.加载网络

# 4.构造ZG330Backend

# 5.关闭算子连贯执行
zg_backend.disableEtmOptimize()

# 6. 完成网络在设备上的部署
zg_backend.apply()
```

##### 2.5.3.5 ocmOptimize ocm优化

选择ocm优化方案，ZG330Backend默认使用OcmOpt::BEST_SCORE，即开启ocm优化并选取方案一、方案二、方案三中评分最高的作为最后使用的方案， 开启ocm优化可以将满足条件的特征图存放在片上存储中，从而减小读写etm的带宽压力，提高模型在硬件上的运行效率。

(代码来源：icraft docs)

**C++示例：**

```cpp
// 1.打开设备

// 2.加载网络

// 3.构建ZG330Backend

// 4.关闭算子连贯执行
zg_backend.ocmOptimize(OcmOpt::None);       //不做ocm优化
zg_backend.ocmOptimize(OcmOpt::OPTION1);    //方案一：全局评分法
zg_backend.ocmOptimize(OcmOpt::OPTION2);    //方案二：局部最优动态规划法
zg_backend.ocmOptimize(OcmOpt::OPTION3);    //方案三：顺序按评分踢出法
zg_backend.ocmOptimize(OcmOpt::BEST_SCORE); //选取方案一、二、三中评分最优的方案

// 5.完成网络在设备上的部署
zg_backend.apply()
```

**python示例：**

```python
# 1.导入必要的库

# 2.打开设备

# 3.加载网络

# 4.构造ZG330Backend

# 5.关闭算子连贯执行
zg_backend.ocmOptimize(OcmOpt.NONE);
zg_backend.ocmOptimize(OcmOpt.OPTION1);
zg_backend.ocmOptimize(OcmOpt.OPTION2);
zg_backend.ocmOptimize(OcmOpt.OPTION3);
zg_backend.ocmOptimize(OcmOpt.BEST_SCORE);

# 6. 完成网络在设备上的部署
zg_backend.apply()
```

> 需要注意，在一个完成的运行时工程中，每个session只需要apply一次，后续session可以推理多次，不要在session循环推理过程中多次apply。
>

### 2.6 模型前向推理

```Python
# 模型前向推理
generated_output = session.forward( [input_tensor] )	# 单输入时
# generated_output = session.forward( [input_tensor_0, input_tensor_1] )	# 多输入时
```

> 注意事项：session.forward(inputs:list(Tensor))中inputs为输入的icraft.Tensor列表，输出为整个网络视图的前向结果。
>

### 2.7 输出结果搬运

需手动将输出结果从plddr向psddr上搬移，且需将排布转为软件排布，之后可以接自己所需要的模型后处理。

调用以下接口：

**c++示例**

```c++
auto host_tensor = output_tensors[i].to(HostDevice::MemRegion());
```

**python示例**

```python
out_tensor = out_tensor[i].to(icraft.xrt.HostDevice.MemRegion())
```

### 2.8 模型后处理

获得正确排布的输出结果后，模型推理输出tensor最终搬移到ps端之后，可进一步在ps端进行进行模型自身所需要的后处理，这部分因模型而异。

可以参考yolov5的后处理部分代码来仿写图像检测相关模型的后处理。

如果想进一步加速后处理计算，icraft buyi提供运行时直调硬算子，也可自行开发硬算子进行加速。

总结：同模型前处理部分，最简单的要求是与框架下模型后处理对齐。

### 2.9 关闭设备​	

```Python
# 关闭设备
Device.Close(device)
```

## 2.10 特别说明

连续推理多个样本时，执行完前向需要waitForReady，搬数结束后需要重置，重置相关接口为`device.reset(0)`（充分重置，相关寄存器也会清空，耗时稍久但更安全）或`device.reset(1)`（快速重置，不会清空相关寄存器，耗时短），**推荐device.reset(1)**。

python示例：

```python
# ...
# session.apply()

# 测试样本文件夹
test_path = os.listdir('./test_data')
for file_name in test_path:
    data_in = os.path.join('./test_data',file_name)
    
	'''
	你的模型源码前处理
	'''
    
    # torch.tensor转成numpy
    input_0 = speech.numpy().astype(np.float32)
    input_1 = speech_lengths.numpy().astype(np.float32) 
    
    # numpy转成icraft.tensor
    input_tensor_0 = icraft.xrt.Tensor(input_0, icraft.xir.Layout("**C"))
    input_tensor_1 = icraft.xrt.Tensor(input_1, icraft.xir.Layout("C"))
	
    # 模型前向，这里的demo是两个ftmp输入
    icraft_outputs = session.forward( [input_tensor_0,input_tensor_1] )
    print('session forward done')
	
    # 输出结果搬运
    outputs = []
    for out in icraft_outputs:
      # 因模型较大，所以连续推理样本时，需waitForReady
      ready = out.waitForReady(datetime.timedelta(seconds=20), datetime.timedelta(milliseconds=50))
      # 数据搬运
      out = out.to(icraft.xrt.HostDevice.MemRegion())
      out_np = np.array(out)
      outputs.append(out_np)
	
    # 重置
    device.reset(1)

	'''
    模型后处理
    yseq= torch.from_numpy(outputs[1])[0, :100, :].argmax(dim=-1)
    '''
  # 关闭设备，整体运行时结束  
  icraft.xrt.Device.Close(device)    
```

## 3 结束语

常用数据类型介绍与api用法可参见我们[modelzoo_utils](https://gitee.com/mxh-spiger/modelzoo_utils/tree/master/)仓库，当然啦，安装了icraft后还可以使用icraft docs命令查看所有icraft相关的使用方法以及一些基础知识介绍，再次强调，我们这里的所有教程均是基于icraft docs的展开与总结。
