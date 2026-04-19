# 量化调优方法总结
![1719562056507](./assets/quantized0.png)
## 1. Transformer结构

对于部分包含transformer的模型（Detr、VIT等），该层的数据在通道间可能存在较大差异，导致精度损失严重。对于上述模型，可通过配置相关文件，调整transformer内部算子的量化策略减少精度损失。



![1719562056507](./assets/quantized2.png)



### 1.1 调优建议：

- Multi-Head Attention结构：
  - 拆分Multihead：编译阶段配置相关pass（icraft.SplitMultiHeadPassDETR、icraft.SplitMultiHeadPassViT）
  - matmul算子：group=head_num 来实现多头注意力按组量化
  - softmax算子：量化饱和点采用null策略
- Add算子：输入输出特征使用per_channel量化
- Layernorm算子：该层与上一层的输入输出特征使用per_channel量化



### 1.2 相关案例：

VIT模型：由于Layernorm算子导致精度损失严重

- 现象分析：icraft-show的量化分析工具仿真，发现该模型在`Layernorm`算子处有明显的下降（vit模型），余弦相似度为-0.131

![1719560937248](./assets/quantized1.png)

- 问题定位：多次下降都为Layernorm处，推断模型在每个transformer层的数据在通道间存在较大差异

- 优化方法：修改Layernorm算子的输出特征量化粒度改为channel

  - 配置参数`mix_precision = "auto"`，生成相应的csv配置文件`vit_mix_configcsv`

  ![quantized3](./assets/quantized3.png)

  参数含义：
  
  > 算子id、算子类型、数据类型、方差、计算量占比、量化位宽（权重、输出特征）、饱和点（权重、输出特征）、量化方式（权重、输出特征）
  
  
  
  - 修改`vit_mix_configcsv`文件中Layernorm算子对应的量化方式，权重、输出特征都改为channel
  
  ![1719562571312](./assets/quantized5.png)
  
  （经过验证，需要将Layernorm算子以及上面两个算子都调整为channel量化，此时效果最好）
  
  
  
  - 重新icraft-show的量化分析工具仿真，余弦相似度：0.951
  
  ![1719563915652](./assets/quantized6.png)
  
  
  
- 精度测试：top1: 0.7924, top5: 0.948 (parse精度：parse: top1: 0.8124, top5: 0.9624)



## 2. 峰值类模型

当模型输出数据中只有少数极大值为目标数据时，输出特征数值差异较大，导致精度损失严重。对于该类模型，可通过配置相关文件，调整内部算子输出特征的量化策略减少精度损失。

该类模型输出损失如下图：

![1719796037973](./assets/quantized7.png)



### 2.1 调优建议：

对输出的特征图也采取按通道量化

> 编译时pre = channel ，表示权重按照channel方式量化
>
> 通过修改混合精度的csv文件，可通过配置per参数输出特征按通道量化

![1719817378643](./assets/quantized11.png)

### 2.2 相关案例

yolov7_pose：输出输出数据中只有少数极大值为目标数据，量化阶段采用pre =channel配置仍有较大损失



- 现象分析: icraft-show的量化分析工具仿真，发现该模型整体并无过多损失；但后处理后图片结果显示偏移

![1719817378643](./assets/quantized8.png)



- 问题定位： icraft-show的量化分析工具发现，只在较大数值上会有明显差异，进行量化调优尝试

![1719796037973](./assets/quantized7.png)



- 优化方法：修改模型中所有的输出特征量化粒度改为channel

  结果如下：偏移基本被修复，量化前后图片结果显示差别不大

![1719817629114](./assets/quantized9.png)



- 精度测试：

![1719817755434](./assets/quantized10.png)

> 第一行为浮点精度、第二行为优化前量化精度、第三行为量化调优后精度

## 3. 分类模型

当模型输出层数据的数值大小对精度至关重要时，可通过配置饱和点选取方式（saturation），调整内部算子的量化策略减少精度损失。

该类模型输出损失如下图：

![1719796037973](./assets/quantized12.png)

### 3.1 调优建议：

对输出的特征图采取Min-Max的饱和点选取方式

> 编译时saturation=null ，表示按照Min-Max方式量化
>

### 3.2 相关案例

SeResNet：是一个分类网络，最后几层对分类结果至关重要，若选择KLD的饱和点选取策略，会导致较大的量化损失

- 问题定位： icraft-show的量化分析工具发现，只在靠近输出的结果层存在较大的量化误差，进行量化调优尝试

![1719796037973](./assets/quantized13.png)



- 优化方法：修改模型中quantize阶段的饱和点选取方式saturation = null

  结果如下：偏移基本被修复，量化前后差别不大

![1719817629114](./assets/quantized14.png)