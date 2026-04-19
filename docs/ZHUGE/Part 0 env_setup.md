# 前言

零基础 icraft 入门教程：手把手教你完成 AI 模型板卡部署。

第一步，相关环境搭建，模型库默认开发系统是**win10+wsl**，其中win10用来编译模型，以及使用socket模式（上位机通过网口控制硬件平台）进行调试和精度测试；wsl（Windows Subsystem for Linux）为win10的子系统，主要用于交叉编译（生成axi模式下的可执行文件），远程调试等。

本章节主要包含以下内容：

- windows系统环境搭建，主要是推荐便于开发的工具软件，包括下载安装与简单的使用方法，其中，重点介绍了串口调试、获取板卡ip的相关方法。
- 交叉编译环境搭建，如果需要axi模式执行上板推理必须构建交叉编译环境，我们提供了相关docker镜像，按照下述教程即可成功构建。
- 程序执行环境构建，主要是python和c++相关依赖环境构建。
- 片上系统环境准备，主要是关于在片上系统（soc-Linux）的一些操作，包括sd卡拷贝镜像操作，icraft在片上系统的相关命令，位流如何更换等。

> 注：关于交叉编译环境搭建，本教程主推win10系统下导入封装好的docker，该方法最为便捷。当然也可在Linux系统下导入下述封装好的docker进行交叉编译环境构建，还可自行在ubuntu20.04系统下直接安装libdw、c++编译器、cmake、ninja、交叉编译器、icraft等直接构建相关编译环境，本教程不具体介绍。

# 1.windows系统环境搭建

本节介绍windows系统的环境搭建。

## 1.1 软件下载安装
|软件|安装|描述|
|---|---|---|
|Visual Studio Code|[官网](https://code.visualstudio.com/)下载exe，双击exe，根据自己需求安装，如果看不懂安装过程的提示框，就都直接默认选项安装也能正常使用|日常开发python运行时及debug|
|visual studio 2022|[官网](https://visualstudio.microsoft.com/zh-hans/vs/)下载exe，双击exe，根据自己需求安装，如果看不懂安装过程的提示框，就都直接默认选项安装也能正常使用|日常开发C++运行时及debug|
|mobaXterm|[官网](https://mobaxterm.mobatek.net/)下载exe，双击exe，根据自己需求安装，如果看不懂安装过程的提示框，就都直接默认选项安装也能正常使用|串口、远程调试工具。<br/>通过**Serial**进行串口调试（speed:115200）<br/>通过**ssh**进行远程调试（Remote host:板卡IP，Specify username:root）|
|Git|参照[链接](https://blog.csdn.net/weixin_42242910/article/details/136297201)中的教程安装与使用|源码、模型库工程下载。<br/>常用命令：git clone|
|netron|参照[链接](https://blog.csdn.net/nan355655600/article/details/106245563)中的教程安装与使用|模型可视化工具，双击.pt或者.onnx，即可查看模型参数与算子等|
|cmake|参照[链接](https://blog.csdn.net/weixin_52677672/article/details/135815928)中的教程安装与使用|运行时工程编译所需。<br>常用命令：<br />cmake..<br />cmake --build . --config Release|
|Anaconda|参照[链接](https://zhuanlan.zhihu.com/p/409477095)中的教程安装与使用|conda是包管理系统和环境管理系统。<br>常用命令：<br />conda create -n name python=3.8<br />conda activate name<br />conda env list|
|icraft|在下载中心下载exe，依次双击Icraft Setup.exe、CustomOp_Setup.exe即可安装|编译器icraft<br>常用命令：<br />icraft --version #查看版本号<br />icraft docs # 说明文档<br />icraft compile #模型编译<br />icraft show #可视化工具|

## 1.2 串口调试

板卡的调试可以通过串口连接也可以通过ssh连接。如果是首次连接板卡必须先通过串口调试获取ip，后续才能通过ip进行远程上板调试。下面介绍如何通过串口调试。

#### 1.2.1 先安装串口驱动

不同系统对应不同版本的串口驱动，请根据自己电脑的windows版本下载对应的安装包：

windows7：在下载中心下载**串口驱动win7**安装包，将下载的zip解压后，双击电脑对应架构的exe即可安装。

windows10：在下载中心下载**串口驱动win10**安装包，驱动安装之前，请确保设备从电脑拔除，解压zip压缩包，右键 ``silabser.inf``文件，选择安装，安装完成后需要重启电脑。

<img src="assets/silabser.png" >



#### 1.2.2 通过MobaXterm进入系统

将串口线的usb端插入电脑，另一端插入板卡的 `psin`接口。<br>

打开MobaXterm，点击**Session** -> 点击**Serial** -> **Serial port**选择端口（一般标识为Silicon Labs CP210x USB to UART Bridge，最保险的做法是选择插上后新增的那个串口）-> **speed**调整为**115200** -> 进入系统，输入用户名：root，密码：fmsh

<img src="./assets/mobaxterm_serial.png" alt="mobaxterm" style="zoom: 67%;" />

#### 1.2.3  板卡的ip获取

获取板卡ip，确保是root账户，输入命令：

```bash
sudo -i
```

进入/etc/rc.local，执行命令：

```bash
vim /etc/rc.local
```

可以动态获取IP和手动修改IP两种方式，下面会分别介绍。

使修改后的/etc/rc.local生效，执行命令：

```bash
source /etc/rc.local
```

1） 由路由器分配 `ip`

将/etc/rc.local文件内容改成如下：

```bash
#!/bin/bash
ifconfig eth0 down
macchanger -m 20:5f:52:aa:cc:44 eth0 #这里必须修改mac地址，避免mac冲突。mac号可通过随机生成获取，也可以手动修改。
ifconfig eth0 up

systemctl start systemd-resolved

dhclient

systemctl start sshd
```

输入`：wq` 保存退出，输入`ifconfig`即可看到分配到的ip

2）设置静态 `ip`（容易 `ip`冲突，一般用于 `pc`直连板卡）

将/etc/rc.local文件内容改成如下：

```bash
#!/bin/bash
ifconfig eth0 down
macchanger -m 20:5f:52:aa:cc:44 eth0 #这里必须修改mac地址，避免mac冲突。mac号可通过随机生成获取，也可以手动修改。
ifconfig eth0 up
ifconfig eth0 192.168.125.171 netmask 255.255.255.0
systemctl start sshd
```

如图所示：

![](./assets/rc_local_ip.png )

3）如果使用 `pc`直接连板卡（不是经过路由器连板卡），则需要：**将 pc的 ip段调整到与 sd卡一致**<img src = "assets/ipv4.png" style="zoom: 50%;" >

- 在以太网属性-配置-高级-链接速度和双工模式选择 `100mbps`全双工

<img src = "./assets/internet.png" style="zoom: 80%;" align="left">

<img src="./assets/100mbps.png" alt="100mbps" style="zoom:80%;">

<br/>

# 2.交叉编译环境搭建

推荐开发系统：win10+wsl

问：为什么要搭建交叉编译环境呢？

答：因为板上系统为Ubuntu20.04-aarch64，如果想上板跑plin工程，就必须构建交叉编译环境编译相关程序以生成在板上可直接运行的可执行程序。

问：为什么不能直接在板卡上编译相关程序呢？

答：因为板上资源有限，编译会很慢，还有可能由于内存问题编译不成功，所以提前准备好交叉编译环境，生成可执行文件后直接放入板卡上执行。

我们已将相关环境配置好封装为docker，去下载中心下载**docker镜像**，可正确且快速的完成交叉编译环境构建。

## 2.1 docker安装与导入

1、系统要求

| 项目     | 最低配置               | 推荐配置              |
| -------- | ---------------------- | --------------------- |
| 操作系统 | Windows 7              | Windows 10/11 24H2    |
| 内存     | 4GB                    | 16GB+                 |
| 虚拟化   | 需开启BIOS的VT-x/AMD-V | **启用Hyper-V与WSL2** |

2、下载Docker Desktop，选项均选择默认即可。

3、**导出镜像与创建容器**

1. 在安装好`docker Desktop`的环境中打开`powershell/cmd`

2. 下载好docker镜像后，进入存放`docker`镜像文件 `Icraft_3.0.0.tar` 的路径下 

3. 依次运行（此处以`Icraft3.0.0`版本提供的`docker`名称为例，若使用其他版本的`docker`，请查看注释中的变量含义并进行相应替换）：

```cmd
# 载入镜像到本地:
# modelzoo_v3.0.0_image		:	可以起任意名称,作为该镜像在本地的名字
docker import Icraft_3.0.0.tar modelzoo_v3.0.0_image

# 将镜像映射为容器，开启容器，进入容器中的bash:
# modelzoo_v3.0.0_container	:	可以起任意名称，作为该容器在本地的名字
# E:\docker_share			:	是本地任意一个存在的路径，作为宿主机与容器文件交互的传送门
# /mnt/SHARE				:	是容器内的路径，作为容器文件与宿主机文件交互的传送门
# modelzoo_v3.0.0_image		: 	使用哪个镜像来映射为容器，使用上一条命令中给镜像起的名字即可
docker run --name=modelzoo_v3.0.0_container -it -v E:\docker_share:/mnt/SHARE modelzoo_v3.0.0_image /bin/bash	

# 如果执行docker run时产生以下报错——
# docker: Error response from daemon: invalid mode: /mnt/SHARE.
# 请先cd到你想要作为宿主机与容器文件交互的传送门的文件夹，然后把文件夹的绝对路径替换成$PWD，如下所示
# docker run --name=try_v3.0.0_container -it -v $PWD:/mnt/SHARE modelzoo_v3.0.0_image /bin/bash
```

**Tips:**  如果您正在使用的是`Icraft3.0`的`docker`，请注意——开启容器进入bash后，需要执行`source /etc/profile`命令，将一些编译所需的文件加入环境变量，不然交叉编译的`cmakelist`中无法找到所需的环境变量。

## 2.2 docker安装常见问题排查(Optional)

Q1：双击安装包时提示"Installation failed. One preprequisite is not fulfilled. Docker Desktop requires the Server service to be enabled."

原因：虚拟化状态为启动

解决方法：

- 控制面板->程序->启用或关闭Windows功能->Hyper-V开启

![Hyper-V](./assets/Hyper-V.png)

- 右键点击任务栏 -> 选择「任务管理器」-> 展开「详细信息」 -> 切换至「性能」标签页 -> 确认虚拟化状态为「已启用」

Q2：报错如图：

<img src="./assets/docker_error.png" alt="docker_error" style="zoom: 80%;" />

原因：未安装wsl

解决办法：安装wsl，参考教程：https://blog.csdn.net/Cike___/article/details/146415836

Q3：安装时提示"WSL2 installation is incomplete"

- 运行命令：`wsl --update`
- 下载[WSL2内核更新包](https://link.zhihu.com/?target=https%3A//aka.ms/wsl2kernel)

Q4：容器无法访问外部网络

- 检查防火墙设置：放行Docker.exe 进程
- 重置网络配置：`docker network prune`

Q5：docker 桌面版报错error during connect: This error may indicate that the docker daemon is not running.:

解决参考链接：

https://blog.csdn.net/tangcv/article/details/112238084#:~:text=https://desktop.docker.com/win/stable/Docker%20Desktop%20Installer.exe

Q6：docker常用命令：

```bash
docker images -a      			 # 查看本地所有镜像
docker ps -a          			 # 查看本地所有容器
docker start 容器ID    			# 启动一个已停止的容器
docker stop 容器ID     			# 停止一个运行中的容器
docker restart 容器ID 			# 重启容器
docker rm 容器ID       			# 删除一个容器
docker rmi 镜像ID 				# 删除一个镜像
docker exec -it 容器ID /bin/bsh   # 进入一个运行中的容器（推荐）
docker attach 容器ID 				# 进入一个运行中的容器
```

# 3.程序执行环境构建

在不同编译环境下会编译出适用于不同平台下的运行程序，在win32、x86-linux系统下可编译出socket工程；在soc-linux、交叉编译环境（可以是win32构建的交叉编译环境（上述第二章）、也可以是x86-linux构建的交叉编译环境）中可编译出axi工程。

您可以按需求灵活选择编译环境：）

## 3.1 依赖下载

如果运行模型库中的工程，任何系统下，都必须先下载相关依赖，一共两个文件，一个叫modelzoo_utils，另一个叫thirdparty。下载后放到工程文件夹（如**3_deploy/modelzoo_utils**）中以供调用。

* **modelzoo_utils**：在[gitee](https://gitee.com/mxh-spiger/modelzoo_utils)上拉取你所需版本，每个分支对应icraft相应版本，注意切换到自己所需版本的分支在进行下载。

  使用git工具下载更方便，打开终端输入命令：

  ```powershell
  git clone https://gitee.com/mxh-spiger/modelzoo_utils.git
  cd modelzoo_utils
  git checkout mzu_3.31.0 #切到你需要的分支
  git pull #拉最新
  ```

  **这里注意依赖文件名字与路径需要与代码中的对应。**

  举个例子：若c++工程的CMakeLists.txt文件中相关内容为

  ```cmake
  include_directories(../modelzoo_utils)
  ```

  那么，该依赖文件夹名字为modelzoo_utils，且该文件需在CMakeLists.txt的上一层文件夹中。

* **thirdparty**： 在下载中心下载对应版本后安装即可。

## 3.2 python环境构建

**深度学习框架说明**：针对icraft开发现状，目前支持三个深度学习框架的模型导出，分别为pytorch、paddle、darknet。对其版本限定如下：

- pytorch：支持pytorch1.9.0、pytorch2.0.1两个版本的原生网络模型文件（.pt格式），以及pytorch框架保存为onnx（opset=17）格式的模型文件（.onnx格式）；
- paddle：仅支持PaddlePaddle框架保存为onnx（opset=11）格式的模型文件（.onnx格式），不支持框架原生网络模型文件；

推荐优先选择pytorch！pytorch的安装不是简单的pip install，安装命令需要参考官网[PyTorch](https://pytorch.org/)，如果参照官网时遇到困难可以参考相应博客，如：
* 在线安装：[在Windows下安装配置CPU版的PyTorch](https://blog.csdn.net/qq_41048413/article/details/115335211)；
* 离线安装：[pytorch、torchvision历史版本的whl文件下载地址、版本对应关系与离线安装方法](https://blog.csdn.net/weixin_39833897/article/details/123072353)

根据源码中的requirement.txt文件安装模型所需要的库，如果没有意外，这时你已经可以在框架下跑通模型推理了。那么想在板卡上跑通相应工程，还需安装icraft扩展包，命令如下：

```powershell
pip install icraft-3.33.1-cp38-none-win_amd64.whl # windows系统对应的icraft库安装
pip install icraft-3.33.1-cp38-none-manylinux2014_aarch64.whl	# Ubuntu20.04-aarch64系统对应的icraft库安装
pip install icraft-3.33.1-cp38-none-manylinux2014_x86_64.whl	# x86-linux系统对应的icraft库安装
pip uninstall icraft # 卸载
```
## 3.3 Python环境管理工具uv的安装和使用(Optional)
如果您使用modelscope的相关模型库，涉及到环境管理工具uv的安装和使用，请参考此章节。
#### 安装uv

虽然可以用`pip install uv`命令安装，但是更加推荐使用官方脚本安装，因为安装uv并不应该受python环境影响

**Windows**

```powershell
powershell -ExecutionPolicy ByPass -c "irm https://astral.sh/uv/install.ps1 | iex"
```

**Linux**

```bash
curl -LsSf https://astral.sh/uv/install.sh | sh
```

#### 更换镜像源

使用前，推荐参考 [uv换清华源](https://mirrors.tuna.tsinghua.edu.cn/help/pypi/) ，使用国内镜像加快下载速度，也即创建 `C:\\Users\\yourname\\AppData\\Local\\uv\\uv.toml` 文件（windows），并添加以下内容

```toml
[[index]]
url = "<https://mirrors.tuna.tsinghua.edu.cn/pypi/web/simple/>"
default = true
```

#### 常用命令

执行`uv run`命令时，uv会自动先从父文件夹中寻找uv项目环境，如果找到了`pyproject.toml`文件，则会自动加载项目环境并在其中运行，如果没有发现pyproject.toml文件，则会使用系统环境变量中位置最靠前的python解释器来执行

```shell
uv run xxx_script_name.py 
```

添加某个依赖包

```shell
uv add xxx_package_name
```

移除某个已经安装的包及其依赖包

```shell
uv remove xxx_package_name
```

查看当前uv使用的环境位置

```shell
uv python find
```

查看当前项目的库安装情况及其依赖关系

```shell
uv tree
```

加载项目环境，执行后uv会在项目的根目录生成.venv文件夹，里面为项目环境文件（以硬链接方式从全局缓存中引入，不占真实存储），一般只有不想运行脚本，但是希望先生成环境，以便在编辑器里选择对应环境进行调试时，才需要使用该命令

```shell
uv sync
```

# 4.片上系统环境准备

如果你已拥有了我们的相关板卡，本章节介绍需要准备的片上系统环境以及位流更换。

## 4.1 sd卡镜像拷贝

在下载中心下载sd卡镜像，将镜像下载解压后使用sd卡镜像拷贝工具拷贝到sd卡（或其他存储）上,拷贝镜像步骤如下：

![imageUSB使用说明](./assets/imageUSB使用说明.png)

## 4.2 icraft版本更换

先卸载原镜像中的旧的icraft 和CustomOp；

```bash
dpkg -r Icraft
dpkg -r CustomOp
```

把安装包从本地拷到板卡上，切换到安装包所在目录，使用以下命令为安装包设置权限；

```bash
chmod 777 Icraft_x.x.x_onchip.deb
chomd 777 CustomOp_x.x.x_onchip.deb
```

使用以下命令即可开始安装

```bash
dpkg -i Icraft_x.x.x_onchip.deb
dpkg -i CustomOp_x.x.x_onchip.deb
```

安装完成后，在命令行输入``icraft --version ``，若已经正常安装则会显示当前icraft版本，如：

```powershell
Icraft 版本:
 * v3.33.1

CLI 版本:
3.33.1.0-bccd576(2512171607)
```

## 4.3 位流更换

更换位流，选择所需版本的位流替换/root/bits/BOOT.bin文件即可（系统正常启动需要的文件为BOOT.bin、fmqlmp-verify.dtb、Image、uEnv.txt，更换位流时只替换bin)，更换后记得**重启**板卡即可生效。

## 4.4 上板执行

到这里恭喜你完成了所有前期环境准备工作，那么你可以下载模型库[icraft_modelzoo合集详情-来自AIBS · 魔搭社区](https://www.modelscope.cn/collections/icraft_modelzoo-18b52923d4854f)中的任一模型，根据模型库中的相关教程完成模型上板推断！这里简要说明两种模式下对板卡系统进行的操作。

1. 如果运行 **socket模式** ，需先在AI硬件上开启server：

使用网口或串口进入板上系统打开server，例如使用mobaXterm这个软件通过网口进入板上系统，如图，输入板卡ip，输入username为root。

<img src="./assets/mobaxterm_ssh.png" alt="mobaxterm_ssh" style="zoom:80%;" />

进入系统后，终端执行命令：

```powershell
icraft-serve
```

请确保在root账户下执行上述命令，设备成功打开示意图

```powershell
root@U:~# icraft-serve
[02/22/24 02:02:00.388] [I] Using port : 9981
[02/22/24 02:02:00.388] [I] synchronous mode
[02/22/24 02:02:00.388] [I] [irpc::port::tcp::_waitNewConn] wait for new connection
```

开启成功后可根据模型库中的教程执行psin相关程序。

2. 如果运行**axi模式**

将交叉编译环境下生成的可执行程序相关文件（对应模型库中build_arm、cfg、imodel、io等相关文件），按原目录结构复制到AI硬件上，即放入片上系统任意目录下，然后转到build_arm目录下，执行命令：

```powershell
chmod 777 * # 开权限
./model_plin ../cfg/model.yaml # model为生成可执行程序的名字，yaml为其对应的配置文件
```

如此即可看到模型的实时效果啦！！

# 5. 结束语

到此，你可以适配自己的模型啦！为了方便理解，我将模型部署的整体流程总结为以下几个步骤：

1、开源平台上（如GitHub）寻找所需模型开源代码（对应模型库中的源码文件夹）

2、在相关深度学习框架下（如pytorch）跑通该模型推理（对应模型库中1_scripts文件中的0_infer.py）

3、模型导出（对应模型库中1_scripts文件中的1_save.py）

4、模型编译(icraft compile编译生成model_name.json&model_name.raw)

5、模型上板推理(对应模型库中3_deploy文件中的内容)

其中，1-3是在框架下进行（即不依赖于我们产品环境，是运行相关AI模型的环境搭建），4是执行icraft compile一键式编译（安装了icraft即可实现），5是适应板卡与源码形成前后处理的对齐，完成上板推理（环境依赖于提供的modelzoo_utils、thirdparty等依赖）。

**注意**：icraft编译的.pt文件与步骤1源码中的.pt文件不是同一个，icraft编译的.pt文件是trace出的torchscript模型（目前也支持onnx模型），步骤1中源码的.pt文件代表的是模型weights。

更多模型与示例可关注我们相关模型库：

[modelzoo-icraft v3.31及之前版本](https://gitee.com/link?target=https%3A%2F%2Fgitlink.org.cn%2Fspider-mxh%2Fmdz)

[modelzoo-icraft v3.33及之后版本](https://gitee.com/link?target=https%3A%2F%2Fwww.modelscope.cn%2Fcollections%2Ficraft_modelzoo-18b52923d4854f)
