#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <opencv2/core/mat.hpp>

namespace demo
{
    struct InputImage
    {
        std::filesystem::path path;
        cv::Mat image;
    };

    struct ImageLoadResult
    {
        std::filesystem::path path;
        cv::Mat image;
        std::string reason;
        bool valid = false;
    };

    std::vector<std::filesystem::path> CollectImages(const std::filesystem::path &input_dir);
    ImageLoadResult LoadOneImage(const std::filesystem::path &path);
}
