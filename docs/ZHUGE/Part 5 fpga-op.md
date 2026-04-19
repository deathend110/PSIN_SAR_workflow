# Part 5 硬算子
目前诸葛框架下只有`Detpost`硬算子用于检测网络后处理。

## 硬算子定义

自定义硬算子是FPAI芯片区别于其他AI芯片的一大技术特色，硬算子(FPGA_op)是指用逻辑资源实现的功能模块，目的有2点：1. 补充npu未支持的新算子或对已有算子序列进行加速；2.实现AI算法前后处理部分的功能，实现全流程性能优化加速。

## Detpost

**详细信息**： [Detpost技术手册](https://download.fdwxhb.com/data/04_FPAI/02_Icraft/FPGA%e7%a1%ac%e7%ae%97%e5%ad%90CustomOP%e8%ae%be%e8%ae%a1%e8%af%b4%e6%98%8e%ef%bc%88%e5%85%b7%e4%bd%93%e8%af%b7%e5%8f%82%e8%a7%81%e5%af%b9%e7%94%a8%e5%8f%82%e8%80%83%e8%ae%be%e8%ae%a1%ef%bc%89/24090904/%e7%a1%ac%e7%ae%97%e5%ad%90%e6%8a%80%e6%9c%af%e6%89%8b%e5%86%8c/)<br>
**功能介绍**<br>

* Detpost是yolo系列检测网络定制的一个fpga模块，将读取 PLDDR中的网络计算结果，通过硬件实现数据筛选，然后将大于阈值的结果数据写回至PSDDR。
* 经统计经过Detpost筛选可以减少约 90%的数据量。

* 实现的功能具体包括：

  ```
  去通道：代替PruneAxis将通道对齐（去除无效通道）
  排布转换：代替Cast将数据搬移（PL->PS）、排布转换
  阈值筛选：只将大于阈值的结果传出到PL，经统计经过筛选可以减少约90%的数据量
  ```

**使用限制**<br>

* 需要最后算子是普通卷积（分组卷积、深度可分离卷积有风险）
  
* detpost目前是通过修改网络结构，加在最后几层卷积后面的，所以当前版本，想用detpost要保证网络最后输出的算子是普通卷积
  
* 支持最多2048个检测类别

* 需要做阈值筛选的部分必须在每个head的前端。这种限制一般是对于decouple类型。
  * 示例1：yolox要保证代表score的1通道出现在每个head的前面。
  * 示例2：yolov6要保证筛选的80类别出现在前面。
  * 要满足此要求需要在保存浮点模型时就注意。例如，pytorch模型导出时需要先return阈值信息。
  
* 最大支持4K尺寸的ftmp数据

* 由于含有阈值的ftmp一般需要先做sigmoid在做阈值比较，因此detpost的阈值比较，实际上是将未做simoid的阈值和阈值参数的sigmoid反函数值做对比。如果出现需要阈值筛选的ftmp在源码中不需要做sigmoid而是直接阈值筛选，则此处可能会有问题。


**编译配置**

* detpost算子在编译toml中完成配置之后就不需要在代码中额外的初始化和启动，在网络前向中会自动进行结果筛选和搬数
* 若编译模型不带detpost，则会自动添加cast算子完成数据搬移

**注册Detpost**

Detpost硬算子在adapt阶段加入网络，所以在adapt阶段通过
`pass_on`参数配置`customop.DetPostZGPass`
yolov5模型参考如下：

```toml
[adapt]
target = "zhuge"	# 这里指定为zhuge
json = "imodel/yolov5s/yolov5s_quantized.json"
raw = "imodel/yolov5s/yolov5s_quantized.raw"
jr_path = "imodel/yolov5s/"
pass_on =  "customop.DetPostZGPass"	# 硬算子名称
custom_config = "config/customop/yolov5.toml"	# 硬算子相关参数配置文件地址
```
设置Detpost参数

`custom_config`参数用来指定custom_config.toml 的路径，该文件负责对Detpost相关参数进行设置，Detpost目前的参数主要有以下几个
* cmp_en : 0表示不进行阈值比较，1表示进行阈值比较
* thr_f : 硬件筛选阈值， 如0.001
* groups:有几个输出 head
  
* anchor_num : 每个 cell 对应几个 anchor；anchor free 的设置为1
* position :代表需要筛选的阈值所在的位置。
* config_id:代表哪几个output的输入会经过Detpost。
  * 通常情况，本参数不用配置，默认情况下，所有接到 Output 层的输入都会经过Detpost
  
  * 如 config_id= “0,1,2”，表示 第0、1、2个output的输入接到Detpost，Detpost的输出再接到Output层，其他层的连接关系不改变
  

**不同后处理情况下detpost配置示例**

Detpost支持对Yolo类后处理进行阈值筛选，关键参数配置总结如下：

- 有无置信度得分socre：

  - nosocre：通过遍历类别概率判断是否存在物体（yolov6, position = -1）
  - socre：score位于anchor的第i个位置（ yolov5 score位于第5个通道,position = 5）

- 输出层数是否与head数对应：

  - decouple：目标信息分离（yolox有9个输出层对应3个head，groups = 3）
  - couple：目标信息一致（yolov5有3个输出层对应3个head，groups = 3)

- 是否基于anchor先验知识：

  - ancor base：同一个cell对应的box信息可能要用n种先验anchor来计算(anchor_num = n，一般n = 3)
  - anchor free：不需要先验anchor（yolox, anchor_num = 1）

  

| Model      | channel  | type                         | Detpost参数                                 |
| ---------- | -------- | ---------------------------- | ------------------------------------------- |
| yolov3,4,5 | 3*85     | anchor_base,couple,score     | groups = 3 ，anchor_num = 3 ，position = 5  |
| yolox      | (1,4,80) | anchor_free,decouple,score   | groups = 3 ，anchor_num = 1 ，position = 1  |
| yolov6     | (80,4)   | anchor_free,decouple,noscore | groups = 3 ，anchor_num = 1 ，position = -1 |



运行时注意

- detpost输出数据会直接搬移到ps端，不需要手动搬移
- 传输的数据为量化之后的数据，实际使用的时候，需要乘以量化参数（json中的value）做反量化



- 输出数据格式：
  - 输出的数据是icraft::Tensor,是一个智能指针的vector，其个数=group数
  - 在icraft3.x下单个的输出维度为[1,1,obj_num,anchor_len]
  - 对于decouple类型的网络，detpost会将每个group中的bbox信息合并在一起
  - 对于anchor base的网络，假如每个cell对应3个anchor，由于每个anchor里目标数目不固定，所以没有基于anchor将其分开，而是合到了一起 

  - 输出排布示例
    - 以yolov5为例，模型输出层分为3个head，每个head输出255通道，
    - 255通道分别代表3个anchor，每个anchor占据85通道，分别存放(框信息、score、类别概率)，85个通道依次存放下述信息：

      ```python
      x,y,w,h,s,probs,0,location_x,location_y,anchor_index
      ```
  - 如何接着depost写后处理
    - 上述输出排布与Detpost的参数配置相对应，即groups=3,anchor_num=3
    - score = 5代表类别置信度位于第5个位置
    - 如果需要接后处理，需要按照此排布提取正确的信息；如根据类别置信度选择置信度高的作为检测结果
