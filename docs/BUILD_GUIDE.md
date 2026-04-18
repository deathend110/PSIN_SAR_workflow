# FPAI Demo Package - Build Guide

本文档详细说明如何编译和配置 FPAI 示例工程。

---

## 目录

- [环境准备](#环境准备)
- [编译配置](#编译配置)
- [常见问题](#常见问题)

---

## 环境准备

### 1. 系统要求

准备好交叉编译环境，优先选择docker。

- **Ubuntu 20.04 LTS** (推荐)
- **CMake 3.16+**
- **GCC/G++ 9.4+** 或 **Clang 10+**
- **iCraft SDK** (需单独安装)

### 2. 安装编译工具

```bash
sudo apt update
sudo apt install -y     cmake     build-essential     git     wget     pkg-config
```

### 3. 安装 iCraft SDK

SDK 已随外发压缩包提供，解压后到对应目录安装：

```bash
cd icraft
sudo dpkg -i Icraft_3.33.1_onchip.deb CustomOp_3.33.1_onchip.deb

# 验证安装
dpkg -l icraft
dpkg -l customop
```

---

## 编译配置

### CMake 参数说明

| 参数 | 可选值 | 默认值 | 说明 |
|------|--------|--------|------|
| `TARGET_CHIP` | `BY`, `ZG` | `BY` | 选择后端（Buyi 或 Zhuge） |

### 编译步骤

#### 分别构建不同的工程（推荐）

```bash
cd examples/1_single_input+ai/PLin+SingleNet+VPU

# 配置 Buyi 后端
cmake -S . -B build/ZG -DTARGET_CHIP=ZG

# 编译
cmake --build build/ZG -j$(nproc)

# 可执行文件位置
ls build/ZG/sdicamera+yolov5+vpu2file
```

## 下一步
- 查看 [全部编译脚本](../tools/build_all_examples.sh) 一键编译所有示例
- 查看 [API_REFERENCE.md](API_REFERENCE.md) 了解如何修改 Pipeline
- 查看 [README.md](../README.md) 了解更多示例
