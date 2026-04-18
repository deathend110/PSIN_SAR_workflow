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
                    section_stack.resize(level);
                }

                if (value.empty())
                {
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
                values[dotted_key] = unquote(value);
            }

            std::fclose(input);

            return SimpleYamlConfig(std::move(values));
        }

        std::string getString(const std::string &key) const
        {
            auto it = values_.find(key);
            if (it == values_.end())
            {
                throw std::runtime_error("Missing config key: " + key);
            }
            return it->second;
        }

        std::string getStringOr(const std::string &key, const std::string &default_value) const
        {
            auto it = values_.find(key);
            if (it == values_.end())
            {
                return default_value;
            }
            return it->second;
        }

        int getInt(const std::string &key) const
        {
            return std::stoi(getString(key));
        }

        int getIntOr(const std::string &key, int default_value) const
        {
            auto it = values_.find(key);
            if (it == values_.end())
            {
                return default_value;
            }
            return std::stoi(it->second);
        }

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

        static std::string stripComment(const std::string &value)
        {
            const auto hash_pos = value.find('#');
            if (hash_pos == std::string::npos)
            {
                return value;
            }
            return value.substr(0, hash_pos);
        }

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
