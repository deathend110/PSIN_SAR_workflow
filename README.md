# FPAI Demo Package

**版本**: 26010502
**平台**: ZG
**AI Mate 版本**: 25122301
**iCraft 版本**: 3.33.1
**用途**: FPAI 30TAI 参考设计示例工程: 单路PLIN+pHDMI(外置detpost)参考实现
**打包时间**: 2026-01-05 14:39:09

---

## 📦 Package Contents

```
fpai_demo_package_26010502/
├── deps/                   # 依赖库和工具
│   ├── modelzoo_utils/    # 核心工具库（Pipeline、Actor、NetInfo 等）
│   └── thirdparty/        # 第三方库（OpenCV、spdlog、FFmpeg 等）
├── examples/              # 示例工程
│   └── .../               # 示例工程合集
├── docs/                  # 文档
│   ├── BUILD_GUIDE.md     # 编译指南
│   └── API_REFERENCE.md   # API 参考
├── tools/                 # 辅助工具脚本
├── icraft/                # iCraft SDK
└── README.md              # 本文件
```

---

## 📋 Supported Examples

| Example Path | Description | Input | AI Model | Output |
|--------------|-------------|-------|----------|--------|
| `1_single_input+ai/PLin+SingleNet+HDMI` | 单网络推理 | SDI Camera | YOLOv5 | VPU Encoder |

---

## 🚀 Quick Start

### 1. 系统要求

- **操作系统**: Ubuntu 20.04 LTS
- **硬件**: FPAI 设备（BUYI 100TAI/ZHUGE 30TAI）
- **编译工具**:
  ```bash
  sudo apt install -y cmake build-essential
  ```
- **运行时依赖**:
  ```bash
  # iCraft XRT/XIR 库（需要从 FPAI SDK 安装）
  # OpenCV, spdlog 等（已包含在 deps/thirdparty/）
  ```

### 2. 快速编译运行

#### 示例 1: SDI 摄像头 + YOLOv5 推理

```bash
cd examples/1_single_input+ai/PLin+SingleNet+VPU

# 配置ZG后端（可以选择后端：BY 或 ZG）
cmake -S . -B build/ZG -DTARGET_CHIP=ZG

# 编译
cmake --build build/ZG -j$(nproc)

# 准备
把交叉编译的结果赋值到目标板卡上，包括可执行文件、模型文件和配置文件等：
[运行目录]/
├── imodel/ZG/                
│   └── .json .raw        # 部署于FPAI平台的icraft模型文件
├── configs/ ZG/              
│   └── ZG                 # 适用ZG平台的配置
├── names/                
│   └── ZG                 # 示例yolov5所需的coco.names
└── sdicamera+yolov5+vpu2file                  # 可执行文件

# 运行
chmod a+x sdicamera+yolov5+vpu2file
./sdicamera+yolov5+vpu2file configs/ZG/sdicamera+yolov5+vpu2file.yaml
```

## 📖 Documentation

- [BUILD_GUIDE.md](docs/BUILD_GUIDE.md) - 详细的编译配置指南
- [API_REFERENCE.md](docs/API_REFERENCE.md) - Pipeline 和 Actor 的 API 文档
- [deps/modelzoo_utils/C++_API_reference.md](deps/modelzoo_utils/C++_API_reference.md) - 核心工具库 API

---

## 🔧 Architecture Overview

### Pipeline + Actor 模式

```
┌─────────────┐    ┌──────────┐    ┌───────────────────────────┐
│ InputActor  │───▶│ NPUActor │───▶│       OutputActor         │
│ (Camera    )│    │(Inference)│    │ Postprocess+(Display/File)│
└─────────────┘    └──────────┘    └───────────────────────────┘
       │                  │                          │
       └──────────────────┴──────────────────────────┘
                          Buffer Manager
```

### 关键组件

- **Actor**: 独立线程，负责数据处理的一个阶段
- **BufferManager**: 零拷贝内存管理
- **ThreadSafeQueue**: 线程安全的消息队列
- **NetInfo**: 网络模型信息封装

---

## 📞 Support & Contact

- **技术支持**: chenjiannan@fmsh.com.cn
- **项目主页**: https://www.modelscope.cn/models/AIBS/fpai_reference_design

---

## 📄 License

Copyright © 2025 FMSH. All rights reserved.

See [LICENSE](LICENSE) for details.
