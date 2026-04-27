#include "result_writer.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include <opencv2/imgcodecs.hpp>

namespace
{
    void Fail(const std::string &message)
    {
        std::cerr << "result_writer_test failed: " << message << '\n';
        std::exit(1);
    }

    void Expect(bool ok, const std::string &message)
    {
        if (!ok)
        {
            Fail(message);
        }
    }

    void TestWritesBothOutputs()
    {
        const auto root = std::filesystem::temp_directory_path() / "result_writer_test";
        std::filesystem::remove_all(root);

        cv::Mat restore(512, 512, CV_8UC1, cv::Scalar(120));
        cv::Mat mask(512, 512, CV_8UC1, cv::Scalar(3));
        demo::WriteOutputs(root, "sample", restore, mask);

        const auto restore_path = root / "sample" / "restore.png";
        const auto mask_path = root / "sample" / "mask_class.png";
        Expect(std::filesystem::exists(restore_path), "restore.png missing");
        Expect(std::filesystem::exists(mask_path), "mask_class.png missing");

        const cv::Mat restore_loaded = cv::imread(restore_path.string(), cv::IMREAD_UNCHANGED);
        const cv::Mat mask_loaded = cv::imread(mask_path.string(), cv::IMREAD_UNCHANGED);
        Expect(restore_loaded.type() == CV_8UC1, "restore must be single-channel u8");
        Expect(mask_loaded.type() == CV_8UC1, "mask must be single-channel u8");
        Expect(mask_loaded.at<unsigned char>(0, 0) == 3, "mask class value mismatch");
    }
}

int main()
{
    TestWritesBothOutputs();
    std::cout << "result_writer_test passed\n";
    return 0;
}
