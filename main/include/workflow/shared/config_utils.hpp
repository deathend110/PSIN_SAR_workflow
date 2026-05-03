#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>

namespace workflow::shared
{
    // 去掉字符串首尾空白，供命令行输入和 YAML 文本解析复用。
    std::string Trim(std::string value);
    // 去掉首尾成对引号，避免配置值在读取后仍保留包裹字符。
    std::string StripQuotes(std::string value);
    // 把字符串转成小写，便于做大小写无关比较。
    std::string ToLower(std::string value);
    // 按仓库约定解析布尔文本，失败时带上上下文抛错。
    bool ParseBool(const std::string &value, const std::string &error_context = {});

    // 读取简化版 YAML，输出 "扁平键 -> 字符串值" 映射。
    std::unordered_map<std::string, std::string> LoadSimpleYaml(const std::filesystem::path &config_path);
    // 把逻辑配置路径映射到 runtime 配置路径。
    std::filesystem::path RuntimeConfigPath(const std::filesystem::path &config_path);
    // 确保 runtime 配置文件存在；必要时从原始配置复制一份。
    std::filesystem::path EnsureRuntimeConfigFile(const std::filesystem::path &config_path);

    // 从键值表中取字符串；缺失时返回默认值。
    std::string ValueOr(const std::unordered_map<std::string, std::string> &values,
                        const std::string &key,
                        const std::string &default_value);

    // 从键值表中取整数；缺失时返回默认值。
    int IntValueOr(const std::unordered_map<std::string, std::string> &values,
                   const std::string &key,
                   int default_value);

    // 从键值表中取布尔值；缺失时返回默认值，解析失败时抛错。
    bool BoolValueOr(const std::unordered_map<std::string, std::string> &values,
                     const std::string &key,
                     bool default_value,
                     const std::string &error_context = {});

    // 以“先写临时文件再替换”的方式原子写文本，避免配置文件被写坏。
    void WriteTextFileAtomically(const std::filesystem::path &path, const std::string &content);
}
