#pragma once

#include "pipeline/actor/base_actors.hpp"
#include "pipeline/base/messages.hpp"
#include "pipeline/base/enums.hpp"
#include "pipeline/memory/buffer_manager.hpp"
#include "pipeline/base/thread_safe_queue.hpp"

#include "NetInfo.hpp"
#include "icraft_utils.hpp"
#include "log_utils.hpp"

#include <icraft-xrt/core/session.h>
#include "compile_fpai_target.hpp"

#include <atomic>
#include <thread>
#include <vector>
#include <array>
#include <memory>
#include <string>
#include <chrono>

namespace fpai
{ // 添加命名空间开始

    struct CFTConfig
    {
        std::string name = "MOD_CFT";
        int RGB_IMK_OP_ID = -1;
        int IR_IMK_OP_ID = -1;
        float CONF = 0.25;
        float IOU_THRESHOLD = 0.45;
        bool MULTILABEL = false;
        int N_CLASS = 80; // coco 80 classes
        bool FPGA_NMS = true;
        bool DETPOST = true;
        std::string JSON_PATH;
        std::string RAW_PATH;
        int NET_W = 640;
        int NET_H = 640;
        int FRAME_W = 1920;
        int FRAME_H = 1080;
        std::string NAMES_PATH = "names/coco.names";
        std::vector<std::string> LABELS{};
        crop_position CROP_POS = crop_position::center;
        std::vector<std::vector<std::vector<float>>> ANCHORS = {{{10, 13}, {16, 30}, {33, 23}},
                                                                {{30, 61}, {62, 45}, {59, 119}},
                                                                {{116, 90}, {156, 198}, {373, 326}}};
    };

    void PrintCFTConfig(const CFTConfig &cfg)
    {
        spdlog::info("CFT Configuration:");
        spdlog::info("  Name: {}", cfg.name);
        spdlog::info("  Confidence Threshold: {}", cfg.CONF);
        spdlog::info("  IOU Threshold: {}", cfg.IOU_THRESHOLD);
        spdlog::info("  Multi-label: {}", cfg.MULTILABEL);
        spdlog::info("  Number of Classes: {}", cfg.N_CLASS);
        spdlog::info("  FPGA NMS: {}", cfg.FPGA_NMS);
        spdlog::info("  DETPOST: {}", cfg.DETPOST);
        spdlog::info("  Network Width: {}", cfg.NET_W);
        spdlog::info("  Network Height: {}", cfg.NET_H);
        spdlog::info("  Frame Width: {}", cfg.FRAME_W);
        spdlog::info("  Frame Height: {}", cfg.FRAME_H);
        spdlog::info("  Names Path: {}", cfg.NAMES_PATH);
        spdlog::info("  JSON Path: {}", cfg.JSON_PATH);
        spdlog::info("  RAW Path: {}", cfg.RAW_PATH);
        spdlog::info("  Anchors: ");
        for (const auto &scale : cfg.ANCHORS)
        {
            std::string anchor_str;
            for (const auto &anchor : scale)
            {
                anchor_str += "[" + std::to_string(anchor[0]) + ", " + std::to_string(anchor[1]) + "] ";
            }
            spdlog::info("    - {}", anchor_str);
        }
    }

    // 构建多网络NPU Actor
    template <typename DeviceType, typename BackendType>
    class CFTIcoreActor : public BaseActor<MultiSourceInputMessage, IcoreMessageForPost>
    {
    public:
        static constexpr std::string_view LogP = "[ICORE]";
        const int RGB_INDEX = 0;
        const int IR_INDEX = 1;
        const bool OPT_MERGE_OPS = false;
        const bool OPT_COMPRESS_FTMP = false;
        const size_t INPUT_COUNT = 2;                      // RGB & IR
        static constexpr int OPT_DEBUG_FLAGS = BIT_MASK_0; // | BIT_MASK_1 | BIT_MASK_2 | BIT_MASK_3 | BIT_MASK_4 | BIT_MASK_5 | BIT_MASK_6 | BIT_MASK_7;
        // BIT_MASK_0: log info
        // BIT_MASK_1: dump network json
        // BIT_MASK_2: dump input/output tensors as images, or dump ftmps of network
    public:
        // 构造函数加载神经网络并进行多网络拆分
        CFTIcoreActor(
            int icore_id,
            DeviceType &device,
            BufferManager &buffer_manager,
            const CFTConfig &cft_cfg = CFTConfig(),
            int choose_display = 0)
            : BaseActor<InputMsgType, OutputMsgType>(buffer_manager),
            device_(device),
              icore_id_(icore_id),
              input_queue_(input_queue),
              output_queue_(output_queue),
              FRAME_W_(cft_cfg.FRAME_W),
              FRAME_H_(cft_cfg.FRAME_H),
              NET_W_(cft_cfg.NET_W),
              NET_H_(cft_cfg.NET_H),
              CHUNK_COUNT_{this->buffer_manager_.getChunkCount()},
              net_cfg_(cft_cfg),
              inet_{},
              netinfo_{},
              network_sessions_{CHUNK_COUNT_},
              imk_session_groups_{CFTIcoreActor::INPUT_COUNT}, // 包含可将光和红外两路输入
              icore_sessions_{CHUNK_COUNT_}
        {
            if(choose_display == 0)
            {
                DISPLAY_INDEX_ = RGB_INDEX;
                DISCARD_INDEX_ = IR_INDEX;
            }
            else
            {
                DISPLAY_INDEX_ = IR_INDEX;
                DISCARD_INDEX_ = RGB_INDEX;
            }
            // 加载网络
            PrintCFTConfig(net_cfg_);
            spdlog::info("MOD_CFT json: {}, raw: {}", net_cfg_.JSON_PATH, net_cfg_.RAW_PATH);
            inet_ = loadNetwork(net_cfg_.JSON_PATH, net_cfg_.RAW_PATH);
            netinfo_ = NetInfo(inet_);

            // 切分网络
            icraft::xir::NetworkView rgb_imk;
            icraft::xir::NetworkView ir_imk;
            icraft::xir::NetworkView icore;
            if (netinfo_.ImageMake_on)
            {
                for (int i = 0; i < CFTIcoreActor::INPUT_COUNT; i++)
                {
                    imk_session_groups_[i].resize(CHUNK_COUNT_);
                }
                rgb_imk = inet_.viewByOpId(net_cfg_.RGB_IMK_OP_ID, net_cfg_.RGB_IMK_OP_ID);
                ir_imk = inet_.viewByOpId(net_cfg_.IR_IMK_OP_ID, net_cfg_.IR_IMK_OP_ID);
                icore = inet_.viewExcept({0, net_cfg_.RGB_IMK_OP_ID, net_cfg_.IR_IMK_OP_ID}); // view except input+imagemakes
                if (OPT_DEBUG_FLAGS & BIT_MASK_1)
                {
                    rgb_imk.toNetwork().dumpJsonToFile("imk0.json");
                    ir_imk.toNetwork().dumpJsonToFile("imk1.json");
                }
            }
            else
            {
                // FD编译出来的***C不带表正确的shape，需要自己手动重新赋值netinfo_.i_cubic，否则后处理会死给你看。
                Cubic temp;
                temp.w = NET_W_;
                temp.h = NET_H_;
                temp.c = 3;
                netinfo_.i_cubic[0] = temp;
                netinfo_.i_cubic[1] = temp;
                icore = inet_; // view except output
            }
            if (netinfo_.ImageMake_on)
            {
                // 准备多NetworkView连接，为了获得网络输入ftmp大小和多输入偏移地址，伪初始化
                icraft::xrt::Session dummy_icore_session = icraft::xrt::Session::Create<BackendType, icraft::xrt::HostBackend>(icore, {this->device_, icraft::xrt::HostDevice::Default()});
                auto dummy_icore_buyibackend = dummy_icore_session->backends[0].cast<BackendType>();
                if (OPT_MERGE_OPS)
                    dummy_icore_buyibackend.speedMode();
                if (OPT_COMPRESS_FTMP)
                    dummy_icore_buyibackend.compressFtmp();
                auto icore_ftmp_segment = dummy_icore_buyibackend->logic_segment_map.at(icraft::xrt::Segment::FTMP);
                auto icore_ftmp_size = icore_ftmp_segment->byte_size;
                auto icore_ftmp_chunk = this->device_.defaultMemRegion().malloc(icore_ftmp_size);
                std::cout << "icore ftmp size is: " << icore_ftmp_size << ", phy addr: " << icore_ftmp_chunk->begin.addr() << std::endl;
                auto icore_input_segment = dummy_icore_buyibackend->logic_segment_map.at(icraft::xrt::Segment::INPUT);
                auto icore_input_size = icore_input_segment->byte_size;
                std::cout << "icore input size is: " << icore_input_size << std::endl;
                auto ftmp_id = icore.inputs()[1]->v_id;
                std::cout << "first input ftmp id is:" << icore.inputs()[0]->v_id << ", second input ftmp id is: " << ftmp_id << std::endl;
                auto offset = icore_input_segment->info_map.at(ftmp_id)->logic_addr - icore_input_segment->logic_addr;
                std::cout << "second input offset is: " << offset << std::endl;
                // 申请PLDDR，用于icore的输入复用imk0&imk1的输出
                this->buffer_manager_.createPLDDRChunkGroup("icore_buf_group", this->device_, icore_input_size);
                // 构建sessions
                for (int i = 0; i < CHUNK_COUNT_; i++)
                {
                    // RGB
                    imk_session_groups_[0][i] = icraft::xrt::Session::Create<BackendType, icraft::xrt::HostBackend>(rgb_imk, {this->device_, icraft::xrt::HostDevice::Default()});
                    // IR
                    imk_session_groups_[1][i] = icraft::xrt::Session::Create<BackendType, icraft::xrt::HostBackend>(ir_imk, {this->device_, icraft::xrt::HostDevice::Default()});
                    // ICORE
                    icore_sessions_[i] = icraft::xrt::Session::Create<BackendType, icraft::xrt::HostBackend>(icore, {this->device_, icraft::xrt::HostDevice::Default()});
                    auto rgb_imk_buyibackend = imk_session_groups_[0][i]->backends[0].cast<BackendType>();
                    auto ir_imk_buyibackend = imk_session_groups_[1][i]->backends[0].cast<BackendType>();
                    auto icore_buyibackend = icore_sessions_[i]->backends[0].cast<BackendType>();
                    if (OPT_MERGE_OPS)
                        icore_buyibackend.speedMode();
                    if (OPT_COMPRESS_FTMP)
                        icore_buyibackend.compressFtmp();
                    // 将同一组imagemake和icore的输入输出连接起来
                    auto &icore_memchunk = this->buffer_manager_.getChunk("icore_buf_group", i);
                    std::cout << "icore input chunk phys addr: " << icore_memchunk->begin.addr() << std::endl;
                    rgb_imk_buyibackend.userSetSegment(icore_memchunk, icraft::xrt::Segment::OUTPUT);
                    ir_imk_buyibackend.userSetSegment(icore_memchunk, icraft::xrt::Segment::OUTPUT, offset);
                    icore_buyibackend.userSetSegment(icore_memchunk, icraft::xrt::Segment::INPUT);
                    // 节约内存，复用ftmp段空间
                    icore_buyibackend.userSetSegment(icore_ftmp_chunk, icraft::xrt::Segment::FTMP);
                    // 开启计时功能
                    imk_session_groups_[0][i].enableTimeProfile(true);
                    imk_session_groups_[1][i].enableTimeProfile(true);
                    icore_sessions_[i].enableTimeProfile(true);

                    imk_session_groups_[0][i].apply();
                    imk_session_groups_[1][i].apply();
                    icore_sessions_[i].apply();

                    rgb_imk_buyibackend.log();
                    ir_imk_buyibackend.log();
                    icore_buyibackend.log();

                    // DEBUG CODE, delete after fixing!
                    // auto imk_ofm_id = rgb_imk.outputs()[0]->v_id;
                    // auto buyibackend = imk_session_groups_[0][i]->backends[0].template cast<BackendType>();
                    // uint64_t imk0_write_addr = buyibackend->forward_info->value_map.at(imk_ofm_id)->phy_addr;
                    // LOG_DEBUG(LogP, "SESS#{}, IMK0 output id={} write address: {:#x}", i, imk_ofm_id, imk0_write_addr);
                    // DEBUG CODE END
                }
            }
            else
            {
                // 构建sessions
                for (int i = 0; i < CHUNK_COUNT_; i++)
                {
                    // ICORE
                    icore_sessions_[i] = icraft::xrt::Session::Create<BackendType, icraft::xrt::HostBackend>(icore, {this->device_, icraft::xrt::HostDevice::Default()});
                    auto icore_buyibackend = icore_sessions_[i]->backends[0].cast<BackendType>();
                    if (OPT_MERGE_OPS)
                        icore_buyibackend.speedMode();
                    if (OPT_COMPRESS_FTMP)
                        icore_buyibackend.compressFtmp();
                    // 开启计时功能
                    icore_sessions_[i].enableTimeProfile(true);
                    icore_sessions_[i].apply();
                    // icore_buyibackend.log();
                }
            }

            LOG_INFO(LogP, "CFTIcoreActor initialized with {} sessions. ImageMakeOn=<{}>", CHUNK_COUNT_, netinfo_.ImageMake_on);
        }
        ~CFTIcoreActor()
        {
            this->stop();
        }

    protected:
        void loop() override
        {
            startfps_ = std::chrono::steady_clock::now();
            std::stringstream ss;
            ss << std::this_thread::get_id();
            uint64_t id = std::stoull(ss.str());
            LOG_INFO(LogP, "Icore thread start!, id={}", id);
            while (!this->stop_flag_)
            {
                auto t_start = std::chrono::steady_clock::now();
                MultiSourceInputMessage input_msg;
                if (!input_queue_.wait_and_pop(input_msg))
                {
                    throw std::runtime_error("IcoreActor: Failed to pop from input queue");
                }
                LOG_DEBUG(LogP, "Received message from source {}, ts={}ms.", input_msg.meta.source_id, input_msg.meta.timestamp);
                auto t_end_pop = std::chrono::steady_clock::now();
                auto wait_pop_duration = std::chrono::duration_cast<std::chrono::microseconds>(t_end_pop - t_start);
                // Create the message on the heap using std::make_unique
                IcoreMessageForPost post_msg;
                // remember to Use the -> operator to access members of the pointed-to object
                post_msg.meta.source_id = DISPLAY_INDEX_;
                post_msg.meta.error_input = input_msg.meta.error_frame;
                post_msg.meta.error_frame = input_msg.meta.error_frame;
                post_msg.meta.timestamp = input_msg.meta.timestamp;

                post_msg.meta.buffer_index = input_msg.buffer_indices[DISPLAY_INDEX_];
                post_msg.meta.chunk_group_id = input_msg.chunk_group_ids[DISPLAY_INDEX_];
                // 因为不把IR一路的图像输入VPU压缩，所以提前释放IR的memchunk
                this->buffer_manager_.returnIndex(input_msg.chunk_group_ids[DISCARD_INDEX_], input_msg.buffer_indices[DISCARD_INDEX_]);
                // 运行红外可将光双模态检测网络CFT的前向
                auto icore_start = std::chrono::steady_clock::now();
                if (OPT_DEBUG_FLAGS & BIT_MASK_2)
                {
                    dumpImkOutAsImage(input_msg.icore_tensors[0], "./io/output/icore_input", 0);
                    dumpImkOutAsImage(input_msg.icore_tensors[1], "./io/output/icore_input", 1);
                }
                post_msg.icore_tensors = icore_sessions_[input_msg.buffer_indices[DISPLAY_INDEX_]].forward(input_msg.icore_tensors);
                for (auto &&t : post_msg.icore_tensors)
                {
                    t.waitForReady(std::chrono::milliseconds(10000)); // 同步
                }
                if (OPT_DEBUG_FLAGS & BIT_MASK_2)
                {
                    // dumpOutputFtmp(inet_.viewExcept({0, net_cfg_.RGB_IMK_OP_ID, net_cfg_.IR_IMK_OP_ID}), post_msg.icore_tensors, "SFB", "./logs/icore_ftmp_dump/");
                    dumpFtmps("cft", icore_sessions_[input_msg.buffer_indices[DISPLAY_INDEX_]]->backends[0].template cast<BackendType>());
                    /*
                    auto &memchunk = this->buffer_manager_.getChunk(post_msg.chunk_group_id, post_msg.buffer_index);
                    // dump out the icore input memchunk data
                    auto phy_addr = memchunk->begin.addr();
                    size_t bytesize = 640 * 640 * 4;
                    std::shared_ptr<int8_t[]> input0 = std::shared_ptr<int8_t[]>(new int8_t[bytesize]{0});
                    memchunk.read(reinterpret_cast<char *>(input0.get()), 0, bytesize);
                    std::ofstream out_file0("./logs/icore_input_memchunk/2590.ftmp", std::ios::out | std::ios::binary);
                    out_file0.write(reinterpret_cast<char *>(input0.get()), bytesize);
                    out_file0.close();
                    std::shared_ptr<int8_t[]> input1 = std::shared_ptr<int8_t[]>(new int8_t[bytesize]{0});
                    memchunk.read(reinterpret_cast<char *>(input1.get()), bytesize, bytesize);
                    std::ofstream out_file1("./logs/icore_input_memchunk/2592.ftmp", std::ios::out | std::ios::binary);
                    out_file1.write(reinterpret_cast<char *>(input1.get()), bytesize);
                    out_file1.close();
                    */
                }
                auto icore_end = std::chrono::steady_clock::now();
                auto icore_duration = std::chrono::duration_cast<std::chrono::microseconds>(icore_end - icore_start);
                this->device_.reset(1);

                // 将结果传递给下游
                output_queue_.push(std::move(post_msg));
                LOG_DEBUG(LogP, "[{}][Buff{}] wait={}ms+icore={}ms",
                          input_msg.meta.source_id,
                          input_msg.buffer_indices[DISPLAY_INDEX_],
                          float(wait_pop_duration.count()) / 1000,
                          float(icore_duration.count()) / 1000);
            }
        }

    public:
        std::vector<std::vector<icraft::xrt::Session>> &getImkSessionGroups()
        {
            return imk_session_groups_;
        }

        std::vector<icraft::xrt::Session> &getIcoreSessions()
        {
            return icore_sessions_;
        }

        std::vector<icraft::xir::NetworkView> &getImkNetworkViews()
        {
            if (netinfo_.ImageMake_on)
            {
                static icraft::xir::NetworkView rgb_imk = inet_.viewByOpId(net_cfg_.RGB_IMK_OP_ID, net_cfg_.RGB_IMK_OP_ID);
                static icraft::xir::NetworkView ir_imk = inet_.viewByOpId(net_cfg_.IR_IMK_OP_ID, net_cfg_.IR_IMK_OP_ID);
                static std::vector<icraft::xir::NetworkView> imk_views = {rgb_imk, ir_imk};
                return imk_views;
            }
            else
            {
                static std::vector<icraft::xir::NetworkView> imk_views = {inet_.view(1), inet_.view(1)};
                return imk_views;
            }
        }

        NetInfo &getNetInfo()
        {
            return netinfo_;
        }

    private:
        // constants
        const int icore_id_;
        const int FRAME_W_;
        const int FRAME_H_;
        const int NET_W_;
        const int NET_H_;
        const size_t CHUNK_COUNT_;
        int DISPLAY_INDEX_ = 0;
        int DISCARD_INDEX_ = 1;
        // icraft members
        NetInfo yolov5_netinfo_;
        CFTConfig net_cfg_;
        std::string chunk_group_id_;
        // for multisession npu forward
        DeviceType &device_;
        icraft::xir::Network inet_;
        NetInfo netinfo_;
        std::vector<icraft::xrt::Session> network_sessions_;
        std::vector<std::vector<icraft::xrt::Session>> imk_session_groups_;
        std::vector<icraft::xrt::Session> icore_sessions_;

        // for computing
        icraft::xrt::Tensor tensor_placeholder_1_;

        // profiling members
        const int FPS_COUNT_NUM = 10;
        std::atomic<uint64_t> ai_num_ = 0;
        std::atomic<float> ai_fps_ = 0.f;
        std::chrono::steady_clock::time_point startfps_;
    };
} // 添加命名空间结束
