# ARCHITECTURE_TEMPLATE.md

> 用于描述系统边界与关键设计。  
> 目标不是写成长文，而是让 Codex 和维护者快速理解项目。

---

## 1. System goal

一句话说明系统目标：

- 这个工程是做什么的？
- 主要运行环境是什么？
- 最关键的性能 / 稳定性约束是什么？

示例：

- 板端 C++ 推理后端，负责大图切 patch、调用 NPU runtime、做后处理并通过 HDMI 实时显示。

---

## 2. Non-goals

明确不属于本工程职责的部分：

- 不负责训练
- 不负责模型导出
- 不负责网络通信
- 不负责上位机 GUI

---

## 3. Module breakdown

| Module | Responsibility | Inputs | Outputs | Key dependencies |
|---|---|---|---|---|
| preprocess | 大图读取与 patch 生成 | 灰度大图 | patch 流 | OpenCV / image io |
| runtime | 调用推理引擎 | NHWC tensor | raw outputs | vendor runtime |
| postprocess | 输出解码与颜色映射 | logits / mask | RGB overlay | utils |
| display | HDMI 显示与叠加 | image frames | screen output | framebuffer / SDL / DRM |
| app | 主流程编排 | config + modules | end-to-end run | all above |

---

## 4. Main data flow

按顺序写清楚数据形态：

1. 输入大图：`uint8` grayscale, shape = H x W
2. patch：`uint8`, 512 x 512
3. runtime input tensor：NHWC, `1 x 512 x 512 x 1`
4. runtime output：...
5. postprocess output：mask / RGB / restored image
6. display frame：...

---

## 5. Thread model

说明每个线程做什么：

- main thread：
- capture / input thread：
- inference thread：
- display thread：

说明共享资源和同步机制：

- queue / ring buffer / mutex / condition variable
- 哪些对象只允许单线程访问

---

## 6. Ownership and lifetime rules

关键对象的创建与销毁：

| Object | Created by | Owned by | Destroyed by | Notes |
|---|---|---|---|---|
| runtime context | app init | app | app shutdown | singleton-like |
| frame buffer | display module | display | display | reuse allowed |
| patch scanner | app | app / preprocess | scope end | non-owning image view? |

明确规则：

- 谁拥有谁
- 是否允许共享所有权
- 是否允许缓存复用
- 是否允许跨线程传裸指针

---

## 7. Error handling strategy

选择并写清楚：

- exceptions
- status codes
- bool + logging
- expected-like return types

并给出约束：

- 哪些错误必须立刻失败
- 哪些错误允许降级处理
- 哪些错误必须日志记录

---

## 8. Performance constraints

列出量化约束：

- target FPS:
- target latency per frame:
- max memory budget:
- hot path modules:
- operations to avoid in hot path:

---

## 9. Invariants / do-not-break rules

这里最重要。写死不能破坏的约束。

示例：

- 输入 patch 顺序必须保持蛇形扫描。
- 边缘不足 patch 必须丢弃，不补零。
- runtime 输入必须保持 NHWC。
- display 线程不能阻塞推理主循环超过 N ms。
- 所有 runtime 输出后处理必须无越界访问。

---

## 10. Extension points

允许未来扩展的位置：

- 支持不同 patch size
- 支持不同 runtime backend
- 支持多种 overlay mode
- 支持录屏 / 保存结果

写清楚目前不做哪些扩展。
