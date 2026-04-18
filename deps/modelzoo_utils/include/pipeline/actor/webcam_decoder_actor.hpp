#pragma once

#include "pipeline/io/input/webcam.hpp"
#include "pipeline/base/messages.hpp"
#include "pipeline/base/thread_safe_queue.hpp"
#include "pipeline/base/enums.hpp"
#include "pipeline/actor/base_actors.hpp"
#include "pipeline/memory/buffer_manager.hpp"
#include "pipeline/io/output/istream_sink.hpp"
// VPU related codes
#include "pipeline/vpu/vpu.h"
#include "pipeline/vpu/video_utils.hpp"
#include "log_utils.hpp"
#include "bit_masks.hpp"

#include <opencv2/opencv.hpp>
#include <thread>
#include <vector>
#include <memory>
#include <string>
#include <atomic>
#include <filesystem>
#include <string_view>

namespace fpai
{
    struct VPUDecoderConfig
    {
        std::string device = "/dev/video0";                                                             // VPU 设备节点
        int width = 1920;                                                                               // 解码宽度
        int height = 1080;                                                                              // 解码高度
        double fps = 25;                                                                                // 帧率
        int buffer_count = 6;                                                                           // buffer数量，大于等于6
        v4l2_buf_type input_buf_type = V4L2_BUF_TYPE_VIDEO_OUTPUT;                                      // 输入buffer类型
        uint32_t input_pixel_format = V4L2_PIX_FMT_HEVC;                                                // VPU输入格式，推荐使用yuv422
        std::string codec = "hevc";                                                                     // 编码格式，支持"h264"或"hevc"
        v4l2_buf_type output_buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;                             // 输出buffer类型
        uint32_t output_pixel_format = V4L2_PIX_FMT_NV21;                                               // 输出格式
        std::string rc_mode = "cbr";                                                                    // 码率控制模式，支持"cbr"或"vbr"
        std::string source_url = "rtsp://admin:fmsh123456!@192.168.125.99:554/h264/ch1/main/av_stream"; // 输出文件或流的URI
        int net_w = 640;                                                                                // 网络输入宽度，如果使用yuv2rgb功能
        int net_h = 640;                                                                                // 网络输入高度，如果使用yuv2rgb功能
        bool fpga_yuv2rgb = false;                                                                      // 是否使用FPGA进行YUV到RGB的转换，根据位流进行配置
    };

    inline void PrintDecoderConfig(const VPUDecoderConfig &config)
    {
        LOG_DEBUG("VPUDecoderConfig", ":\n"
                                      "  device: {}\n"
                                      "  width: {}\n"
                                      "  height: {}\n"
                                      "  fps: {}\n"
                                      "  buffer_count: {}\n"
                                      "  input_buf_type: {}\n"
                                      "  input_pixel_format: {:#x}\n"
                                      "  codec: {}\n"
                                      "  output_buf_type: {}\n"
                                      "  output_pixel_format: {:#x}\n"
                                      "  rc_mode: {}\n"
                                      "  source_url: {}\n"
                                      "  net_w: {}\n"
                                      "  net_h: {}\n"
                                      "  fpga_yuv2rgb: {} 重要！带yuv2rgb功能的位流不支持psin dma！",
                  config.device,
                  config.width,
                  config.height,
                  config.fps,
                  config.buffer_count,
                  config.input_buf_type,
                  config.input_pixel_format,
                  config.codec,
                  config.output_buf_type,
                  config.output_pixel_format,
                  config.rc_mode,
                  config.source_url,
                  config.net_w,
                  config.net_h,
                  config.fpga_yuv2rgb);
    }

    template <typename DeviceType, typename BackendType>
    class WebcamDecoderInputActor : public BaseActor<void, InputMessageForIcore>
    {
    public:
        static constexpr std::string_view LogP = "[WebcamInput]";
        static constexpr int OPT_DEBUG_FLAGS = BIT_MASK_0; // | BIT_MASK_1 | BIT_MASK_2 | BIT_MASK_3 | BIT_MASK_4 | BIT_MASK_5 | BIT_MASK_6 | BIT_MASK_7;
        // BIT_MASK_0: log info
        // BIT_MASK_1: dump imagemake output as image

    public:
        WebcamDecoderInputActor(int id,
                                std::unique_ptr<Webcam> camera,
                                DeviceType &device,
                                BufferManager &buffer_manager,
                                uint64_t yuv2rgb_base_addr,
                                uint64_t imk_base_addr = 0x80000400,
                                uint64_t dma_reg_base = 0x800C0000,
                                const VPUDecoderConfig &config = VPUDecoderConfig(),
                                icraft::xir::NetworkView &imk_netview = empty_networkviews_,
                                std::vector<icraft::xrt::Session> &imk_sessions = empty_sessions_)
            : BaseActor<void, InputMessageForIcore>(buffer_manager),
              device_(device),
              source_id_(id),
              webcam_(std::move(camera)),
              imk_netview_(imk_netview),
              imk_network_(imk_netview.toNetwork()),
              imk_netinfo_(NetInfo(imk_network_)),
              imk_or_icore_sessions_(imk_sessions),
              config_{config},
              source_url_{config.source_url},
              yuv2rgb_reg_base_(yuv2rgb_base_addr),
              imk_reg_base_(imk_base_addr),
              dma_reg_base_(dma_reg_base),
              vpu_decoder_(nullptr), // Initialize to nullptr
              img_tensor_list_(1),
              DO_IMAGEMAKE_(!imk_sessions.empty()),
              imagemake_wddr_base_groups_(buffer_manager.getChunkCount(), 0),
              source_type_(INPUT_SOURCE::WEBCAM),
              data_type_(DATA_TYPE::STREAM)
        {
            // 1. Connect to RTSP stream and get actual parameters
            // 在访问 webcam_ 之前，必须检查它是否为空指针。
            if (!webcam_)
            {
                throw std::invalid_argument("WebcamDecoderInputActor received a null Webcam pointer. Cannot proceed.");
            }
            if (config_.width != webcam_->width || config_.height != webcam_->height)
            {
                LOG_ERROR(LogP, "Warning: User config width={}*height={} not match the actual stream width={}*height={}, use actual webcam values",
                          config_.width, config_.height, webcam_->width, webcam_->height);
                throw std::runtime_error("WebcamDecoderInputActor config width/height not match actual stream values");
            }
            if (webcam_->frame_rate == 0)
            {
                LOG_WARN(LogP, "Webcam frame_rate is 0, using user configuration fps={}", config_.fps);
            }
            config_.codec = webcam_->codec_name;
            config_.input_pixel_format = vpu::to4cc(config_.codec);
            LOG_INFO(LogP, "Webcam source url={}, codec={}, resolution=width{}*height{}", config_.source_url, config_.codec, config_.width, config_.height);
            PrintDecoderConfig(config_);

            // 2. Create VPU decoder with the correct parameters
            vpu_decoder_ = std::make_unique<vpu::Decoder>(
                config_.device.c_str(),
                config_.height, config_.width, 1,
                /*input buf type*/ config_.input_buf_type,          // V4L2_BUF_TYPE_VIDEO_OUTPUT
                /*input pixel formt*/ config_.input_pixel_format,   // V4L2_PIX_FMT_H264
                /*output buf type*/ config_.output_buf_type,        // V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                /*output pixel formt*/ config_.output_pixel_format, // V4L2_PIX_FMT_NV12,
                /* buffer number */ config_.buffer_count);

            // 3. Create buffer manager group based on decoded frame size
            auto zeros = V4L2format2cvmat(config_.output_pixel_format, config_.height, config_.width);
            if (zeros.type() == -1)
            {
                throw std::runtime_error("VPU encoder not support this pix fmt");
            }
            auto CHUNK_COUNT = this->buffer_manager_.getChunkCount();
            auto psddr_chunk_bytesize = zeros.total() * zeros.elemSize();
            // 在psddr-udmabuf上申请摄像头图像缓存区
            chunk_group_id_ = "psddr_udma_decoded_cam" + std::to_string(source_id_);
            // ********** 记得申请足够的MemchunkGroup，每份MemChunk的场地和解码后的 output_pixel_format 相关 **********
            this->buffer_manager_.createChunkGroup(chunk_group_id_, this->device_, psddr_chunk_bytesize);

            LOG_INFO(LogP, "VPU Decoder input format: {:#x}, size: w{}*h{}, elemSize: {}, total: {}, display_data size: {}",
                     config_.input_pixel_format, zeros.cols, zeros.rows, zeros.elemSize(), zeros.total(), psddr_chunk_bytesize);
            LOG_INFO(LogP, "VPU Decoder output format: {:#x}, codec={}", config_.output_pixel_format, config_.codec);
            int stream_sink_type = parse_stream_sink(source_url_);
            LOG_INFO(LogP, "Stream source url={}, type: {}", config_.source_url, stream_sink_type);

            if (DO_IMAGEMAKE_)
            {
                if (config_.fpga_yuv2rgb && yuv2rgb_reg_base_ != 0)
                {
                    // fake input
                    std::vector<int64_t> output_shape = {1, config_.net_h, config_.net_w, 3};
                    auto tensor_layout = icraft::xir::Layout("NHWC");
                    auto output_type = icraft::xrt::TensorType(icraft::xir::IntegerType::UInt8(), output_shape, tensor_layout);
                    auto output_tensor = icraft::xrt::Tensor(output_type).mallocOn(icraft::xrt::HostDevice::MemRegion());
                    img_tensor_list_[0] = output_tensor;
                    LOG_WARN(LogP, "WebcamDecoderInputActor use FPGA YUV2RGB conversion.");
                }
                else
                {
                    LOG_WARN(LogP, "WebcamDecoderInputActor use software YUV2RGB conversion, performance may be affected!");
                }
                // 提取ImageMake的输出地址
                for (int i = 0; i < CHUNK_COUNT; i++)
                {
                    // 取帧之前，先对image make进行配置，保证该帧数据写到正确的plddr位置
                    // PLIN工程，基本需要保证输入算子被view掉，否则无法获取到phy addr
                    // 需要确保第一个Forward是HardOp
                    auto forwards = imk_or_icore_sessions_[i].getForwards(); // in ZG330, this is icore sessions
                                                                             // LOG_DEBUG(LogP, "[{}] imagemake OR icore session {} forwards size={}", source_id_, i, forwards.size());
                    auto backend = std::get<1>(forwards[0]);                 // first is default device backend
                    if (!backend.is<BackendType>())
                    {
                        LOG_ERROR(LogP, "[{}] Backend cast to BackendType failed! First forward_info may on HostBackend", source_id_);
                        throw std::runtime_error("Type cast error!");
                    }
                    auto device_backend = backend.cast<BackendType>();
                    auto input_op = std::get<0>(forwards[0]);
#if defined(USE_BUYI_BACKEND)
                    // net_sess.sub(1)以后，并不改变网络结构，所以第一个forward仍然是input算子，所以需要拿output的phy addr
                    auto value_info = device_backend->forward_info->value_map.at(input_op->outputs[0]->v_id); // outputs
#elif defined(USE_ZG330_BACKEND)
                    // ZG330的icore session通过view掉第一个input算子，改变了网络结构，取view之后第一个op的物理地址
                    auto value_info = device_backend->forward_info->value_map.at(input_op->inputs[0]->v_id); // inputs
#endif
                    imagemake_wddr_base_groups_[i] = value_info->phy_addr; // byte地址
                    LOG_DEBUG(LogP, "Imagemake output{}, plddr addr={:#x}", i, imagemake_wddr_base_groups_[i]);
                }
            }

            // 计算硬件或者软件resize的ratio and bias
            ratio_bias_ = calc_actual_ratio_bias();
        }
        // 虚析构函数是好习惯
        ~WebcamDecoderInputActor() override
        {
            this->stop();
        };

        void start() override
        {
            this->stop_flag_.store(false);
            input_worker_ = std::thread(&WebcamDecoderInputActor::camera_input, this);
            this->worker_ = std::thread(&WebcamDecoderInputActor::loop, this);
        }
        void stop() override
        {
            if (this->stop_flag_.exchange(true))
            {
                return; // Already stopping
            }
            if (input_worker_.joinable())
            {
                input_worker_.join(); // 等待线程结束
            }

            LOG_INFO(LogP, "VPU decoder stopped!");

            if (this->worker_.joinable())
            {
                this->worker_.join(); // 等待线程结束
            }
            // ... VPU 资源释放代码 ...
            vpu_decoder_->streamOff();
            LOG_INFO(LogP, "WebcamDecoderInputActor stopped!");
        }

        std::string getChunkGroupId() const
        {
            return chunk_group_id_;
        }

        // 获取ratio和bias
        // 实际缩放比例和偏移量<是否硬件缩放, RATIO_W, RATIO_H, BIAS_W, BIAS_H>
        std::tuple<bool, float, float, int, int> getRatioBias() const
        {
            return ratio_bias_;
        }

    protected:
        void loop() override
        {
            std::stringstream ss;
            ss << std::this_thread::get_id();
            uint64_t id = std::stoull(ss.str());
            LOG_INFO(LogP, "Decoder thread start!, id={}", id);
            bool eos = false;
            int data_bytesize = 0;
            static int DEBUG_COUNTER = 0;
            while (!vpu_decoder_->streamon_flag_.load())
            {
            }
            LOG_INFO(LogP, "Stream on={}", vpu_decoder_->streamon_flag_.load());
            while (!this->stop_flag_)
            {
                auto t_start = std::chrono::steady_clock::now();
                // 1. 请求一个可用的缓冲区，这是阻塞操作，天然同步
                int buffer_index = this->buffer_manager_.requestIndex(chunk_group_id_); // pending if no buffer available

                // 2. 使用获取到的索引，通过图像源对象从硬件捕获数据
                auto &ps_udma_chunk = this->buffer_manager_.getChunk(chunk_group_id_, buffer_index);
                auto t_buffer_ready = std::chrono::steady_clock::now();
                auto wait_buf_duration = std::chrono::duration_cast<std::chrono::microseconds>(t_buffer_ready - t_start);
                // 解码网络摄像头，获取输入yuv420图像
                vpu_decoder_->getOutput(ps_udma_chunk->begin.cptr(), data_bytesize, eos);
                if (data_bytesize != ps_udma_chunk->byte_size && data_bytesize != 0)
                {
                    LOG_ERROR(LogP, "[{}] Decoded frame size mismatch, expected {}, got {}", source_id_, ps_udma_chunk->byte_size, data_bytesize);
                    break;
                }
                if (data_bytesize == 0)
                {
                    LOG_WARN(LogP, "[{}] Decoder get empty frame, CONTINUE TO NEXT FRAME...", source_id_);
                    this->buffer_manager_.returnIndex(chunk_group_id_, buffer_index);
                    continue;
                }
                if (eos)
                {
                    LOG_WARN(LogP, "[{}] Decoder captures EOS", source_id_);
                    break;
                }
                auto t_camera_take_done = std::chrono::steady_clock::now();
                auto camera_take_duration = std::chrono::duration_cast<std::chrono::microseconds>(t_camera_take_done - t_buffer_ready);

                // 3. 启动硬件yuv2rgb模块
                if (DO_IMAGEMAKE_)
                {
#if defined(USE_ZG330_BACKEND)
                    const std::vector<float> empty_float_vec;
                    initImageMake(this->device_, source_id_, config_.net_w, config_.net_h, 3,
                                  imagemake_wddr_base_groups_[buffer_index], 8, empty_float_vec, empty_float_vec);
#endif
                    // using hw yuv2rgb conversion，带有yuv2rgb模块的多路PLin位流，不支持dma！！！！！
                    if (config_.fpga_yuv2rgb && yuv2rgb_reg_base_ != 0)
                    {
                        LOG_DEBUG(LogP, "Start yuv2rgb for buffer index={}, yuv2rgb_base_reg={:#x}, imk_base_reg={:#x}, imk_waddr={:#x}", buffer_index, yuv2rgb_reg_base_, imk_reg_base_, imagemake_wddr_base_groups_[buffer_index]);

                        if (!yuv2rgb(this->device_, imagemake_wddr_base_groups_[buffer_index],
                                     config_.width, config_.height, config_.net_w, config_.net_h,
                                     ps_udma_chunk, yuv2rgb_reg_base_, imk_reg_base_))
                        {
                            LOG_ERROR(LogP, "Input Src{} yuv 2 rgb error, buffer_index={}", source_id_, buffer_index);
                            this->buffer_manager_.returnIndex(chunk_group_id_, buffer_index);
                            continue;
                        }
                    }
                    // using soft yuv2rgb conversion
                    else
                    {
                        // 复制到 UDMA chunk，提供给下游actor
                        cv::Mat yuvmat = V4L2format2cvmat(config_.output_pixel_format, config_.height, config_.width, ps_udma_chunk->begin.cptr());
                        // ******************** Option 1 先cvColor再resize，耗时48ms左右
                        // cv::Mat bgrMat;
                        // cv::cvtColor(yuvmat, bgrMat, cv::COLOR_YUV2BGR_NV21);
                        // PicPre img_pre(bgrMat);
                        // img_pre.Resize({imk_netinfo_.i_cubic[0].h, imk_netinfo_.i_cubic[0].w}, PicPre::LONG_SIDE).rPad(PicPre::PadModes::AROUND);
                        // if (DEBUG_COUNTER == 0)
                        // {
                        //     cv::imwrite("./io/output/debug_input_actor_src" + std::to_string(source_id_) + ".jpg", img_pre.dst_img);
                        //     cv::imwrite("./io/output/debug_input_actor_src" + std::to_string(source_id_) + "_raw.jpg", bgrMat);
                        //     DEBUG_COUNTER++;
                        // }
                        // **************** Option 2 先resize再cvColor，耗时23~27ms左右，但是需要有长宽数值倍数限制，宽是偶数，最终会在代码里体现。
                        PicPre img_pre(yuvmat, config_.height, config_.width, PicPre::YUVFormat::YUV_NV21);
                        img_pre.Resize({imk_netinfo_.i_cubic[0].h, imk_netinfo_.i_cubic[0].w}, PicPre::ResizeModes::LONG_SIDE);
                        // LOG_DEBUG(LogP, "After resize, real resized size: ({}, {})", img_pre.getResizedHW().first, img_pre.getResizedHW().second);
                        cv::Mat resized_bgrMat;
                        cv::cvtColor(img_pre.dst_img, resized_bgrMat, cv::COLOR_YUV2RGB_NV21);
                        // LOG_DEBUG(LogP, "After cvtColor, real resized size: ({}, {})", resized_bgrMat.rows, resized_bgrMat.cols);
                        img_pre.dst_img = resized_bgrMat;
                        img_pre.rPad(PicPre::PadModes::AROUND);
                        if (OPT_DEBUG_FLAGS & BIT_MASK_2)
                        {
                            cv::imwrite(fmt::format("./io/output/webcam{}_frame{}_input_resized.jpg", source_id_, DEBUG_COUNTER), img_pre.dst_img);
                            // 保存YUV原始数据用于专业分析
                            std::ofstream yuv_file(fmt::format("./io/output/webcam{}_org_yuv.yuv", source_id_), std::ios::binary);
                            yuv_file.write((const char *)yuvmat.data, yuvmat.total() * yuvmat.elemSize());
                            yuv_file.close();
                        }
                        auto input_shape = std::make_tuple(imk_netinfo_.i_cubic[0].h, imk_netinfo_.i_cubic[0].w, 3);

                        icraft::xrt::Tensor img_tensor = CvMat2Tensor(img_pre.dst_img, imk_network_, input_shape);

                        auto t_preprocess = std::chrono::steady_clock::now();

                        // 取帧之前，先对image make进行配置，保证该帧数据写到正确的plddr位置
                        // 请注意寄存器基地址可能随fpga工程变化而变化, 类内用绝对地址，API用相对地址，所以需要+0x80000000
                        fpgaDma(img_tensor, this->device_, imagemake_wddr_base_groups_[buffer_index], imk_reg_base_ + 0x80000000, dma_reg_base_ + 0x80000000); // 多路时生效
                        LOG_DEBUG(LogP, "[{}] FPGA DMA done for buffer index={}, Destination ImageMakeWddrBase_a={:#x}", source_id_, buffer_index, imagemake_wddr_base_groups_[buffer_index]);
                        img_tensor_list_[0] = img_tensor;
                    }
                }

                auto t_conversion_done = std::chrono::steady_clock::now();
                auto conversion_duration = std::chrono::duration_cast<std::chrono::microseconds>(t_conversion_done - t_camera_take_done);

                InputMessageForIcore msg;
                msg.meta.source_id = source_id_;
                msg.meta.buffer_index = buffer_index;
                msg.meta.chunk_group_id = chunk_group_id_;
                msg.meta.error_input = false;
                msg.meta.invalid_ps_frame = false;
                // 3. 封装消息
                if (DO_IMAGEMAKE_) // 如果下游是AI推理，则封装InputMessageForIcore
                {
                    msg.meta.timestamp = std::chrono::system_clock::now().time_since_epoch().count();
                    try
                    {
#if defined(USE_BUYI_BACKEND)
                        // imk前向
                        msg.image_tensors = imk_or_icore_sessions_[buffer_index].forward(img_tensor_list_);
                        for (auto &&t : msg.image_tensors)
                        {
                            t.waitForReady(std::chrono::milliseconds(1000)); // 同步
                        }
#elif defined(USE_ZG330_BACKEND)
                        runImageMakeForward(this->device_, source_id_, config_.net_w, config_.net_h, 3,
                                            imagemake_wddr_base_groups_[buffer_index], 8, true, 0);
                        // spdlog::warn("After imagemake");
                        LOG_DEBUG(LogP, "RegRead(0x4008_0090)={}", this->device_.defaultRegRegion().read(0x40080090)); // 统计了前述寄存器 cycle 数超出监测阈值的帧个数
                        LOG_DEBUG(LogP, "RegRead(0x4008_0084)={}", this->device_.defaultRegRegion().read(0x40080084)); // 统计了视频帧从wr_ps_done 到下一次取帧所用的 cycle 数
                        // 构建PL tensor imk_output(ZG330)
                        auto zg_imk_end_ts = std::chrono::steady_clock::now();
                        auto zg_imk_duration = std::chrono::duration_cast<std::chrono::microseconds>(zg_imk_end_ts - t_camera_take_done);
                        LOG_WARN(LogP, "[{}] ZG330 imagemake forward done, time={}us", source_id_, zg_imk_duration.count());

                        auto forwards = imk_or_icore_sessions_[buffer_index].getForwards();
                        std::cout << "ZG330 icore forwards size=" << forwards.size() << std::endl;
                        auto forward_info = imk_or_icore_sessions_[buffer_index]->backends[0].cast<BackendType>()->forward_info;
                        auto input_op = std::get<0>(forwards[0]); // should be on device backend
                        auto op_backend = std::get<1>(forwards[0]);
                        std::cout << "op_backend is BackendType: " << op_backend.is<BackendType>() << std::endl;
                        auto vid = input_op->inputs[0]->v_id;
                        auto memchunkmap = forward_info->memchunk_map.at(vid)->memChunk;
                        std::cout << "memchunkmap->region type_key: " << memchunkmap->region->typeKey() << std::endl;
                        auto value_obj = forward_info->value_map.at(vid)->value;

                        auto input_tensor = icraft::xrt::Tensor(value_obj);
                        std::cout << "before setData" << std::endl;
                        input_tensor.setData(memchunkmap, forward_info->value_map.at(vid)->phy_addr - memchunkmap->begin.addr());

                        msg.image_tensors = {input_tensor};
                        auto zg_input_tensor_done_ts = std::chrono::steady_clock::now();
                        auto zg_input_tensor_duration = std::chrono::duration_cast<std::chrono::microseconds>(zg_input_tensor_done_ts - zg_imk_end_ts);
                        LOG_WARN(LogP, "[{}] ZG330 icore input tensor setData done, time={}us", source_id_, zg_input_tensor_duration.count());
#endif
                        if (OPT_DEBUG_FLAGS & BIT_MASK_1)
                        {
#if defined(USE_BUYI_BACKEND)
                            std::string runBackend = "buyi";
                            dumpImkOutAsImage(this->device_,
                                              imagemake_wddr_base_groups_[buffer_index],
                                              config_.net_w,
                                              config_.net_h,
                                              4,
                                              "io/output/imagemake",
                                              runBackend, fmt::format("webcam{}_", source_id_));
#elif defined(USE_ZG330_BACKEND)
                            std::string runBackend = "zg330";
                            dumpImkOutAsImage(this->device_,
                                              imagemake_wddr_base_groups_[buffer_index],
                                              config_.net_w,
                                              config_.net_h,
                                              3,
                                              "io/output/imagemake",
                                              runBackend, fmt::format("webcam{}_", source_id_));
#endif
                        }
                    }
                    catch (const std::exception &e)
                    {
                        msg.meta.error_input = true;
                        LOG_ERROR(LogP, "Webcam{} Imk forward error on buffer{}: {}", source_id_, buffer_index, e.what());
                    }

                    auto t_imk_done = std::chrono::steady_clock::now();
                    auto imk_duration = std::chrono::duration_cast<std::chrono::microseconds>(t_imk_done - t_conversion_done);
                    // 4. log后将消息推送到下游NPUActor的队列中
                    auto t_end = std::chrono::steady_clock::now();
                    auto all_duration = std::chrono::duration_cast<std::chrono::microseconds>(t_end - t_start);
                    if (OPT_DEBUG_FLAGS & BIT_MASK_0)
                    {
                        auto s_chunkstatus = this->buffer_manager_.getStatusString(chunk_group_id_);
                        std::string s_status = std::string("<Input:") + (msg.meta.error_input ? "error" : "ok") + (msg.meta.invalid_ps_frame ? ", ps_frame_error" : ", ps_frame_ok") + ">";
                        LOG_INFO(LogP,
                                 "[{}][Buff{}] t_duration={:.2f}ms: wait={:.2f}ms + cam_take={:.2f}ms + yuv2rgb={:.2f}ms + imk={:.2f}ms, status={}, buff usage:{}",
                                 source_id_,
                                 buffer_index,
                                 float(all_duration.count()) / 1000,
                                 float(wait_buf_duration.count()) / 1000,
                                 float(camera_take_duration.count()) / 1000,
                                 float(conversion_duration.count()) / 1000,
                                 float(imk_duration.count()) / 1000,
                                 s_status,
                                 s_chunkstatus);
                    }
                }
                this->output_queue_->push(std::move(msg)); // 无论是否error frame，都push到下游
                DEBUG_COUNTER++;
            }
        }

        void camera_input()
        {
            std::stringstream ss;
            ss << std::this_thread::get_id();
            uint64_t id = std::stoull(ss.str());
            LOG_INFO(LogP, "[Input] Webcam thread start!, id={}", id);
            std::unique_ptr<unsigned char[]> avpkt_ptr(new unsigned char[V4L2_READ_LEN_BUFFER_ROI]);
            while (!this->stop_flag_)
            {
                int pkt_bytesize = 0;
                webcam_->getImage(avpkt_ptr.get());
                vpu_decoder_->enqueueInput((char *)avpkt_ptr.get());
                LOG_DEBUG(LogP, "Webcam pushed data to Decoder!");
            }
        }

        // 获取整数倍的缩放比例和偏移量
        // 按照 <是否硬件缩放, RATIO_W, RATIO_H, BIAS_W, BIAS_H> 顺序
        std::tuple<bool, float, float, int, int> calc_actual_ratio_bias()
        {
            std::tuple<bool, float, float, int, int> ratio_bias;

            // 硬件为整数倍缩放
            if (config_.fpga_yuv2rgb && yuv2rgb_reg_base_ != 0)
            {
                int RATIO_W = config_.width / config_.net_w;
                int RATIO_H = config_.height / config_.net_h;
                int IMG_W = RATIO_W * config_.net_w;
                int IMG_H = RATIO_H * config_.net_h;
                int BIAS_W = (config_.width - IMG_W) / 2;
                int BIAS_H = (config_.height - IMG_H) / 2;
                ratio_bias = std::make_tuple(true, RATIO_W, RATIO_H, BIAS_W, BIAS_H);
            }
            else
            {
                // create empty mat to get actual resized size
                cv::Mat yuvmat = V4L2format2cvmat(config_.output_pixel_format, config_.height, config_.width);
                PicPre img_pre(yuvmat, config_.height, config_.width, PicPre::YUVFormat::YUV_NV21);
                img_pre.Resize({imk_netinfo_.i_cubic[0].h, imk_netinfo_.i_cubic[0].w}, PicPre::ResizeModes::LONG_SIDE);
                LOG_DEBUG(LogP, "After software resize, real resized size: ({}, {})", img_pre.getResizedHW().first, img_pre.getResizedHW().second);
                img_pre.rPad(PicPre::PadModes::AROUND);
                auto RATIO_H = img_pre.getResizedRatio().first;
                auto RATIO_W = img_pre.getResizedRatio().second;
                auto BIAS_H = img_pre.getPadInfo().first;
                auto BIAS_W = img_pre.getPadInfo().second;
                ratio_bias = std::make_tuple(false, RATIO_W, RATIO_H, BIAS_W, BIAS_H);
            }
            spdlog::info("Calculated actual ratio and bias: hard YUV2RGB = {}, RATIO_W={}, RATIO_H={}, BIAS_W={}, BIAS_H={}",
                         std::get<0>(ratio_bias), std::get<1>(ratio_bias),
                         std::get<2>(ratio_bias), std::get<3>(ratio_bias), std::get<4>(ratio_bias));
            return ratio_bias;
        }

    protected:
        int source_id_;
        std::string chunk_group_id_;
        // VPU相关成员
        VPUDecoderConfig config_;
        std::unique_ptr<vpu::Decoder> vpu_decoder_;
        std::unique_ptr<Webcam> webcam_;
        const std::string source_url_;
        uint64_t yuv2rgb_reg_base_;
        uint64_t imk_reg_base_;
        uint64_t dma_reg_base_;
        // this->worker_ is protected in BaseActor
        std::thread input_worker_;
        std::tuple<bool, float, float, int, int> ratio_bias_; // 实际缩放比例和偏移量<是否硬件缩放, RATIO_W, RATIO_H, BIAS_W, BIAS_H>

    protected:
        inline static std::vector<icraft::xrt::Session> empty_sessions_; // 用于默认参数
        inline static icraft::xir::NetworkView empty_networkviews_;      // 用于默认参数
    private:
        // 引用的外部对象
        icraft::xir::NetworkView &imk_netview_;
        icraft::xir::Network imk_network_;
        NetInfo imk_netinfo_;
        std::vector<icraft::xrt::Session> &imk_or_icore_sessions_;
        std::vector<icraft::xrt::Tensor> img_tensor_list_;
        std::vector<uint64_t> imagemake_wddr_base_groups_;

        DeviceType &device_;
        bool DO_IMAGEMAKE_;
        // RFU attributes
        INPUT_SOURCE source_type_;
        DATA_TYPE data_type_;
    };
}
