# Part 1-2 概念附录

> 如果你对我们icraft中的一些技术名词感到疑惑，以下仅供参考


## 名词解释
### Icraft

Icraft 是复微智能计算平台基于自主研发的 FPAI 芯片的软件工具套件，由编译、运行 和分析工具等几部分组成。

布衣架构：布衣(BY)架构为100TAI产品对应的NPU架构 BuyiBackend:是Icraft运行时的后端之一， 负责处理指定100TAI为计算资源的算子

诸葛架构：诸葛(ZG)架构为30TAI产品对应的NPU架构 ZG330Backend:是Icraft运行时的后端之一， 负责处理指定诸葛架构30TAI芯片为计算资源的算子

### XIR

Icraft各组件采用的统一的自定义中间层数据结构。

### XRT

Icraft统一前向后的运行时。

#### Device

抽象了具体的硬件设备，比如一个板卡。一个 Device 中包含若干个 RegRegion 和 MemRegion ，分别表示不同的寄存器区域和内存区域。

#### MemChunk

MemRegion 分配的一块连续内存。

#### Tensor

表示前向的输入和输出，包含 dtype 和 chunk 等属性分别描述数据类型和和数据。

#### Backend

表示某一种计算资源，比如Host(CPU), Cuda（GPU）和Buyi(加速器)。 Backend 包含了其所支持的每一个算子在该计算资源上初始化和前向计算的逻辑。

#### HostBackend

Icraft XRT的计算后端之一，主要负责Host(CPU)和Cuda(NvidiaGpu)计算资源的初始化和前向计算，其中x86 CPU支持所有的内置算子，SOC CPU仅支持 add、 align_axis、 cast、 input、 multiply、 output、 prune_axis、 reorg、 reshape、 resize、 swap_order、 yolo、 region 13个算子。

#### BuyiBackend

Icraft运行时的后端之一， 负责处理指定为NPU和PL等计算资源的算子， 只支持 HardOp 和 Cast 等利用NPU完成前向的算子。

#### zg330backend

Icraft运行时的后端之一， 负责处理指定诸葛架构30TAI芯片为计算资源的算子。

#### Session

表示会话（进程），主要的用户接口。创建 Session 的过程就是资源分配的过程，即将网络的每一个算子绑定到合适的计算资源（ Backend ）上。

### parse

Icraft解析组件,将框架的模型文件转换为Icraft的中间层。

### optimize

Icraft优化组件,支持的优化Pass有：算子替换、算子融合、算子消除、排布优化、硬件适配等,对解析后的网络结构或算子属性进行变换，以优化性能或匹配硬件。

### quantize

Icraft量化组件，将浮点网络模型转换为定点模型，以匹配硬件加速器。

### adapt

Icraft适配组件，执行一系列Pass，对量化后的网络结构或算子属性进行变换，以优化性能或匹配硬件。

### generate

Icraft指令生成组件，将NPU支持加速的的算子转换为相应的指令序列，以在NPU上执行。

### json&raw

json 文件描述了网络结构，以及与 raw 文件数据的对应关系；raw 文件描述了网络参数、指令序列等。

### Icraft show

Icraft可视化组件，用于可视化网络、量化分析等。

## 运行时
### compressFtmp

将网络输出的中间层进行优化，可以显著减少网络在ETM上（即PL DDR）的内存占用。

### userSetSegment

将网络某个分段使用的内存与用户申请的内存相绑定，从而可以实现多网络间的内存复用。

### speedMode

将执行序上连续的、相同计算精度的HardOp合并执行，能够降低软件调度产生的时间开销。

### view & sub

- view用于从原网络中创建一个子网络，创建session时buyibackend会重新计算物理分段信息。
-  sub用于从原session中创建一个新的session, 这两个session共享相同的backends资源，但是network_view不同。
-   view相比于sub的使用场景更为灵活和广泛，但对于只涉及一个网络的情况下， sub可以不使用内存复用（userSetSegment）实现输入输出相连，更适用于较为简单的网络拆分场景。

### MMU

- **MMU**模式： 把网络操作地址看作逻辑地址，运行时根据实际情况构造逻辑地址和物理地址的映射关系，无需修改网络指令；在编译阶段完成合并算子、内存优化的功能配置，可以有效减少运行时占用的内存。
- **非MMU**模式：网络操作地址都直接针对物理地址，面对多网络地址冲突的情况，运行时需要修改网络指令；对于复杂的网络使用情况（例如使用view拆分网络，对子网络进行合并算子和内存优化配置）， 建议使用非mmu模式。
- 不同mmu模式下，拆分网络的方法也不同，若使用mmu模式，需使用XRT中的sub接口， 若使用非mmu模式，需使用XIR中的view接口。

### MemRegion

表示设备上的一块独立的内存区域，不同的内存区域的内存访问实现方式可能不相同

- MemRegion **udma**:表示使用一块系统的CMA空间实现的交换空间，用于操作系统和PL DDR之间的数据交换。
- MemRegion **plddr**:表示NPU和PL所共同使用的PL DDR内存。

### RegRegion

 表示设备上的一块独立的寄存器区域，不同的寄存器区域的内存访问实现方式可能不相同。

- RegRegion **npu**:表示NPU上的寄存器区域。

### reset

 Device 提供了 reset 方法来对设备进行复位，该方法接受一个参数 level，表示复位等级。 不同设备对复位等级有着不同的实现。

- 0：复位全部状态，包括FPGA，NPU。
- 1：只复位NPU的计数状态。

### status

Device 提供了 showStatus 方法来显示设备的状态，该方法接受一个参数 level，表示显示等级。

- 0：显示全部信息，包括状态寄存器、计数寄存器和错误寄存器
- 1：只显示状态寄存器
- 2：只显示计数寄存器
- 3：只显示错误寄存器
- 其他：无效值，不做任何操作

### version

Device 提供了 version 方法来显示设备的版本。

- device:设备的版本，即系统位流的版本，比如 101844d2
- icore:NPU的版本，即架构设计的版本，比如 FMSHBYV1-20200903

## 部署
### 仿真

仿真编译的各阶段结果是否正确。

### 上板

将指令生成的结果部署上板。

### axi工程

基于AXI总线协议实现处理器系统(PS)和可编程逻辑(PL)之间通信的工程，多用于上板部署、时间评估。

### socket工程

基于socket完成客户端(Host)与服务端(30TAI)的通信，多用于精度测试。

### PS in

数据从PS端输入，送到PL端处理。

### PL in

数据从PL端(如摄像头)输入。
