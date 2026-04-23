# CODEBASE_MAP_TEMPLATE.md

> 这个文件不是一次性文档，而是持续维护的“代码地图”。  
> 目标：让你和 Codex 都能快速理解项目，而不是每次都从零读代码。

---

## 1. Main entrypoints

列出主入口：

| Entry | File | Responsibility |
|---|---|---|
| `main()` | `src/app/main.cpp` | 程序启动、配置加载、模块初始化 |
| `run_pipeline()` | `src/app/pipeline.cpp` | 主流程编排 |
| ... | ... | ... |

---

## 2. Main call chain

从入口到关键路径写成链条。

示例：

```text
main
 -> load_config
 -> init_runtime
 -> init_display
 -> create_patch_scanner
 -> process_loop
    -> next_patch
    -> convert_to_nhwc
    -> runtime_infer
    -> decode_outputs
    -> compose_overlay
    -> render_frame
```

---

## 3. Core classes and responsibilities

| Class | File | Responsibility | Owns resources? | Thread affinity |
|---|---|---|---|---|
| `PatchScanner` | `src/preprocess/...` | 生成 patch 顺序流 | no / partial | main thread |
| `RuntimeRunner` | `src/runtime/...` | 推理调用封装 | yes | inference thread |
| `DisplayRenderer` | `src/display/...` | HDMI 显示 | yes | display thread |

---

## 4. Data format map

把关键数据格式写清楚：

| Stage | Type | Shape | Notes |
|---|---|---|---|
| input image | `uint8` | H x W | grayscale |
| patch | `uint8` | 512 x 512 | crop result |
| tensor | `float` / `uint8` | 1 x 512 x 512 x 1 | NHWC |
| logits | ... | ... | runtime output |
| mask | `uint8` | 512 x 512 | class ids |
| overlay | RGB | 1920 x 1080 or region | display frame |

---

## 5. Resource lifecycle map

记录这些对象的生灭关系：

- 配置对象
- runtime context
- 输入大图 buffer
- patch buffer
- 输出 buffer
- display buffer

建议格式：

```text
App
 ├── owns RuntimeRunner
 ├── owns DisplayRenderer
 ├── owns PatchScanner
 └── manages frame loop lifetime
```

---

## 6. Known hotspots

列出热路径和敏感点：

- patch 切片
- 格式转换
- 推理调用
- 后处理 argmax / decode
- overlay 合成
- HDMI 送显

---

## 7. Known risks

持续更新高风险点：

- 越界访问风险：
- 空指针风险：
- 生命周期风险：
- race / deadlock 风险：
- 帧率抖动风险：
- 内存峰值风险：

---

## 8. Recent changes log

每次重要变动后补一条：

### YYYY-MM-DD
- changed:
- reason:
- risk:
- validation:

---

## 9. Questions / ambiguities

这里记录项目里仍未完全想清楚的地方，避免 Codex 擅自脑补：

- 某 buffer 是否允许跨帧复用？
- display 线程是否允许降帧但不阻塞主流程？
- 某模块是否必须保持无异常风格？
