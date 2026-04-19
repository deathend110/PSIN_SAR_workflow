# Part 2-1 Icraft编译

本教程主要介绍icraft compile过程中**诸葛**框架下的toml文件如何配置，更多详细内容可参考icraft docs。

## 1. 概述

编译各个阶段相关参数配置说明，可展开阅读。

<details close>  <!-- 添加 open 属性可默认展开 -->
<summary>parse参数说明</summary>


<div align="center">  <!-- 可选：居中显示 -->
<img src="./assets/compile_ZG_parse.png" alt="parse参数说明" width="80%">
</div>


</details>

<details close>  <!-- 添加 open 属性可默认展开 -->
<summary>optimize参数说明</summary>


<div align="center">  <!-- 可选：居中显示 -->
<img src="./assets/compile_ZG_optimize.png" alt="optimize参数说明" width="80%">
</div>


</details>

<details close>  <!-- 添加 open 属性可默认展开 -->
<summary>quantize参数说明</summary>


<div align="center">  <!-- 可选：居中显示 -->
<img src="./assets/compile_ZG_quantize.png" alt="quantize参数说明" width="80%">
</div>


</details>

<details close>  <!-- 添加 open 属性可默认展开 -->
<summary>adapt参数说明</summary>


<div align="center">  <!-- 可选：居中显示 -->
<img src="./assets/compile_ZG_adapt.png" alt="adapt参数说明" width="80%">
</div>


</details>

<details close>  <!-- 添加 open 属性可默认展开 -->
<summary>generate参数说明</summary>


<div align="center">  <!-- 可选：居中显示 -->
<img src="./assets/compile_ZG_generate.png" alt="generate参数说明" width="80%">
</div>

</details>

## 2. 样例

诸葛编译各个阶段相关配置样例

```toml
[parse]
net_name = "yolov5s"
framework = "pytorch"
inputs = [ 1, 640, 640, 3]
inputs_layout = "NHWC"
inputs_dtype = "UINT8"	# zhuge新增参数，指定输入数据类型，支持uint8、sint8、sint16、bf16、fp16、fp32、tf32，默认fp32
pre_method = "nop"
pre_scale = [ 255.0, 255.0, 255.0]
pre_mean = [ 0.0, 0.0, 0.0]
channel_swap = [ 2, 1, 0]
network = "./fmodel/yolov5s_640x640.pt"
jr_path = "imodel/yolov5s/"
frame_version = "1.9"
target = "zhuge"	# 这里指定为zhuge

[optimize]
target = "zhuge"	# 这里指定为zhuge
json = "imodel/yolov5s/yolov5s_parsed.json"
raw = "imodel/yolov5s/yolov5s_parsed.raw"
jr_path = "imodel/yolov5s/"

[quantize]
saturation = "kld"
forward_dir = "./qtset/coco"
forward_list = "./qtset/coco.txt"
# bits = 8
qdtype = "int8" # zhuge参数，指定量化数据类型，支持int8/bf16/fp16/tf32
json = "imodel/yolov5s/yolov5s_optimized.json"
raw = "imodel/yolov5s/yolov5s_optimized.raw"
jr_path = "imodel/yolov5s/"
per = "channel"
target = "zhuge"	# 这里指定为zhuge

[adapt]
target = "zhuge"	# 这里指定为zhuge
json = "imodel/yolov5s/yolov5s_quantized.json"
raw = "imodel/yolov5s/yolov5s_quantized.raw"
jr_path = "imodel/yolov5s/"

[generate]
json = "imodel/yolov5s/yolov5s_adapted.json"
raw = "imodel/yolov5s/yolov5s_adapted.raw"
jr_path = "imodel/yolov5s/"
```

## 3. 说明

下面详细说明各个阶段的相关配置参数。

### 3.1 解析阶段[parse]

```toml
[parse]
net_name = "model_name"	# 模型名字，喜欢叫什么就叫什么，但需要保持一致
framework = "onnx"				# 当你导出的模型是.onnx模型时 
# framework = "pytorch"
inputs = [[1,100,560],[1]]		# 输入shape，我们支持的输入只有两种，图片和非图片
inputs_layout = "FD;FD"			# 图片一般都是NHWC排布进行输入，非图片输入我们都将其转成ftmp输入，排布配置成FD
inputs_dtype = "FP32;FP32"		# 输入的数据类型需要指定，支持uint8、sint8、sint16、bf16、fp16、fp32、tf32，默认fp32
pre_method = "nop;nop"			# 前处理配置
pre_scale = "nop;nop"			# 前处理配置
pre_mean = "nop;nop"			# 前处理配置
channel_swap ="nop;nop"			# 前处理配置
network = "./fmodel/model_no_masked_fill_100x560.onnx"	# 框架下导出模型的路径
jr_path = "./imodel/model_name/bf16/"				# 编译生成文件存放的路径
# frame_version = "1.9"			# 当你导出的模型是pt时，需配置framework = "pytorch"，且frame_version = "1.9"	
target = "zhuge"				# buyi or zhuge
```

#### 3.1.1 net_name 

含义：必配，网络模型的名字

说明：喜欢叫什么就叫什么，注意后续都保持一致即可

```toml
net_name = "whatever"
```

#### 3.1.2 framework

含义：必配，产生网络的框架，仅支持 `Pytorch、Onnx`

trace出模型为.onnx时配置为：

```toml
framework = "onnx"
```

trace出模型为.pt时配置为：

```toml
framework = "pytorch"
```

#### 3.1.3 inputs

含义：必配，模型每个输入的尺寸

说明:

- 对于**图像**输入情况，一般框架中的输入排布是NCHW(pytorch)或NHWC(tensorflow)，在导出模型时（torchscript, onnx,pb）保持框架输入维度即可。icraft中此处排布需要写成**NHWC**是因为，在调用硬件做推理时，需要输入的数据为**NHWC**排布，因此要给待转化的模型一个输入维度layout的信息。
- 对于**非图像**输入（在框架下输入非NHWC, NCHW排布的输入），都与框架维度保持一致即可，其中inputs_layout的FD表示framework dims，即与框架保持相同排布的含义。
- 对于**多输入**的情况：每个输入内部以`”,”`区分维度，如：

```toml
inputs = [[1, 384, 384, 3], [144,1,256], [100,1,256], [100,1,256]]
```

#### 3.1.4 inputs_layout

含义：必配，网络输入的排布格式 

说明：

- 对于**图像**输入情况：使用NHWC，inputs维度顺序对应修改（硬件排布需求：通道在最后一维）
- 对于**非图像**输入:使用FD
- 对于**多输入**的情况：需要给每个输入配置layout，以`”;”`分隔，如：

```toml
inputs_layout = "NHWC;FD;FD;FD"
```

> 注意inputs_layout参数与inputs参数是对应的

#### 3.1.5 pre_method

含义：必配，每个输入的前处理方式，如resize代表放缩，nop代表不进行任何前处理 

说明：对于多输入的情况：每个输入配置输入尺寸，以”;”分隔，每个输入内部以”,”区分维度，如：

```toml
pre_method = 'resize;nop;nop;nop'
```

#### 3.1.6 inputs_dtype 

含义：必配，表示输入数据的类型，该参数仅在target=zhuge时生效，支持uint8、sint8、sint16、bf16、fp16、fp32、tf32，默认fp32

```toml
inputs_dtype = "uint8;FP32;FP32;FP32"
```

#### 3.1.7 pre_mean、pre_scale

含义：必配，表示对输入数据进行归一化处理的pre_mean、pre_scale系数

```
Y =（X - pre_mean ）/ pre_scale  # 归一化处理 
```

配置样例：

```toml
pre_scale = [ 58.395, 57.12, 57.375]
pre_mean = [ 123.675, 116.28, 103.53]
```

说明：

- **重点**：premean/scale参数顺序问题，请记住一个原则：**与模型需要的通道顺序保持一致**

  例如：模型输入的通道顺序为RGB，那您就需要先从源码中确认，RGB分别对应的归一化参数，然后按次顺序配到[Rs,Gs,Bs]中

- 案例说明： yolov5中编译参数 pre_scale、pre_mean是根据其原代码中前处理配置：

  `编译参数配置：`

  ```toml
  # yolov5中，数据预处理为读取图像后除以255，故配置如下：
  pre_scale = [ 255.0, 255.0, 255.0]
  pre_mean = [ 0.0, 0.0, 0.0]
  ```

  `对应的python框架下代码：`

  ```toml
  # 前处理：读取图像后除以255
  img = letterbox(img_raw, new_shape=test_size, stride=32, auto=True)[0] 
  img = img.transpose((2, 0, 1))[::-1] 
  img = np.ascontiguousarray(img)
  im = torch.from_numpy(img).float().unsqueeze(0)
  im /= 255 
  ```

#### 3.1.8 channel_swap

含义：必配，表示是否对输入进行通道顺序调整

❓为什么有channel_swap 这一项

> 在实际上板部署推理时，由于数据来源等因素，输入的图像通道排布可能是RGB或BGR。神经网络训练时传到模型中的图像通道RGB或BGR两种情况都有存在。这就导致可能实际部署时输入数据与模型需要的通道排布不一致，这种不一致可以通过在外面进行转换，例如硬件转换或cpu做转换，但是这两种方式可能会有一些效率损失。因此编译阶段提供了一个trick，通过一些等效手段，可以不需要在外部做额外的排布转换操作。其实现原理是通过改变第一个卷积权重的顺序来避免掉输入数据与需要的排布不一致的问题。

💛**重点：在配置channel_swap 之前一定要搞清楚您部署时实际传入数据排布**，**模型需要的数据排布**

> 注意事项：单通道图像，配置channel_swap = “nop” 

配置样例：

多输入时用“；”分隔

```toml
channel_swap ="nop;nop;nop;nop"	
```

模型输入图像通道排布与硬件输入通道排布相同时

```toml
channel_swap =[0, 1, 2]	
```

模型输入图像通道排布与硬件输入通道排布不同时

```toml
channel_swap =[2, 1, 0]
```

#### 3.1.9 network

含义：必配，表示网络模型文件的路径

```toml
network = "./fmodel/model.onnx"
```

#### 3.1.10 jr_path

含义：必配，表示解析组件输出文件的存放路径

```toml
jr_path = "./imodel/"	
```

#### 3.1.11 frame_version

含义：框架版本号，因为只有 `PytorchParser` 支持多个版本，因此该参数仅在`framework=Pytorch`时需要配置，默认值为2.0.1。

说明：组件对于编译的模型有对应的版本要求

- Pytoch，支持pytorch1.9.0、pytorch2.0.1两个版本的原生网络模型文件（.pt格式），以及pytorch框架保存为onnx（opset=17）格式的模型文件（.onnx格式）；
- PaddlePaddle，仅支持PaddlePaddle框架保存为onnx（opset=11）格式的模型文件（.onnx格式），不支持框架原生网络模型文件；
- Darknet，支持Darknet框架原生网络模型；

> 上述版本要求以该版本icraft-doc说明为主
>
> 需要注意：在onnx模型中，需满足 ir_version ≤ 8

#### 3.1.12 target

含义：必配，表示期望的AI后端，支持buyi和zhuge，默认buyi，30TAI部署只能配zhuge。

```toml
target = "zhuge"				
```

#### 3.1.13 pre_check

含义：可选，表示是否启动整个编译器级别的模型预检，仅针对OnnxParser有效，默认为true，即对模型中不支持的算子（如不支持Sin、Cos算子）、和算子不支持的边界条件（如Conv不支持五维输入）进行预检；设置为false时，只完成Parser组件的预检，即仅对模型中不支持的算子进行预检；预检不通过则不进行编译。

#### 3.1.14 image_channel_order

含义：可选，表示期望的输入图片颜色通道顺序，仅支持 `bgr、rgb`，默认bgr。

#### 3.1.15 log_path

含义：可选，表示本组件执行过程输出的log文件的存放路径，省略则放到默认路径”./.icraft/logs/”下

#### 总结

由上述例子可以看出，当你的输入为ftmp时，只需个性化配置模型输入大小与输入数据类型；当你的输入为图片时，除输入大小与输入数据类型必须配置外，还可以选择根据源码配置模型相关前处理；

### 3.2 优化阶段[optimize]

```toml
[optimize]
target = "zhuge"	# 必配
json = "./imodel/model_parsed.json"	# 必配
raw = "./imodel/model_parsed.raw"	# 必配
jr_path = "./imodel/"	# 必配
log_path = "./imodel/optimize"	# 可选
# pass_on = "icraft.InputfoldPass"	# 可选
# pass_off = "NegitaveSampePass"	# 可选
```

#### 3.2.1 target

含义：必配，表示期望的AI后端，支持buyi和zhuge，默认buyi，30TAI部署只能配zhuge。

```toml
target = "zhuge"				
```

#### 3.2.2 json

含义：必配，表示icraft模型json文件的路径，通常与raw（参数文件）配合使用。

#### 3.2.3 raw

含义：必配，表示icraft模型raw文件的路径，通常与json（参数文件）配合使用。

#### 3.2.4 jr_path

含义：必配，表示本组件输出的模型文件的存放路径

#### 3.2.5 log_path

含义：可选，表示本组件执行过程输出的log文件的存放路径，省略则放到默认路径”./.icraft/logs/”下

#### 3.2.6 pass_on

含义： 可选，表示需要打开的pass，该pass需要放在icraft安装目录的icraft/CLI/bin目录下，对于需要打开多个pass的情况，以；间隔

```toml
pass_on = "SamplePass;Sample1Pass;Sample2Pass"
```

#### 3.2.7 pass_off

含义：可选，表示需要关闭的pass，该pass需要放在icraft安装目录的icraft/CLI/bin目录下，对于需要打开多个pass的情况，以；间隔

```toml
pass_off = "NegativeSamplePass;NegativeSample1Pass;NegativeSample2Pass"
```

### 3.3 量化阶段[quantize]

> 诸葛是新一代AI核架构的名称，拥有强大的定点算力和浮点算力，因此诸葛量化支持将网络量化到INT8、BF16、FP16、TF32等数据类型。 相较于上一代布衣架构的量化而言，诸葛量化更灵活，精度更高，支持的量化配置更多。

```toml
[quantize]
saturation = "kld"	# 模型量化时，表示统计饱和点的方法（kld/ems/null）
forward_dir = "./qtset/coco"	# 校准集样本所在的文件夹
forward_list = "./qtset/coco.txt"	# 校准集包含的样本列表
# bits = 8
qdtype = "int8" # zhuge参数，指定量化数据类型，支持int8/bf16/fp16/tf32
json = "imodel/yolov5s/yolov5s_optimized.json"
raw = "imodel/yolov5s/yolov5s_optimized.raw"
jr_path = "imodel/yolov5s/"
per = "channel"	# 按层还是按通道量化（tensor/channel）
target = "zhuge"	# 这里指定为zhuge
# mix_precision = "auto" # 开启混合精度
```

> 注：与buyi不同的是该阶段去除了一些no_tansinput等一些参数配置，以及将bit参数替换为qdtype参数，buyi只支持8bit和16bit的量化，而诸葛丰富了量化数据类型，支持int8/bf16/fp16/tf32这四种。（如果你没配置过buyi可忽略此条注释）

#### 3.3.1 saturation

含义：表示统计饱和点的方法（kld/ems/null），默认为kld ，当qdtype=bf16/fp16/tf32时候为非必配项，其他情况下（包含配置了mix_precision）则为必配项。

#### 3.3.2 forward_dir

含义： 表示校准集图片所在的文件夹，当qdtype=bf16/fp16/tf32时候为非必配项,其他情况下（包含配置了mix_precision）则为必配项。

说明：非RGB图像需要将输入转成二进制文件，类型为float32，后缀名为.ftmp。

例：决策模型中输入1*13的一个特征时，将输入数据转为2进制的ftmp文件

```python
input = torch.tensor([[ 0.7737,  0.0958, -0.0158, -0.2041,  0.7759,
 -0.2657, -0.1717,  0.0658, 0.6063,  0.1865,  0.0096,  1.0697,  0.2896]])
input.numpy().astype(np.float32).tofile('qtset/input.ftmp')
```

强调：量化校准集的选择，需是模型训练集同源数据集，也可以理解成你通过源码导出模型的输入是什么那么你的量化校准即就是什么。举例说明，如果你的模型输入是三通道图片，那么你的量化校准集需是三通道图片样本；如果你的模型输入是单通道图片，那么你的量化校准集需是单通道图片样本；如果你的模型做语音、NLP、决策等非视觉任务，导出模型输入为ftmp，那么你的量化校准集需为二进制的ftmp文件，这种模式下parse中的premethod参数必须配置成nop。

#### 3.3.2 forward_list

含义：  表示校准集包含的图片列表，当qdtype=bf16/fp16/tf32时候为非必配项,其他情况下（包含配置了mix_precision）则为必配项。

对于需要打开多个输入的情况，量化校准集以；间隔：detr中量化校准集对应的dert.txt文件配置如下：

```toml
# 其中每行为一组，每组中的输入之间用“;”分隔
000000000139.jpg;pos384x384.ftmp;tgt.ftmp;query_embed.ftmp
000000000285.jpg;pos384x384.ftmp;tgt.ftmp;query_embed.ftmp
000000000632.jpg;pos384x384.ftmp;tgt.ftmp;query_embed.ftmp
```

#### 3.3.3 qdtype

含义： 必配，表示量化数据类型（int8/bf16/fp16/tf32）。

#### 3.3.4 json

含义：必配，表示输入的中间层json文件路径，通常与raw（参数文件）配合使用。

#### 3.3.5 raw

含义：必配， 表示输入的中间层raw文件路径，通常与json（参数文件）配合使用。

#### 3.3.6 jr_path

含义：必配，表示输出的中间文件存放的路径。

#### 3.3.7 per

含义：表示按层还是按通道量化（tensor/channel）,默认为channel，当qdtype=bf16/fp16/tf32时候为非必配项,其他情况下（包含配置了mix_precision）则为必配项。

#### 3.3.8 target

含义：必配，表示期望的AI后端，支持buyi和zhuge，默认buyi，30TAI部署只能配zhuge。

```toml
target = "zhuge"				
```

#### 3.3.9 decode_dll

含义： 可选，表示调用的解码图片dll的路径，用户自定义读图方式的接入接口，默认为OpenCV三通道读图方式。

#### 3.3.10 mix_precision

含义：可选，配置mix_precision= “auto”：表示自动实现混合精度量化，并按默认混合方案进行混合精度配置； 配置mix_precision= “xxx.csv”：表示根据用户自定义的位宽配置文件实现混合精度量化。

说明：当对网络的运行速度和精度都有较高要求时，即希望同时实现低位宽的速度和高位宽的精度，可以尝试使用混合精度量化。 混合精度量化通过采用特定策略选择网络中量化影响较大的算子，并使用更高位宽进行计算，旨在减少量化损失并提高网络的精度。 通过将关键算子使用高位宽进行计算，可以在一定程度上缓解量化引入的精度损失。 在混合精度量化中，位宽配置可以通过两种方式实现：

1、内置选取策略： 量化组件内置了一些策略，根据各层数据分布的差异，自动选择需要使用更高精度计算的层。

内置的策略包含lstm_strategy、cosimilarity_strategy以及variance_strategy三种，lstm_strategy是指lstm算子默认使用更高位宽， cosimilarity_strategy是指通过分析各层特征图的余弦相似度来确定量化敏感的层使用更高位宽，variance_strategy是指通过分析各层特征图 的方差来确定哪些层使用更高位宽。位宽选择的逻辑是：如果当前配置qdtype=int8，则高位宽指的是fp16； 如果当前配置qdtype=bf16/fp16，则高位宽指的是tf32。

使用方式上均可以通过mix_precision = “auto”开启，当mix_precision=”auto”时布衣的默认策略是variance， 诸葛的默认策略是cosimilarity+lstm。在诸葛量化中也可以通过mix_precision = “variance”来指定混合精度的策略。

**注意：当前variance策略下不支持qdtype=tf32的配置；qdtype=bf16/fp16/tf32时不支持在mix.csv中配置int8类型。**

2、用户自定义配置： 另一种方式是用户根据自己的需求自定义配置网络中各层算子使用的计算位宽，通过配置mix_precision=”xx.csv”实现。

使用自定义配置时建议如下：

> 1.先配auto，然后在生成csv上修改
>
> 2.可以根据量化前后的仿真结果中余弦相似度判断，当余弦相似度低则表示该层量化效果不好，当某些层的数据在通道间存在较大差异时可通过配置下表对应层的per来实现按通道量化。

**注意事项：在精度效果不好时，可利用混合精度进行量化调优**，可参考性能优化章节，也可参照icraft docs里面有详细说明。

#### 3.3.11 一些高阶用法（optional）

##### 3.3.11.1  batch

含义：可选，将校准集分为几份，依次进行前向计算（电脑内存较小时使用），默认为1。

##### 3.3.11.2  bin_num

含义：可选，表示统计饱和点时将数据分成多少个bin，默认4096。

##### 3.3.11.3 ftmp_csv

含义：可选，表示ftmp_csv的路径，从该ftmp_csv中获取ftmp的饱和点时使用。

##### 3.3.11.4  raw_csv

含义：可选，表示raw_csv的路径，从该raw_csv中获取weight的饱和点时使用。

##### 3.3.11.5 smooth_alpha

含义：可选，表示smoothquant方法中的alpha系数，可调整特征图与参数间的放缩比例关系，建议选择（0，1）之间的数。

##### 3.3.11.6 autoqset_num

含义：可选， 表示从提供的数据集中自动挑选的校准集数量，默认为None，即不进行自动校准集选择，仅当输入大于等于1时候，开启自动校准集选择功能。

##### 3.3.11.7 quant_autotune_config

含义：可选，表示自动调优配置项，进行不同模式下的量化自动调优；只有配置了mix_precision= “auto”之后，配置的quant_autotune_config才会生效，详细信息见icraft docs量化组件中的量化自动调优策略。

### 3.4 适配阶段[adapt]

```toml
[adapt]
target = "zhuge"	# 这里指定为zhuge
json = "imodel/yolov5s/yolov5s_quantized.json"
raw = "imodel/yolov5s/yolov5s_quantized.raw"
jr_path = "imodel/yolov5s/"
pass_on =  "customop.DetPostZGPass"	# 硬算子名称
custom_config = "config/customop/yolov5.toml"	# 硬算子相关参数配置文件地址
```

#### 3.4.1 target 

含义：必配，表示期望的AI后端，支持buyi和zhuge，默认buyi，30TAI部署只能配zhuge。

```toml
target = "zhuge"				
```

#### 3.4.2 json

含义：必配，表示icraft模型json文件的路径，通常与raw（参数文件）配合使用。

#### 3.4.3 raw

含义：必配，表示icraft模型raw文件的路径，通常与json（参数文件）配合使用。

#### 3.4.4 jr_path

含义：必配，表示本组件输出的模型文件的存放路径。

#### 3.4.5 pass_on 

含义：可选，表示需要打开的FPGA硬算子，主要作用是支持当前npu中不支持算子、利用硬件进行加速。

目前诸葛框架下只支持`detpost`硬算子，主要用于目标检测网络后处理，详情请参考[Part 5 fpga-op](<./Part 5 fpga-op>)。

#### 3.4.6 custom_config

含义：与`pass_on`参数绑定， 配置文件的路径，在该文件内可以向硬算子传入相应的参数。

`config/customop/yolov5.toml`：

```toml
[DetPost]
forward_dll = ""
forward_so = ""
thr_f = 0.1 # 阈值
cmp_en = 1  # 是否做阈值比较
groups = 3  # 几个head：3个
anchor_num = 3  # 每个cell对应几个anchor:3个
position = 5  # score在第几个通道：第5个
config_id = "0,1,2" # 第0、1、2个输出接到Detpost（默认为所有输出都添加)
```

### 3.5  指令生成阶段[codegen]

```toml
[generate]
json = "imodel/yolov5s/yolov5s_adapted.json"
raw = "imodel/yolov5s/yolov5s_adapted.raw"
jr_path = "imodel/yolov5s/"
```

> 该阶段只需配置如上三项且均为必配项，每个参数含义与上述中同名含义相同，这里不再赘述。

