#include "image_collector.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

#include <opencv2/imgcodecs.hpp>

namespace demo
{
    namespace
    {
        constexpr int kExpectedSize = 512;

        std::string ToLower(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch)
                           { return static_cast<char>(std::tolower(ch)); });
            return value;
        }

        bool IsSupportedImageExtension(const std::filesystem::path &path)
        {
            const std::string ext = ToLower(path.extension().string());
            return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp";
        }
    }

    std::vector<std::filesystem::path> CollectImages(const std::filesystem::path &input_dir)
    {
        if (!std::filesystem::exists(input_dir))
        {
            throw std::runtime_error("input_dir does not exist: " + input_dir.string());
        }
        if (!std::filesystem::is_directory(input_dir))
        {
            throw std::runtime_error("input_dir is not a directory: " + input_dir.string());
        }

        std::vector<std::filesystem::path> paths;
        for (const auto &entry : std::filesystem::directory_iterator(input_dir))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }
            if (!IsSupportedImageExtension(entry.path()))
            {
                continue;
            }
            paths.push_back(entry.path());
        }

        std::sort(paths.begin(), paths.end());
        return paths;
    }

    ImageLoadResult LoadOneImage(const std::filesystem::path &path)
    {
        ImageLoadResult result;
        result.path = path;

        cv::Mat image = cv::imread(path.string(), cv::IMREAD_GRAYSCALE);
        if (image.empty())
        {
            result.reason = "image unreadable";
            return result;
        }
        if (image.rows != kExpectedSize || image.cols != kExpectedSize)
        {
            result.reason = "image size must be exactly 512x512";
            return result;
        }
        if (image.type() != CV_8UC1)
        {
            result.reason = "image must decode as single-channel grayscale";
            return result;
        }

        result.image = std::move(image);
        result.valid = true;
        return result;
    }
}
