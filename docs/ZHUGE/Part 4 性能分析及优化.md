![](https://pandao.github.io/editor.md/images/logos/editormd-logo-180x180.png) 
# Part 4 性能分析及优化


本教程主要介绍模型部署的性能分析（如耗时分析）及对应的优化方法。

## 1. 耗时分析及优化
- 内部计时统计方法详见`Part 3-1 runtime/2.5.2 开启内部计时`，对获取网络内部运行时间进行了相关介绍；

- 如需进一步获取网络部署各阶段的详细信息，可使用时间分析工具，即`modelzoo utils/pyrtutils/`中的analyze_time_utils_zg.py对网络进行耗时瓶颈分析、从而制定针对性优化部署方案等。

### 1.1 耗时分析

#### 1.1.1 环境配置

在使用耗时评估工具前，需先安装数据分析支持库pandas、openpyxl，安装命令如下：

```bash
pip install pandas==1.3.5
pip install openpyxl
```

#### 1.1.2 使用方法

**前提条件**：已通过PyRT工程中的`calctime_detail(session, network, name=result_table_name)`接口或CRT工程中的`calctime_detail(session)`接口获得时间测试的结果（PyRT:excel、CRT: txt）。

耗时分析代码如下：

```python
import os 
from modelzoo_utils.pyrtutils.analyze_time_utils_zg import analyze_time,txt_to_excel
root_path = "./30TAI_release/30TAI_time/Time_Test_Log_V3.33.0/ZG/"
file_name = "yolov5_obb/yolov5m_obb_int8_ocm-1/yolov5m_obb_time.txt"
# file_name = "CAMpplus/CAMpplus_int8_ocm-1/CAMpplus_time.xlsx"
file_path = os.path.join(root_path,file_name)
# 获取文件扩展名（不区分大小写）
_, extension = os.path.splitext(file_path)
extension = extension.lower()

if extension == '.txt':
    excel_file_path = file_path.replace('.txt','.xlsx')
    txt_to_excel(file_path,excel_file_path)
    analyze_time(filepath=excel_file_path,speedMode=False)
elif extension == '.xlsx':
    analyze_time(filepath=file_path,speedMode=False)
else:
    print(f"错误：不支持的文件格式 '{extension}'。请提供 .txt 或 .xlsx 文件。")
    
print('Analyze done.')
```

其中file_path为PyRT工程中的`calctime_detail(session, network, name=result_table_name)`接口或CRT工程中的`calctime_detail(session)`接口获得时间测试的结果（PyRT:excel、CRT: txt）<br>
耗时分析结果默认保存至file_path路径下<br>

最终可以得到：

- 网络各阶段耗时细则
- 单帧延时&吞吐率分析结果
- 各类硬算子耗时统计结果
- 所有op的总耗时情况
- 各类Hardop算子的耗时细则
- 各类Hardop算子的耗时堆叠柱状图（hard_time\other_time）

#### 1.1.3 指标详解

计时统计结果中主要统计了以下耗时情况：
* total_time:总耗时
* memcpy_time:数据搬移耗时，包括数据在psddr↔plddr之间的传输时间
* hard_time:硬件计算耗时
* other_time:软件调度时间等
  

以yolov5s axi上板为例：

![image](./assets/yolov5s_time.png)

Total_TotalTime：所有算子总耗时

Total_MemcpyTime：所有算子总数据搬移耗时

Total_HardTime：所有算子总硬件计算耗时

Total_OtherTime：所有算子总软件调度耗时

Hardop_TotalTime：hardOp算子总耗时

Hardop_MemcpyTime：hardOp算子总数据搬移耗时

Hardop_HardTime：hardOp算子总数据搬移耗时

Customop_Total_Time：fpgaOp算子总耗时

Customop_Hard_Time:fpgaOp算子硬件计算耗时

IO_MemcpyTime:IO算子数据搬移耗时

IO_HardTIme：IO算子硬件计算耗时

IO_OtherTime：IO算子软件调度时间

基于上述时间，可进一步得到：数据传入时间、网络主体耗时、cpu算子耗时、io算子时间。

用户需要对网络进行**各阶段耗时细则分析**、**单帧延时&吞吐率分析**、**各类硬算子耗时统计分析**，以及计算**所有op的总耗时情况**。

若单帧延时&吞吐率分析结果表明**性能瓶颈在于hardop**时，则**在运行时不要使用speedmode，来规定网络不要合并算子**，对算子timeprofile进行分析，主要是对网络进行**各类Hardop算子的耗时细则分析**、**各类Hardop算子的耗时堆叠柱状图**（hard_time\other_time）、**所有Hardop算子的耗时堆叠柱状图**（hard_time\other_time）分析。

下面将介绍不同耗时评估表的含义：

#####  （一）整体性能评估

##### 1.1.3.1 网络各阶段耗时细则（需确认）

主要阶段有： 数据输入+icore(npu)+数据处理+Icraft-Cpu算子操作+后处理时间(需自行填写)。

目前版本诸葛只支持硬算子detpost

- 数据输入：∑hardop:memcpy_time，即 cdma数据搬移耗时。
- 网络主体耗时 - Icore(npu)：∑ （is_io_process = False的hardop）:（total_time -memcpy_time），即 npu上io无关的HardOp算子耗时。
- Icraft-CPU算子耗时：
∑(非HardOp&非CustomOp):total_time
- 数据处理时间:∑(is_io_process=True 的 HardOp)(total_time - memcpy_time)


##### 1.1.3.2 单帧延时&吞吐率分析 

- 单帧延时：各阶段耗时总和

- 极限fps：

  - 若网络允许多线程并行处理数据，则**fps=1000/max(不同阶段耗时)**

  - 若网络不允许多线程，则**fps=1000/max(实际划分线程耗时)**

- 耗时瓶颈：耗时最长阶段所对应的名称

**注意**：**上述均可实时计算出来，用户可在耗时分析生成的表格中自行填写网络后处理耗时，表格会根据用户填写的值自动计算单帧延时、吞吐率、耗时瓶颈**

### 1.2 耗时优化

#### 1.2.1 各阶段算子耗时说明

目前版本诸葛框架仅支持`detpost`硬算子，用于检测网络的后处理加速。

**(一) 不带detpost时**

以yolov5-axi上板为例，用`icraft show`命令查看yolov5模型的相关算子(icraft show展示中的灰色框是前处理和后处理算子，不属于模型内算子)：

- 数据输入阶段主要包含了以下类型：`icraft::xir::Input、icraft::xir::Reshape、icraft::xir::Cast、icraft::xir::Conv2d`，其中这里的`icraft::xir::Conv2d`算子主要用于前处理归一化等一些计算（代替了之前的`add`和`alignaxis`等前处理算子)。
- 数据输出阶段主要包含以下类型：
  - 带detpost硬算子：`customop::DetPostZG、icraft::xir::output`
  - 不带硬算子：`icraft::xir::Reshape、icraft::xir::Matmul、icraft::xir::Cast、icraft::xir::Transpose、icraft::xir::PruneAxis、icraft::xir::output`，这里的算子主要用于将数据的硬件排布转换成软件排布，以及将相关数据从plddr搬到psddr。


**不同算子类型含义说明：**

- `icraft::xir::Input`：输入；
- `icraft::xir::Cast`：is_io_process=True的HardOp，用于数据类型转换等；
- `icraft::xir::output`：输出；
- `icraft::xir::HardOp`：is_io_process=False的HardOp，利用NPU完成前向的算子类型；
- `icraft::xir::PruneAxis`：is_io_process=True的HardOp，adapt阶段增加的算子类型，进行去通道操作，用于恢复原来数据通道排布；
- `customop::DetPostZG`：fpga端硬算子，可用于数据从plddr搬到psddr，以及完成部分检测网络的后处理操作；


当了解各阶段中算子的功能后，再结合耗时评估分析得到的各类数据，就能对Icraft算子各阶段耗时情况更加清晰，也能帮助评估算子耗时是否合理，进而制定针对性的部署优化方案。

#### 1.2.2 耗时优化建议

根据耗时分析结果，可发现耗时瓶颈无非在于以下各阶段： 

 **数据输入、icore(npu)、数据输出、 Icraft-cpu算子操作耗时、后处理**

因此，对应不同的情形，大致可做以下优化：

- 情形1：**耗时瓶颈在于icore(npu)**，即hardop的hardtime不满足要求时，可采取以下优化手段：
  - 查看网络**是否有性能瓶颈算子**（不开speedmode,分析时间），交由开发人员优化；
  - 考虑**减小输入图片尺寸**，对输入图片进行resize是减少hardtime最有效手段；
  - 评估**网络内部能否继续裁剪**，可通过调整卷积大小缩小h，w，c等，修改后重训；
  - 尝试**替换更小的或者性能更好的模型**；
  - 使用多芯片并行（未来）
- 情形2：**耗时瓶颈在于数据输入or数据输出or后处理**，即npu的io耗时长导致延时长时，可采取以下优化手段：
  - 通过`detpost`硬算子缩短数据输出+后处理时间；
  - 数据输入时间过长时，可以**自定义搬数fpga模块**；
  - 数据输出+后处理时间过长时，可以**自定义后处理fpga模块**；
- 情形3：**耗时瓶颈在于Icraft-cpu算子操作耗时时**，可采取以下优化手段：
  - 可以**设计硬算子来合并部分Icraft-cpu算子**，减少耗时；
  - **去掉**icraft中的一些通用化设计产生的**cpu算子**（如cast算子等），**自行搬数**或**直接使用硬件排布与数据作为网络的输出**，接后处理操作；
- 情形4：**对吞吐率有一定要求时**，可采取以下优化手段：
  - plin运行时中可使用**多线程并行计算**来提速；
  - 多网络存在数据复用时，可在plddr上申请chunk，通过**内存复用或PLDDR<->PLDDR搬数**来减少ps↔pl搬数的时间。

#### 1.2.3 常用优化方法

##### 1.2.3.1 多线程并行计算

当网络各阶段耗时相当的情况下，可将网络拆分为多个线程并行计算，来提升吞吐率；

##### 1.2.3.2 内存复用

当多个session之间具有以下图示的连接关系时，可考虑采用**复用同一块内存区域**的方法，来减少PSDDR<->PLDDR之间反复搬数的耗时。

![image](./assets/same_chunk.png)

内存复用主要分为三步，对齐量化系数、申请chunk、链接多个session

- **步骤1：量化阶段编译时，需对齐多网络之间的normratio**

  常用的normratio对齐方法：

  ① 将net1 optimized仿真输出的ftmp作为net2的量化校准集；

  ② 将net1和net2拼接成一个网络，进行量化编译，运行时阶段再使用view将其分开；

- **步骤2：PLDDR上申请一段共用内存块memchunk**

  **方法**：分析net1输出段和net2输入段哪个更大，**谁大取谁**，申请内存块的大小 = max_bytesize
  auto chunk = device.defaultMemRegion().malloc(max_bytesize);

- **步骤3：链接多个session对象**，方法如下：

  - 首先，view掉net1输出部分和net2输入部分的cpu算子；
  - 其次，计算出net1的输出数据放入内存块的**偏移量offset；**
  - 最后，将net1输出和net2输入链接起来，**复用同一块内存块**；
    `backend_1.userSetSegment(chunk, Segment:OUTPUT,offset);`
    `backend_2.userSetSegment(chunk, Segment:INPUT,0);`


##### 1.2.3.3  PLDDR<->PLDDR搬数

当多个session之间的连接关系并非能复用连续空间地址，或者是 net_1多次运行结果存储起来才能作为net_2的输入等复杂的多网络衔接时，如下图所示，可考虑采用PLDDR<->PLDDR搬数的接口：`PLDDRMemRegion::Plddr_memcpy()`**来实现PLDDR上的数据拷贝**。

![image](./assets/plddr2plddr.png)

**说明**：`PLDDRMemRegion::Plddr_memcpy()`是将PLDDR上src的数据拷贝给PLDDR上dst的一个函数；需用户给定src存储在PLDDR上的起始&结束地址，以及需要将src拷贝到dest在PLDDR上的起始&结束地址

**参考代码**：

```c++
// 计算
auto src_base_addr = net1_output_segment->memchunk->begin.addr();
auto src_end_addr = net1_output_segment->memchunk->begin.addr() + offset;
auto dest_base_addr = net2_input_chunk->begin.addr()+ offset * index;
auto dest_end_addr = net2_input_chunk->begin.addr() + offset * (index + 1);
// PLDDR->PLDDR搬数，将net1的输出搬移至net2输入的指定位置
PLDDRMemRegion::Plddr_memcpy(src_base_addr, src_end_addr, dest_base_addr, dest_end_addr, device);

```

**注意：src和dest地址长度需一致，且必须是64整数倍**

##### 1.2.3.4 去除Cast算子

当使用耗时分析接口发现网络中**输出部分`cast`等cpu算子耗时较长时**，可在运行时阶段考虑去除耗时长的`cast`等cpu算子，**手动进行搬数&反量化**。

详细方法如下：

```c++
// 步骤1：view掉imk&后处理cpu算子, 只保留硬算子和hardop 
auto net2_icore = network_2.viewExcept
({ 0,134, 151, 152, 174, 175, 212, 213, 214, 215, 216, 217, 45 });
...
    
// 步骤2：从json文件中读取输出层的缩放系数，网络输出的是定点数据，需要该系数来转换为浮点数据
std::vector<float> net2_normratio = getOutputsNormratio(net2_icore);
std::cout << "net2_normratio:" << std::endl;
std::cout << net2_normratio[0] << std::endl;
...
session.forward()
...
    
// 步骤3：手动搬数&反量化：将输出的tensor从plddr搬移到psddr，并反量化回float32
// 手动搬数，plddr->psddr
auto host_tensor_0 = output_tensors[0].to(HostDevice::MemRegion());
auto net2_data_ptr_0 = (int8_t*)host_tensor_0.data().cptr();
cv::Mat cls_score = cv::Mat(289, 1, CV_8S, net2_data_ptr_0);
// 反量化，int8->float32
cls_score.convertTo(cls_score, CV_32F);
cls_score = cls_score * net2_normratio[0];

```

##### 1.2.3.5 合并算子

当使用耗时分析工具发现，**hardop算子的other_time耗时过长，导致hardop的totaltime远高于hardtime时**，可以**合并算子**，加速计算。

### 1.3  注意事项

* 在使用PyRT的计时接口`calctime_detail(session, network, name=result_table_name)`或耗时分析接口`analyze_time_zg()`前，需**先安装指定版本的pandas**（`pip install pandas==1.3.5`）；

* 卡`session.forward()`代码前后的时间与调用计时接口得到的时间统计结果会有一些偏差，是因为计时接口中统计的是Icraft内部的耗时，不包含从运行工程到Icraft的软件调度等时间；

* 若需要对每个算子耗时进行分析，查看npu算子性能瓶颈，则**不要合并算子**，**在运行时中不要使用speedmode**；


## 2. 精度分析及优化

本节主要介绍网络部署后精度损失的分析思路和调优方法，模型部署需要经过若干个环节，每个环节都可能引起误差，与软件层面的模型导出、模型编译、硬件的具体实现均有关系。<br>
<img src = ./assets/model_life.png  ><br>
可能引入误差的三大环节：<br>

* (1)模型导出

  * 算子的非等效修改/替换
* (2)模型编译
  * 量化不可避免会引入误差
  * 其他组件(如图优化)也可能引入误差
* (3)运行时部署


如果精度与浮点有差距，排查流程为：<br>

* （1）测试导出的torchscript或onnx模型与框架下浮点模型是否一致
* （2）测试各阶段(parse/optimize/quantize/adapt/generate)仿真结果与框架下浮点模型是否一致
* （3）检查影响精度的参数

### 1.精度分析

建议按照部署流程，自顶向下依次排查以下环节：模型导出、解析、优化、量化、适配、指令生成、运行时。
<img src = ./assets/models_life2.jpg  >

#### 1.模型导出误差分析

**可能误差来源**：

* 修改源码引入误差
* weights与code版本不一致(如模型库中的模型请使用与commits_id对应的weights)
  <img src = ./assets/models_life3.jpg  >
  <br>
  **检查方法**：
* 对比0_infer.py vs 2_save_infer.py结果是否一致
  * 0_infer.py 为用源码推理一张图
  * 2_save_infer.py为用导出模型推理一张图
* 确保输入一致，对输出进行数据级别比较，用于验证导出的模型与原模型一致 

#### 2.模型编译误差分析

##### 2.1 parse阶段可能误差来源：

* input size与导出的模型输入尺寸不一致
* 归一化参数、顺序不对
* channel swap参数
* 导出模型时基于的(onnx、pytorch)框架版本不对
* 导出模型时
* 组件bug 
  注意事项：
* icraft 2.x 与 icraft 3.x对框架版本要求发生了一定变化，请注意区分
  * icraft 2.x框架要求
    <img src = ./assets/models_life4_icraft2.jpg  > 
  * icraft 3.x框架要求
    <img src = ./assets/models_life4_icraft3.jpg  >
* 归一化参数示例
  <img src = ./assets/models_life5_premean.jpg  >
* channel_swap参数示例
  <img src = ./assets/models_life6_channelswap.jpg  >

* 模型输入、归一化系数、channel_swap、框架版本与源码工程不一致，都会引起误差
  应着重分析此部分

**检查方法**：

* 对比2_save_infer.py vs 3_sim_infer.py结果
* (如果该模型未提供3_sim_infer.py)对比2_save_infer.py vs 3_deploy/modelzoo/modelx/的parse仿真结果
  <img src = ./assets/models_life7_compile.jpg  > 

##### 2.2 optimize阶段可能误差来源：

* 某些pass引起的误差，确认后可通过开关特定pass解决
* 组件bug

**检查方法**：
对比3_deploy/modelzoo/modelx/的parse和optimize仿真结果
<img src = ./assets/models_life8_parsevsopt.jpg  >

##### 2.3 quantize阶段可能误差来源：

* 量化误差
* 组件bug

**检查方法**：

* 借助icraft量化分析工具，取单帧输出结果进行数据级的对比
* 对比3_deploy/modelzoo/modelx/的optimize和quantize仿真结果，可以基于完整(或采样)数据集

**参考资料**：

> - icraft编译阶段的量化配置教程请参考：[Part 2_1 compile/3.quantize教程与案例](./Part%202_1%20compile.md)
> - 可通过量化分析工具进行量化调优：相关调优经验总结位于[量化调优经验](./assets/quantization_tricks/quantization_tricks.md)
> - icraft量化分析工具使用教程请参考:[Part 2_2 icraft_tools/2.2量化分析工具](./Part%202_2%20icraft%20tools.md)

##### 2.4 adapt阶段可能误差来源：

* 某些pass引起的误差，确认后可通过开关特定pass解决
* 组件bug

**检查方法**：
对比3_deploy/modelzoo/modelx/的quantize和adapt仿真结果

##### 2.5 generate阶段可能误差来源：

* 截位误差
* 组件bug


**检查方法**：

* 对比3_deploy/modelzoo/modelx/的adapt和generate仿真结果
* fake_qf 参数说明：
  * false(默认)，表示对特征图进行截位操作
  * true，表示不对特征图进行截位操作
* 场景1： 如果要对比adapt 和 generate的结果，应设置fake_qf = true，将adapt结果作为golden，验证generate是否正确
* 场景2：如果要对比generate和runtime上板结果，应设置fake_qf = false，验证上板结果是否与generate仿真结果一致

#### 3.运行时误差分析

可能误差来源：

* 前处理与框架不一致
* 后处理不一致
* generate或runtime阶段存在bug
  **检查方法**：
  依次排查：
* 网络参数设置  
  * 如果用了customop/Detpost，请检查模型编译时config/customop/toml中thr是否设置为0.001
  * 如果用了customop/Detpost，请检查精度测试时cfg/test.yaml中thr是否设置为0.001
  * 请检查影响精度的参数(如conf、相似度阈值等)是否与框架下保持一致
* 前处理流程与框架是否一致
* 网络输入
* 网络输出
  * 如果用了customop/Detpost,网络输出layout发生变化，请检查是否对输出layout正确进行了转换
* 后处理流程与框架是否一致

## 3. 资源占用分析及优化

### 3.1 硬件资源

以下列举针对30TAI

| 芯片型号 | JFMQL30TAI                                                   |
| :------- | ------------------------------------------------------------ |
| 制程     | 国产 28nm 工艺                                               |
| SoC      | 集成 4 核处理器 @1GHzVPU 支持 4K@30Hz 视频编解码             |
| NPU      | 集成 1 个 ZG330 iCore，@1GHz8TOPS@INT8 / 4TFLOPS@FP16 / 2TFLOPS@TF32iCore 支持 CNN/RNN/Transformer |
| FPGA     | 125K 逻辑资源，400 DSPs兼容 Xilinx 7Z030 的 PL               |
| DDR 支持 | 支持 DDR3，速率 1600Mbps位宽：PS 32bit，PL 64bit大小：PS 侧 1GB，PL 侧 4GB |
| 接口     | PCIe Gen2.0 x4，GTX x4                                       |
| 封装     | FCBGA676，27×27 mm                                           |
| 功耗     | ~10W                                                         |

### 3.2 资源占用测试方法

#### 3.2.1 测试cpu占用率

可命令行输入top指令查看 

#### 3.2.2 测试plddr占用率 

借助icraft show查看内存使用情况
借助运行时工程进行统计

#### 3.2.3 内存使用率

<img src = ./assets/xterm-monitoring.png  > 
可开启ssh工具的监测功能（如MobaXterm- Remote Monitoring）,关注ext4和FAT32分区内的剩余容量，如果快满了可以把文件删掉一些，避免sd卡损坏或文件丢失。



