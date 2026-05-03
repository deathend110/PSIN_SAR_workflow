#pragma once

#include <icraft-xrt/core/session.h>

#include <cstdint>
#include <stdexcept>

#include <spdlog/spdlog.h>

namespace infer_workflow
{
    // 仅供 infer 工作流使用的 HDMI 适配器。
    // 这个模板把“把一帧 RGB565 数据写到指定寄存器和 UDMA 缓冲”的细节局部封装起来，
    // 这样上层推理流程只需要关心“给我一帧数据并显示”。
    template <typename DeviceType>
    class RGB565HDMIDisplay
    {
    public:
        // 绑定设备并申请一块足够容纳整帧 RGB565 数据的 UDMA 缓冲区。
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

        // 把一整帧 RGB565 数据写入显示缓冲，并通过寄存器通知硬件刷新。
        void show(int8_t *frame_data) const
        {
            if (frame_data == nullptr)
            {
                throw std::invalid_argument("RGB565HDMIDisplay show received null frame_data");
            }
            display_chunk_.write(0, reinterpret_cast<char *>(frame_data), buffer_size_);
            device_.defaultRegRegion().write(DISPLAY_READ_ADDR, display_chunk_->begin.addr() >> 3);
        }

        // 返回当前逻辑 sink 编号。
        int getSinkId() const
        {
            return sink_id_;
        }

        // 返回底层缓冲区总字节数。
        uint64_t getBufferSize() const
        {
            return buffer_size_;
        }

        // 返回当前显示帧宽。
        int getFrameWidth() const
        {
            return frame_width_;
        }

        // 返回当前显示帧高。
        int getFrameHeight() const
        {
            return frame_height_;
        }

        // RGB565 固定为 2 字节每像素。
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
