#pragma once

#include "workflow/infer/infer_config.hpp"
#include "workflow/shared/run_control.hpp"

#include <filesystem>
#include <string>

namespace workflow::infer
{
    // Web 控制面可修改的 manual_flight 参数集合。
    struct ManualFlightSettings
    {
        // 常规单步移动像素。
        int manual_step_px = 128;
        // 加速步长；当前主要作为保留参数向下传递。
        int boost_step_px = 256;
        // 历史语义保留字段，兼容旧配置。
        int trigger_distance_px = 128;
        // 小地图路径采样网格大小。
        int cache_grid_px = 64;
        // 是否在 UI 中叠加路径轨迹。
        bool path_overlay = true;
        // 当前控制绑定字符串，仅用于 UI/配置展示。
        std::string control_bindings = "W/A/S/D";
    };

    // 暴露给 Web/UI 的 manual_flight 运行态快照。
    struct ManualFlightTelemetry
    {
        bool configured = false;
        bool active = false;
        bool paused = false;
        bool path_overlay = true;
        bool edge_blocked = false;
        int position_x = 0;
        int position_y = 0;
        int last_inferred_center_x = 0;
        int last_inferred_center_y = 0;
        int path_points = 0;
        int patch_count = 0;
        std::string current_direction;
        std::string pending_direction;
    };

    // 更新进程级 manual_flight 参数；后续 runtime 激活时会读取它。
    void ConfigureManualFlight(const ManualFlightSettings &settings);
    // 把活动 runtime 重置到初始状态。
    void ResetManualFlight();
    // 更新 manual runtime 的暂停状态。
    void SetManualFlightPaused(bool paused);
    // 提交一条人工方向键输入；message 用于向 Web 返回解释文本。
    bool SubmitManualFlightKey(const std::string &key, bool pressed, std::string *message = nullptr);
    // 读取当前 manual runtime 的对外可见遥测。
    ManualFlightTelemetry GetManualFlightTelemetry();

    // 从配置文件读取配置并执行一轮 Infer。
    int Run(const std::filesystem::path &config_path);
    // 直接用内存配置执行 Infer；control 为空时表示独立运行。
    int Run(const AppConfig &config, shared::WorkflowRunControl *control = nullptr);
}
