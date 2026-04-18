#pragma once
#include "pipeline/base/enums.hpp"
#include "pipeline/io/output/istream_sink.hpp"
#include "pipeline/base/ring_queue.hpp"
// for rtsp
#include "liveMedia.hh"
// using Live555FileSink = FileSink; // distinguish from icraft::xrt::FileSink
#include "live_rtsp_server.hpp"

#include <spdlog/spdlog.h>
#include <fstream>
#include <string>

/**
*   集成RTSP Server，用于给上位机提供拉流服务
*   可选：live555\smolrtsp\ffmpeg等
 */

class StreamToRTSPServer : public IStreamSink
{
public:
    StreamToRTSPServer(int sink_id, const std::string &url)
        : IStreamSink(sink_id, OUTPUT_SINK::DISK),
          url_{url},
          file_size_(0),
    {
        OutPacketBuffer::maxSize = 1400000; // newly set
    }

    int open(const std::string& url) override
    {
        //-------------------------------------//
        //       live rtsp server setup
        //-------------------------------------//
        // 1) 创建live555基础环境
        scheduler_ = BasicTaskScheduler::createNew();
        env_ = BasicUsageEnvironment::createNew(*scheduler_);
        // 2) 创建RTSP服务器
        rtspServer_ = RTSPServer::createNew(*env_, 8554);
        if (rtspServer_ == NULL)
        {
            *env_ << "Failed to create RTSP server: " << env_->getResultMsg() << "\n";
            return -1;
        }
        // 3) 创建ServerMediaSession，并添加自定义的H264LiveServerMediaSession
        void* data_ptr = nullptr;
        int data_bytesize = 0;
        bool eos = false;
        std::shared_ptr<vpu::Buffer> buffer_ptr;
        buffer_ptr = vpu_encoder_.getOutput(data_ptr, data_bytesize, eos);
        *env << "==============first vpu data_bytesize: " << data_bytesize << "\n";
        printout_hex(static_cast<uint8_t*>(data_ptr), data_bytesize);
        // the first packet from VPU is SPS+PPS, no need to extract from H264
        spspps_len = data_bytesize;
        memcpy(sps_pps, data_ptr, spspps_len); //save SPS/PPS
        // SPS数据压入rQueue
        rQueue_data e_sps;
        e_sps.buffer = sps_pps; // + 4;
        e_sps.len = 24; // - 4;
        if(rQueue_en(rQueue, &e_sps) == 1) {
            throw std::runtime_error("rQueue_en sps failed");
        }
        // PPS数据压入rQueue
        rQueue_data e_pps;
        e_pps.buffer = sps_pps+4+20; // + 4;
        e_pps.len = 8; // - 4;
        if(rQueue_en(rQueue, &e_pps) == 1) {
            throw std::runtime_error("rQueue_en pps failed");
        }
        vpu_encoder.queueOutputBuffer(buffer_ptr);
        std::cout << "============== enqueue spspps done." << std::endl;
        // 输出SPS/PPS以后，会有3个protections
        for(int ii=0;ii<3;ii++) {
            auto buffer_ptr1 = vpu_encoder.getOutput(data_ptr, data_bytesize, eos);
            std::cout << "==============protections #" << ii << ": " << data_bytesize << std::endl;
            printout_hex(static_cast<uint8_t*>(data_ptr), data_bytesize);
            rQueue_data e;
            e.buffer = data_ptr;// + 4;
            e.len = data_bytesize;// - 4;
            if(rQueue_en(rQueue, &e) == 1) {
                throw std::runtime_error("rQueue_en failed");
            }
            vpu_encoder.queueOutputBuffer(buffer_ptr1);
        }
        // test behavior of vpu_encoder.getOutput if not waiting output
        // result: the process will be blocked.
        // auto buffer_ptr1 = vpu_encoder.getOutput(data_ptr, data_bytesize, eos);

        auto on_rtsp_client_enter = [&]() {
            isRTSPClientConnected.store(true);
            std::cout << "on_rtsp_client_enter" << std::endl;
        };
        auto on_rtsp_client_exit = [&]() {
            isRTSPClientConnected.store(false);
            std::cout << "on_rtsp_client_exit" << std::endl;
        };
        const char* stream_name = "liveH264";
        bool reuseFirstSource = true;
        ServerMediaSession *sms = ServerMediaSession::createNew(*env, stream_name, stream_name, "Session streamed by Live555 using ringQueue");
        sms->addSubsession(
            H264LiveServerMediaSubsession::createNew(*env,
                reuseFirstSource,
                on_rtsp_client_enter,
                on_rtsp_client_exit,
                sps_pps + 4,
                20,
                sps_pps + 4 + 20 + 4,
                4)
            );
        rtspServer->addServerMediaSession(sms);
        char *url = rtspServer->rtspURL(sms);
        *env << "Play the stream using url " << url << "\n";
        delete[] url;
    }

    int handlePacket(const EncodedPacket& packet) override
    {
        packet_queue_.push(packet);
        file_size_+= packet.size;
        return 0;
    }

    void close() override
    {

    }
    

protected:
    RingQueue<SinkPacket> packet_queue_;
    TaskScheduler *scheduler_;
    UsageEnvironment *env_;
    RTSPServer *rtspServer_;
    

private:
    uint64_t file_size_ = 0;
    // constants
    const std::string url_;

};
