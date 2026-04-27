#include "result_writer.hpp"

#include <stdexcept>
#include <string>

#include <opencv2/imgcodecs.hpp>

namespace demo
{
    namespace
    {
        void ValidateOutputImage(const cv::Mat &image, const char *label)
        {
            if (image.empty())
            {
                throw std::runtime_error(std::string(label) + " is empty");
            }
            if (image.type() != CV_8UC1)
            {
                throw std::runtime_error(std::string(label) + " must be CV_8UC1");
            }
            if (image.rows != 512 || image.cols != 512)
            {
                throw std::runtime_error(std::string(label) + " must be 512x512");
            }
        }
    }

    void WriteOutputs(const std::filesystem::path &output_dir,
                      std::string_view stem,
                      const cv::Mat &restore,
                      const cv::Mat &mask_class)
    {
        ValidateOutputImage(restore, "restore");
        ValidateOutputImage(mask_class, "mask_class");

        const std::filesystem::path sample_dir = output_dir / std::filesystem::path(stem);
        std::filesystem::create_directories(sample_dir);

        const auto restore_path = sample_dir / "restore.png";
        const auto mask_path = sample_dir / "mask_class.png";
        if (!cv::imwrite(restore_path.string(), restore))
        {
            throw std::runtime_error("failed to write restore.png: " + restore_path.string());
        }
        if (!cv::imwrite(mask_path.string(), mask_class))
        {
            throw std::runtime_error("failed to write mask_class.png: " + mask_path.string());
        }
    }
}
