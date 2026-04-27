#pragma once

#include <filesystem>
#include <memory>

#include <opencv2/core/mat.hpp>

namespace demo
{
    struct InferenceResult
    {
        cv::Mat restore;
        cv::Mat mask_class;
    };

    class ModelRunner
    {
    public:
        ModelRunner(const std::filesystem::path &json_path, const std::filesystem::path &raw_path);
        ~ModelRunner();

        ModelRunner(ModelRunner &&other) noexcept;
        ModelRunner &operator=(ModelRunner &&other) noexcept;

        ModelRunner(const ModelRunner &) = delete;
        ModelRunner &operator=(const ModelRunner &) = delete;

        InferenceResult Run(const cv::Mat &input_gray_512);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
