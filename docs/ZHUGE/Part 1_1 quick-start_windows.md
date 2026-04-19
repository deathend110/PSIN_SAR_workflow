# Part 1-1 quick_start_windows
本章节介绍在windows系统上icraft诸葛架构下（如30TAI）适配AI模型的相关教程。

## 1.icraft简介

关于windows操作系统，icraft目前支持在win7、win10上的模型编译，推荐使用win10。icraft是我们自己命名的一个软件工具套件，在windows系统上安装与卸载步骤同其他普通软件一样，即下载安装包、双击可执行程序、它就会出现在C盘。

### 1.1 安装icraft

下载[安装包](https://download.fdwxhb.com/data/04_FPAI/02_Icraft/v3.33.1/Icraft%e5%ae%89%e8%a3%85%e5%8c%85/)，依次双击Icraft Setup.exe、CustomOp_Setup.exe即可开始安装。

### 1.2 安装成功验证
安装成功后，打开cmd 运行：icraft --version   

若已正常安装，则会显示当前icraft版本，如：

```powershell
Icraft 版本:
 * 3.33.1

CLI 版本:
3.33.1.0-bccd576(2512171611)
```
命令行运行： icraft docs 则会显示Icraft详细使用文档，帮助您了解和快速上手使用Icraft。
```powershell
icraft docs
```
其他icraft提供的常用工具使用方法可参考章节[Part 2_2 icraft tools](<./Part 2_2 icraft tools.md>)，如：

```powershell
icraft show
```

```powershell
icraft run
```

### 1.3 卸载

双击uninst.exe即可开始卸载该版本Icraft。

## 2. 环境准备
本节介绍在windows系统下编译和运行模型库运行时工程所需的必要环境准备。

### 2.1 环境前提

windows系统下需包含如下工具： cmake3.28、visual studio2022、mobaXterm、icraft 3.x ；

如未安装上述工具，您可参考[Part0 env_setup](<./Part 0 env_setup.md>)完成基础环境准备。

### 2.2 版本依赖

部署模型时的依赖为`modelzoo_utils`和`thirdparty`。

- **modelzoo依赖**：从gitee上的[modelzoo_utils](https://gitee.com/mxh-spiger/modelzoo_utils)下载对应版本的相关依赖，存放至`modelzoo_utils`目录。
- **thirdparty依赖**：[下载](https://download.fdwxhb.com/data/04_FPAI/02_Icraft/v3.33.1/Icraft%e5%ae%89%e8%a3%85%e5%8c%85/)并安装。

### 2.3 python API

如果你的工程基于python，需安装Icraft在Windows10下提供的基于python3.8扩展包，名为 [icraft-3.33.1-cp38-none-win_amd64.whl](https://download.fdwxhb.com/data/04_FPAI/02_Icraft/v3.33.1/Icraft%e5%ae%89%e8%a3%85%e5%8c%85/) 。

与其它python扩展包相同，icraft扩展包的安装与卸载命令如下：

```powershell
pip install icraft-3.33.1-cp38-none-win_amd64.whl
pip uninstall icraft
```

## 3. 模型编译与部署

将主流框架下模型使用Icraft AI编译器将静态模型转化为硬件可部署模型 。本教程以yolov5模型为例进行说明，请先下载我们的[demo工程](https://www.modelscope.cn/models/AIBS/yolov5_demo_for_icraft_zg)，你应有的文件结构如下：

```
cd yolov5_demo_for_icraft_zg/
|---compile
|     |——config/yolov5s_ZG.toml	    >编译配置
|     |--fmodel/yolov5s_640x640.pt	>浮点模型，trace源码后得到的.onnx模型或者是.pt模型（上述demo已提供）
|	  |--imodel/					>运行icraft compile ./config/yolov5s_ZG.toml后在此生成.json和.raw文件
|     |--qtset
|	  |    |__coco					>量化校准集，可以是图片也可以是ftmp,一般出自模型同源数据集
|	  |	   |__coco.txt
|
|---deploy
|	  |--build_win				 > windows系统socket模式下创建的文件
|	  |--build_arm				 > Linux系统axi模式下创建的文件
|     |--cfg/yolov5s_psin.yaml	 >一些参数和路径配置，你可以都选择默认，也可根据需求自己修改
|     |--io/input				 >在input文件夹中存放测试样本，运行代码后会生成output文件夹保存结果
|     |--names					 >目标名字列表（可选，目标检测一般会有）
|     |--pyrt					 >python运行时相关代码
|	  |--src 					 >c++运行时相关代码
|	  |--CMakeLists.txt			 >c++模型编译时必要文件
|     |--CMakePresets.json		 >c++模型编译时必要文件
|     |——requirement.txt		 >python运行时需要安装的一些环境库，pip install -r requirement.txt
|---modelzoo_utils				 >icraft相关依赖，参照2.2章节按自己需要版本下载（上述demo工程已提供）
```

那么需要哪些步骤实现在30TAI芯片上执行模型前向推理呢？

### 3.1 icraft一键编译

在上述compile文件夹下打开终端，输入命令：

```powershell
icraft compile ./config/yolov5s_ZG.toml
```

你将得到icraft各个阶段生成的json和raw文件，我们通常把它们放在命名为imodel文件夹中。

另，如果想了解这个toml文件具体怎么配置，每个参数的意义请参照[Part 2_1 compile](<./Part 2_1 compile.md>)章节。

### 3.2 runtime工程实现推理

Icarft提供了Python和C++接口来帮助用户将编译生成的网络进行仿真和上板。仿真和上板的区别在于：

* 仿真工程可以加载icraft编译出来的任意阶段网络json/raw，最终输出结果可以和上板结果比对用于验证。
* 上板部署工程仅适用于加载icraft编译出来的指令生成阶段网络（_ZG.json/raw），获得板级部署结果。

如果对提供的相应代码接口感兴趣且更加进一步的了解与开发请参照[Part 3_1 runtime](<./Part 3_1 runtime.md>)章节。

#### 3.2.1 python runtime

在提供的demo中，通过配置cfg/yolov5s_psin.yaml中的runbackend字段来实现仿真或者是上板，仿真时设置如下：

```yaml
imodel:
  ...
  runbackend: host
  ...

```

如果要实现python在AI硬件上**socket模式**下的模型前向推理，修改runbackend字段为`zg330`，并把ip字段改成板子ip，设置如下：

```yaml
imodel:
  ...
  runbackend: zg330
  ip：192.168.xxx.xxx
  ...

```

**注**：socket模式下，需根据[Part0 env_setup](<./Part 0 env_setup.md>)中的第4章节准备好片上环境，即，已开启了icraft serve。

我们通常会把基于python运行时的相关代码放入命名为pyrt文件夹中，那么准备好上述相应环境后只需根据上述文件结构转到pyrt文件夹中，执行：

```powershell
# cd pyrt
python ./yolov5s_psin.py ../cfg/yolov5s_psin.yaml
```

即可在io/output中查看结果。

#### 3.2.2 c++ runtime

同样的c++ runtime 也是通过配置cfg/yolov5s_psin.yaml中的runbackend字段来实现仿真或者socket模式下模型推理，我们通常会把基于c++运行时的相关代码放入命名为src文件夹中，确保CMakeLists.txt中的路径和命名正确，那么准备好上述相应环境后只需转到build_win文件夹中，执行：

```powershell
# cd build_win
cmake ..
cmake --build . --config Release
./Release/yolov5s_psin.exe ../cfg/yolov5s_psin.yaml
```

即可在io/output中查看结果。

## 4. 结束语

如果对上述一些专有词汇感到困惑，可参考[Part 1_2 概念附录](<./Part 1_2 Icraft Terminology.md>)，里面包含了所有的名词解释。

如果你只想快速开始，那么参照part 0安装好环境，下载好demo，便可以快速在我们板卡上部署一个目标检测的模型；

如果你想快速部署自己模型，先根据[part 2_1 compile](<./Part 2_1 compile.md>)配置toml生成json和raw文件，再参考我们模型库中的pyrt或crt中相关内容快速调用接口，即可完成简单的模型部署；

如果你想进一步优化，可参考[Part 3_1 runtime](<./Part 3_1 runtime.md>)了解运行时相关接口进一步优化，还可借助[icraft show工具](<./Part 2_2 icraft tools.md>)进行量化调优与分析，还有一些成熟的优化方案（性能优化章节）以及一些问题经验（FAQ模块）可供参考；

如果你想从开发者的角度了解我们每个组件的具体功能以及全面的使用方法细节，最佳参考是在终端中输入命令icraft docs；如果该命令没有使网页跳转到我们icraft文档内容，那么你可以在C:\Icraft\CLI v3.33.0\docs这个路径下找到index.html，打开即可。

更多模型与示例可关注我们相关模型库：

[modelzoo-icraft v3.31及之前版本](https://gitee.com/link?target=https%3A%2F%2Fgitlink.org.cn%2Fspider-mxh%2Fmdz)

[modelzoo-icraft v3.33及之后版本](https://gitee.com/link?target=https%3A%2F%2Fwww.modelscope.cn%2Fcollections%2Ficraft_modelzoo-18b52923d4854f)
