# Part 2-2 Icraft工具
本教程主要介绍icraft提供的各种分析工具及使用场景，详细内容请参考icraft docs。

# icraft docs 
使用方法： icraft安装完毕后，运行：
```shell
icraft docs
```


* 备注：如果此前未设置html文件的默认开启方式，可选择html默认开启方式（如设置默认浏览器）后打开。
参考指令:
```shell
#设置icraft docs
cmd /c start C:/Icraft/CLI/docs/index.html
```

**强调**：docs里面介绍了所有组件的功能和使用方法，如果你无法用命令打开，可以在C:\Icraft\CLI v3.33.0\docs这里找到index.html打开即可。
以下所有介绍都是基于icraft docs里面的内容，主要是对常用工具icraft show和icraft run进行详细描述，如果想了解更多内容请关注icraft docs。

## 1.icraft show

使用方法： icraft安装完毕后，运行：
```shell
icraft show
```
可打开icraft show工具，支持以下功能：网络参数可视化、量化分析工具。<br/>

![ps1执行策略](./assets/icraft-show5.png)

* 备注：如果此前未设置html文件的默认开启方式，可选择html默认开启方式（如设置默认浏览器）后打开。参考指令:
```
# 设置icraft show
cmd /c start http://localhost:8080/
```
### 2.1网络参数可视化
     具体应用场景，如拆网络 -  观察网络结构和算子、量化分析 看参数、维度 
#### 2.1.1 输入

点击网络参数可视化按键后会弹出输入文件窗口，当前支持以下输入模式

- 输入模型的json文件
- 输入模型的json和raw文件

#### 2.1.2 显示

左上的固定节点表示了一些全局信息

- 网络名称
- 源网络所处框架
- 算子节点数目
- 当前网络所处阶段

![show主页面](./assets/icraft-show0.png)

每个节点表示一个算子，包括算子类型、算子id以及算子参数的维度信息（如果有权重的话）

每个边字表示算子间输入输出的ftmp，冒号前是ftmp的id, 冒号后面是ftmp的维度信息

![ps1执行策略](./assets/icraft-show1.png)

左键点击节点会在右侧侧边栏弹出算子的详细信息，如果在输入时输入了raw文件则可以在侧边栏 **查看weights** 按钮切换显示算子的参数信息,参数信息提供了 **十进制** 和 **十六进制** 切换显示，并且可以保存到文件按钮放到一个csv文件或二进制文件进行查看

> *值得注意的是ZG阶段网络HardOp算子instr参数表示硬件指令，其十进制无特别含义*

#### 2.1.3 搜索

通过快捷键 **Ctrl+F** 打开功能，通过 **Esc** 退出搜索界面

输入关键字后按下回车即可将对应节点或者边导航到屏幕中心,搜索关键字包括以下三种

- 算子类型（支持不完全匹配）
- 算子id（完全匹配）
- ftmp id（完全匹配）

### 2.2量化分析工具

为了更加方便的分析编译器对模型的转换效果，我们使用分析工具对相邻阶段的模型进行相同输入前向，通过对比各算子输出ftmp的余弦相似度、相对欧氏距离和平均绝对误差来评判转换效果

<img src="./assets/icraft-show7.png" alt="image-20251119153457868"  /><img src="./assets/icraft-show8.png" alt="image-20251119153601868"  /><img src="./assets/icraft-show9.png" alt="image-20251119154617008"  />


功能说明：
* 使用XRUN独立前向分析： 网络实际前向结果，包含前层的累积误差
* 逐层前向分析：网络当前层为量化前向，前序层均为浮点前向，不包含前层的累积误差
* Golden Model vs Test Model:任意输入两个阶段的模型，便于分析哪个阶段引入了误差


#### 2.2.1 开始新分析

两阶段的模型需要输入完整的模型(json&raw文件)

当模型是单输入时，输入图片选择n张图片输入即代表进行n次前向 当模型是多输入，输入数量为num时，当输入n*num张图片,需要额外输入一个用于匹配各组输入的txt文本文件 每行代表一组输入，以分号进行分隔

```
0.ftmp;1.ftmp
2.ftmp;3.ftmp
```

- Cuda模式：需要设备的cuda可用，默认是CPU前向
- Fake_qf_n 选项表示对量化前向使用fake_qf功能，取消截位限制，并乘以normratio,主要用于对比分析乘累加溢出的问题
- DecodeDll 表示网络输入预处理所使用的动态链接库

输入完成后点击开始分析即可，视模型大小需要稍等一段时间

分析完成即会在 .icraft\log 目录下生成对应模型名称的文件夹,包含了model_structure.json 、analysis_results.json文件和对应每组输入的ftmp文件夹

> *请勿修改生成的文件名称*

#### 2.2.2 加载已有结果

将分析结果得到的model_structure.json 、analysis_results.json两个文件上传即可看到两个阶段的ftmp对比

![image-20251120104551903](./assets/icraft_show.png)

左上角下拉框可以选择每组输入对应结果或者所有输入结果的平均值；

上方折线图横坐标以算子在网络中的执行序排列,四条线分别表示余弦相似度、平均绝对误差、相对欧氏距离和平均均方误差，可以通过点击对应表头来隐藏对应折线段；

输入源选择具体的一组输入（不是平均值选项）时，中间的折线图会显示每个ftmp的具体值，通过点击上方折线图的点切换ftmp，最大显示1000个采样点 初次切换为均匀采样，步长和起始点选项可以通过上下方向键或者输入对应数字调节，按回车键刷新 基于显示效果考虑限制了最大显示采样点数量，比如起始点为0，步长为1表示取前1000个点，可根据需要调节采样范围

> - icraft编译阶段的量化配置教程请参考：[Part 2_1 compile/3.quantize教程与案例](./Part%202_1%20compile.md)
> - 可通过量化分析工具进行量化调优：相关调优经验总结位于[量化调优经验](./assets/quantization_tricks/quantization_tricks.md)

注：最新版本增加了均方差指标
### 2.3量化自动调优工具
除了icraft内置的量化策略，icraft还提供更加自动化、兼顾位宽和精度的自动调优策略。
#### 2.3.1 使用方法
使用方法：在编译toml中[quantize]阶段配置mix_precision= “auto”,且配置quant_autotune_config之后，则可以开启量化自动调优策略。

支持版本：icraft v3.33.0及后续版本
```shell
[quantize]
saturation = "kld"
per = "tensor"
...
mix_precision= "auto"
quant_autotune_config = "config/testcase/quant_autotune_config.toml"
```
对应的自动调优配置文件示例为：
```shell
#quant_autotune_config.toml
[AutoTune]
priority = "accuracy" # accuracy  tradeoff performance
round_num = 10
float_cal = 0
cos_targetvalue = 0.99
# dis_targetvalue = 0.05
# mae_targetvalue = 0.05
# mse_targetvalue = 0.05
# metrics_targetvalue = 1
# evalscript_path = "./modelzoo_dev/pytorch/PointNet/3_deploy/modelzoo/PointNet/pyrt"
#evalscript_cmd = "./mmrotate/python.exe test1.py"
#evalscript_cmd = "metrics_test_commands.txt"
# evalscript_cmd = "run.bat"
# skip_comfirm = 1
```
#### 2.3.2 参数配置说明
| 参数名字| 数据格式| 类型|说明|
| :------------------ | :------- | :------- |:----------------------------------------------------------- |
| priority            | string   |Optional  |表示选择的调优模式；可配置为 accuracy(默认)、tradeoff、performance。|
| threshold_level            | int   |Optional  |<br>表示阈值筛选级别，可配置为1(默认)/2/3<br/> <br>threshold_level=1：_cos < 0.97 或者 _dis>0.05 或者 _mse > 0.05 或者 _mae > 0.05<br/><br>threshold_level=2：_cos < 0.98 或者 _dis>0.03 或者 _mse > 0.03 或者 _mae > 0.03<br/> <br>threshold_level=3：_cos < 0.99 或者 _dis>0.01 或者 _mse > 0.01 或者 _mae > 0.01<br/>|
| round_num           | int      |  Optional           |表示设置的调优轮数；若未配置，默认设置50轮。|
| float_cal           | int      | Optional|表示BY下是否开启FP32混合，是非必配项；默认配置为0，当配置为0，表示不允许BY下混合FP32,,配置为1则表示允许BY下混合FP32 |
| cos_targetvalue     | float    |Optional |表示期望量化网络输出与浮点网络输出的余弦相似度；有效范围0-1。 |
| dis_targetvalue     | float    |Optional| 表示期望量化网络输出与浮点网络输出的相对欧氏距离；有效范围0-1。 |
| mae_targetvalue     | float    |Optional| 表示期望量化网络输出与浮点网络输出的平均绝对误差；有效范围0-1。 |
| mse_targetvalue     | float    |Optional| 表示期望量化网络输出与浮点网络输出的均方误差；有效范围0-1。 |
| metrics_targetvalue | float    | Optional|表示期望量化网络的精度测试指标，例如yolov5期望map50是80，则配置为80；若配置了此项则必须配置evalscript_path和evalscript_cmd。 |
| evalscript_path     | string   |Optional| 表示精度测试脚本存放路径                                     |
| evalscript_cmd      | string   |Optional| 表示精度测试脚本执行的一系列命令                             |
| skip_comfirm        | int      | Optional|默认配置为0，当配置为0表示需要经过自动调优开始前的参数确认环节,配置为1则表示跳过自动调优开始前的参数确认环节 |
* cos_targetvalue/dis_targetvalue/mae_targetvalue/mse_targetvalue/metrics_targetvalue：
  * 上述参数若配置，只能配置其中一项
  * 上述参数若均未配置，默认按照cos_targetvalue=0.99作为调优判据
* priority 
  * priority = "accuracy"
    * 逐层比较_cos,_dis,_mse,_mae指标，对指标不满足的当前层进行调整（如位宽调优、结构调整）
  * priority = "tradeoff"
    * 筛选标准：基于余弦相似度，按照阈值0.8 0.85 0.9 0.95的顺序查找不满足要求的层ftmp_0,ftmp_1,ftmp_2,ftmp_3,ftmp_4；以ftmp_0为起点，递归查找不满足指标(如threshold_level=1对应_cos < 0.97 或者 _dis>0.03 或者 _mse > 0.03 或者 _mae > 0.03)的前序层作为待调优层
    * 调优策略：从前向后依次调整待调优层的位宽
  * priority = "performance"
    * 筛选标准：基于余弦相似度，按照阈值0.8 0.85 0.9 0.95的顺序查找不满足要求的层ftmp_0,ftmp_1,ftmp_2,ftmp_3,ftmp_4；以ftmp_0为起点，递归查找不满足指标(如threshold_level=1对应_cos < 0.97 或者 _dis>0.03 或者 _mse > 0.03 或者 _mae > 0.03)的前序层作为待调优层
    * 调优策略：从前向后依次调整待调优层的(1)量化策略,如量化粒度(per)和饱和点选取策略(sat)(2)位宽
* evalscript_cmd
  * 格式要求：evalscript_cmd需要配置为一个bat批处理文件（例如run.bat）
  * 功能要求：bat批处理文件需要实现在evalscript_path中从jr_path中读取量化后阶段jsonraw并完成精度测试的若干条执行命令， 且要保证最终将想要作为判据指标的精度指标写入evalscript_path下的metrics.txt文件中，例如yolov5期望map50是80，那么每次完成精度测试后得到的map50的值就需要写入metrics.txt第一行
#### 补充说明
* 位宽调整策略：基于当前层的量化位数，调整为相近的更高精度位宽
  * 当前配置qdtype=int8，则调整为int16
  * 当前配置qdtype=bf16，则调整为fp16
  * 当前配置qdtype=fp16，则调整为tf32
* 输出log:自动调优的具体过程记录在模型编译目录下的.icraft/logs/model/model_quantize_autotuning.log中
```Yolov12n_quantize_autotuning.log
[09/24/25 10:49:25.340] [I] Measures at the beginning: outputOP_avg_cosvalue:0.99383813 outputOP_avg_disvalue:0.09057646 outputOP_avg_maevalue:0.22726929 outputOP_avg_msevalue:0.14659168
[09/24/25 10:52:50.016] [I] Accuracy mode: the number of operators to be tuned is 241
[09/24/25 10:52:50.016] [I] op_id428/ftmp_id1147 autotuning:precision,convert SINT8 to BF16
...
[09/24/25 10:52:50.020] [I] op_id374/ftmp_id986 autotuning:precision,convert SINT8 to BF16
[09/24/25 10:52:50.020] [I] Accuracy mode: the number of adjusted operators is 241
[09/24/25 10:56:37.231] [I] Measures at round0: outputOP_avg_cosvalue:0.9988486 outputOP_avg_disvalue:0.036854748 outputOP_avg_maevalue:0.08830113 outputOP_avg_msevalue:0.02117869
[09/24/25 10:59:43.505] [I] Accuracy mode: the number of operators to be tuned is 1
[09/24/25 10:59:43.505] [I] op_id86/ftmp_id230 autotuning:precision,convert BF16 to FP16
[09/24/25 10:59:43.505] [I] Accuracy mode: the number of adjusted operators is 1
[09/24/25 11:03:27.671] [I] Measures at round1: outputOP_avg_cosvalue:0.9988434 outputOP_avg_disvalue:0.036941335 outputOP_avg_maevalue:0.08917656 outputOP_avg_msevalue:0.021629266
[09/24/25 11:06:34.236] [I] Accuracy mode: the number of operators to be tuned is 0
[09/24/25 11:06:34.236] [I] 在性能优先模式下，没有待调优算子,结束调优

```

## 2.icraft run
应用场景：对编译完的模型快速进行仿真/部署验证/时间分析
### 3.1 使用方法 
- 相关模型完成icraft编译，得到对应阶段的json&raw模型
- 命令行配置参数或toml文件中的[run]字段配置相关参数
### 3.2 参数说明
- 必须配置的参数
  - json:待运行的json文件路径
  - raw:待运行的raw文件路径
  - input:网络输入文件路径，支持图片或ftmp，支持多输入，多输入以“;”分隔
  - jr_path: 如果未配置json、raw参数，也可传入待运行的json和raw文件夹路径
  - load_snapshot:如果未配置json、raw参数，也可传入快照文件路径，由快照文件反序列化得到session
- 可选配置的参数
  - log_path：log存放路径，默认为.icraft/logs；完整路径为log_path/network/*.log
  - backends:指定后端，默认为Host;如需上板请配置为"ZG330,socket://zg330aiu@ip:9981?npu=0x40000000&dma=0x80000000;Host",如"ZG330,socket://zg330aiu@192.168.125.171:9981?npu=0x40000000&dma=0x80000000;Host"
  - dump_format:指定dump特征图使用的格式；支持SFB/SFT/SQB/SQT/HQB/HQT等；默认为空，即不dump
    - 第一个字母表示排布，H表示硬件，S表示软件
    - 第二个字母表示数值，F表示浮点，Q表示定点
    - 第三个字母表示序列化形式，B表示二进制，T表示文本
  - dump_ftmp:指定dump哪些特征图；默认为空，即dump所有特征图
  - log_time:记录每个算子的执行时间信息到屏幕和run_time.log文件中，默认不记录
  - log_io:记录每个算子的输入输出信息到屏幕和run_io.log文件中，默认不记录
  - fake_qf:打开fake_qf模式，仅针对HostBackend有效，默认不打开;如果需要对比adapter和generate仿真结果是否一致，则需要设置fake_qf = true
  - compress：打开内存压缩模式，仅针对BYBackend/ZG330Backend有效，默认不打开
  - merge_hardop：打开合并硬算子模式，仅对BYBackend/ZG330Backend有效，默认不打开
  - save_snapshot：序列化session生成快照文件，保存在log_path目录下，默认为不开启
  - lazy_load:开启惰性加载，仅对输入模型文件为json和raw时有效，默认不开启
  - precheck:开启device预检，包括dma读写检查、寄存器读写检查、位流是否打patch，默认不开启
  - frequency:设置ICore的时钟频率，单位为MHz
  - decode_dll:指定网络输入预处理所使用的动态链接库，默认为空
  - ocmopt:选择ocm优化方案，仅针对ZG330Backend有效，默认为-1，可选择-1，0，1，2，3，分别为选择方案一~三中评分最高的方案、不开启ocm优化、选择方案一、选择方案二、选择方案三
### 3.3 使用示例

#### 3.3.1 三种输入方式
传入的网络文件可通过3种方式配置，分别是：json + raw、jr_path + network、load_snapsho，均支持绝对路径和相对路径
##### 3.3.1.1 输入方式：json + raw
toml文件中的参数配置示例如下：
```shell
[run]
json = "./imodel/yolov5s_hard/yolov5s_BY.json"
raw = "./imodel/yolov5s_hard/yolov5s_BY.raw"
input = "./qtset/coco/000000000139.jpg"
```
命令行输入如下：
```shell
icraft-run --json "./imodel/yolov5s_hard/yolov5s_BY.json" --raw "./imodel/yolov5s_hard/yolov5s_BY.raw" --input "./qtset/coco/000000000139.jpg" 
```
##### 3.3.1.2 输入方式：jr_path + network
toml文件中的参数配置示例如下：
```shell
[run]
jr_path = "./imodel/yolov5s_hard"
network = "./yolov5s_BY"
input = "./qtset/coco/000000000139.jpg"
```
##### 3.3.1.3 输入方式：load_snapshot
使用快照文件作为输入文件，mmu模式、合并算子和内存优化等配置无效，而是与生成快照文件时的配置一致
toml文件中的参数配置示例如下：
```shell
[run]
load_snapshot = "./snapshot/yolov5s_hard.snapshot"
input = "./qtset/coco/000000000139.jpg"
backends = "ZG330,socket://zg330aiu@192.168.125.141:9981?npu=0x40000000&dma=0x80000000;Host"
```
#### 3.3.2 后端指定
目前支持两种后端， 后端HostBackend支持CPU、Cuda等多种Device， 后端ZG330Backend支持Socket、AXI等多种Device， 不指定后端时，默认为HostCPU。
##### 3.3.2.1 仿真
toml文件中的参数配置示例如下：
```shell
[run]
json = "./imodel/yolov5s_hard/yolov5s_BY.json"
raw = "./imodel/yolov5s_hard/yolov5s_BY.raw"
input = "./qtset/coco/000000000139.jpg"
backends = "Host"
```
##### 3.3.2.2 socket

toml文件中的参数配置示例如下：
```shell
[run]
json = "./imodel/yolov5s_hard/yolov5s_BY.json"
raw = "./imodel/yolov5s_hard/yolov5s_BY.raw"
input = "./qtset/coco/000000000139.jpg"
backends = "ZG330,socket://zg330aiu@192.168.125.83:9981?npu=0x40000000&dma=0x80000000;Host"
```
* socket模式对应的url包含板子IP地址、npu起始地址、dma起始地址
* 对应连接的板子需开启icraft-serve
##### 3.3.2.3 AXI

toml文件中的参数配置示例如下：
```shell
[run]
json = "./imodel/yolov5s_hard/yolov5s_BY.json"
raw = "./imodel/yolov5s_hard/yolov5s_BY.raw"
input = "./qtset/coco/000000000139.jpg"
backends = "ZG330,axi://zg330aiu?npu=0x40000000&dma=0x80000000;Host"
```
* axi模式对应的url包含npu起始地址、dma起始地址
##### 3.3.2.4 Mock
toml文件中的参数配置示例如下：
```shell
[run]
json = "./imodel/yolov5s_hard/yolov5s_BY.json"
raw = "./imodel/yolov5s_hard/yolov5s_BY.raw"
input = "./qtset/coco/000000000139.jpg"
backends = "ZG,mock://zg330ai?npu=0x40000000&dma=0x80000000;Host"
save_snapshot = true
```
* 适用场景：目前仅用于快照功能，与参数–save_snapshot配合使用
* mock模式对应的url包含npu起始地址、dma起始地址，使用该模式无需上板
#### 3.3.3 多输入
##### 3.3.3.1 通过；间隔
toml文件中的参数配置示例如下：
```shell
[run]
json = "./imodel/yolov5s_hard/yolov5s_BY.json"
raw = "./imodel/yolov5s_hard/yolov5s_BY.raw"
input = "./qtset/coco/000000000139.jpg;/qtset/coco/elephant.ftmp"
```
##### 3.3.3.2 通过文本文件作为输入
toml文件中的参数配置示例如下：
```shell
[run]
json = "./imodel/yolov5s_hard/yolov5s_BY.json"
raw = "./imodel/yolov5s_hard/yolov5s_BY.raw"
input = "./qtset/img_list.txt"
```
文本文件(img_list.txt)中记录所有输入文件的路径，用换行符隔开。
#### 3.3.4 记录时间
toml文件中的参数配置示例如下：
```shell
[run]
json = "./imodel/yolov5s_hard/yolov5s_BY.json"
raw = "./imodel/yolov5s_hard/yolov5s_BY.raw"
input = "./qtset/img_list.txt"
log_time = true
```
命令行运行：
```shell
icraft compile .\config\yolov5s_hard.toml
# 得到各阶段的编译结果
icraft run config\yolov5s_hard.toml
# 运行icraft run得到仿真/部署结果
```
即可得到时间统计结果：
```shell
################# SUMMARY #################

OpNum 7   OpType Add(H)           total_wall:   25.882 ms  total_mem:    0.000 ms  total_hard:    0.374 ms  total_other:   25.508 ms
OpNum 13  OpType Concat(H)        total_wall:   16.838 ms  total_mem:    0.000 ms  total_hard:    0.000 ms  total_other:   16.838 ms
OpNum 60  OpType Conv2d(H)        total_wall:  226.553 ms  total_mem:    0.000 ms  total_hard:    5.570 ms  total_other:  220.983 ms
OpNum 1   OpType DetPost          total_wall:   47.162 ms  total_mem:    0.000 ms  total_hard:    0.229 ms  total_other:   46.933 ms
OpNum 1   OpType ImageMake        total_wall:    1.457 ms  total_mem:    0.000 ms  total_hard:    2.252 ms  total_other:   -0.795 ms
OpNum 1   OpType Input            total_wall:    0.029 ms  total_mem:    0.000 ms  total_hard:    0.000 ms  total_other:    0.029 ms
OpNum 3   OpType Maxpool(H)       total_wall:   16.519 ms  total_mem:    0.000 ms  total_hard:    0.134 ms  total_other:   16.385 ms
OpNum 1   OpType Output           total_wall:    0.008 ms  total_mem:    0.000 ms  total_hard:    0.000 ms  total_other:    0.008 ms
OpNum 2   OpType Upsample(H)      total_wall:    7.192 ms  total_mem:    0.000 ms  total_hard:    0.249 ms  total_other:    6.943 ms
total_time: 0.010 ms, total_hard_time: 6.326 ms
DetPost: 0.229 ms
ImageMake: 2.252 ms
```
其中total_hard_time为icore的粗略估计结果；准确的时间分析请参考Part 4 性能分析及优化

#### 3.3.5 记录io
toml文件中的参数配置示例如下：
```shell
[run]
json = "./imodel/yolov5s_hard/yolov5s_BY.json"
raw = "./imodel/yolov5s_hard/yolov5s_BY.raw"
input = "./qtset/img_list.txt"
log_io = true
```
配置并运行icraft run后可得到每个算子的所有输入和输出特征图, 不记录HardOp这种算子内部隐藏了的特征图，记录结果保存至log_path路径下
#### 3.3.6 dump特征图
toml文件中的参数配置示例如下：
```shell
[run]
json = "./imodel/yolov5s_hard/yolov5s_BY.json"
raw = "./imodel/yolov5s_hard/yolov5s_BY.raw"
input = "./qtset/img_list.txt"
dump_format = "SFB"
dump_ftmp = "5,12,75"
```
* 配置并运行icraft run后会在指定的log路径下创建ftmp+dump_format的文件夹，将各特征图保存其中
* 无论是二进制还是文本文件，后缀名均为ftmp，用户需要根据文件夹名称判断是否可读
* 若配置了–dump_ftmp，则只会保存指定vid的特征图，用户需自行保证特征图v_id有效，不存在的v_id不会被dump
#### 3.3.7 Device配置
Device配置主要是针对ZGDevice的配置，需绑定ZG330backend后端, 包括设置ICore的时钟频率(frequency)、device预检(precheck)和设置mmu模式(close_mmu)，用户可按需配置<br>

toml文件中的参数配置示例如下：
```shell
[run]
json = "./imodel/yolov5s_hard/yolov5s_BY.json"
raw = "./imodel/yolov5s_hard/yolov5s_BY.raw"
input = "./qtset/img_list.txt"
frequency = 860
precheck = true
close_mmu = false
backends = "ZG330,axi://zg330aiu?npu=0x40000000&dma=0x80000000;Host"
```
#### 3.3.8 Backend配置
Backend配置是针对HostBackend、ZG330Backend的配置，包括fake_qf、内存压缩(compress)、合并算子(merge_hardop)，用户可按需配置。<br>
toml文件中的参数配置示例如下：
```shell
[run]
json = "./imodel/yolov5s_hard/yolov5s_BY.json"
raw = "./imodel/yolov5s_hard/yolov5s_BY.raw"
input = "./qtset/img_list.txt"
fake_qf = true
compress = true
merge_hardop = true
close_mmu = true
backends = "ZG330,axi://zg330aiu?npu=0x40000000&dma=0x80000000;Host"
```
#### 3.3.9 快照
快照功能对当前session进行序列化，并保存快照文件至log_path下。 目前只支持BY阶段的网络。<br>

toml文件中的参数配置示例如下：
```shell
[run]
json = "./imodel/yolov5s_hard/yolov5s_BY.json"
raw = "./imodel/yolov5s_hard/yolov5s_BY.raw"
input = "./qtset/img_list.txt"
backends = "ZG330,mock://zg330ai?npu=0x40000000&dma=0x80000000;Host"
save_snapshot = true
```
### 3.4 结果分析
命令行运行：
```shell
icraft compile .\config\yolov5s_hard.toml
# 得到各阶段的编译结果
icraft run config\yolov5s_hard.toml
# 运行icraft run得到部署结果
```
**结果内容**：
* icraft run会首先打印设备信息，然后执行网络前向，进度条执行完毕会打印每一层的IO信息
* icraft run会在.icraft/logs/yolov5s/ftmpSFB下保存每一个算子的输出特征图。

**对比方法**：
* 通过指定后端为ZG330Backend可进行耗时分析
* 通过指定后端为Host可以使用CPU对编译的各阶段网络进行仿真，可以对比不同阶段的特征图来检验仿真结果是否正确
  * 对于指令生成后的网络， 当在 ZG330Backend 上运行时，会对中间层的特征图进行OCM (片上存储) 优化，而被优化的中间层特征图不会被导出。因此在 ZG330Backend 导出的中间层特征图数量有可能会比 HostBackend 导出的少。
  * 在对比不同阶段的仿真结果时，只需要保证同名文件二进制相同即可
  * 对结果层ftmp接对应后处理可获得最终结果，用于和框架下结果对比

**使用局限**
  * 仅适合粗略时间分析，详细时间分析请参考Part 4.1 性能分析及优化
  * 由于icraft run无法进行复杂的前处理，因此如果框架下前处理较为复杂，则无法做到完全仿真，仅适合粗略的结果仿真