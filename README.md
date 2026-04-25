# `PSIN_SAR_workflow`

`PSIN_SAR_workflow` 是一个面向 `Linux + aarch64 + ZG330` 平台的 SAR 主机侧工作流仓库。  
当前仓库的核心目标是把板端实际运行所需的 RD 成像、推理处理和 Web 控制台收敛到同一个 C++ 主程序中。

当前主程序支持 3 个真实运行模式：

- `RD only`
- `Inference only`
- `Web Console`

典型处理链路为：

```text
echo.bin -> RD 成像 -> SAR 灰度 PNG -> 512x512 patch 推理 -> 恢复图 / 分割图 -> HDMI 或 PNG
```

## 仓库用途

这个仓库主要用于承载以下内容：

- 板端主工程源码与构建脚本
- RD / Inference / Web Console 的集成实现
- 当前代码结构、架构边界和代码地图文档
- bug 修复任务单、评审检查单和提示词模板

它不是一个“只有算法代码”的仓库，也不是一个纯文档仓库，而是：

- `main/` 下的可运行 C++ 主工程
- 根目录下的开发辅助文档与工作流模板

## 仓库结构

```text
PSIN_SAR_workflow/
  main/         当前真正参与构建和运行的主工程
  deps/         第三方依赖与底层库
  docs/         其他补充文档
  bug/          bug 修复任务单
  task/         任务文档
  AGENTS.md     本仓库协作约束
  ARCHITECTURE.md
  CODEBASE_MAP.md
  REVIEW_CHECKLIST.md
  PROMPT_SNIPPETS.md
```

### `main/`

`main/` 是当前最重要的子目录。它包含：

- `src/` 运行时代码
- `include/` 头文件
- `configs/` 配置文件
- `build_main.sh` 构建脚本
- `CMakeLists.txt` 交叉构建入口

当前主程序产物为：

```text
main/build/ZG/psin_workflow
```

更详细的模块和运行说明见：

- [main/README.md](./main/README.md)

## 关键文档入口

如果你是第一次进入这个仓库，建议按下面顺序阅读：

1. [README.md](./README.md)
   先了解仓库用途与顶层结构
2. [main/README.md](./main/README.md)
   了解当前主工程、模式、配置和运行方式
3. [ARCHITECTURE.md](./ARCHITECTURE.md)
   了解系统边界、线程关系、约束和风险点
4. [CODEBASE_MAP.md](./CODEBASE_MAP.md)
   了解主调用链、核心类型职责和数据流
5. [AGENTS.md](./AGENTS.md)
   了解本仓库的协作、修改范围和验证要求

## 当前主工程能力

当前 `main/` 工程的真实能力包括：

- 读取 echo bin 并执行 RD 成像
- 读取 SAR PNG 并执行 patch 推理
- 输出恢复图、分割图以及工业 UI 合成结果
- 通过 Web Console 进行模式选择、配置更新、作业控制和状态查看
- 支持 `manual_flight` 模式下的 patch 级扫描游标控制

当前明确不覆盖的内容包括：

- 模型训练、量化与导出
- 独立外部 Web 服务进程
- 多作业并发推理
- 完整三维飞控系统

## 构建与运行

当前仓库的主构建入口在 `main/`：

```bash
cd main
./build_main.sh
```

运行入口同样在 `main/`：

```bash
cd main
./build/ZG/psin_workflow
```

注意：

- `main/CMakeLists.txt` 当前明确面向 `Linux/aarch64`
- Windows 环境下不会直接配置通过
- 菜单、配置和运行模式的细节请以 [main/README.md](./main/README.md) 为准

## 适合谁看

这个 README 主要给以下读者使用：

- 第一次接手这个仓库的开发者
- 想快速知道“代码到底在哪、从哪开始看”的维护者
- 需要先理解仓库用途，再决定深入哪份文档的人

如果你要修改运行时代码，优先看：

- [ARCHITECTURE.md](./ARCHITECTURE.md)
- [CODEBASE_MAP.md](./CODEBASE_MAP.md)
- [main/README.md](./main/README.md)

## 当前状态说明

当前仓库已经从早期的“只有 RD / Inference”演进到包含 `Web Console` 的单入口主程序形态。  
如果你看到历史笔记、旧文档或注释与当前代码不一致，请优先以 `main/` 下的真实实现为准。
