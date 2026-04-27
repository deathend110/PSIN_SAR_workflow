#include "image_collector.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <opencv2/imgcodecs.hpp>

namespace
{
    void Fail(const std::string &message)
    {
        std::cerr << "image_collector_test failed: " << message << '\n';
        std::exit(1);
    }

    void Expect(bool ok, const std::string &message)
    {
        if (!ok)
        {
            Fail(message);
        }
    }

    std::filesystem::path MakeRoot()
    {
        const auto root = std::filesystem::temp_directory_path() / "image_collector_test";
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        return root;
    }

    void WriteImage(const std::filesystem::path &path, int width, int height)
    {
        cv::Mat image(height, width, CV_8UC1, cv::Scalar(7));
        if (!cv::imwrite(path.string(), image))
        {
            Fail("failed to write temp image");
        }
    }

    void TestCollectsSupportedImages()
    {
        const auto root = MakeRoot();
        WriteImage(root / "a.png", 512, 512);
        WriteImage(root / "b.jpg", 512, 512);
        WriteImage(root / "bad.png", 300, 512);
        std::ofstream(root / "note.txt") << "ignore";

        const auto items = demo::CollectImages(root);
        Expect(items.size() == 3, "should collect supported image extensions only");
        Expect(items.front().filename() == "a.png", "paths should be lexicographically sorted");
    }

    void TestLoadsSingleValidImage()
    {
        const auto root = MakeRoot();
        const auto path = root / "ok.png";
        WriteImage(path, 512, 512);

        const auto result = demo::LoadOneImage(path);
        Expect(result.valid, "valid image should load successfully");
        Expect(result.path == path, "loaded path mismatch");
        Expect(result.image.rows == 512 && result.image.cols == 512, "valid image must stay 512x512");
    }

    void TestSkipsInvalidSizeImage()
    {
        const auto root = MakeRoot();
        const auto path = root / "bad.png";
        WriteImage(path, 511, 512);

        const auto result = demo::LoadOneImage(path);
        Expect(!result.valid, "invalid-size image should be skipped");
        Expect(result.reason.find("512x512") != std::string::npos,
               "skip reason should mention 512x512");
    }
}

int main()
{
    TestCollectsSupportedImages();
    TestLoadsSingleValidImage();
    TestSkipsInvalidSizeImage();
    std::cout << "image_collector_test passed\n";
    return 0;
}
