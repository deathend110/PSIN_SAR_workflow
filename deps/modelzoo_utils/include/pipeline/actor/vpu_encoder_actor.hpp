#pragma once

#include "pipeline/io/output/istream_sink.hpp"
#include "pipeline/base/messages.hpp"
#include "pipeline/base/thread_safe_queue.hpp"
#include "pipeline/base/enums.hpp"
#include "pipeline/actor/base_actors.hpp"
#include "pipeline/memory/buffer_manager.hpp"
// VPU related codes
#include "pipeline/vpu/vpu.h"
#include "pipeline/vpu/video_utils.hpp"
#include "log_utils.hpp"

#include <opencv2/opencv.hpp>
#include <thread>
#include <vector>
#include <memory>
#include <string>
#include <atomic>
#include <functional> // 引入 functional 头文件
#include <string_view>

namespace fpai
{

    struct VPUEncoderConfig
    {
        std::string device = "/dev/video0";                               // VPU 设备节点
        int width = 1920;                                                 // 编码宽度
        int height = 1080;                                                // 编码高度
        int fps = 60;                                                     // 帧率
        int bitrate = 4000000;                                            // 比特率，单位bps
        int buffer_count = 6;                                             // buffer数量
        v4l2_buf_type input_buf_type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE; // 输入buffer类型，如果是webcam输入需要配置和webcam_decoder一致
        uint32_t input_pixel_format = V4L2_PIX_FMT_NV21;                  // VPU输入格式，PLin SDICamera推荐使用UYVY, Webcam推荐使用V4L2_PIX_FMT_NV12
        std::string codec = "h265";                                       // 编码格式，支持"h264"或"h265"
        v4l2_buf_type output_buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;      // 输出buffer类型
        uint32_t output_pixel_format = V4L2_PIX_FMT_HEVC;                 // 输出格式
        std::string rc_mode = "cbr";                                      // 码率控制模式，支持"cbr"或"vbr"
        std::string output_url = "output.hevc";                           // 输出文件或流的URI
    };

    inline void PrintEncoderConfig(const VPUEncoderConfig &config)
    {
        LOG_DEBUG("VPUEncoderConfig", ":\n"
                                      "  device: {}\n"
                                      "  width: {}\n"
                                      "  height: {}\n"
                                      "  fps: {}\n"
                                      "  bitrate: {}\n"
                                      "  buffer_count: {}\n"
                                      "  input_buf_type: {}\n"
                                      "  input_pixel_format: {:#x}\n"
                                      "  codec: {}\n"
                                      "  output_buf_type: {}\n"
                                      "  output_pixel_format: {:#x}\n"
                                      "  rc_mode: {}\n"
                                      "  output_url: {}",
                  config.device,
                  config.width,
                  config.height,
                  config.fps,
                  config.bitrate,
                  config.buffer_count,
                  config.input_buf_type,
                  config.input_pixel_format,
                  config.codec,
                  config.output_buf_type,
                  config.output_pixel_format,
                  config.rc_mode,
                  config.output_url);
    }

    template <typename DeviceType, typename InputMsg>
    class VPUEncoderActor : public BaseActor<InputMsg, void>
    {
    public:
        using PostProcessingFunc = std::function<void(const InputMsg &, cv::Mat &)>;

        static constexpr std::string_view LogP = "[VPUOut]";

    public:
        VPUEncoderActor(int sink_id,
                        std::unique_ptr<IStreamSink> sink,
                        DeviceType &device,
                        BufferManager &buffer_manager,
                        std::string chunk_group_id,
                        const VPUEncoderConfig &config = VPUEncoderConfig(),
                        PostProcessingFunc post_processor = nullptr)
            : BaseActor<InputMsg, void>(buffer_manager),
              device_(device),
              sink_id_(sink_id),
              sink_(std::move(sink)),
              chunk_group_id_(chunk_group_id),
              config_{config},
              output_url_{config.output_url},
              post_processor_{std::move(post_processor)}, // 存储后处理函数
              vpu_encoder_(nullptr),
              input_frame_data_(0)
        {
            PrintEncoderConfig(config_);
            vpu_encoder_ = std::make_unique<vpu::Encoder>(
                config_.device.c_str(),
                config_.height, config_.width,
                /*input buf type*/ config_.input_buf_type,
                /*input pixel formt*/ config_.input_pixel_format, // V4L2_PIX_FMT_UYVY recommended
                /*output buf type*/ config_.output_buf_type,
                /*output pixel formt*/ config_.output_pixel_format, // V4L2_PIX_FMT_HEVC,
                /* buffer number */ config_.buffer_count);
            vpu_encoder_->setFrameRate(config_.fps);
            cv::Mat zeros;
            // plincam的输入格式为UYVY，webcam解码后输入是NV12,  // UYVY每个像素2字节，NV21每个像素1.5字节
            zeros = V4L2format2cvmat(config_.input_pixel_format, config_.height, config_.width);
            if (zeros.type() == -1)
            {
                LOG_ERROR(LogP, "VPU encoder not support this pix fmt: {}", config_.input_pixel_format);
                throw std::runtime_error("VPU encoder not support this pix fmt");
            }
            auto frame_bytesize = zeros.total() * zeros.elemSize();
            input_frame_data_.resize(frame_bytesize);
            LOG_INFO(LogP, "VPU Encoder input format: {:#x}, size: w{}*h{}, elemSize: {}, total: {}, frame_bytesize: {}",
                     config_.input_pixel_format, zeros.cols, zeros.rows, zeros.elemSize(), zeros.total(), frame_bytesize);
            LOG_INFO(LogP, "VPU Encoder output format: {:#x}, codec={}", config_.output_pixel_format, config_.codec);
            int stream_sink_type = parse_stream_sink(output_url_);
            LOG_INFO(LogP, "Stream sink url={}, type: {}", config_.output_url, stream_sink_type);
        }
        // 虚析构函数是好习惯
        ~VPUEncoderActor() override
        {
            this->stop();
            if (sink_)
            {
                sink_->close();
            }
        };

        void start() override
        {
            this->stop_flag_.store(false);
            this->worker_ = std::thread(&VPUEncoderActor::loop, this);
            // added thread to handle vpu output
            stream_thread_ = std::thread(&VPUEncoderActor::stream_out_loop, this);
        }

        void stop() override
        {
            if (this->stop_flag_.exchange(true))
            {
                return; // Already stopping
            }
            // 向队列中推入一个空指针作为“停止”信号
            // 这将解除 input_queue_.wait_and_pop(msg) 的阻塞
            if (this->input_queue_)
            {
                this->input_queue_->stop();
            }
            LOG_INFO(LogP, "[Output] VPU encoder destroyed!");
            if (this->worker_.joinable())
            {
                this->worker_.join(); // 等待线程结束
            }
            if (stream_thread_.joinable())
            {
                stream_thread_.join(); // 等待线程结束
            }
            // ... VPU 资源释放代码 ...
            vpu_encoder_->streamOff();
        }

    protected:
        void loop() override
        {
            std::stringstream ss;
            ss << std::this_thread::get_id();
            uint64_t id = std::stoull(ss.str());
            LOG_INFO(LogP, "[VPU] Encoder thread start!, id={}", id);

            while (!this->stop_flag_)
            {
                auto start_ts = std::chrono::steady_clock::now();
                InputMsg input_msg;
                // auto s_chunkstatus = this->buffer_manager_.getStatusString(chunk_group_id_);
                // LOG_DEBUG(LogP,"[Output] before popping msg, buffer={}:{}.", chunk_group_id_, s_chunkstatus);
                if (!this->input_queue_->wait_and_pop(input_msg))
                {
                    LOG_ERROR(LogP, "Input queue is closed, VPU encode loop is stopping.");
                    break;
                }
                // 安全检查
                if (input_msg.meta.buffer_index < 0) // 收到空指针“哨兵”消息，立即退出循环
                {
                    LOG_WARN(LogP, "VPU input loop received stop signal, idx={}.", input_msg.meta.buffer_index);
                    return;
                }
                auto meta = input_msg.meta;
                LOG_DEBUG(LogP, "Encoder got msg from source_id={}, chunk_group_id={}, buffer_index={}",
                          meta.source_id, meta.chunk_group_id, meta.buffer_index);
                if (meta.source_id != sink_id_)
                {
                    LOG_WARN(LogP, "Mismatched source_id in message. Expected {}, got {}. Skipping.",
                             sink_id_, meta.source_id);
                    this->buffer_manager_.returnIndex(chunk_group_id_, meta.buffer_index);
                    continue;
                }
                // 1. 读取数据
                auto &udma_chunk = this->buffer_manager_.getChunk(chunk_group_id_, meta.buffer_index);
                auto t_wait_end = std::chrono::steady_clock::now();
                auto wait_duration = std::chrono::duration_cast<std::chrono::microseconds>(t_wait_end - start_ts);
                // memcpy(input_frame_data_.data(), udma_chunk->begin.cptr(), input_frame_data_.size());
                // this->buffer_manager_.returnIndex(chunk_group_id_, meta.buffer_index);
                auto t_copy_end = std::chrono::steady_clock::now();
                auto copy_duration = std::chrono::duration_cast<std::chrono::microseconds>(t_copy_end - t_wait_end);
                // 可选的后处理步骤
                if (meta.error_input)
                {
                    LOG_ERROR(LogP, "[{}][Buff{}] Skip post-processing due to error input frame.",
                              meta.source_id,
                              meta.buffer_index);
                }
                else if (post_processor_)
                {
                    // 将原始图像数据包装成 cv::Mat 以便绘图
                    // 注意：这里的格式需要与 readout_udma_memchunk 写入的格式匹配, 假设是 UYVY (YUV422)
                    cv::Mat frame_to_draw = V4L2format2cvmat(config_.input_pixel_format, config_.height, config_.width, udma_chunk->begin.cptr());
                    // cv::Mat frame_to_draw = V4L2format2cvmat(config_.input_pixel_format, config_.height, config_.width, input_frame_data_.data());

                    // 调用注入的后处理函数
                    post_processor_(input_msg, frame_to_draw);
                }
                auto t_post_end = std::chrono::steady_clock::now();
                auto post_duration = std::chrono::duration_cast<std::chrono::microseconds>(t_post_end - t_wait_end);
                // 2. 获取vpu可用的buffer输入需要压缩的帧
                // -----------使用下面被注释的代码会减少DDR带宽和VPU占用率，提升最大帧率，但是会带来帧率的波动，特别是多路的时候---------
                // if (meta.invalid_ps_frame)
                // {
                //     LOG_ERROR(LogP, "[{}][Buff{}] Skip VPU encode due to invalid ps frame.",
                //               meta.source_id,
                //               meta.buffer_index);
                //     this->buffer_manager_.returnIndex(chunk_group_id_, meta.buffer_index);
                //     continue;
                // }

                try
                {
                    vpu_encoder_->enqueueDataPtr(udma_chunk->begin.cptr());
                    // vpu_encoder_->enqueueDataPtr(input_frame_data_.data());
                }
                catch (const std::exception &e)
                {
                    LOG_ERROR(LogP, "VPU exception in output loop: {}. Stopping.", e.what());
                    this->stop();
                    return;
                }

                // 3. 归还 buffer index
                this->buffer_manager_.returnIndex(chunk_group_id_, meta.buffer_index);
                auto t_end = std::chrono::steady_clock::now();
                auto vpu_duration = std::chrono::duration_cast<std::chrono::microseconds>(t_end - t_post_end);
                auto loop_duration = std::chrono::duration_cast<std::chrono::microseconds>(t_end - start_ts);
                auto b_status = this->buffer_manager_.listAllStatus();
                LOG_INFO(LogP, "[{}] T({}ms)=wait({}ms)+copy({}ms)+post({}ms)+vpu_enqueue({}ms), buffer: {}",
                         sink_id_,
                         float(loop_duration.count() / 1000),
                         float(wait_duration.count() / 1000),
                         float(copy_duration.count() / 1000),
                         float(post_duration.count() / 1000),
                         float(vpu_duration.count() / 1000),
                         b_status);
            }
        }

    protected:
        int sink_id_;
        std::unique_ptr<IStreamSink> sink_;
        std::string chunk_group_id_;
        // VPU相关成员
        const VPUEncoderConfig config_;
        std::unique_ptr<vpu::Encoder> vpu_encoder_;
        const std::string output_url_;
        PostProcessingFunc post_processor_; // 存储后处理函数
        std::vector<char> input_frame_data_;
        DeviceType &device_;

    private:
        void stream_out_loop()
        {
            std::stringstream ss;
            ss << std::this_thread::get_id();
            uint64_t id = std::stoull(ss.str());
            LOG_INFO(LogP, "[Output] StreamOut thread start!, id={}", id);
            void *data_ptr = nullptr;
            int data_bytesize = 0;
            // 在循环开始时打开 sink
            if (!sink_ || 0 != sink_->open(output_url_))
            {
                throw std::runtime_error("VPUEncoderActor: Failed to open stream sink for URL: " + output_url_);
            }
            // wait until vpu_encoder actually stream on
            while (!vpu_encoder_->streamon_flag_.load())
            {
            }
            LOG_INFO(LogP, "[output] encoder stream on {}", vpu_encoder_->streamon_flag_.load());
            while (!this->stop_flag_)
            {
                if (!vpu_encoder_->getOutput(data_ptr, data_bytesize))
                    break;
                // printout_hex(static_cast<uint8_t*>(data_ptr), data_bytesize);

                // 3. 将数据包传递给Sink
                SinkPacket packet;
                packet.data = data_ptr;
                packet.size = data_bytesize;
                auto ret = sink_->handlePacket(packet);
                if (ret == -1)
                {
                    LOG_WARN(LogP, "[Output] Sink handlePacket return -1, stream maybe broken, stop streaming.");
                    vpu_encoder_->streamOff(); // 这个调用是唤醒阻塞线程的关键
                    break;
                }
                else if (ret != 0)
                {
                    LOG_ERROR(LogP, "[Output] Sink handlePacket error, ret={},", ret);
                    break;
                }
            }
        }

    private:
        std::thread stream_thread_; // 处理VPU输出的私有线程
    };
}
