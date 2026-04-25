#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>

namespace workflow::shared
{
    std::string Trim(std::string value);
    std::string StripQuotes(std::string value);
    std::string ToLower(std::string value);
    bool ParseBool(const std::string &value, const std::string &error_context = {});

    std::unordered_map<std::string, std::string> LoadSimpleYaml(const std::filesystem::path &config_path);

    std::string ValueOr(const std::unordered_map<std::string, std::string> &values,
                        const std::string &key,
                        const std::string &default_value);

    int IntValueOr(const std::unordered_map<std::string, std::string> &values,
                   const std::string &key,
                   int default_value);

    bool BoolValueOr(const std::unordered_map<std::string, std::string> &values,
                     const std::string &key,
                     bool default_value,
                     const std::string &error_context = {});

    void WriteTextFileAtomically(const std::filesystem::path &path, const std::string &content);
}
