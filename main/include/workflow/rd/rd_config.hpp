#pragma once

#include <filesystem>
#include <string>

namespace workflow::rd
{
    // RD 成像流程的配置快照；字段直接对应 YAML 可调项。
    struct AppConfig
    {
        // echo 输入目录或单文件路径。
        std::filesystem::path echo_dir = "./io/echo";
        // 允许扫描的 echo 文件后缀。
        std::string echo_ext = ".bin";
        // 成像后的 PNG 输出目录。
        std::filesystem::path output_dir = "./io/sar_img";
        // scratch 双精度管线中间文件目录。
        std::filesystem::path scratch_dir = "./io/rd_scratch";
        // 执行模式：auto / memory_float32 / scratch_double。
        std::string execution_mode = "auto";
        // 距离向分块列宽。
        int column_tile = 64;
        // 方位向分块行高。
        int row_tile = 128;
        // 允许使用的内存预算，单位 MB。
        int memory_limit_mb = 500;
        // 在可行时优先使用内存态 double 管线。
        bool prefer_memory_pipeline = true;
        // 是否保留 scratch 文件，便于调试。
        bool keep_scratch = false;
        // 输出已存在时是否覆盖。
        bool overwrite = true;
    };

    // 从 YAML 读取 RD 配置。
    AppConfig LoadConfig(const std::filesystem::path &config_path);
    // 把 RD 配置写回 runtime YAML。
    void SaveConfig(const std::filesystem::path &config_path, const AppConfig &cfg);
}
