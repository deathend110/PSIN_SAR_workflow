/**
 * @brief An actor responsible for displaying raw image data on an HDMI output.
 *
 * This class acts as a final sink in a pipeline, taking image data from its
 * input queue and sending it to an HDMI display. It is designed for simple
 * display tasks without any AI post-processing. Its primary function is to
 * read the raw image data from a buffer (e.g., a udma_buf) into its internal
 * display buffer (`display_data_`) and then use the `RGB565HDMIDisplay`
 * object to render it on the screen.
 */
#pragma once
#include "bit_masks.hpp"

#include "pipeline/io/output/hdmi_display.hpp"
#include "pipeline/io/base/output_sink.hpp"
#include "pipeline/base/messages.hpp"
#include "pipeline/base/thread_safe_queue.hpp"
#include "pipeline/base/enums.hpp"
#include "pipeline/actor/base_actors.hpp"
#include "pipeline/memory/buffer_manager.hpp"

#include <opencv2/opencv.hpp>
#include <thread>
#include <vector>
#include <memory>
#include <atomic>
#include <variant>

namespace fpai
{
    template <typename DeviceType, typename InputMsgType>
    class HDMIDisplayActor : public BaseActor<InputMsgType, void>
    {
    public:
        static constexpr std::string_view LogP = "[HDMI]";
        constexpr static int OPT_DEBUG_FLAGS = BIT_MASK_0; // 调试标志位

        // 定义后处理函数类型别名
        using PostProcessingFunc = std::function<void(const InputMsgType &, cv::Mat &)>;

    public:
        HDMIDisplayActor(int id,
                         std::unique_ptr<RGB565HDMIDisplay<DeviceType>> display,
                         DeviceType &device,
                         BufferManager &buffer_manager,
                         std::string chunk_group_id,
                         PostProcessingFunc post_processor = nullptr)
            : BaseActor<InputMsgType, void>(buffer_manager),
              device_(device),
              sink_id_(id),
              display_(std::move(display)),
              display_data_(display_->getBufferSize()),
              chunk_group_id_(chunk_group_id),
              post_processor_{std::move(post_processor)} // 存储后处理函数
        {
        }
        // 虚析构函数是好习惯
        ~HDMIDisplayActor()
        {
            this->stop();
        };

    protected:
        void loop() override
        {
            while (!this->stop_flag_)
            {
                auto t_start = std::chrono::steady_clock::now();
                InputMsgType msg;
                this->input_queue_->wait_and_pop(msg);
                auto t_wait_pop_end = std::chrono::steady_clock::now();
                auto t_wait_pop_duration = std::chrono::duration_cast<std::chrono::microseconds>(t_wait_pop_end - t_start);

                // buffer_index 是基类成员，可以直接访问。
                if (msg.meta.buffer_index < 0)
                    continue; // 安全检查
                if (msg.meta.error_input)
                {
                    spdlog::warn("HDMIDisplayActor received an error frame, skipping display.");
                    // 归还 buffer index
                    this->buffer_manager_.returnIndex(chunk_group_id_, msg.meta.buffer_index);
                    continue;
                }
                // 1. 读取数据
                auto &chunk = this->buffer_manager_.getChunk(chunk_group_id_, msg.meta.buffer_index);
                readout_udma_memchunk(chunk);
                auto t_read_end = std::chrono::steady_clock::now();
                auto t_read_duration = std::chrono::duration_cast<std::chrono::microseconds>(t_read_end - t_wait_pop_end);
                // 2. 可选的后处理
                if (msg.meta.error_input)
                {
                    LOG_ERROR(LogP, "[{}][Buff{}] Skip post-processing due to error input frame.",
                              msg.meta.source_id,
                              msg.meta.buffer_index);
                }
                else if (post_processor_)
                {
                    // The format check is useful, but for RGB565, the type is CV_8UC2
                    if (display_->getCameraFmt() != camera_fmt::RGB565)
                    {
                        spdlog::error("HDMIDisplayActor only support RGB565 format for post processing");
                        throw std::invalid_argument("HDMIDisplayActor only support RGB565 format for post processing");
                    }
                    cv::Mat current_frame = cv::Mat(display_->getFrameHeight(), display_->getFrameWidth(), CV_8UC2, display_data_.data());
                    post_processor_(msg, current_frame);
                }
                auto t_post_end = std::chrono::steady_clock::now();
                auto t_post_duration = std::chrono::duration_cast<std::chrono::microseconds>(t_post_end - t_read_end);

                // 3. 直接显示
                display_->show(display_data_.data()); // 将内部向量的数据发送到 HDMI 显示器

                // 4. 归还 buffer index
                this->buffer_manager_.returnIndex(chunk_group_id_, msg.meta.buffer_index);
                auto t_end = std::chrono::steady_clock::now();
                auto t_duration = std::chrono::duration_cast<std::chrono::microseconds>(t_end - t_start);
                if (OPT_DEBUG_FLAGS & BIT_MASK_0)
                {
                    spdlog::info("[POST][{}] T({:.2f}ms)=pop({:.2f}ms)+read({:.2f}ms)+post({:.2f}ms); buff[{}]: {}",
                                 msg.meta.source_id,
                                 float(t_duration.count()) / 1000,
                                 float(t_wait_pop_duration.count()) / 1000,
                                 float(t_read_duration.count()) / 1000,
                                 float(t_post_duration.count()) / 1000,
                                 msg.meta.buffer_index,
                                 this->buffer_manager_.getStatusString(chunk_group_id_));
                }
            }
        }

        void readout_udma_memchunk(const icraft::xrt::MemChunk &memchunk)
        {
            memchunk.read(reinterpret_cast<char *>(display_data_.data()), 0, display_data_.size());
        }

        int sink_id_;
        std::unique_ptr<RGB565HDMIDisplay<DeviceType>> display_;
        std::vector<int8_t> display_data_;
        std::string chunk_group_id_;
        PostProcessingFunc post_processor_;
        DeviceType &device_;
    };
}
