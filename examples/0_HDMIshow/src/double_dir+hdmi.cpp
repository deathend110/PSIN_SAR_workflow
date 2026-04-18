#include "unistd.h"
#include <shared_mutex>
#include "log_utils.hpp"
// pipeline includes
#include "pipeline/actor/hdmi_display_actor.hpp"
#include "pipeline/memory/buffer_manager.hpp"
// local includes
#include "double_directory_input_actor.hpp"
#include "simple_yaml_config.hpp"
// icraft includes
#include <icraft-xrt/core/session.h>
#include "compile_fpai_target.hpp"
// 3rd-party dependency includes
#include <opencv2/opencv.hpp>
#include <spdlog/spdlog.h>
// system libs
#include <cstdio>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

using namespace icraft::xrt;
using namespace std::chrono_literals;
using namespace fpai;

namespace
{
    // HDMI 显示链路这里采用 4 个循环 buffer，和示例整体的轻量需求相匹配。
    constexpr int BUFFER_COUNT = 4;
    constexpr const char *DEFAULT_PREVIEW_WINDOW = "double_dir+hdmi_preview";

    std::string toLower(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch)
                       { return static_cast<char>(std::tolower(ch)); });
        return value;
    }

    // dump 模式下把合成好的 BGR 帧落盘，方便在 PC 或板端排查布局是否正确。
    void dumpFrame(const cv::Mat &frame_bgr, const std::string &dump_dir, size_t pair_index)
    {
        std::filesystem::create_directories(dump_dir);
        const std::string filename = dump_dir + "/frame_" + cv::format("%06zu", pair_index) + ".png";
        cv::imwrite(filename, frame_bgr);
    }

    // 桌面路径：
    // 不访问板端 HDMI，只做 preview / dump，便于在 Windows 上先验证读图和拼帧逻辑。
    int runPreviewOrDump(const hdmi_show::SimpleYamlConfig &config)
    {
        const std::string gray_dir = config.getString("sys.input.gray_dir");
        const std::string rgb_dir = config.getString("sys.input.rgb_dir");
        const int input_fps = config.getIntOr("sys.input.fps", 0);
        const bool loop = config.getBoolOr("sys.input.loop", false);
        const int frame_w = config.getInt("sys.display.width");
        const int frame_h = config.getInt("sys.display.height");
        const std::string output_mode = toLower(config.getStringOr("sys.output.mode", "preview"));
        const std::string dump_dir = config.getStringOr("sys.output.dump_dir", "./io/output");
        const int preview_wait_ms = config.getIntOr("sys.output.preview_wait_ms", input_fps > 0 ? std::max(1, 1000 / input_fps) : 1);
        const std::string window_name = config.getStringOr("sys.output.window_name", DEFAULT_PREVIEW_WINDOW);

        const bool enable_preview = (output_mode == "preview" || output_mode == "preview_dump");
        const bool enable_dump = (output_mode == "dump" || output_mode == "preview_dump");
        if (!enable_preview && !enable_dump)
        {
            spdlog::error("Unsupported output mode '{}' for desktop path. Use preview, dump, preview_dump, or hdmi.", output_mode);
            return -1;
        }

        // Linux 板端默认不编入 highgui 预览接口，避免链接到 namedWindow/imshow 等桌面依赖。
#if !defined(_WIN32)
        if (enable_preview)
        {
            spdlog::error("Preview mode is only compiled on Windows. On Linux use sys.output.mode=hdmi or dump.");
            return -1;
        }
#endif

        DoubleDirectoryFrameSource frame_source(gray_dir, rgb_dir, frame_w, frame_h, loop);

#if defined(_WIN32)
        if (enable_preview)
        {
            cv::namedWindow(window_name, cv::WINDOW_NORMAL);
        }
#endif

        size_t pair_index = 0;
        cv::Mat composed_bgr;
        while (frame_source.nextFrame(composed_bgr, pair_index))
        {
            if (enable_dump)
            {
                dumpFrame(composed_bgr, dump_dir, pair_index);
            }

            if (enable_preview)
            {
#if defined(_WIN32)
                // preview 只是调试辅助能力，不影响 HDMI 主流程。
                cv::imshow(window_name, composed_bgr);
                const int key = cv::waitKey(preview_wait_ms);
                if (key == 27 || key == 'q' || key == 'Q')
                {
                    break;
                }
#endif
            }
        }

#if defined(_WIN32)
        if (enable_preview)
        {
            cv::destroyWindow(window_name);
        }
#endif

        return 0;
    }

    // 板端 HDMI 路径：
    // 双目录输入 actor 负责产生 RGB565 帧，
    // HDMI actor 负责从 buffer 中取出这些帧并送到显示硬件。
    int runHdmi(const hdmi_show::SimpleYamlConfig &config)
    {
        const std::string device_fn = config.getString("sys.icore");
        const std::string gray_dir = config.getString("sys.input.gray_dir");
        const std::string rgb_dir = config.getString("sys.input.rgb_dir");
        const int input_fps = config.getIntOr("sys.input.fps", 0);
        const bool loop = config.getBoolOr("sys.input.loop", false);
        const int frame_w = config.getInt("sys.display.width");
        const int frame_h = config.getInt("sys.display.height");

        auto device = Device::Open(device_fn.c_str());
        auto fpai_device = device.cast<FPAIDevice>();

#if defined(USE_BUYI_BACKEND)
        // BY 后端下沿用现有示例的 MMU 设置。
        fpai_device.mmuModeSwitch(false);
#endif

        BufferManager buffer_manager(BUFFER_COUNT);
        ThreadSafeQueue<InputMessageForIcore> display_queue(BUFFER_COUNT);

        int source_id = 0;
        // 输入 actor：磁盘双目录 -> 合成整帧 -> 写入 RGB565 buffer。
        auto input_actor = std::make_unique<DoubleDirectoryInputActor<FPAIDevice>>(source_id,
                                                                                   fpai_device,
                                                                                   buffer_manager,
                                                                                   gray_dir,
                                                                                   rgb_dir,
                                                                                   frame_w,
                                                                                   frame_h,
                                                                                   input_fps,
                                                                                   loop);
        input_actor->bindOutputQueue(&display_queue);

        int hdmi_id = 0;
        // 显示 actor：消费 buffer_index，并把对应帧真正输出到 HDMI。
        auto hdmi_display = std::make_unique<RGB565HDMIDisplay<FPAIDevice>>(hdmi_id, fpai_device, frame_w, frame_h);
        HDMIDisplayActor<FPAIDevice, InputMessageForIcore> display_actor(hdmi_display->getSinkId(),
                                                                         std::move(hdmi_display),
                                                                         fpai_device,
                                                                         buffer_manager,
                                                                         input_actor->getChunkGroupId());
        display_actor.bindInputQueue(&display_queue);

        input_actor->start();
        display_actor.start();
        spdlog::info("double_dir+hdmi started. gray_dir='{}', rgb_dir='{}', loop={}", gray_dir, rgb_dir, loop);

        if (loop)
        {
            // 循环模式下没有自然结束条件，因此通过回车手动停止。
            spdlog::info("HDMI loop mode enabled. Press Enter to stop.");
            std::getchar();
            input_actor->stop();
        }
        else
        {
            // 非循环模式下，输入播完就退出。
            while (!input_actor->isFinished())
            {
                std::this_thread::sleep_for(50ms);
            }
            input_actor->stop();
        }

        display_actor.stop();
        spdlog::info("All actors stopped...");
        Device::Close(device);
        return 0;
    }
}

int main(int argc, char *argv[])
{
    // 命令行只要求一个参数：配置文件路径。
    if (argc < 2)
    {
        std::fprintf(stderr, "Usage: %s <config yaml>\n", argv[0]);
        return 1;
    }

    hdmi_show::SimpleYamlConfig config({});
    try
    {
        config = hdmi_show::SimpleYamlConfig::LoadFile(argv[1]);
    }
    catch (const std::exception &e)
    {
        spdlog::error("Failed to load config file {}: {}", argv[1], e.what());
        return -1;
    }

    auto log_level = spdlog::level::debug;
    spdlog::set_level(log_level);
    spdlog::info("Log level set to '{}'", spdlog::level::to_string_view(log_level));

    // 通过 sys.output.mode 选择运行路径：
    // - hdmi: 板端真实 HDMI 输出
    // - 其他: preview / dump / preview_dump
    const std::string output_mode = toLower(config.getStringOr("sys.output.mode", "hdmi"));
    if (output_mode == "hdmi")
    {
        return runHdmi(config);
    }
    return runPreviewOrDump(config);
}
