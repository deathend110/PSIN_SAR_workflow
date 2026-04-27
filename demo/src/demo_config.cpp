#include "demo_config.hpp"

#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace demo
{
    namespace
    {
        std::string RemoveUtf8Bom(std::string value)
        {
            if (value.size() >= 3 &&
                static_cast<unsigned char>(value[0]) == 0xEF &&
                static_cast<unsigned char>(value[1]) == 0xBB &&
                static_cast<unsigned char>(value[2]) == 0xBF)
            {
                value.erase(0, 3);
            }
            return value;
        }

        std::string Trim(const std::string &value)
        {
            const auto begin = value.find_first_not_of(" \t\r\n");
            if (begin == std::string::npos)
            {
                return {};
            }
            const auto end = value.find_last_not_of(" \t\r\n");
            return value.substr(begin, end - begin + 1);
        }

        std::string StripQuotes(std::string value)
        {
            value = Trim(std::move(value));
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

        std::unordered_map<std::string, std::string> LoadSimpleYaml(const std::filesystem::path &path)
        {
            std::ifstream input(path);
            if (!input)
            {
                throw std::runtime_error("failed to open config: " + path.string());
            }

            std::unordered_map<std::string, std::string> values;
            std::string line;
            std::string scope;
            while (std::getline(input, line))
            {
                line = RemoveUtf8Bom(std::move(line));
                const auto hash = line.find('#');
                if (hash != std::string::npos)
                {
                    line = line.substr(0, hash);
                }
                const std::string trimmed = Trim(line);
                if (trimmed.empty())
                {
                    continue;
                }
                if (trimmed.back() == ':')
                {
                    scope = Trim(trimmed.substr(0, trimmed.size() - 1));
                    continue;
                }

                const auto colon = trimmed.find(':');
                if (colon == std::string::npos)
                {
                    continue;
                }

                const std::string key = Trim(trimmed.substr(0, colon));
                const std::string value = StripQuotes(trimmed.substr(colon + 1));
                values[scope.empty() ? key : scope + "." + key] = value;
            }
            return values;
        }

        std::filesystem::path RequirePath(const std::unordered_map<std::string, std::string> &values,
                                          const std::string &key)
        {
            const auto it = values.find(key);
            if (it == values.end() || Trim(it->second).empty())
            {
                throw std::runtime_error("missing required config field: " + key);
            }
            return it->second;
        }
    }

    DemoConfig LoadDemoConfig(const std::filesystem::path &config_path)
    {
        const auto values = LoadSimpleYaml(config_path);

        DemoConfig cfg;
        cfg.input_dir = RequirePath(values, "demo.input_dir");
        cfg.output_dir = RequirePath(values, "demo.output_dir");
        cfg.json_path = RequirePath(values, "demo.json_path");
        cfg.raw_path = RequirePath(values, "demo.raw_path");
        return cfg;
    }
}
