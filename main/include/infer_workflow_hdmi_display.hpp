#pragma once

#include <icraft-xrt/core/session.h>

#include <cstdint>
#include <stdexcept>

#include <spdlog/spdlog.h>

namespace infer_workflow
{
    // HDMI adapter used by the inference-stage workflow only.
    // Keep this wrapper local so we do not pull camera/pipeline dependencies into infer_workflow.
    template <typename DeviceType>
    class RGB565HDMIDisplay
    {
    public:
        RGB565HDMIDisplay(int sink_id, DeviceType &device, int frame_width = 1920, int frame_height = 1080)
            : sink_id_(sink_id), device_(device), frame_width_(frame_width), frame_height_(frame_height)
        {
            if (frame_width_ <= 0 || frame_height_ <= 0)
            {
                throw std::invalid_argument("RGB565HDMIDisplay frame size must be positive");
            }

            buffer_size_ = static_cast<uint64_t>(frame_width_) * static_cast<uint64_t>(frame_height_) * 2;
            display_chunk_ = device_.getMemRegion("udma").malloc(buffer_size_, false);
            spdlog::info("RGB565HDMIDisplay buffer udma addr: {}, size: {}",
                         display_chunk_->begin.addr(),
                         buffer_size_);
        }

        void show(int8_t *frame_data) const
        {
            if (frame_data == nullptr)
            {
                throw std::invalid_argument("RGB565HDMIDisplay show received null frame_data");
            }
            display_chunk_.write(0, reinterpret_cast<char *>(frame_data), buffer_size_);
            device_.defaultRegRegion().write(DISPLAY_READ_ADDR, display_chunk_->begin.addr() >> 3);
        }

        int getSinkId() const
        {
            return sink_id_;
        }

        uint64_t getBufferSize() const
        {
            return buffer_size_;
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
            return 2;
        }

    private:
        int sink_id_ = 0;
        DeviceType &device_;
        uint64_t buffer_size_ = 0;
        icraft::xrt::MemChunk display_chunk_;
        int frame_width_ = 1920;
        int frame_height_ = 1080;
        static constexpr uint32_t DISPLAY_READ_ADDR = 0x40080054;
    };
}
