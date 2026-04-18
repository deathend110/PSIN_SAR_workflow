#pragma once

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace hdmi_show
{
    // 这是一个非常轻量的配置解析器，只覆盖当前示例需要的 YAML 子集：
    // - 通过缩进表示层级
    // - 通过 key: value 表示叶子节点
    // 解析后统一拍平成 "sys.input.gray_dir" 这种点分路径，方便直接读取。
    class SimpleYamlConfig
    {
    public:
        explicit SimpleYamlConfig(std::map<std::string, std::string> values)
            : values_(std::move(values))
        {
        }

        static SimpleYamlConfig LoadFile(const std::string &path)
        {
            std::FILE *input = std::fopen(path.c_str(), "rb");
            if (input == nullptr)
            {
                throw std::runtime_error("Failed to open config file: " + path);
            }

            std::map<std::string, std::string> values;
            std::vector<std::string> section_stack;
            char raw_buffer[4096];

            while (std::fgets(raw_buffer, sizeof(raw_buffer), input) != nullptr)
            {
                // 原始行先去掉换行，再去掉注释和首尾空白。
                std::string raw_line(raw_buffer);
                while (!raw_line.empty() && (raw_line.back() == '\n' || raw_line.back() == '\r'))
                {
                    raw_line.pop_back();
                }
                auto sanitized = trim(stripComment(raw_line));
                if (sanitized.empty())
                {
                    continue;
                }

                const auto indent = raw_line.find_first_not_of(' ');
                const size_t level = (indent == std::string::npos ? 0 : indent / 2);
                const auto colon_pos = sanitized.find(':');
                if (colon_pos == std::string::npos)
                {
                    continue;
                }

                const std::string key = trim(sanitized.substr(0, colon_pos));
                const std::string value = trim(sanitized.substr(colon_pos + 1));

                if (section_stack.size() > level)
                {
                    // 当前缩进层级变浅时，说明从子节点退回到了上层 section。
                    section_stack.resize(level);
                }

                if (value.empty())
                {
                    // 例如 "sys:" 这种只有键没有值的行，表示进入一个新的 section。
                    if (section_stack.size() == level)
                    {
                        section_stack.push_back(key);
                    }
                    else
                    {
                        section_stack[level] = key;
                    }
                    continue;
                }

                std::string dotted_key;
                for (const auto &section : section_stack)
                {
                    if (!dotted_key.empty())
                    {
                        dotted_key += ".";
                    }
                    dotted_key += section;
                }
                if (!dotted_key.empty())
                {
                    dotted_key += ".";
                }
                dotted_key += key;
                // 例如 sys -> input -> gray_dir 会被拍平成 sys.input.gray_dir。
                values[dotted_key] = unquote(value);
            }

            std::fclose(input);

            return SimpleYamlConfig(std::move(values));
        }

        // 必填字符串；不存在时直接抛错。
        std::string getString(const std::string &key) const
        {
            auto it = values_.find(key);
            if (it == values_.end())
            {
                throw std::runtime_error("Missing config key: " + key);
            }
            return it->second;
        }

        // 可选字符串；不存在时返回默认值。
        std::string getStringOr(const std::string &key, const std::string &default_value) const
        {
            auto it = values_.find(key);
            if (it == values_.end())
            {
                return default_value;
            }
            return it->second;
        }

        // 必填整数。
        int getInt(const std::string &key) const
        {
            return std::stoi(getString(key));
        }

        // 可选整数。
        int getIntOr(const std::string &key, int default_value) const
        {
            auto it = values_.find(key);
            if (it == values_.end())
            {
                return default_value;
            }
            return std::stoi(it->second);
        }

        // 可选布尔值，只接受 true / false（大小写不敏感）。
        bool getBoolOr(const std::string &key, bool default_value) const
        {
            auto it = values_.find(key);
            if (it == values_.end())
            {
                return default_value;
            }
            std::string value = it->second;
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch)
                           { return static_cast<char>(std::tolower(ch)); });
            if (value == "true")
            {
                return true;
            }
            if (value == "false")
            {
                return false;
            }
            throw std::runtime_error("Invalid boolean config value for key: " + key);
        }

    private:
        // 去掉首尾空白字符。
        static std::string trim(const std::string &value)
        {
            const auto begin = std::find_if_not(value.begin(), value.end(), [](unsigned char ch)
                                                { return std::isspace(ch) != 0; });
            const auto end = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch)
                                              { return std::isspace(ch) != 0; }).base();
            if (begin >= end)
            {
                return "";
            }
            return std::string(begin, end);
        }

        // 去掉行内注释，形如 "key: value # comment"。
        static std::string stripComment(const std::string &value)
        {
            const auto hash_pos = value.find('#');
            if (hash_pos == std::string::npos)
            {
                return value;
            }
            return value.substr(0, hash_pos);
        }

        // 去掉包裹值的单引号或双引号。
        static std::string unquote(const std::string &value)
        {
            if (value.size() >= 2)
            {
                const char first = value.front();
                const char last = value.back();
                if ((first == '"' && last == '"') || (first == '\'' && last == '\''))
                {
                    return value.substr(1, value.size() - 2);
                }
            }
            return value;
        }

        std::map<std::string, std::string> values_;
    };
}
