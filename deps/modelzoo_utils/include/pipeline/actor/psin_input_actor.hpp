#pragma once
/*
                 +----------------------+
                 |     BufferManager    |
                 +----------------------+
                         ^      |
(2. 请求/获取buffer index)|      | (由下游最终归还)
                         |      v
+------------------+   +-----------------------+   +--------------------+
| SDICamera Device |<--| PSinImageInputActor   |-->|      data_queue    |--> [NPUActor/OutputActor]
+------------------+   | (数据生产者)           |   +--------------------+
 (1. take捕获图像)      | (拥有SDICamera对象)    |    (3. 推送IQueueMessage)
                       +-----------------------+
*/
#pragma once

#include "bit_masks.hpp"
#include "pipeline/actor/base_actors.hpp"
#include "pipeline/base/messages.hpp"
#include "pipeline/base/thread_safe_queue.hpp"
#include "pipeline/memory/buffer_manager.hpp"
#include "pipeline/io/input/directory_image_sequence.hpp" // Include our new class

#include <icraft-xrt/core/tensor.h>
#include "compile_fpai_target.hpp"

#include "log_utils.hpp"
#include "et_device.hpp" // For fpgaDma_psin
#include "icraft_utils.hpp"
#include "modelzoo_utils.hpp" // For PicPre & CvMat2Tensor
#include "PicPre.hpp"

#include <opencv2/dnn.hpp> // 添加此头文件
#include <spdlog/spdlog.h>
#include <chrono>
#include <thread>
#include <memory>
#include <vector>

// PS-side Input Actor: Reads from a file sequence, performs pre-processing,
// DMA, and ImageMake forward pass.
namespace fpai
{
    template <typename DeviceType, typename BackendType>
    class PSinImageInputActor : public BaseActor<InputMessageForIcore>
    {
    public:
        static constexpr std::string_view LogP = "[PSIN]";
        constexpr static int OPT_DEBUG_FLAGS = BIT_MASK_0; // | BIT_MASK_1 | BIT_MASK_2 | BIT_MASK_3 | BIT_MASK_4 | BIT_MASK_5 | BIT_MASK_6 | BIT_MASK_7;
        // BIT_MASK_0: log info
        // BIT_MASK_1: dump imagemake output as image

    public:
        PSinImageInputActor(int id,
                            std::unique_ptr<DirectoryImageSequence> image_source,
                            DeviceType &device,
                            BufferManager &buffer_manager,
                            int target_fps,
                            int frame_w, int frame_h,
                            int net_w, int net_h,
                            icraft::xir::NetworkView &imk_netview,
                            std::vector<icraft::xrt::Session> &imk_sessions,
                            uint64_t imk_reg_base = 0,
                            uint64_t dma_reg_base = 0)
            : BaseActor<void, InputMessageForIcore>(buffer_manager),
              device_(device),
              source_id_(id),
              image_source_(std::move(image_source)),
              output_queue_(output),
              FRAME_W_(frame_w),
              FRAME_H_(frame_h),
              target_fps_(target_fps),
              imk_netview_(imk_netview),
              imk_network_(imk_netview.toNetwork()),
              imk_sessions_(imk_sessions),
              NET_W_(net_w),
              NET_H_(net_h),
              imk_reg_base_(imk_reg_base),
              dma_reg_base_(dma_reg_base),
              frame_interval_us_(0),
              chunk_group_id_("")
        {
            // No need to create buffer group here, as it's assumed to be managed externally
            // for PS-side processing where buffers are more complex.
            if (target_fps_ > 0)
            {
                frame_interval_us_ = std::chrono::microseconds(1000000 / target_fps_);
            }
            // 将cv Mat构造为输入网络的TENSOR
            chunk_group_id_ = "udma_buf" + std::to_string(source_id_);
            this->buffer_manager_.createChunkGroup(chunk_group_id_, this->device_, FRAME_W_ * FRAME_H_ * 2); // Assuming 2 channels (YUV)
            ratio_bias_ = calc_actual_ratio_bias();
            LOG_INFO(LogP, "PSinImageInputActor [{}] initialized for chunk group '{}', net shape=<{}x{}>", id, chunk_group_id_, NET_W_, NET_H_);
        }
        ~PSinImageInputActor()
        {
            this->stop();
        };

    public:
        std::string getChunkGroupId() const
        {
            return chunk_group_id_;
        }

        bool hasImageMake() const
        {
            return imk_sessions_.empty() == false;
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
            spdlog::info("[Input][{}]- PSinImageInputActor thread start!", source_id_);

            while (!this->stop_flag_ && !image_source_->isFinished())
            {
                auto t_start = std::chrono::steady_clock::now();

                // 1. 请求一个可用的缓冲区索引
                int buffer_index = this->buffer_manager_.requestIndex(chunk_group_id_);
                auto t_wait_buf = std::chrono::steady_clock::now();

                // 2. 触发图像源读取下一张图（这会增加内部的文件计数器）
                auto &udma_chunk = this->buffer_manager_.getChunk(chunk_group_id_, buffer_index);
                cv::Mat org_image;
                size_t index;
                bool valid = image_source_->take(org_image, index);
                if (!valid)
                {
                    spdlog::info("[Input][{}] No more images to read(idx={}), exiting loop.", source_id_, index);
                    this->buffer_manager_.returnIndex(chunk_group_id_, buffer_index); // 归还buffer
                    break;                                                            // 退出循环
                }

                // 3. PSin 前处理 (Resize, Pad)
                PicPre img_pre(org_image);                                  // HINT: taking origin BGR mat and convert to RGB
                img_pre.Resize({NET_H_, NET_W_}, PicPre::LONG_SIDE).rPad(); // letterbox
                // 复制到 UDMA chunk，提供给下游actor
                cv::Mat yuv_mat(FRAME_H_, FRAME_W_, CV_8UC2, (void *)udma_chunk->begin.cptr());
                cv::cvtColor(org_image, yuv_mat, cv::COLOR_BGR2YUV_UYVY);
                memcpy(udma_chunk->begin.cptr(), yuv_mat.data, yuv_mat.total() * yuv_mat.elemSize());
                LOG_DEBUG(LogP, "[{}] {:#x} org image copied to UDMA buffer len={}.", source_id_, udma_chunk->begin.addr(), yuv_mat.total() * yuv_mat.elemSize());
                // if (index == 0)
                // {
                //     cv::imwrite("debug_org_image" + std::to_string(source_id_) + ".jpg", org_image); // For debug
                //     cv::imwrite("debug_preprocessed_image" + std::to_string(source_id_) + ".jpg", img_pre.dst_img);
                // }
                // 4. 转换为 Tensor
                icraft::xrt::Tensor img_tensor;
                if (this->hasImageMake())
                {
                    img_tensor = CvMat2Tensor(img_pre.dst_img, imk_network_);
                }
                else
                {
                    // 使用 cv::dnn::blobFromImage 一步完成转换
                    cv::Mat blob;
                    // 参数说明:
                    // 1. image: 输入图像 (HWC, BGR)
                    // 2. blob: 输出 4D blob (NCHW)
                    // 3. scalefactor: 1.0/255.0 (归一化)
                    // 4. size: cv::Size() 保持原大小 (因为已经 resize 过了)
                    // 5. mean: cv::Scalar() 均值 (此处为0)
                    // 6. swapRB: true (执行 BGR -> RGB，对应 Python 的 [::-1])
                    // 7. crop: false
                    cv::dnn::blobFromImage(img_pre.dst_img, blob, 1.0 / 255.0, cv::Size(), cv::Scalar(), true, false, CV_32F);

                    // blob 现在的内存布局是 NCHW (1, C, H, W)，且为 float32
                    auto input_value = imk_network_.inputs()[this->source_id_];
                    std::cout << "Input source_id=" << this->source_id_ << ", input size=" << imk_network_.inputs().size() << std::endl;
                    std::cout << "Blob shape: ";
                    for (int i = 0; i < blob.dims; ++i)
                    {
                        std::cout << blob.size[i] << " ";
                    }
                    std::cout << std::endl;
                    std::cout << "Blob created with type: " << blob.type() << " (CV_8U=" << CV_8U << ", CV_32F=" << CV_32F << ")" << std::endl;
                    img_tensor = data2Tensor(reinterpret_cast<const float *>(blob.data), input_value);
                }
                auto t_preprocess = std::chrono::steady_clock::now();

                // 取帧之前，先对image make进行配置，保证该帧数据写到正确的plddr位置
                if (this->hasImageMake())
                {
                    // 4.1 配置 ImageMake 的 PLDDR 写地址
                    auto forwards = imk_sessions_[buffer_index].getForwards();
                    auto imk_op = std::get<0>(forwards[0]);
                    auto backend = std::get<1>(forwards[0]).cast<BackendType>();
                    auto ImageMakeWddrBase_a = backend->forward_info->value_map.at(imk_op->outputs[0]->v_id)->phy_addr; // byte地址

                    // 请注意该地址可能随fpga工程变化而变化, 类内用绝对地址，API用相对地址，所以需要+0x80000000
                    // 5. 获取 IMK 输出的 PLDDR 物理地址
                    fpgaDma(img_tensor, this->device_, ImageMakeWddrBase_a, imk_reg_base_ + 0x80000000, dma_reg_base_ + 0x80000000);
                }

                auto t_dma = std::chrono::steady_clock::now();

                // 7. 封装消息
                InputMessageForIcore msg;
                msg.meta.buffer_index = buffer_index;
                msg.meta.chunk_group_id = chunk_group_id_;
                msg.meta.timestamp = index; // 使用图像序列的索引作为timestamp
                msg.meta.source_id = source_id_;
                msg.meta.error_input = false;
                msg.meta.invalid_ps_frame = false;

                if (!this->hasImageMake())
                {
                    // 如果没有imagemake模块，直接将预处理后的tensor传递给下游
                    msg.image_tensors = {img_tensor};
                    output_queue_.push(std::move(msg));
                    continue; // 继续下一帧
                }
                else
                {
                    // 8. 执行 IMK forward
                    try
                    {
                        msg.image_tensors = imk_sessions_[buffer_index].forward({img_tensor});
                        for (auto &&t : msg.image_tensors)
                        {
                            t.waitForReady(std::chrono::milliseconds(1000)); // 同步
                        }
                        if (OPT_DEBUG_FLAGS & BIT_MASK_1)
                        {
                            dumpImkOutAsImage(msg.image_tensors[0], "./io/output/imagemake", source_id_);
                        }
                    }
                    catch (const std::exception &e)
                    {
                        msg.meta.error_input = true;
                        spdlog::error("[INPUT][{}] IMK forward error on image#{}: {}", source_id_, index, e.what());
                        this->buffer_manager_.returnIndex(chunk_group_id_, buffer_index); // 出错时立即归还buffer
                        continue;                                                         // 继续下一帧
                    }
                }

                auto t_imk = std::chrono::steady_clock::now();

                // 9. 性能日志
                if (OPT_DEBUG_FLAGS & BIT_MASK_0)
                {
                    auto wait_d = std::chrono::duration<float, std::milli>(t_wait_buf - t_start).count();
                    auto prep_d = std::chrono::duration<float, std::milli>(t_preprocess - t_wait_buf).count();
                    auto dma_d = std::chrono::duration<float, std::milli>(t_dma - t_preprocess).count();
                    auto imk_d = std::chrono::duration<float, std::milli>(t_imk - t_dma).count();
                    auto total_d = std::chrono::duration<float, std::milli>(t_imk - t_start).count();

                    spdlog::info("[PSin][{}] T({:.2f}ms)=wait_buf({:.2f}ms)+prep({:.2f}ms)+dma({:.2f}ms)+imk({:.2f}ms) | buff_idx={}",
                                 source_id_, total_d, wait_d, prep_d, dma_d, imk_d, buffer_index);
                }

                // 10. 推送到下游

                output_queue_.push(std::move(msg));
                auto t_end_loop = std::chrono::steady_clock::now();

                auto loop_duration = std::chrono::duration_cast<std::chrono::microseconds>(t_end_loop - t_start);

                if (loop_duration < frame_interval_us_)
                {
                    std::this_thread::sleep_for(frame_interval_us_ - loop_duration);
                }
            }
            spdlog::info("[Input][{}]PSinImageInputActor thread finished.", source_id_);
        }

        // 获取整数倍的缩放比例和偏移量
        // 按照 <是否硬件缩放, RATIO_W, RATIO_H, BIAS_W, BIAS_H> 顺序
        std::tuple<bool, float, float, int, int> calc_actual_ratio_bias()
        {
            std::tuple<bool, float, float, int, int> ratio_bias;

            // create empty mat to get actual resized size
            cv::Mat rgbmat(FRAME_H_, FRAME_W_, CV_8UC3);
            PicPre img_pre(rgbmat, FRAME_H_, FRAME_W_);
            img_pre.Resize({NET_H_, NET_W_}, PicPre::ResizeModes::LONG_SIDE);
            LOG_DEBUG(LogP, "After software resize, real resized size: ({}, {})", img_pre.getResizedHW().first, img_pre.getResizedHW().second);
            img_pre.rPad(PicPre::PadModes::AROUND);
            auto RATIO_H = img_pre.getResizedRatio().first;
            auto RATIO_W = img_pre.getResizedRatio().second;
            auto BIAS_H = img_pre.getPadInfo().first;
            auto BIAS_W = img_pre.getPadInfo().second;
            ratio_bias = std::make_tuple(false, RATIO_W, RATIO_H, BIAS_W, BIAS_H);

            spdlog::info("Calculated actual ratio and bias: hard YUV2RGB = {}, RATIO_W={}, RATIO_H={}, BIAS_W={}, BIAS_H={}",
                         std::get<0>(ratio_bias), std::get<1>(ratio_bias),
                         std::get<2>(ratio_bias), std::get<3>(ratio_bias), std::get<4>(ratio_bias));
            return ratio_bias;
        }

    protected:
        inline static std::vector<icraft::xrt::Session> empty_sessions_; // 用于默认参数
        std::tuple<bool, float, float, int, int> ratio_bias_;            // 实际缩放比例和偏移量<是否硬件缩放, RATIO_W, RATIO_H, BIAS_W, BIAS_H>

    private:
        // 注意变量声明顺序，和构造函数初始化列表顺序一致
        int source_id_;
        std::unique_ptr<DirectoryImageSequence> image_source_;
        DeviceType &device_;
        icraft::xir::NetworkView &imk_netview_;
        icraft::xir::Network imk_network_;
        uint64_t imk_reg_base_;
        uint64_t dma_reg_base_;
        std::string chunk_group_id_;
        int target_fps_;

        std::vector<icraft::xrt::Session> &imk_sessions_;
        std::chrono::microseconds frame_interval_us_;
        const int FRAME_W_;
        const int FRAME_H_;
        const int NET_W_;
        const int NET_H_;
    };
}
