#pragma once

#include <filesystem>
#include <string_view>

#include <opencv2/core/mat.hpp>

namespace demo
{
    void WriteOutputs(const std::filesystem::path &output_dir,
                      std::string_view stem,
                      const cv::Mat &restore,
                      const cv::Mat &mask_class);
}
