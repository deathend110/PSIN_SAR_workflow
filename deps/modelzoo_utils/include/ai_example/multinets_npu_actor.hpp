#include "ai_example/siamfcpp_tracker.hpp"
#include "ai_example/postprocesses.hpp"
// modelzoo_utils includes
#include "NetInfo.hpp"
#include "icraft_utils.hpp"
#include "et_device.hpp"
#include "bit_masks.hpp"
#include "demo_utils.hpp"
#include "fps_calculator.hpp"
// pipeline includes
#include "pipeline/actor/base_actors.hpp"
#include "pipeline/memory/buffer_manager.hpp"
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

namespace fpai
{
    struct Yolov5Config
    {
        std::string name = "yolov5s";
        float CONF = 0.25;
        float IOU_THRESHOLD = 0.45;
        bool MULTILABEL = false;
        int N_CLASS = 1;
        bool FPGA_NMS = true;
        bool DETPOST = true;
        std::string JSON_PATH;
        std::string RAW_PATH;
    };

    const bool OPT_DRAW = true;    // 是否保存结果
    const bool OPT_SHOW = true;    // 是否显示结果
    constexpr size_t AI_NET_COUNT = 3; // NPUActor中session数量 YOLO, SIAM_NET1, SIAM_NET2

    // constants
    // for GroupMessageForPost
    const int YOLOV5_INDEX = 0;
    const int SIAM_NET1_INDEX = 1;
    const int SIAM_NET2_INDEX = 2;

    // 构建多网络NPU Actor
    template <typename DeviceType, typename BackendType>
    class DetectTrackNPUActor : public BaseActor<InputMessageForIcore, MultiNetMessageForPost<AI_NET_COUNT>>
    {
    public:
        static constexpr std::string_view LogP = "[ICORE]";
        static const int OPT_DEBUG_FLAGS = BIT_MASK_0; //| BIT_MASK_1 | BIT_MASK_2 | BIT_MASK_3 | BIT_MASK_4 | BIT_MASK_5 | BIT_MASK_6 | BIT_MASK_7;

    public:
        // 构造函数加载神经网络并进行多网络拆分
        DetectTrackNPUActor(DeviceType &device,
                            BufferManager &buffer_manager,
                            const Yolov5Config &yolov5_cfg,
                            const SiamFCppConfig &siamfcpp_cfg,
                            const int frame_w,
                            const int frame_h,
                            SiamFCppTracker &tracker)
            : BaseActor<InputMessageForIcore, MultiNetMessageForPost<AI_NET_COUNT>>(buffer_manager),
            device_(device),
              yolov5_cfg_(yolov5_cfg),
              siamfcpp_cfg_(siamfcpp_cfg),
              FRAME_W_(frame_w),
              FRAME_H_(frame_h),
              siam_tracker_(tracker),
              siam_net2_to_init_(true),
              CHUNK_COUNT_{this->buffer_manager_.getChunkCount()}
        {

            // 加载yolov5m网络
            Network inet_yolov5 = loadNetwork(yolov5_cfg.JSON_PATH, yolov5_cfg_.RAW_PATH);
            Network inet_siamfcpp_template = loadNetwork(siamfcpp_cfg_.TEMPLATE_JSON_PATH, siamfcpp_cfg_.TEMPLATE_RAW_PATH);
            Network inet_siamfcpp_search = loadNetwork(siamfcpp_cfg_.SEARCH_JSON_PATH, siamfcpp_cfg_.SEARCH_RAW_PATH);

            // 去除siamfc++-net1输出部分的Cast&PruneAxis,并连接output<->hardop算子
            removeOutputCast(inet_siamfcpp_template, false); // false指是否开启MMU
            // 去除siamfc++-net2输入部分的Cast&AlignAxis,并连接input<->hardop算子
            removeInputCast(inet_siamfcpp_search, false); // false指是否开启MMU
            // 去除siamfc++-net2输出部分的Cast&PruneAxis,并连接output<->hardop算子
            removeOutputCast(inet_siamfcpp_search, false); // false指是否开启MMU
            // 将网络拆分为imagemake和icore
            image_make_ = inet_yolov5.view(1, 2);                                             // imk
            NetworkView yolov5_icore = inet_yolov5.view(2);                                   // view掉imk
            NetworkView siamfcpp_template_icore = inet_siamfcpp_template.viewExcept({0, 72}); // view掉imk
            NetworkView siamfcpp_search_icore = inet_siamfcpp_search.viewExcept({0, 134});    // view掉imk

            yolov5_netinfo_ = NetInfo(inet_yolov5);
            siamfcpp_template_netinfo_ = NetInfo(inet_siamfcpp_template);
            siamfcpp_search_netinfo_ = NetInfo(inet_siamfcpp_search);
            // 从json文件中读取输出层的缩放系数，网络输出的是定点数据，需要该系数来转换为浮点数据
            siamfcpp_out_normratio_ = getOutputsNormratio(siamfcpp_search_icore);
            spdlog::info("siamfcpp net 2 out normratio:");
            for (const auto &nr : siamfcpp_out_normratio_)
            {
                spdlog::info("  {}", nr);
            }

            // 初始化net2_session，用于计算内存复用chunk尺寸
            auto sess_net2 = icraft::xrt::Session::Create<BackendType, icraft::xrt::HostBackend>(siamfcpp_search_icore, {this->device_, icraft::xrt::HostDevice::Default()});
            auto buyi_backend_net2 = sess_net2->backends[0].template cast<BackendType>();

            // const uint64_t imk_output_size= FRAME_H * FRAME_W * 4;

            // 内存复用net2-申请chunk
            auto net2_input_segment = buyi_backend_net2->logic_segment_map.at(icraft::xrt::Segment::INPUT);
            net2_input_size_ = net2_input_segment->byte_size;
            auto ftmp_id = siamfcpp_search_icore.inputs()[1]->v_id;
            // std::cout << "second input ftmp id is: " << ftmp_id << std::endl;
            net2_input2nd_offset_ = net2_input_segment->info_map.at(ftmp_id)->logic_addr - net2_input_segment->logic_addr;

            // 申请多块PLDDR，用于：①3个网络输入复用imk的输出  ②siamfc++net2其中两个输入复用net1的输出
            chunk_group_id_ = "plddr_net2_input_pool";
            this->buffer_manager_.createPLDDRChunkGroup(chunk_group_id_, this->device_, net2_input_size_);

            // 申请多块PLDDR，用于：①3个网络输入复用imk的输出  ②siamfc++net2其中两个输入复用net1的输出
            auto net2_buf_group = std::vector<icraft::xrt::MemChunk>(CHUNK_COUNT_);
            for (int i = 0; i < CHUNK_COUNT_; i++)
            {
                auto net2_input_chunk = device.defaultMemRegion().malloc(net2_input_size_);
                net2_buf_group[i] = net2_input_chunk;
            }

            // 构建多个session
            spdlog::info("Creating {} sessions for icore...", CHUNK_COUNT_);
            imk_sessions_ = std::vector<icraft::xrt::Session>(CHUNK_COUNT_);
            yolov5_sessions_ = std::vector<icraft::xrt::Session>(CHUNK_COUNT_);
            siamfcpp_template_sessions_ = std::vector<icraft::xrt::Session>(CHUNK_COUNT_);
            siamfcpp_search_sessions_ = std::vector<icraft::xrt::Session>(CHUNK_COUNT_);

            for (int i = 0; i < CHUNK_COUNT_; i++)
            {
                imk_sessions_[i] = icraft::xrt::Session::Create<BackendType, icraft::xrt::HostBackend>(image_make_, {this->device_, icraft::xrt::HostDevice::Default()});
                yolov5_sessions_[i] = icraft::xrt::Session::Create<BackendType, icraft::xrt::HostBackend>(yolov5_icore, {this->device_, icraft::xrt::HostDevice::Default()});
                siamfcpp_template_sessions_[i] = icraft::xrt::Session::Create<BackendType, icraft::xrt::HostBackend>(siamfcpp_template_icore, {this->device_, icraft::xrt::HostDevice::Default()});
                siamfcpp_search_sessions_[i] = icraft::xrt::Session::Create<BackendType, icraft::xrt::HostBackend>(siamfcpp_search_icore, {this->device_, icraft::xrt::HostDevice::Default()});

                auto bybe_imk = imk_sessions_[i]->backends[0].cast<BackendType>();
                auto bybe_yolov5 = yolov5_sessions_[i]->backends[0].cast<BackendType>();
                auto bybe_siamfcpp_template = siamfcpp_template_sessions_[i]->backends[0].cast<BackendType>();
                auto bybe_siamfcpp_search = siamfcpp_search_sessions_[i]->backends[0].cast<BackendType>();
                icore_session_count_ = 3; //
                // 将同一组imagemake和icore的输入输出连接起来
                bybe_imk.userSetSegment(this->buffer_manager_.getChunk(chunk_group_id_, i), icraft::xrt::Segment::OUTPUT);
                bybe_yolov5.userSetSegment(this->buffer_manager_.getChunk(chunk_group_id_, i), icraft::xrt::Segment::INPUT);
                bybe_siamfcpp_template.userSetSegment(this->buffer_manager_.getChunk(chunk_group_id_, i), icraft::xrt::Segment::INPUT);

                // 将同一组 net1输出和net2输入连接起来
                bybe_siamfcpp_template.userSetSegment(this->buffer_manager_.getChunk(chunk_group_id_, i), icraft::xrt::Segment::OUTPUT, net2_input2nd_offset_);
                bybe_siamfcpp_search.userSetSegment(this->buffer_manager_.getChunk(chunk_group_id_, i), icraft::xrt::Segment::INPUT, 0);

                imk_sessions_[i].enableTimeProfile(true);
                siamfcpp_template_sessions_[i].enableTimeProfile(true);
                siamfcpp_search_sessions_[i].enableTimeProfile(true);
                yolov5_sessions_[i].enableTimeProfile(true);

                bybe_yolov5.speedMode();
                bybe_siamfcpp_template.speedMode();
                bybe_siamfcpp_search.speedMode();

                imk_sessions_[i].apply();
                siamfcpp_template_sessions_[i].apply();
                siamfcpp_search_sessions_[i].apply();
                yolov5_sessions_[i].apply();
            }

            Operation WarpAffine_net0 = yolov5_netinfo_.WarpAffine_;
            auto WarpAffine_M_inversed = WarpAffine_net0->attrs().at("M_inversed").cast<Array<Array<FloatImm>>>();
            warpaffine_ratio_w_ = WarpAffine_M_inversed[0][0];
            warpaffine_ratio_h_ = WarpAffine_M_inversed[1][1];
            warpaffine_bias_w_ = WarpAffine_M_inversed[0][2];
            warpaffine_bias_h_ = WarpAffine_M_inversed[1][2];
            spdlog::info("Yolov5: warpaffine_ratio_w_={}, warpaffine_ratio_h_={}, warpaffine_bias_w_={}, warpaffine_bias_h_={}",
                         warpaffine_ratio_w_, warpaffine_ratio_h_, warpaffine_bias_w_, warpaffine_bias_h_);
        }

        ~DetectTrackNPUActor() override
        {
            spdlog::info("DetectTrackNPUActor destructor called.");
            this->stop();
            spdlog::info("DetectTrackNPUActor stopped.");
        }

    protected:
        void loop() override
        {
            using namespace std::chrono;
            using namespace std::chrono_literals;

            startfps_ = std::chrono::steady_clock::now();
            std::stringstream ss;
            ss << std::this_thread::get_id();
            uint64_t id = std::stoull(ss.str());
            LOG_INFO(LogP, "Icore thread start!, id={}", id);
            while (!this->stop_flag_)
            {
                auto t_start = std::chrono::steady_clock::now();
                InputMessageForIcore input_msg;
                if (!this->input_queue_->wait_and_pop(input_msg))
                {
                    LOG_ERROR(LogP, "Input queue is closed, DetectTrackNPUActor loop is stopping.");
                    break; // 队列关闭且为空，退出线程
                }
                auto t_end_pop = std::chrono::steady_clock::now();
                auto wait_pop_duration = std::chrono::duration_cast<std::chrono::microseconds>(t_end_pop - t_start);
                // Create the message on the heap using std::make_unique
                MultiNetMessageForPost<AI_NET_COUNT> post_msg;
                // remember to Use the -> operator to access members of the pointed-to object
                post_msg.meta = input_msg.meta;
                for (int i = 0; i < AI_NET_COUNT; i++)
                {
                    post_msg.is_icore_executed[i] = false;
                }
                auto t_start_sot = std::chrono::steady_clock::now();
                if (siam_tracker_.get_state() == TrackerState::NONE)
                {
                    spdlog::debug("[ICORE] TrackerState is NONE");
                    // 情况1-无单目标跟踪，直接执行yolov5检测
                    std::vector<std::vector<float>> yolov5_inv_matrix = {// 仿射变换逆矩阵
                                                                         {float(warpaffine_ratio_w_), 0.0f, float(warpaffine_bias_w_)},
                                                                         {0.0f, float(warpaffine_ratio_h_), float(warpaffine_bias_h_)}};

                    Operation WarpAffine_yolov5 = yolov5_netinfo_.WarpAffine_;
                    yolov5_sessions_[input_msg.meta.buffer_index]->backends[0].cast<BackendType>().initOp(WarpAffine_yolov5);
                    if (yolov5_netinfo_.WarpAffine_on)
                        fpgaWarpaffine(yolov5_inv_matrix, this->device_);
                    // std::cout << "yolov5_fpga_warpaffine done;" << std::endl;
                    // 从yolo中获取目标框，运行YOLO.forward, YOLO.detpost, siam_pre, siam_net1
                    auto net0_start = std::chrono::steady_clock::now();
                    auto net0_tensors = yolov5_sessions_[input_msg.meta.buffer_index].forward(input_msg.image_tensors);
                    auto net0_end = std::chrono::steady_clock::now();
                    auto net0_duration = std::chrono::duration_cast<std::chrono::microseconds>(net0_end - net0_start);
                    // 手动同步
                    for (auto &&tensor : net0_tensors)
                    {
                        tensor.waitForReady(std::chrono::milliseconds(1000));
                    }
                    this->device_.reset(1);
                    // 目标检测后处理
                    auto yolov5_post_results = yolov5_post_detpost_plin(net0_tensors, yolov5_netinfo_,
                                                                        yolov5_cfg_.CONF, yolov5_cfg_.IOU_THRESHOLD, yolov5_cfg_.MULTILABEL,
                                                                        yolov5_cfg_.N_CLASS, this->device_);
                    auto valid = std::get<0>(yolov5_post_results);
                    // std::cout << "valid: " << valid << std::endl;
                    cv::Rect2f bbox = std::get<1>(yolov5_post_results);

                    float x0 = bbox.tl().x * warpaffine_ratio_w_ + warpaffine_bias_w_;
                    float y0 = bbox.tl().y * warpaffine_ratio_h_ + warpaffine_bias_h_;
                    float w0 = bbox.width * warpaffine_ratio_w_;
                    float h0 = bbox.height * warpaffine_ratio_h_;
                    x0 = std::max(0.0f, std::min(float(FRAME_W_ - 10), x0));
                    y0 = std::max(0.0f, std::min(float(FRAME_H_ - 10), y0));
                    w0 = std::max(10.0f, std::min(float(FRAME_W_ - x0), w0));
                    h0 = std::max(10.0f, std::min(float(FRAME_H_ - y0), h0));
                    TargetCXYWH initrect;
                    initrect.cx = round(x0 + w0 / 2);
                    initrect.cy = round(y0 + h0 / 2);
                    initrect.w = w0;
                    initrect.h = h0;
                    // if(initrect.w * initrect.h > 176 * 320) valid = false; // 过滤过大的检测框
                    spdlog::warn("[ICORE] DET bbox(xywh): {:.2f} {:.2f} {:.2f} {:.2f}, transform to frame target(cxywh): {:.2f} {:.2f} {:.2f} {:.2f}", bbox.tl().x, bbox.tl().y, bbox.width, bbox.height, initrect.cx, initrect.cy, initrect.w, initrect.h);
                    auto net0_post_end = std::chrono::steady_clock::now();
                    auto net0_post_duration = std::chrono::duration_cast<std::chrono::microseconds>(net0_post_end - net0_end);
                    if (valid)
                    {
                        siam_tracker_.siamfc_template_preprocess(initrect); // 初始化跟踪器, 输入初始模板，计算siam_inv_matrix_
                    }
                    post_msg.is_icore_executed[YOLOV5_INDEX] = true;
                    auto net1_pre_end = std::chrono::steady_clock::now();
                    auto net1_pre_duration = std::chrono::duration_cast<std::chrono::microseconds>(net1_pre_end - net0_post_end);
                    if (OPT_DEBUG_FLAGS & BIT_MASK_0)
                    {
                        spdlog::info("[ICORE] DETECT: yolo_forward={:.2f}ms, yolo_post={:.2f}ms, net1_pre={:.2f}ms",
                                     float(net0_duration.count()) / 1000,
                                     float(net0_post_duration.count()) / 1000,
                                     float(net1_pre_duration.count()) / 1000);
                    }
                }
                if (siam_tracker_.get_state() == TrackerState::TEMPLATE)
                {
                    spdlog::debug("[ICORE] TrackerState is TEMPLATE");
                    // 情况2-执行siamfc++net1
                    auto net1_start = std::chrono::steady_clock::now();
                    Operation WarpAffine_net1 = siamfcpp_template_netinfo_.WarpAffine_;
                    siamfcpp_template_sessions_[input_msg.meta.buffer_index]->backends[0].cast<BackendType>().initOp(WarpAffine_net1);
                    if (siamfcpp_template_netinfo_.WarpAffine_on)
                        siam_tracker_.fpga_warpaffine(this->device_);
                    auto warpaffine_end = std::chrono::steady_clock::now();
                    auto warpaffine_duration = std::chrono::duration_cast<std::chrono::microseconds>(warpaffine_end - net1_start);
                    // net1前向推理
                    auto net1_tensors = siamfcpp_template_sessions_[input_msg.meta.buffer_index].forward(input_msg.image_tensors);
                    tensor_placeholder_1_ = net1_tensors[0];
                    tensor_placeholder_2_ = net1_tensors[1];

                    // 内存块复用-同步操作
                    auto net1_out_flag1 = tensor_placeholder_1_.waitForReady(1000ms);
                    auto net1_out_flag2 = tensor_placeholder_2_.waitForReady(1000ms);
                    tensor_placeholder_1_.setReady(net1_out_flag1);
                    tensor_placeholder_2_.setReady(net1_out_flag2);

                    post_msg.icore_tensor_group[SIAM_NET1_INDEX] = net1_tensors;
                    post_msg.is_icore_executed[SIAM_NET1_INDEX] = true;
                    auto net1_forward_end = std::chrono::steady_clock::now();
                    auto net1_forward_duration = std::chrono::duration_cast<std::chrono::microseconds>(net1_forward_end - warpaffine_end);
                    // 再PLDDR上，将两个输出复制到其它buffer上，PLDDR->PLDDR，比如buffer_index=2
                    // idx 0   [░░░░░░█████████████░░░░░░]   <--|    [表示一段完整PLDDR memchunk]
                    // idx 1   [░░░░░░█████████████░░░░░░]   <--|
                    // idx 2   [░░░░░░*************░░░░░░] -----|   siam_net1输出的结果在PLDDR这段，需要复制到其它chunk上
                    // idx 3   [░░░░░░█████████████░░░░░░]   <--|
                    this->buffer_manager_.plDDR_ChunkGroupSync(chunk_group_id_, input_msg.meta.buffer_index, net2_input2nd_offset_, net2_input_size_, this->device_);
                    spdlog::debug("After chunk sync, buffer[{}] chunk group:", input_msg.meta.buffer_index);
                    this->device_.reset(1);

                    auto net1_post_end = std::chrono::steady_clock::now();
                    auto net1_post_duration = std::chrono::duration_cast<std::chrono::microseconds>(net1_post_end - net1_forward_end);
                    siam_tracker_.siamfc_search_preprocess();
                    auto target = siam_tracker_.get_target();
                    spdlog::warn("[ICORE] SOT target(cxywh) changed to: {:.2f} {:.2f} {:.2f} {:.2f}", target.cx, target.cy, target.w, target.h);
                    auto net2_preprocess = std::chrono::steady_clock::now();
                    auto net2_preprocess_duration = std::chrono::duration_cast<std::chrono::microseconds>(net2_preprocess - net1_post_end);
                    if (OPT_DEBUG_FLAGS & BIT_MASK_0)
                    {
                        spdlog::info("[ICORE] Template: net1_initop+warpaffine={:.2f}ms, net1_forward={:.2f}ms, net1_post={:.2f}ms, net2_pre={:.2f}ms",
                                     float(warpaffine_duration.count()) / 1000,
                                     float(net1_forward_duration.count()) / 1000,
                                     float(net1_post_duration.count()) / 1000,
                                     float(net2_preprocess_duration.count()) / 1000);
                    }
                }
                else if (siam_tracker_.get_state() == TrackerState::SEARCH)
                {
                    spdlog::debug("[ICORE] TrackerState is SEARCH");
                    // 情况3-执行siamfc++net2
                    auto net2_start = std::chrono::steady_clock::now();
                    if (siam_net2_to_init_)
                    {
                        Operation WarpAffine_net2 = siamfcpp_search_netinfo_.WarpAffine_;
                        for (int i = 0; i < CHUNK_COUNT_; i++)
                        {
                            siamfcpp_search_sessions_[i]->backends[0].cast<BackendType>().initOp(WarpAffine_net2);
                        }
                        siam_net2_to_init_ = false;
                    }
                    // 配置warpaffine寄存器
                    if (siamfcpp_search_netinfo_.WarpAffine_on)
                        siam_tracker_.fpga_warpaffine(this->device_);

                    auto net2_warpaffine_end = std::chrono::steady_clock::now();
                    auto net2_warpaffine_duration = std::chrono::duration_cast<std::chrono::microseconds>(net2_warpaffine_end - net2_start);
                    // net2：前向推理
                    auto net2_tensors = siamfcpp_search_sessions_[input_msg.meta.buffer_index].forward({input_msg.image_tensors[0], tensor_placeholder_1_, tensor_placeholder_2_});
                    // 手动同步
                    for (auto &&tensor : net2_tensors)
                    {
                        tensor.waitForReady(std::chrono::milliseconds(1000));
                    }
                    this->device_.reset(1);
                    post_msg.icore_tensor_group[SIAM_NET2_INDEX] = net2_tensors;
                    post_msg.is_icore_executed[SIAM_NET2_INDEX] = true;
                    auto net2_forward_end = std::chrono::steady_clock::now();
                    auto net2_forward_duration = std::chrono::duration_cast<std::chrono::microseconds>(net2_forward_end - net2_warpaffine_end);

                    // net2当前帧的后处理
                    siam_tracker_.net2_postprocess_removeoutputcast(post_msg.icore_tensor_group[SIAM_NET2_INDEX], siamfcpp_out_normratio_, FRAME_W_, FRAME_H_);
                    // net2下一帧的前处理
                    siam_tracker_.siamfc_search_preprocess();
                    auto target = siam_tracker_.get_target();
                    spdlog::warn("[ICORE] After search update target(cxywh): {:.2f} {:.2f} {:.2f} {:.2f}", target.cx, target.cy, target.w, target.h);
                    if (target.cx > FRAME_W_ - 50 || target.cx < 50 || target.cy > FRAME_H_ - 50 || target.cy < 50)
                    {
                        spdlog::error("[ICORE] WARNING! Tracking target out of frame boundary, re-init tracker!");
                        siam_tracker_.reset();
                        siam_net2_to_init_ = true;
                    }
                    auto net2_postprocess_end = std::chrono::steady_clock::now();
                    auto net2_postprocess_duration = std::chrono::duration_cast<std::chrono::microseconds>(net2_postprocess_end - net2_forward_end);

                    if (OPT_DEBUG_FLAGS & BIT_MASK_0)
                    {
                        spdlog::info("[ICORE] SEARCH: net2_initop+warpaffine={:.2f}ms, net2_forward={:.2f}ms, net2_postprocess={:.2f}ms",
                                     float(net2_warpaffine_duration.count()) / 1000,
                                     float(net2_forward_duration.count()) / 1000,
                                     float(net2_postprocess_duration.count()) / 1000);
                    }
                }
                else
                {
                    // 情况4-无跟踪任务，空操作
                    spdlog::warn("[ICORE] No tracking task, idle...");
                }
                this->output_queue_->push(std::move(post_msg)); // now post_msg is nullptr
                //------------------------------------------------------------------//
                //       AI帧数计算
                //------------------------------------------------------------------//
                ai_num_++;
                if (ai_num_ == FPS_COUNT_NUM)
                {
                    ai_num_ = 0;
                    auto duration = std::chrono::duration_cast<microseconds>(std::chrono::steady_clock::now() - startfps_) / FPS_COUNT_NUM;
                    ai_fps_ = 1000 / (float(duration.count()) / 1000);
                    startfps_ = std::chrono::steady_clock::now();
                    // std::cout << "fps: " << fps << std::endl;
                }
                auto t_end = std::chrono::steady_clock::now();
                auto t_duration = std::chrono::duration_cast<std::chrono::microseconds>(t_end - t_start);
                auto t_pop_duration = std::chrono::duration_cast<std::chrono::microseconds>(t_end_pop - t_start);
                auto t_icore_duration = std::chrono::duration_cast<std::chrono::microseconds>(t_end - t_end_pop);
                if (OPT_DEBUG_FLAGS & BIT_MASK_0)
                {
                    int tasks_left = this->output_queue_->size();
                    spdlog::info("[ICORE][{}] T({:.2f}ms)=pop({:.2f}ms)+icore({:.2f}ms); icore_outqueue_count={}; buff[{}]; ai_fps={:.2f}",
                                 input_msg.meta.source_id,
                                 float(t_duration.count()) / 1000,
                                 float(t_pop_duration.count()) / 1000,
                                 float(t_icore_duration.count()) / 1000,
                                 tasks_left,
                                 input_msg.meta.buffer_index,
                                 ai_fps_);
                }
            }
        }

    public:
        NetInfo getYolov5NetInfo()
        {
            return yolov5_netinfo_;
        }
        NetInfo getSiamFCppTemplateNetInfo()
        {
            return siamfcpp_template_netinfo_;
        }
        NetInfo getSiamFCppSearchNetInfo()
        {
            return siamfcpp_search_netinfo_;
        }

        std::vector<icraft::xrt::Session> &getImkSessions()
        {
            return imk_sessions_;
        }

        std::vector<icraft::xrt::Session> &getYolov5Sessions()
        {
            return yolov5_sessions_;
        }

        std::vector<icraft::xrt::Session> &getSiamFCppTemplateSessions()
        {
            return siamfcpp_template_sessions_;
        }

        std::vector<icraft::xrt::Session> &getSiamFCppSearchSessions()
        {
            return siamfcpp_search_sessions_;
        }

        int getIcoreSessionCount() const
        {
            return icore_session_count_;
        }

    private:
        // constants
        const int FRAME_W_;
        const int FRAME_H_;
        // icraft members
        DeviceType &device_;
        NetInfo yolov5_netinfo_;
        NetInfo siamfcpp_template_netinfo_;
        NetInfo siamfcpp_search_netinfo_;
        Yolov5Config yolov5_cfg_;
        SiamFCppConfig siamfcpp_cfg_;
        std::string chunk_group_id_;
        icraft::xir::NetworkView image_make_; // fpga imagemake
        // for multisession npu forward
        int icore_session_count_;
        std::vector<icraft::xrt::Session> imk_sessions_;
        std::vector<icraft::xrt::Session> yolov5_sessions_;
        std::vector<icraft::xrt::Session> siamfcpp_template_sessions_;
        std::vector<icraft::xrt::Session> siamfcpp_search_sessions_;
        std::vector<float> siamfcpp_out_normratio_;
        // for computing
        icraft::xrt::Tensor tensor_placeholder_1_;
        icraft::xrt::Tensor tensor_placeholder_2_;
        const size_t CHUNK_COUNT_;
        size_t net2_input_size_ = 0;
        size_t net2_input2nd_offset_ = 0;

        SiamFCppTracker &siam_tracker_;

        float warpaffine_ratio_w_;
        float warpaffine_ratio_h_;
        int warpaffine_bias_w_;
        int warpaffine_bias_h_;
        bool siam_net2_to_init_;
        // profiling members
        const int FPS_COUNT_NUM = 10;
        std::atomic<uint64_t> ai_num_ = 0;
        std::atomic<float> ai_fps_ = 0.f;
        std::chrono::steady_clock::time_point startfps_;
    };
}
