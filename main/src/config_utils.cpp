#include "workflow/shared/config_utils.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace workflow::shared
{
    namespace
    {
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
    }

    std::string Trim(std::string value)
    {
        const auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
        value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
        return value;
    }

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

    std::string ToLower(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return value;
    }

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

    std::string ValueOr(const std::unordered_map<std::string, std::string> &values,
                        const std::string &key,
                        const std::string &default_value)
    {
        const auto it = values.find(key);
        return it == values.end() ? default_value : it->second;
    }

    int IntValueOr(const std::unordered_map<std::string, std::string> &values,
                   const std::string &key,
                   int default_value)
    {
        const auto it = values.find(key);
        return it == values.end() ? default_value : std::stoi(it->second);
    }

    bool BoolValueOr(const std::unordered_map<std::string, std::string> &values,
                     const std::string &key,
                     bool default_value,
                     const std::string &error_context)
    {
        const auto it = values.find(key);
        return it == values.end() ? default_value : ParseBool(it->second, error_context);
    }
}
