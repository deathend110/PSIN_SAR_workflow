#pragma once
#include "pipeline/base/enums.hpp"
#include "pipeline/io/output/istream_sink.hpp"

// ffmpeg 相关头文件
extern "C"
{
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
#include <libavutil/time.h>
}

#include <spdlog/spdlog.h>
#include <fstream>
#include <string>
#include <memory>
#include <iostream>

struct RTMPConfig
{
    std::string url = "rtmp://192.168.125.200:1935/live/123"; // RTMP服务器URL
    int width = 1920;                                         // 视频宽度
    int height = 1080;                                        // 视频高度
    int fps = 60;                                             // 帧率
    int bitrate = 8000000;                                    // 比特率
    std::string codec = "h264";
    AVCodecID codec_id = AV_CODEC_ID_H264; // 编码格式
    AVPixelFormat pixel_format = AV_PIX_FMT_UYVY422; // 像素格式

};

class StreamToRTMP : public IStreamSink
{
public:
    StreamToRTMP(int sink_id, const RTMPConfig &rtmp_config = RTMPConfig())
        : IStreamSink(sink_id, OUTPUT_SINK::DISK),
          config_(rtmp_config),
          file_size_(0),
          is_vstream_initialized_{false},
          opt_(NULL),
          ofmt_(NULL),
          ofmt_ctx_(NULL),
          out_stream_(NULL),
          codec_(NULL),
          is_waiting_i_frame_(false),
          last_dts_(0)
    {
    }

    int open(const std::string &url) override
    {
        //-------------------------------------//
        //       rtmp push streaming setup
        //-------------------------------------//
        // const char* url = "rtmp://192.168.125.200:1935/live/123";
        avformat_network_init();
        // std::cout << "avformat_network_init" << std::endl;
        // Define output format context
        avformat_alloc_output_context2(&ofmt_ctx_, NULL, "flv", url.c_str()); // rtmp
        // avformat_alloc_output_context2(&ofmt_ctx_, NULL, NULL, url.c_str()); // rtmp
        // std::cout << "avformat_alloc_output_context2" << std::endl;
        if (!ofmt_ctx_)
        {
            spdlog::error("Could not create output context!");
            return -1;
        }

        ofmt_ = ofmt_ctx_->oformat;
        // manually define codec of output stream
        codec_ = avcodec_find_encoder(AV_CODEC_ID_H264);
        std::cout << "avcodec_find_encoder" << std::endl;
        out_stream_ = avformat_new_stream(ofmt_ctx_, codec_);
        std::cout << "avformat_new_stream" << std::endl;
        if (!out_stream_)
        {
            std::cout << "Failed allocating output stream" << std::endl;
            auto ret = AVERROR_UNKNOWN;
            return ret;
        }
        return 0;
    }

    int handlePacket(const SinkPacket &packet) override
    {
        AVPacket pkt = { 0 }; // Modern way: zero-initialize the packet struct
        pkt.data = static_cast<uint8_t *>(packet.data);
        spdlog::info("stream2rtmp::handlePacket: data_bytesize={}", packet.size);
        // Initialize rtmp stream on first packet
        if (!is_vstream_initialized_)
        {
            if (is_idr_frame1((uint8_t *)packet.data, packet.size))
            {
                // the first packet from VPU is SPS+PPS, no need to extract from H264
                // int spspps_len = get_spspps_from_h264((uint8_t*)data_ptr, data_bytesize);
                int spspps_len = packet.size;
                // std::cout << "spspps_len: " << spspps_len << std::endl;
                if (spspps_len > 0)
                {
                    spspps_len = packet.size;
                    AVCodecContext *codec_ctx = avcodec_alloc_context3(codec_);
                    // std::cout << "avcodec_alloc_context3" << std::endl;
                    if (!codec_ctx)
                    {
                        spdlog::error("Could not allocate codec context!");
                        return -1;
                    }
                    codec_ctx->codec_tag = 0;
                    codec_ctx->width = config_.width;
                    codec_ctx->height = config_.height;
                    codec_ctx->codec_id = config_.codec_id; // default AV_CODEC_ID_H264;
                    codec_ctx->pix_fmt = config_.pixel_format;  // default AV_PIX_FMT_UYVY422;
                    codec_ctx->codec_type = AVMEDIA_TYPE_VIDEO;
                    codec_ctx->time_base = (AVRational){1, config_.fps}; // Set frame rate to 60 fps
                    codec_ctx->framerate = (AVRational){config_.fps, 1};
                    codec_ctx->bit_rate = config_.bitrate; // default 8000000; // Set bit rate
                    if (ofmt_ctx_->oformat->flags & AVFMT_GLOBALHEADER)
                    {
                        codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
                    }

                    codec_ctx->extradata_size = spspps_len;
                    codec_ctx->extradata = (uint8_t *)av_malloc(spspps_len + AV_INPUT_BUFFER_PADDING_SIZE);
                    spdlog::debug("AV_INPUT_BUFFER_PADDING_SIZE={}", AV_INPUT_BUFFER_PADDING_SIZE);
                    if (codec_ctx->extradata == NULL)
                    {
                        spdlog::error("could not av_malloc the video params extradata!");
                        return -1;
                    }
                    memcpy(codec_ctx->extradata, packet.data, spspps_len);

                    avcodec_parameters_from_context(out_stream_->codecpar, codec_ctx);
                    // std::cout << "avcodec_parameters_from_context" << std::endl;
                    avcodec_free_context(&codec_ctx);
                    // std::cout << "avcodec_free_context" << std::endl;
                    // DUMP output format
                    av_dump_format(ofmt_ctx_, 0, config_.url.c_str(), 1);
                    // std::cout << "av_dump_format" << std::endl;
                    // Open output URL
                    if (!(ofmt_->flags & AVFMT_NOFILE))
                    {
                        int ret = avio_open(&ofmt_ctx_->pb, config_.url.c_str(), AVIO_FLAG_WRITE);
                        if (ret < 0)
                        {
                            char errbuf[128];
                            av_strerror(ret, errbuf, sizeof(errbuf));
                            spdlog::error("Could not open output URL '{}': {}", config_.url, errbuf);
                            // Clean up allocated context on failure
                            avformat_free_context(ofmt_ctx_);
                            ofmt_ctx_ = NULL;
                            return -1;
                        }
                    }
                    // write file header
                    av_dict_set(&opt_, "flvflags", "no_duration_filesize", 0);
                    av_dict_set(&opt_, "rtmp_chunk_size", "4096", 0);
                    av_dict_set(&opt_, "tune", "zerolatency", 0);
                    av_dict_set(&opt_, "preset", "ultrafast", 0);
                    av_dict_set(&opt_, "framerate", "60", 0);
                    // av_dict_set(&opt, "use_wallclock_as_timestamps", "1", 0);
                    auto rett = avformat_write_header(ofmt_ctx_, &opt_);
                    av_dict_free(&opt_);
                    // std::cout << "avformat_write_header" << std::endl;
                    if (rett < 0)
                    {
                        char errbuf[128];
                        av_strerror(rett, errbuf, sizeof(errbuf));
                        spdlog::error("Error occurred when opening output URL: {}", errbuf);
                        // Clean up allocated resources on failure
                        if (!(ofmt_->flags & AVFMT_NOFILE)) {
                            avio_closep(&ofmt_ctx_->pb);
                        }
                        avformat_free_context(ofmt_ctx_);
                        ofmt_ctx_ = NULL;
                        return -1;
                    }
                    is_waiting_i_frame_ = true;
                    is_vstream_initialized_ = true;
                }
            }
        }
        if (is_vstream_initialized_)
        {
            bool is_I_frame = is_idr_frame1((uint8_t *)packet.data, packet.size);
            spdlog::debug("is_I_frame: {}", is_I_frame);
            pkt.flags |= is_I_frame ? AV_PKT_FLAG_KEY : 0;
            if (is_waiting_i_frame_)
            {
                if (0 == (pkt.flags & AV_PKT_FLAG_KEY))
                {
                    std::cout << "Not I frame, skip" << std::endl;
                    return 0;
                }
                else
                {
                    is_waiting_i_frame_ = false;
                }
            }
            pkt.size = packet.size;
            pkt.stream_index = 0;
            pkt.duration = av_rescale_q(1, AVRational{1, 60}, ofmt_ctx_->streams[0]->time_base);
            // Calculate PTS and DTS
            pkt.pts = last_dts_ + pkt.duration;
            pkt.dts = pkt.pts;
            last_dts_ = pkt.dts;
            pkt.pos = -1;
            spdlog::debug("timing(pts dts duration): {} {} {}", pkt.pts, pkt.dts, pkt.duration);
            auto ret = av_interleaved_write_frame(ofmt_ctx_, &pkt);
            if (ret < 0)
            {
                std::cerr << "Error muxing packet" << std::endl;
                return -1;
            }
            av_packet_unref(&pkt);
        }
        return 0;
    }

    void close() override
    {
        if (is_vstream_initialized_)
        {
            av_write_trailer(ofmt_ctx_);
            if (!(ofmt_->flags & AVFMT_NOFILE))
            {
                /* Close the output file. */
                avio_closep(&ofmt_ctx_->pb);
            }
            avformat_free_context(ofmt_ctx_);
            ofmt_ctx_ = NULL;
            is_vstream_initialized_ = false;
            std::cout << "RTMP streaming closed, total file size: " << file_size_ << " bytes" << std::endl;
        }
        avformat_network_deinit();
        std::cout << "avformat_network_deinit" << std::endl;
    }

private:
    uint64_t file_size_ = 0;
    RTMPConfig config_;
    // constants
    bool is_vstream_initialized_;
    bool is_waiting_i_frame_;
    int64_t last_dts_ = 0;
    AVDictionary *opt_;
    const AVOutputFormat *ofmt_;
    AVFormatContext *ofmt_ctx_;
    AVStream *out_stream_;
    const AVCodec *codec_;
};
