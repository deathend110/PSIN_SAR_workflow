# tutorial 3.33 使用教程

这里是ZHUGE—icraft3.33版本的使用教程

建议在安装完icraft基本环境后，参考此工程，熟悉icraft的使用方法

- 用法
  - 参考Part 0 完成环境准备
  - 参考Part 1 完成快速上手
  - 参考Part 2 了解icraft编译配置参数的使用方法和注意事项
  - 参考Part 3 了解运行时整体流程及API
  - 参考Part 4 了解性能分析与优化
  - 参考Part 5 了解硬算子相关使用方法

## 章节指引
```
├── docs
│   ├── Part 0 env_setup.md 
│   ├── Part 1_1 quick-start_windows.md 
│   ├── Part 1_2 Icraft Terminology.md
│   ├── Part 2_1 compile.md
│   ├── Part 2_2 icraft tools.md
│   ├── Part 3_1 runtime.md
|	|── Part 4 性能分析及优化.md
|	|—— Part 5 fpga-op.md
│   └── assets
└── readme.md
```
* **docs**: 五个章节，内容分别为：
  * [Part 0](<./Part 0 env_setup.md>): 环境准备章节，开始前必读内容。
  
  * Part 1: 快速开始章节，
    * [1-1](<./Part 1_1 quick-start_windows.md>)为windows系统的快速开始
    * [1-2](<./Part 1_2 Icraft Terminology.md>)为基本概念介绍
  
  * Part 2:编译章节
    * [2-1](<./Part 2_1 compile.md>)为icraft编译教程，分组件介绍各组件常用参数的含义、使用方法、注意事项
    * [2-2](<./Part 2_2 icraft tools.md>)为icraft工具介绍如icraft docs和icraft show，icraft show中包含网络可视化、量化分析工具、内存优化分析工具
  
  * Part 3:运行时章节
    * [3-1](<./Part 3_1 runtime.md>)为icraft运行时教程，介绍运行时整体流程及各阶段涉及到的API
  
  * [Part 4](<./Part 4 性能分析及优化.md>):性能分析及优化章节
  
  * [Part 5](<./Part 5 fpga-op.md>):硬算子`detpost`相关使用方法（当前版本诸葛框架下只支持`detpost`硬算子）

## 部署流程推荐
建议按照下述步骤进行模型部署：
* 模型导出
  * 将模型导出为traced_model(.pt或.onnx)
  * 结果一致性测试：框架源码 vs 导出模型 
  * 结果一致性测试建议评估单个样本推理结果和数据集精度结果
* 模型编译
  * 导出模型(traced_model)结合编译配置(toml)进行icraft compile
* 运行时工程开发
  * 开发CRT(C++ Runtime)工程或PYRT(Python Runtime)工程
* 仿真测试
  * 对编译各阶段(p-parse/o-optimize/q-quantize/a-adapt/g-generate)模型进行仿真测试，确保各阶段结果一致性
* 上板部署
  * 结果一致性测试：模型g阶段仿真结果 vs 上板部署结果
  * 结果一致性测试建议评估单个样本推理结果和数据集精度结果
## 开发环节
在模型部署、适配过程中，可能涉及到的开发环节有：
* 模型导出
  * 拆分网络主体和模型的前后处理，仅保留网络主体
  * 模型主体中结构的等效修改
* 模型编译
  * 确保各阶段编译结果的正确性
  * 除了确保单个样本输入的各阶段仿真结果的正确性、还应进行数据集的精度测试
* 运行时工程
  * psin工程
  * plin工程
* 性能优化
  * 如果时间不满足要求，可参考教程的《性能分析及优化》等相关内容，如移除CPU算子、定制化优化瓶颈算子；
  * 可参考多线程例程进一步提升帧率
* 操作系统迁移

## 模型库指引

更多模型与示例可关注icraft模型库：

[modelzoo-icraft v3.31及之前版本](https://gitee.com/link?target=https%3A%2F%2Fgitlink.org.cn%2Fspider-mxh%2Fmdz)

[modelzoo-icraft v3.33及之后版本](https://gitee.com/link?target=https%3A%2F%2Fwww.modelscope.cn%2Fcollections%2Ficraft_modelzoo-18b52923d4854f)
