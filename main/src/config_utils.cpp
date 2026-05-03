#include "workflow/shared/config_utils.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace workflow::shared
{
    namespace
    {
        constexpr const char *kExampleYamlSuffix = ".example.yaml";
        constexpr const char *kYamlSuffix = ".yaml";

        // 把层级键路径如 ["server", "bind"] 拼成 "server.bind"。
        std::string JoinPath(const std::vector<std::string> &scopes)
        {
            std::string path;
            for (const auto &scope : scopes)
            {
                if (!path.empty())
                {
                    path += ".";
                }
                path += scope;
            }
            return path;
        }

        // 判断字符串是否以指定后缀结尾。
        bool EndsWith(const std::string &value, const std::string &suffix)
        {
            return value.size() >= suffix.size() &&
                   value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
        }

        // 根据 runtime 配置路径推导出对应的 example 配置路径。
        std::filesystem::path ExampleConfigPath(const std::filesystem::path &runtime_path)
        {
            const auto filename = runtime_path.filename().string();
            if (EndsWith(filename, kExampleYamlSuffix))
            {
                return runtime_path;
            }

            std::filesystem::path example_path = runtime_path;
            example_path.replace_filename(runtime_path.stem().string() + kExampleYamlSuffix);
            return example_path;
        }

        // 一次性读入整个文本文件，主要供配置 bootstrap 复制使用。
        std::string ReadTextFile(const std::filesystem::path &path)
        {
            std::ifstream ifs(path, std::ios::binary);
            if (!ifs)
            {
                throw std::runtime_error("Failed to open config yaml: " + path.string());
            }

            std::string content;
            ifs.seekg(0, std::ios::end);
            const auto size = ifs.tellg();
            ifs.seekg(0, std::ios::beg);
            if (size > 0)
            {
                content.resize(static_cast<std::size_t>(size));
                ifs.read(content.data(), static_cast<std::streamsize>(content.size()));
                if (!ifs && !ifs.eof())
                {
                    throw std::runtime_error("Failed to read config yaml: " + path.string());
                }
            }
            return content;
        }
    }

    // 去掉字符串首尾空白。
    std::string Trim(std::string value)
    {
        const auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
        value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
        return value;
    }

    // 若字符串被单引号或双引号完整包裹，则去掉最外层引号。
    std::string StripQuotes(std::string value)
    {
        value = Trim(std::move(value));
        if (value.size() >= 2 &&
            ((value.front() == '"' && value.back() == '"') ||
             (value.front() == '\'' && value.back() == '\'')))
        {
            return value.substr(1, value.size() - 2);
        }
        return value;
    }

    // 就地把字符串转换为小写副本。
    std::string ToLower(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return value;
    }

    // 按仓库里常见的 true/false、1/0、yes/no、on/off 规则解析布尔值。
    bool ParseBool(const std::string &value, const std::string &error_context)
    {
        const auto lowered = ToLower(Trim(value));
        if (lowered == "true" || lowered == "1" || lowered == "yes" || lowered == "on")
        {
            return true;
        }
        if (lowered == "false" || lowered == "0" || lowered == "no" || lowered == "off")
        {
            return false;
        }

        const auto prefix = error_context.empty() ? std::string("Failed to parse bool value: ")
                                                  : (error_context + ": ");
        throw std::runtime_error(prefix + value);
    }

    // 读取简化 YAML。
    // 这个解析器不追求完整 YAML 兼容，只支持当前仓库用到的“缩进 + key: value”结构。
    std::unordered_map<std::string, std::string> LoadSimpleYaml(const std::filesystem::path &config_path)
    {
        std::ifstream ifs(config_path);
        if (!ifs)
        {
            throw std::runtime_error("Failed to open config yaml: " + config_path.string());
        }

        std::unordered_map<std::string, std::string> values;
        std::vector<std::string> scopes;
        std::string line;
        while (std::getline(ifs, line))
        {
            const auto comment_pos = line.find('#');
            if (comment_pos != std::string::npos)
            {
                line = line.substr(0, comment_pos);
            }
            if (Trim(line).empty())
            {
                continue;
            }

            const auto indent = line.find_first_not_of(' ');
            const int level = static_cast<int>((indent == std::string::npos ? 0 : indent) / 2);
            const auto content = Trim(line);
            const auto colon_pos = content.find(':');
            if (colon_pos == std::string::npos)
            {
                continue;
            }

            const auto key = Trim(content.substr(0, colon_pos));
            const auto raw_value = Trim(content.substr(colon_pos + 1));
            if (static_cast<int>(scopes.size()) <= level)
            {
                scopes.resize(level + 1);
            }
            scopes[level] = key;
            scopes.resize(level + 1);

            if (!raw_value.empty())
            {
                values[JoinPath(scopes)] = StripQuotes(raw_value);
            }
        }
        return values;
    }

    // 如果传入的是 `*.example.yaml`，则把它映射到对应的 runtime `*.yaml`。
    std::filesystem::path RuntimeConfigPath(const std::filesystem::path &config_path)
    {
        const auto filename = config_path.filename().string();
        if (!EndsWith(filename, kExampleYamlSuffix))
        {
            return config_path;
        }

        std::filesystem::path runtime_path = config_path;
        runtime_path.replace_filename(filename.substr(0, filename.size() - std::char_traits<char>::length(kExampleYamlSuffix)) + kYamlSuffix);
        return runtime_path;
    }

    // 确保 runtime 配置存在。
    // 如果 runtime 文件缺失，则尝试从同名 example 文件复制一份。
    std::filesystem::path EnsureRuntimeConfigFile(const std::filesystem::path &config_path)
    {
        const auto runtime_path = RuntimeConfigPath(config_path);
        if (std::filesystem::exists(runtime_path))
        {
            return runtime_path;
        }

        const auto example_path = ExampleConfigPath(runtime_path);
        if (!std::filesystem::exists(example_path))
        {
            throw std::runtime_error("Missing runtime config and example bootstrap source: " + runtime_path.string());
        }

        WriteTextFileAtomically(runtime_path, ReadTextFile(example_path));
        return runtime_path;
    }

    // 从键值表读取字符串；若不存在则返回默认值。
    std::string ValueOr(const std::unordered_map<std::string, std::string> &values,
                        const std::string &key,
                        const std::string &default_value)
    {
        const auto it = values.find(key);
        return it == values.end() ? default_value : it->second;
    }

    // 从键值表读取整数；若不存在则返回默认值。
    int IntValueOr(const std::unordered_map<std::string, std::string> &values,
                   const std::string &key,
                   int default_value)
    {
        const auto it = values.find(key);
        return it == values.end() ? default_value : std::stoi(it->second);
    }

    // 从键值表读取布尔值；若不存在则返回默认值。
    bool BoolValueOr(const std::unordered_map<std::string, std::string> &values,
                     const std::string &key,
                     bool default_value,
                     const std::string &error_context)
    {
        const auto it = values.find(key);
        return it == values.end() ? default_value : ParseBool(it->second, error_context);
    }

    // 以“临时文件写入成功后再替换正式文件”的方式安全写配置。
    // 这样即使中途失败，也尽量避免留下半写入的损坏文件。
    void WriteTextFileAtomically(const std::filesystem::path &path, const std::string &content)
    {
        const auto parent = path.parent_path();
        if (!parent.empty())
        {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
            if (ec)
            {
                throw std::runtime_error("Failed to prepare config directory: " + parent.string());
            }
        }

        const auto temp_name = path.filename().string() + ".tmp." +
                               std::to_string(static_cast<unsigned long long>(
                                   std::chrono::steady_clock::now().time_since_epoch().count()));
        const auto temp_path = parent.empty() ? std::filesystem::path(temp_name) : parent / temp_name;
        {
            std::ofstream ofs(temp_path, std::ios::binary | std::ios::trunc);
            if (!ofs)
            {
                throw std::runtime_error("Failed to open temp config file for write: " + temp_path.string());
            }
            ofs << content;
            ofs.flush();
            if (!ofs)
            {
                throw std::runtime_error("Failed to write config file: " + temp_path.string());
            }
        }

#ifdef _WIN32
        if (!MoveFileExW(temp_path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            std::error_code ec;
            std::filesystem::remove(temp_path, ec);
            throw std::runtime_error("Failed to replace config file: " + path.string());
        }
#else
        std::error_code ec;
        std::filesystem::rename(temp_path, path, ec);
        if (ec)
        {
            std::filesystem::remove(temp_path, ec);
            throw std::runtime_error("Failed to replace config file: " + path.string());
        }
#endif
    }
}
