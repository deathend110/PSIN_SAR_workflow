#pragma once
/*
 * @Version: Icraft-3.31.0
 * @Autor: QIN LIANG
 * @Date: 2025-08-25
 * @LastEditTime: 2025-08-25
 * @Description:
 *
 * Copyright (c) 2023 by Shanghai Fudan Microelectronics Group Co., Ltd ,
 * All Rights Reserved.
 */

#include "pipeline/io/base/input_source.hpp"
#include "pipeline/vpu/mvx-v4l2-controls.h"
#include "opencv2/opencv.hpp"

#include <cstddef>
#include <utility>
#include <spdlog/spdlog.h>

#include <cstdint>
#include <linux/videodev2.h>
#include <mutex>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

#include <thread>
#include <iostream>
#include <fstream>
#include <cstring>
#include <vector>

extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/avutil.h>
}

class WebcamException : public std::exception
{
public:
    WebcamException() = default;

    WebcamException(const std::string &what)
    {
        what_ = fmt::format("[VPU Exception] {}", what);
    }

    virtual const char *what() const noexcept override
    {
        return what_.data();
    }

    virtual ~WebcamException() = default;

protected:
    std::string what_ = "Unknown WebCam Exception";
};

class Webcam : public IInputSource
{
public:
    Webcam() = default;

    Webcam(int id, const char *ip)
        : IInputSource(id, INPUT_SOURCE::WEBCAM, DATA_TYPE::STREAM)
    {
        ip_ = ip;
    }

    ~Webcam()
    {
        av_free(pFormatCtx_);
        av_packet_unref(packet_);
    }

    int setRtspStream()
    {
        avformat_network_init();
        pFormatCtx_ = avformat_alloc_context();
        AVDictionary *options = NULL;
        av_dict_set(&options, "buffer_size", "2048000", 0);
        // av_dict_set(&options, "probesize", "5000000", 0);
        // av_dict_set(&options, "analyzeduration", "5000000", 0);
        av_dict_set(&options, "rtsp_transport", "rtp", 0); // TCP is more reliable
        av_dict_set(&options, "stimeout", "5000000", 0);
        av_dict_set(&options, "max_delay", "500000", 0);
        packet_ = (AVPacket *)av_malloc(sizeof(AVPacket));

        // if (avformat_open_input(&pFormatCtx_, ip_, NULL, &options) != 0)
        if (avformat_open_input(&pFormatCtx_, ip_, NULL, NULL) != 0)
        {
            spdlog::error("Couldn't open input stream: {}", ip_);
            av_dict_free(&options);
            return -1;
        }
        av_dict_free(&options);
        if (avformat_find_stream_info(pFormatCtx_, NULL) < 0)
        {
            spdlog::error("Couldn't find stream information for: {}", ip_);
            return -1;
        }

        // find video stream
        videoindex_ = -1; // FIX: Reset before search
        for (unsigned i = 0; i < pFormatCtx_->nb_streams; i++)
            if (pFormatCtx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
            {
                videoindex_ = i;
                break;
            }
        if (videoindex_ == -1)
        {
            printf("Didn't find a video stream.\n");
            return -1;
        }

        // 获取编解码器参数
        AVCodecParameters *codec_params = pFormatCtx_->streams[videoindex_]->codecpar;
        AVStream *stream = pFormatCtx_->streams[videoindex_];
        // 查找编解码器
        codec_ = avcodec_find_decoder(codec_params->codec_id);
        if (!codec_)
        {
            spdlog::error("Unsupported codec ID {} for stream: {}", codec_params->codec_id, ip_);
            return -1;
        }
        // 获取编码方式，分辨率
        codec_name = codec_->name;
        width = codec_params->width;
        height = codec_params->height;
        frame_rate = av_q2d(stream->avg_frame_rate);
        if (std::isnan(frame_rate))
        {
            frame_rate = av_q2d(stream->r_frame_rate);
        }
        spdlog::info("RTSP stream opened: {}x{}, codec: {}, frame_rate: {}", width, height, codec_name, frame_rate);
        return 0;
    }

    void streamOn()
    {
        if (setRtspStream() == -1)
        {
            throw WebcamException(fmt::format("Failed connect to Webcam: {}", ip_));
        }
    }

    void getImage(unsigned char *data)
    {
        int read_rstp_ret = -1;
        bool getVideoStream = false;
        while (read_rstp_ret < 0 || !getVideoStream)
        {
            read_rstp_ret = av_read_frame(pFormatCtx_, packet_);
            // std::cout << "read ret =" << read_rstp_ret << ", packet->stream_index=" << packet_->stream_index <<  ", videoindex_=" << videoindex_ <<  ", packet_->size=" << packet_->size << '\n';
            if (packet_->stream_index == videoindex_)
            {
                memcpy(data, packet_->data, packet_->size);
                getVideoStream = true;
            }
            av_packet_unref(packet_);
        }
    }

public:
    AVPacket *packet_;
    int videoindex_ = -1;
    AVFormatContext *pFormatCtx_;
    std::thread input_thread_;
    const char *ip_;

    const AVCodec *codec_;
    const char *codec_name;
    int width;
    int height;
    double frame_rate;
};