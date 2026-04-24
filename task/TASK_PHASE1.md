# Task title

**第一阶段优先做 HDMI 展示增强**
先不要急着做 Web 控制台。你现在主链路已经通了，最先能出效果的是在 HDMI 输出上加：

- 项目标题
- 当前模式
- 当前 echo / SAR / patch / frame 编号
- FPS
- SAR 成像耗时
- NPU 推理耗时
- 总延时
- 分割类别图例
- 左上角小地图区域显示当前的输入SAR图片的完整略缩图,并用红色框出当前patch在地图上的位置,虚线绘制无人机的飞行路线
- UI生成和模型推理模块应该是两个线程.或者模型推理完成后把参数和结果图嵌入UI对应位置,并输出为HDMI或者保存为png
- 参考UI设计里面的分割图的legend可能有点大,你适当缩小,不要占据过大的篇幅,主要还是要展示恢复和分割结果图片.具体的图例颜色以后处理时的字典映射为真正颜色:
```cpp
cv::Vec3b classColorBgr(int cls)
{
    static const cv::Vec3b colors[SEG_CLASSES] = {
        cv::Vec3b(255, 0, 0),
        cv::Vec3b(0, 255, 0),
        cv::Vec3b(0, 0, 255),
        cv::Vec3b(255, 255, 0),
        cv::Vec3b(0, 255, 255),
        cv::Vec3b(255, 0, 255)};
    return colors[std::max(0, std::min(cls, SEG_CLASSES - 1))];
}
```

---

## 1. Background

这个任务为什么存在？

- 当前问题：我们已经完成了第0阶段，打通全链路的流程。并完成了工作区代码模块化。现在要进行第一阶段，整合HDMI显示。
- 相关上下文：关于HDMI输出UI的设计，参考：main\src\hdmi_ui_preview_1080_p_industrial.jsx
            这是我设计的一版，你要做的就是整合这个UI.
- 触发场景：HDMI输出和png保存，都需要用到这个UI
- 已知限制：

---

## 2. Goal

明确写出要实现的行为。

**必须是可验证的。**

示例：

- 实现第一阶段，整合HDMI显示
- 只修改HDMI打包部分，同时同步png模块。既png保存的是要推送给HDMI的图片

---

## 3. Out of scope

明确不做什么。

示例：

- 不修改 runtime 模块
- 不修改模型输入格式
- 不重构整个 preprocess 目录

---

## 4. Allowed files to modify

只列允许改的文件。

- main下的文件
- ARCHITECTURE_TEMPLATE.md
- CODEBASE_MAP_TEMPLATE.md

---

## 6. Functional requirements

列成可验收条目：

具体你自己分析

---

## 7. Non-functional requirements

具体你自己分析

---

## 8. Interface expectations

参考的HDMI UI设计：
main\src\hdmi_ui_preview_1080_p_industrial.jsx

如果接口可以调整，也写清楚哪些部分可变。

---

## 9. Edge cases

要求 Codex 必须考虑这些：
具体你自己分析

---

## 10. Validation

运行编译语法验证：
具体你自己分析
---

## 11. Required response format before editing

要求 Codex 在动手前先输出：

1. 对任务的理解
2. 计划修改的文件
3. 不会修改的文件
4. 实现方案
5. 风险点
6. 验证方案

---

## 12. Required response format after editing

要求 Codex 改完后输出：

1. 修改了哪些文件
2. 每个文件做了什么改动
3. 为什么这样设计
4. 跑了哪些命令
5. 哪些验证通过 / 未通过
6. 剩余风险

---

## 13. Done when

写成客观验收标准：
具体你自己分析

