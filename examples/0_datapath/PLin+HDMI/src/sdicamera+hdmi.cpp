// pipeline includes
#include "pipeline/actor/base_actors.hpp"
#include "pipeline/actor/sdicamera_input_actor.hpp"
#include "pipeline/actor/hdmi_display_actor.hpp"
#include "pipeline/memory/buffer_manager.hpp"
#include "pipeline/io/input/sdicamera.hpp"
// icraft includes
#include <icraft-xrt/core/session.h>
#include "compile_fpai_target.hpp"
// 3rd-party dependency includes
#include <opencv2/opencv.hpp>
#include "yaml-cpp/yaml.h"
// system libs
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <condition_variable>
#include <atomic>
#include <thread>

using namespace icraft::xrt;
using namespace icraft::xir;
using namespace std::chrono;
using namespace std::chrono_literals;

using namespace fpai;

const std::string DEMO_NAME = "sdicamera+hdmi";
bool save = false; // 是否保存结果
bool show = true;  // 是否显示结果
const int BUFFER_COUNT = 4;

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        std::cerr << "Usage: " << argv[0] << " <config yaml>" << std::endl;
        return 1;
    }
    // Load system configuration from YAML file
    YAML::Node config;
    try
    {
        config = YAML::LoadFile(argv[1]);
    }
    catch (const YAML::BadFile &e)
    {
        spdlog::error("Failed to load config file {}: {}", argv[1], e.what());
        return -1;
    }
    // --- 设置 spdlog 日志级别 ---
    // 默认级别为 info
    auto log_level = spdlog::level::debug;
    spdlog::set_level(log_level);
    spdlog::info("Log level set to '{}'", spdlog::level::to_string_view(log_level));
    // Parse system configuration
    const auto &sys_config = config["sys"];
    const std::string device_fn = sys_config["icore"].as<std::string>();
    const int CAMERA_W = sys_config["camera"]["width"].as<int>();
    const int CAMERA_H = sys_config["camera"]["height"].as<int>();
    const int FRAME_W = sys_config["display"]["width"].as<int>();
    const int FRAME_H = sys_config["display"]["height"].as<int>();
    const bool VTC_ON = sys_config["camera"]["vtc"].as<bool>();

    // FPAI设备初始化
    auto device = Device::Open(device_fn);
    auto fpai_device = device.cast<FPAIDevice>();

    // 关闭MMU
#if defined(USE_BUYI_BACKEND)
    fpai_device.mmuModeSwitch(false);
#endif

    // 构建Pipeline
    ThreadSafeQueue<InputMessageForIcore> msgQ4Post(BUFFER_COUNT);

    // 构建buffer manager
    BufferManager buffer_manager(BUFFER_COUNT);

    // 配置摄像头
    int camera_id = 0;
    auto camera = std::make_unique<GenericSDICamera>(camera_id, fpai_device, CAMERA_W, CAMERA_H, camera_fmt::RGB565,
                                                     FRAME_W, FRAME_H, camera_fmt::RGB565, crop_position::center, // PS端的resize，RGB565用于HDMI输出
                                                     CAMERA_W, CAMERA_H, crop_position::center,
                                                     0x40080000, VTC_ON); // PL端输入必须经过hardResizePL，此处不做任何尺度变换

    SDICameraInputActor<FPAIDevice, FPAIBackend> camera_actor(camera->getSourceId(),
                                                              std::move(camera),
                                                              fpai_device,
                                                              buffer_manager);
    camera_actor.bindOutputQueue(&msgQ4Post);
    // 配置HDMI输出
    int hdmi_id = 0;
    auto hdmi_display = std::make_unique<RGB565HDMIDisplay<FPAIDevice>>(hdmi_id, fpai_device, FRAME_W, FRAME_H);
    HDMIDisplayActor<FPAIDevice, InputMessageForIcore> display_actor(hdmi_display->getSinkId(),
                                                                     std::move(hdmi_display),
                                                                     fpai_device,
                                                                     buffer_manager,
                                                                     camera_actor.getChunkGroupId());
    display_actor.bindInputQueue(&msgQ4Post);

    camera_actor.start();
    display_actor.start();
    // should add a blocking loop here
    std::cin.get();
    camera_actor.stop();
    display_actor.stop(); // 关闭设备
    spdlog::info("All actors stopped...");
    Device::Close(device);
    return 0;
}