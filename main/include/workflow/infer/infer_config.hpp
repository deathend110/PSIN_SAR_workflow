#pragma once

#include <filesystem>
#include <string>

namespace workflow::infer
{
    // 当前模型输入 batch 维度固定为 1。
    inline constexpr int kExpectedN = 1;
    // 当前模型输入高度固定为 512。
    inline constexpr int kExpectedH = 512;
    // 当前模型输入宽度固定为 512。
    inline constexpr int kExpectedW = 512;
    // 当前模型输入通道数固定为 1。
    inline constexpr int kExpectedC = 1;
    // 当前语义分割输出类别数固定为 6。
    inline constexpr int kSegClasses = 6;

    // Infer 工作流配置；字段基本与 `infer_workflow.yaml` 一一对应。
    struct AppConfig
    {
        // 设备 URL，例如板端设备标识。
        std::string device_url;
        // 运行后端，目前主要是 zg330 或 host。
        std::string run_backend = "zg330";
        // 是否开启 MMU 模式。
        bool mmu_mode = true;
        // 是否启用更偏速度的后端选项。
        bool speed_mode = false;
        // 是否压缩中间临时缓冲。
        bool compress_ftmp = false;
        // OCM 优化策略编号。
        int ocm_option = 1;
        // 是否开启时间 profiling。
        bool enable_profile = false;

        // SAR 输入目录或单图路径。
        std::filesystem::path sar_img_dir = "./io/sar_img";
        // 允许扫描的图像后缀。
        std::string sar_img_ext = ".png";
        // 是否递归扫描输入目录。
        bool recursive = false;

        // patch 模式：auto_snake / manual_flight / debug_raster。
        std::string patch_mode = "auto_snake";
        // patch 边长，当前实现要求为 512。
        int patch_size = kExpectedH;
        // auto_snake / manual_flight 下的步长。
        int stride = 256;
        // debug_raster 横向步长。
        int debug_stride_x_px = 256;
        // debug_raster 纵向步长。
        int debug_stride_y_px = 256;

        // 模型 JSON 描述文件路径。
        std::string json_path;
        // 模型参数 raw 文件路径。
        std::string raw_path;
        // 单次输出等待超时，单位毫秒。
        int output_wait_ms = 20000;

        // HDMI/UI 合成目标宽度。
        int display_width = 1280;
        // HDMI/UI 合成目标高度。
        int display_height = 720;
        // HDMI 刷新目标 FPS；0 表示按默认节拍。
        int display_fps = 0;

        // 输出模式：hdmi 或 png。
        std::string output_mode = "hdmi";
        // PNG 输出目录。
        std::filesystem::path output_dir = "./io/output";
        // 输出文件是否允许覆盖。
        bool overwrite = true;

        // 是否在初始化后输出后端日志。
        bool dump_backend_log = true;
    };

    // 从 YAML 读取 Infer 配置。
    AppConfig LoadConfig(const std::filesystem::path &config_path);
    // 把 Infer 配置写回 runtime YAML。
    void SaveConfig(const std::filesystem::path &config_path, const AppConfig &cfg);
}
