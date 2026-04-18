#pragma
#include "pipeline/base/enums.hpp"
#include "pipeline/io/base/output_sink.hpp"

#include "et_device.hpp"
#include "compile_fpai_target.hpp"

#include <spdlog/spdlog.h>

/**
 *   Hdmi显示抽象类
 *   用于wukong板
 *   输入的数据为 RGB565
 *   尺寸是1920*1080
 */
namespace fpai
{
    template <typename DeviceType>
    class RGB565HDMIDisplay : public IOutputSink
    {
    public:
        RGB565HDMIDisplay(int sink_id, DeviceType &device, int frame_width = 1920, int frame_height = 1080, camera_fmt fmt = camera_fmt::RGB565)
            : IOutputSink(sink_id, OUTPUT_SINK::HDMI, DATA_TYPE::IMAGE),
              device_(device), frame_height_(frame_height), frame_width_(frame_width), camera_fmt_(fmt)
        {
            // 在psddr-udmabuf上申请display缓存区
            if (fmt != camera_fmt::RGB565)
            {
                spdlog::error("RGB565HDMIDisplay only support RGB565 format");
                throw std::runtime_error("RGB565HDMIDisplay only support RGB565 format");
            }
            buffer_size_ = frame_width_ * frame_height_ * 2; // RGB565每个像素2字节

            display_chunk_ = device_.getMemRegion("udma").malloc(buffer_size_, false);
            spdlog::info("RGB565HDMIDisplay buffer udma addr: {}, size: {}", display_chunk_->begin.addr(), buffer_size_);
        }

        void show(int8_t *frame_data) const
        {
            display_chunk_.write(0, (char *)frame_data, buffer_size_);
            device_.defaultRegRegion().write(DISPLAY_READ_ADDR, display_chunk_->begin.addr() >> 3);
        }

        uint64_t getBufferSize() const
        {
            return buffer_size_;
        }

        camera_fmt getCameraFmt() const
        {
            return camera_fmt_;
        }

        int getFrameWidth() const
        {
            return frame_width_;
        }
        int getFrameHeight() const
        {
            return frame_height_;
        }
        int getFrameByteDepth() const
        {
            if (camera_fmt_ == camera_fmt::RGB565)
            {
                return 2;
            }
            else
            {
                spdlog::error("Unsupported camera format");
                throw std::invalid_argument("Unsupported camera format");
            }
        }

    private:
        DeviceType &device_;
        uint64_t buffer_size_ = 0;
        icraft::xrt::MemChunk display_chunk_;
        camera_fmt camera_fmt_ = camera_fmt::RGB565;
        int frame_width_ = 1920;
        int frame_height_ = 1080;
        const static auto DISPLAY_READ_ADDR = 0x40080054;
    };

    using GenericRGB565HDMIDisplay = RGB565HDMIDisplay<FPAIDevice>;
}
