#pragma once

/*
                 +----------------------+
                 |     BufferManager    |
                 +----------------------+
                         ^      |
(2. 请求/获取buffer index)|      | (由下游最终归还)
                         |      v
+------------------+   +-----------------------+   +--------------------+
| SDICamera Device |<--| SDICameraInputActor   |-->|      data_queue    |--> [NPUActor/OutputActor]
+------------------+   | (数据生产者)           |   +--------------------+
 (1. take捕获图像)      | (拥有SDICamera对象)    |    (3. 推送IQueueMessage)
                       +-----------------------+
*/

#include "pipeline/actor/base_actors.hpp"
#include "pipeline/io/input/sdicamera.hpp"
#include "pipeline/base/messages.hpp"
#include "pipeline/base/thread_safe_queue.hpp"
#include "pipeline/base/enums.hpp"
#include "pipeline/memory/buffer_manager.hpp"

#include <icraft-xrt/core/tensor.h>
#include "compile_fpai_target.hpp"

#include "log_utils.hpp"
#include "et_device.hpp"
#include "bit_masks.hpp"
#include "icraft_utils.hpp"

#include <spdlog/spdlog.h>
#include <chrono>
#include <thread>
#include <memory>
#include <vector>
#include <atomic>

namespace fpai
{
    template <typename DeviceType, typename BackendType, typename OutputMsgType = InputMessageForIcore>
    class SDICameraInputActor : public BaseActor<void, OutputMsgType>
    {
    public:
        static constexpr std::string_view LogP = "[SDICam]";
        static constexpr int OPT_DEBUG_FLAGS = BIT_MASK_0; // | BIT_MASK_1 | BIT_MASK_2 | BIT_MASK_3 | BIT_MASK_4 | BIT_MASK_5 | BIT_MASK_6 | BIT_MASK_7;
        // BIT_MASK_0: log info
        // BIT_MASK_1: dump imagemake output as image

    public:
        // 使用默认参数合并两个构造函数，消除代码重复
        SDICameraInputActor(int id,
                            std::unique_ptr<SDICamera<DeviceType>> camera,
                            DeviceType &device,
                            BufferManager &buffer_manager,
                            std::vector<icraft::xrt::Session> &imk_sessions = empty_sessions_,
                            uint64_t imagemake_reg_base = 0)
            : BaseActor<void, OutputMsgType>(buffer_manager),
              device_(device),
              source_id_(id),
              camera_(std::move(camera)),
              imk_or_icore_sessions_(imk_sessions),
              imagemake_reg_base_(imagemake_reg_base),
              img_tensor_list_{},
              source_type_(INPUT_SOURCE::SDI),
              data_type_(DATA_TYPE::IMAGE),
              imagemake_wddr_base_groups_(buffer_manager.getChunkCount(), 0),
              pl_memchunk_groups_(buffer_manager.getChunkCount()),
              DO_IMAGEMAKE_(false)
        {
            // 在psddr-udmabuf上申请摄像头图像缓存区
            chunk_group_id_ = "input_udma_buff_camera" + std::to_string(source_id_);
            this->buffer_manager_.createChunkGroup(chunk_group_id_, this->device_, camera_->getBufferSize());

            auto CHUNK_COUNT = this->buffer_manager_.getChunkCount();
            for (int i = 0; i < CHUNK_COUNT; i++)
            {
                auto &cam_chunk = this->buffer_manager_.getChunk(chunk_group_id_, i);
                spdlog::info("Cam buffer udma[{}] addr={:#x}", i, cam_chunk->begin.addr());
            }
            DO_IMAGEMAKE_ = !imk_or_icore_sessions_.empty();
            if (DO_IMAGEMAKE_)
            {
                // fake input for imk_input(BY100) or imk_output(ZG330)
                std::vector<int64_t> output_shape = {1,
                                                     static_cast<int64_t>(camera_->getNetHeight()),
                                                     static_cast<int64_t>(camera_->getNetWidth()),
                                                     3};
                auto tensor_layout = icraft::xir::Layout("NHWC");
                auto output_type = icraft::xrt::TensorType(icraft::xir::IntegerType::UInt8(), output_shape, tensor_layout);
                auto output_tensor = icraft::xrt::Tensor(output_type).mallocOn(icraft::xrt::HostDevice::MemRegion());
                img_tensor_list_ = std::vector<icraft::xrt::Tensor>{output_tensor};

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
                    // ZG330的icore session通过view掉第一个input算子，改变了网络结构，所以需要取第二个op的物理地址
                    auto value_info = device_backend->forward_info->value_map.at(input_op->inputs[0]->v_id); // inputs
#endif
                    imagemake_wddr_base_groups_[i] = value_info->phy_addr; // byte地址
                    spdlog::info("Imagemake output{}, plddr addr={:#x}", i, imagemake_wddr_base_groups_[i]);
                }
                LOG_INFO(LogP, "[{}] SDICameraInputActor initialized. Has IMK: {}", source_id_, DO_IMAGEMAKE_);
            }
        }
        ~SDICameraInputActor()
        {
            this->stop();
        }
        std::string getChunkGroupId() const
        {
            return chunk_group_id_;
        }
        uint64_t getCameraBufferSize() const
        {
            return camera_->getBufferSize();
        }
        // 获取ratio和bias
        // 实际缩放比例和偏移量<是否硬件缩放, RATIO_W, RATIO_H, BIAS_W, BIAS_H>
        std::tuple<bool, float, float, int, int> getRatioBias() const
        {
            return this->camera_->getRatioBias();
        }

    protected:
        // 核心的线程循环逻辑
        void loop() override
        {
            std::stringstream ss;
            ss << std::this_thread::get_id();
            uint64_t id = std::stoull(ss.str());
            spdlog::info("[Input][{}]- SDICameraInput thread start!, id={}", source_id_, id);
            while (!this->stop_flag_)
            {
                auto t_start = std::chrono::steady_clock::now();
                // 1. 请求一个可用的缓冲区，这是阻塞操作，天然同步
                int buffer_index = this->buffer_manager_.requestIndex(chunk_group_id_);

                // 2. 使用获取到的索引，通过图像源对象从硬件捕获数据
                auto &raw_chunck = this->buffer_manager_.getChunk(chunk_group_id_, buffer_index);
                auto t_buffer_ready = std::chrono::steady_clock::now();
                auto wait_buf_duration = std::chrono::duration_cast<std::chrono::microseconds>(t_buffer_ready - t_start);
                // spdlog::debug("Camera requested {} buffer index={}, addr={:#x}", chunk_group_id_, buffer_index, raw_chunck->begin.addr());
                if (this->DO_IMAGEMAKE_ && imagemake_reg_base_ != 0)
                {
                    this->device_.defaultRegRegion().write(imagemake_reg_base_ + 0x114, imagemake_wddr_base_groups_[buffer_index]);
#if defined(USE_ZG330_BACKEND)
                    const std::vector<float> empty_float_vec;
                    initImageMake(this->device_, source_id_, camera_->getNetWidth(), camera_->getNetHeight(), camera_->getChannel(),
                                  imagemake_wddr_base_groups_[buffer_index], 8, empty_float_vec, empty_float_vec);
#endif
                }
                // spdlog::debug("Before camera{} take RegRead(0x18)={}", source_id_, camera_->RegRead(0x18)); // 是否使能hdmi显示
                camera_->take(raw_chunck); // 向硬件写寄存器，触发取帧PL-> PSDDR_udma
                // spdlog::warn("After camera take");
                // spdlog::info("RegRead(0x4008_0090)={}", this->device_.defaultRegRegion().read(0x40080090)); // 统计了前述寄存器 cycle 数超出监测阈值的帧个数
                // spdlog::info("RegRead(0x4008_0084)={}", this->device_.defaultRegRegion().read(0x40080084)); // 统计了视频帧从wr_ps_done 到下一次取帧所用的 cycle 数
                auto t_camera_take_done = std::chrono::steady_clock::now();
                auto camera_take_duration = std::chrono::duration_cast<std::chrono::microseconds>(t_camera_take_done - t_buffer_ready);

                // 3. 封装InputMessageForIcore消息
                OutputMsgType msg;
                msg.meta.buffer_index = buffer_index;
                msg.meta.chunk_group_id = chunk_group_id_;
                msg.meta.timestamp = std::chrono::system_clock::now().time_since_epoch().count();
                msg.meta.source_id = source_id_;
                msg.meta.error_input = false;
                msg.meta.invalid_ps_frame = false;
                
                if (this->DO_IMAGEMAKE_)
                {
                    try
                    {
#if defined(USE_BUYI_BACKEND)
                        msg.image_tensors = imk_or_icore_sessions_[buffer_index].forward(img_tensor_list_);
                        for (auto &&t : msg.image_tensors)
                        {
                            t.waitForReady(std::chrono::milliseconds(1000)); // 同步
                        }
#elif defined(USE_ZG330_BACKEND)
                        runImageMakeForward(this->device_, source_id_, camera_->getNetWidth(), camera_->getNetHeight(), camera_->getChannel(),
                                            imagemake_wddr_base_groups_[buffer_index], 8, false, 0);
                        // spdlog::warn("After imagemake");
                        // spdlog::info("RegRead(0x4008_0090)={}", this->device_.defaultRegRegion().read(0x40080090)); // 统计了前述寄存器 cycle 数超出监测阈值的帧个数
                        // spdlog::info("RegRead(0x4008_0084)={}", this->device_.defaultRegRegion().read(0x40080084)); // 统计了视频帧从wr_ps_done 到下一次取帧所用的 cycle 数
                        // 构建PL tensor imk_output(ZG330)
                        auto zg_imk_end_ts = std::chrono::steady_clock::now();
                        auto zg_imk_duration = std::chrono::duration_cast<std::chrono::microseconds>(zg_imk_end_ts - t_camera_take_done);
                        // LOG_WARN(LogP, "[{}] ZG330 imagemake forward done, time={}us", source_id_, zg_imk_duration.count());

                        auto forwards = imk_or_icore_sessions_[buffer_index].getForwards();
                        // std::cout << "ZG330 icore forwards size=" << forwards.size() << std::endl;
                        auto forward_info = imk_or_icore_sessions_[buffer_index]->backends[0].cast<BackendType>()->forward_info;
                        auto input_op = std::get<0>(forwards[0]); // should be on device backend
                        auto op_backend = std::get<1>(forwards[0]);
                        // std::cout << "op_backend is BackendType: " << op_backend.is<BackendType>() << std::endl;
                        auto vid = input_op->inputs[0]->v_id;
                        auto memchunkmap = forward_info->memchunk_map.at(vid)->memChunk;
                        // std::cout << "memchunkmap->region type_key: " << memchunkmap->region->typeKey() << std::endl;
                        auto value_obj = forward_info->value_map.at(vid)->value;

                        auto input_tensor = icraft::xrt::Tensor(value_obj);
                        // std::cout << "before setData" << std::endl;
                        input_tensor.setData(memchunkmap, forward_info->value_map.at(vid)->phy_addr - memchunkmap->begin.addr());

                        msg.image_tensors = {input_tensor};
                        auto zg_input_tensor_done_ts = std::chrono::steady_clock::now();
                        auto zg_input_tensor_duration = std::chrono::duration_cast<std::chrono::microseconds>(zg_input_tensor_done_ts - zg_imk_end_ts);
                        // LOG_WARN(LogP, "[{}] ZG330 icore input tensor setData done, time={}us", source_id_, zg_input_tensor_duration.count());
#endif
                        if (OPT_DEBUG_FLAGS & BIT_MASK_1)
                        {
#if defined(USE_BUYI_BACKEND)
                            std::string runBackend = "buyi";
                            dumpImkOutAsImage(this->device_,
                                              imagemake_wddr_base_groups_[buffer_index],
                                              camera_->getNetWidth(),
                                              camera_->getNetHeight(),
                                              4,
                                              "io/output/imagemake",
                                              runBackend, fmt::format("sdicam{}_", source_id_));
#elif defined(USE_ZG330_BACKEND)
                            std::string runBackend = "zg330";
                            dumpImkOutAsImage(this->device_,
                                              imagemake_wddr_base_groups_[buffer_index],
                                              camera_->getNetWidth(),
                                              camera_->getNetHeight(),
                                              3,
                                              "io/output/imagemake",
                                              runBackend, fmt::format("sdicam{}_", source_id_));
#endif
                        }
                    }
                    catch (const std::exception &e)
                    {
                        msg.meta.error_input = true;
                        spdlog::error("[INPUT][{}] PL imk forward error: {}", source_id_, e.what());
                    }
                }
                auto t_imk_done = std::chrono::steady_clock::now();
                auto imk_duration = std::chrono::duration_cast<std::chrono::microseconds>(t_imk_done - t_camera_take_done);
                // 等待PL to PS frame copy done.
                if (!camera_->wait()) // in ms
                {
                    msg.meta.invalid_ps_frame = true;
                    spdlog::error("[INPUT][{}] camera wait error, buffer={}", source_id_, buffer_index);
                }

                // spdlog::debug("RegRead(0x4008_0090)={}", this->device_.defaultRegRegion().read(0x40080090)); //统计了前述寄存器 cycle 数超出监测阈值的帧个数
                // spdlog::debug("RegRead(0x4008_0084)={}", this->device_.defaultRegRegion().read(0x40080084)); //统计了视频帧从wr_ps_done 到下一次取帧所用的 cycle 数

                auto t_camera_wait_done = std::chrono::steady_clock::now();
                auto camera_wait_duration = std::chrono::duration_cast<std::chrono::microseconds>(t_camera_wait_done - t_imk_done);

                // 4. log后将消息推送到下游NPUActor的队列中
                auto t_end = std::chrono::steady_clock::now();
                auto all_duration = std::chrono::duration_cast<std::chrono::microseconds>(t_end - t_start);
                if (OPT_DEBUG_FLAGS & BIT_MASK_0)
                {
                    auto s_chunkstatus = this->buffer_manager_.getStatusString(chunk_group_id_);
                    std::string s_status = std::string("<Input:") + (msg.meta.error_input ? "error" : "ok") + (msg.meta.invalid_ps_frame ? ", ps_frame_error" : ", ps_frame_ok") + ">";
                    spdlog::info(
                        "[Input][{}] T({:.2f}ms)=wait({:.2f}ms)+cam_take({:.2f}ms)+imk({:.2f}ms)+cam_wait({:.2f}ms); status={}; buff[{}]:{}",
                        source_id_,
                        float(all_duration.count()) / 1000,
                        float(wait_buf_duration.count()) / 1000,
                        float(camera_take_duration.count()) / 1000,
                        float(imk_duration.count()) / 1000,
                        float(camera_wait_duration.count()) / 1000,
                        s_status,
                        buffer_index,
                        s_chunkstatus);
                }
                this->output_queue_->push(std::move(msg)); // 无论是否error frame，都push到下游
            }
        }

    protected:
        inline static std::vector<icraft::xrt::Session> empty_sessions_; // 用于默认参数
        inline static std::vector<uint64_t> empty_input_addrs_;          // 用于默认参数
        DeviceType &device_;
    private:
        int source_id_;
        std::unique_ptr<SDICamera<DeviceType>> camera_;
        // 引用的外部对象
        std::vector<icraft::xrt::Session> &imk_or_icore_sessions_;
        // 私有成员
        std::vector<icraft::xrt::Tensor> img_tensor_list_;

        std::string chunk_group_id_;
        uint64_t imagemake_reg_base_;
        std::vector<uint64_t> imagemake_wddr_base_groups_;
        std::vector<icraft::xrt::MemChunk> pl_memchunk_groups_;
        bool DO_IMAGEMAKE_;

        // RFU attributes
        INPUT_SOURCE source_type_;
        DATA_TYPE data_type_;
    };
}
