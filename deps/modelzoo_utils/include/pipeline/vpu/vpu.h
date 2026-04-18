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
#include <atomic>

#include "pipeline/vpu/mvx-v4l2-controls.h"
#include "opencv2/opencv.hpp"


#ifndef V4L2_BUF_FLAG_LAST
#define V4L2_BUF_FLAG_LAST 0x00100000
#endif

#define V4L2_ALLOCATE_BUFFER_ROI  1048576 * 3
#define V4L2_READ_LEN_BUFFER_ROI  1048576 * 2

namespace vpu
{
    class VpuException : public std::exception {
    public:

        VpuException() = default;

        VpuException(const std::string& what) 
        {
            what_ = fmt::format("[VPU Exception] {}", what);
        }

        virtual const char* what() const noexcept override { return what_.data();}

        virtual ~VpuException() = default;

    protected:
        std::string what_ = "Unknown Vpu Exception";
    };

    
    /*========================================================
        Common function
    =========================================================*/
    inline void printFormat(const struct v4l2_format &format)
    {
        if (V4L2_TYPE_IS_MULTIPLANAR(format.type))
        {
            const struct v4l2_pix_format_mplane &f = format.fmt.pix_mp;

            std::cout << "Format:" << std::dec <<
            " type=" << format.type <<
            ", format=" << f.pixelformat <<
            ", width=" << f.width <<
            ", height=" << f.height <<
            ", nplanes=" << int(f.num_planes) <<
            ", bytesperline=[" << f.plane_fmt[0].bytesperline <<
            ", " << f.plane_fmt[1].bytesperline <<
            ", " << f.plane_fmt[2].bytesperline << "]" <<
            ", sizeimage=[" << f.plane_fmt[0].sizeimage <<
            ", " << f.plane_fmt[1].sizeimage <<
            ", " << f.plane_fmt[2].sizeimage << "]" <<
            ", interlaced:" << f.field <<
            std::endl;
        }
        else
        {
            const struct v4l2_pix_format &f = format.fmt.pix;

            std::cout << "Format:" << std::dec <<
            " type=" << format.type <<
            ", format=" << f.pixelformat <<
            ", width=" << f.width <<
            ", height=" << f.height <<
            ", sizeimage=" << f.sizeimage <<
            ", bytesperline=" << f.bytesperline <<
            ", interlaced:" << f.field << std::endl;
        }
    }

    inline void printBuffer(const v4l2_buffer &buf, const char *prefix)
    {
        std::cout << prefix << ": " <<
        "type=" << buf.type <<
        ", length=" << buf.length <<
        ", index=" << buf.index <<
        ", sequence=" << buf.sequence <<
        ", timestamp={" << buf.timestamp.tv_sec << ", " << buf.timestamp.tv_usec << "}" <<
        ", flags=0x" << std::hex << buf.flags << std::dec;
    

        if (V4L2_TYPE_IS_MULTIPLANAR(buf.type))
        {
            const char *delim;

            std::cout << ", num_planes=" << buf.length;

            delim = "";
            std::cout << ", bytesused=[";
            for (unsigned int i = 0; i < buf.length; ++i)
            {
                std::cout << delim << buf.m.planes[i].bytesused;
                delim = ", ";
            }
            std::cout << "]";

            delim = "";
            std::cout << ", length=[";
            for (unsigned int i = 0; i < buf.length; ++i)
            {
                std::cout << delim << buf.m.planes[i].length;
                delim = ", ";
            }
            std::cout << "]";

            delim = "";
            std::cout << ", offset=[";
            for (unsigned int i = 0; i < buf.length; ++i)
            {
                std::cout << delim << buf.m.planes[i].data_offset;
                delim = ", ";
            }
            std::cout << "]";
        }
        else
        {
            std::cout << ", bytesused=" << buf.bytesused <<
            ", length=" << buf.length;
        }

        std::cout << std::endl;
    }

    inline void enumFormat(int fd)
    {
        struct v4l2_fmtdesc fmtdesc;
        int ret;
        fmtdesc.index = 0;
        fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        std::cout << "[-------------- enum multi-planar capture fmt-------------- ]" << '\n';
        while (1)
        {
            ret = ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc);
            if (ret != 0)
            {
                break;
            }

            std::cout << "fmt: index=" << fmtdesc.index <<
            ", type=" << fmtdesc.type <<
            " , flags=" << std::hex << fmtdesc.flags <<
            ", pixelformat=" << fmtdesc.pixelformat <<
            ", description=" << fmtdesc.description <<
            std::endl;

            fmtdesc.index++;
        }

        fmtdesc.index = 0;
        fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        std::cout << "[-------------- enum single-planar capture fmt-------------- ]" << '\n';
        while (1)
        {
            ret = ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc);
            if (ret != 0)
            {
                break;
            }

            std::cout << "fmt: index=" << fmtdesc.index <<
            ", type=" << fmtdesc.type <<
            " , flags=" << std::hex << fmtdesc.flags <<
            ", pixelformat=" << fmtdesc.pixelformat <<
            ", description=" << fmtdesc.description <<
            std::endl;

            fmtdesc.index++;
        }

    }

    inline uint32_t to4cc(const std::string &str)
    {
        if (str.compare("yuv420_afbc_8") == 0)
        {
            return v4l2_fourcc('Y', '0', 'A', '8');
        }
        else if (str.compare("yuv420_afbc_10") == 0)
        {
            return v4l2_fourcc('Y', '0', 'A', 'A');
        }
        else if (str.compare("yuv422_afbc_8") == 0)
        {
            return v4l2_fourcc('Y', '2', 'A', '8');
        }
        else if (str.compare("yuv422_afbc_10") == 0)
        {
            return v4l2_fourcc('Y', '2', 'A', 'A');
        }
        else if (str.compare("yuv420") == 0)
        {
            return V4L2_PIX_FMT_YUV420M;
        }
        else if (str.compare("yuv420_nv12") == 0)
        {
            return V4L2_PIX_FMT_NV12;
        }
        else if (str.compare("yuv420_nv21") == 0)
        {
            return V4L2_PIX_FMT_NV21;
        }
        else if (str.compare("yuv420_p010") == 0)
        {
            return V4L2_PIX_FMT_P010;
        }
        else if (str.compare("yuv420_y0l2") == 0)
        {
            return V4L2_PIX_FMT_Y0L2;
        }
        else if (str.compare("yuv422_yuy2") == 0)
        {
            return V4L2_PIX_FMT_YUYV;
        }
        else if (str.compare("yuv422_uyvy") == 0)
        {
            return V4L2_PIX_FMT_UYVY;
        }
        else if (str.compare("yuv422_y210") == 0)
        {
            return V4L2_PIX_FMT_Y210;
        }
        else if (str.compare("rgba") == 0)
        {
            return DRM_FORMAT_ABGR8888;
        }
        else if (str.compare("bgra") == 0)
        {
            return DRM_FORMAT_ARGB8888;
        }
        else if (str.compare("argb") == 0)
        {
            return DRM_FORMAT_BGRA8888;
        }
        else if (str.compare("abgr") == 0)
        {
            return DRM_FORMAT_RGBA8888;
        }
        else if (str.compare("avs2") == 0)
        {
            return V4L2_PIX_FMT_AVS2;
        }
        else if (str.compare("avs") == 0)
        {
            return V4L2_PIX_FMT_AVS;
        }
        else if (str.compare("h263") == 0)
        {
            return V4L2_PIX_FMT_H263;
        }
        else if (str.compare("h264") == 0)
        {
            return V4L2_PIX_FMT_H264;
        }
        else if (str.compare("h264_mvc") == 0)
        {
            return V4L2_PIX_FMT_H264_MVC;
        }
        else if (str.compare("h264_no_sc") == 0)
        {
            return V4L2_PIX_FMT_H264_NO_SC;
        }
        else if (str.compare("hevc") == 0)
        {
            return V4L2_PIX_FMT_HEVC;
        }
        else if (str.compare("mjpeg") == 0)
        {
            return V4L2_PIX_FMT_MJPEG;
        }
        else if (str.compare("jpeg") == 0)
        {
            return V4L2_PIX_FMT_JPEG;
        }
        else if (str.compare("mpeg2") == 0)
        {
            return V4L2_PIX_FMT_MPEG2;
        }
        else if (str.compare("mpeg4") == 0)
        {
            return V4L2_PIX_FMT_MPEG4;
        }
        else if (str.compare("rv") == 0)
        {
            return V4L2_PIX_FMT_RV;
        }
        else if (str.compare("vc1") == 0)
        {
            return V4L2_PIX_FMT_VC1_ANNEX_G;
        }
        else if (str.compare("vc1_l") == 0)
        {
            return V4L2_PIX_FMT_VC1_ANNEX_L;
        }
        else if (str.compare("vp8") == 0)
        {
            return V4L2_PIX_FMT_VP8;
        }
        else if (str.compare("vp9") == 0)
        {
            return V4L2_PIX_FMT_VP9;
        }
        else
        {
            std::cout << "Not a valid format " <<  str.c_str() << '\n';
        }

        return 0;
    }

    class Buffer
    {
    public:
        Buffer() = default;
        Buffer(v4l2_buf_type type, int fd, uint32_t index)
        {
            struct v4l2_buffer *inner = &this->v4l2_buf_;
            struct v4l2_plane planes[VIDEO_MAX_PLANES];
        
            memset(inner, 0, sizeof(*inner));
            inner->type = type;
            inner->memory = V4L2_MEMORY_MMAP;
            inner->index = index;
            inner->m.planes = planes;
            inner->length =  V4L2_TYPE_IS_MULTIPLANAR(inner->type) ?  3 : 1;
            this->length_ = inner->length;
        
            if (V4L2_TYPE_IS_MULTIPLANAR(inner->type))
            {
                inner->m.planes = this->planes_; 
            }
        
            if (0 != ioctl(fd, VIDIOC_QUERYBUF, inner)) {
                throw VpuException(fmt::format("Fail to query buffer, buffer type={}", type));
            }
                
        
            if (V4L2_TYPE_IS_MULTIPLANAR(inner->type)) {
                this->length_ = inner->length;
                for (size_t i = 0; i < inner->length; ++i) {
                    this->ptr_[i] = mmap(NULL, inner->m.planes[i].length, PROT_READ | PROT_WRITE,
                    MAP_SHARED, fd, inner->m.planes[i].m.mem_offset);
                    byte_per_planes_.emplace_back(inner->m.planes[i].length);
                    if (this->ptr_[i] == MAP_FAILED) {
                        throw VpuException(fmt::format("Failed to mmap memory!"));
                    }  
                }
        
            } else {
                this->length_ = inner->length;
                byte_per_planes_.emplace_back(inner->length);
                inner->m.offset &= ~((1 << 12) - 1);
                this->ptr_[0] = mmap(NULL, inner->length, PROT_READ | PROT_WRITE,
                    MAP_SHARED, fd, inner->m.offset);
                if (this->ptr_[0] == MAP_FAILED) {
                    throw VpuException(fmt::format("Failed to mmap memory!"));
                }
            }
        }

        virtual ~Buffer()
        {
            //unmap
            if (V4L2_TYPE_IS_MULTIPLANAR(this->v4l2_buf_.type))
            {
                for (uint32_t i = 0; i < this->v4l2_buf_.length; ++i)
                {
                    if (this->ptr_[i] != 0)
                    {
                        munmap(this->ptr_[i], this->planes_[i].length);
                    }
                
                }
            }
            else
            {
                if (this->ptr_[0])
                {
                    munmap(this->ptr_[0], this->v4l2_buf_.length);
                }
            }
        }
        
        void update(v4l2_buffer dequeue_buf)
        {
            v4l2_buf_ = dequeue_buf;
            if (V4L2_TYPE_IS_MULTIPLANAR(v4l2_buf_.type))
            {
                v4l2_buf_.m.planes = planes_;
                for (size_t i = 0; i < v4l2_buf_.length; ++i)
                {
                    v4l2_buf_.m.planes[i] = dequeue_buf.m.planes[i];
                }
            }
        }

        v4l2_buffer& getBuffer()
        {
            return v4l2_buf_;
        }

        void setByteUsed()
        {
            if (V4L2_TYPE_IS_MULTIPLANAR(v4l2_buf_.type)) {
                for (unsigned int i = 0; i < v4l2_buf_.length; ++i) {
                    v4l2_buf_.m.planes[i].bytesused = this->byte_per_planes_[i];  
                }
            } else {
                v4l2_buf_.bytesused = this->byte_per_planes_[0];
            }
            
        }

        void setByteUsed(uint32_t bytesused)
        {
            // only for single plane
            v4l2_buf_.bytesused = bytesused;
        }

        void resetByteUsed()
        {
            // only for single plane
            if (V4L2_TYPE_IS_MULTIPLANAR(v4l2_buf_.type)) {
                for (unsigned int i = 0; i < v4l2_buf_.length; ++i) {
                    v4l2_buf_.m.planes[i].bytesused = 0;  
                }
            } else {
                v4l2_buf_.bytesused = 0;
            }
        }

        void setDownScale(int scale)
        {
            if (scale == 1) {
                return;
            }
            switch (scale) {
                case 2:
                    v4l2_buf_.flags &= ~V4L2_BUF_FRAME_FLAG_SCALING_MASK;
                    v4l2_buf_.flags |= V4L2_BUF_FRAME_FLAG_SCALING_2;
                    break;
                case 4:
                    v4l2_buf_.flags &= ~V4L2_BUF_FRAME_FLAG_SCALING_MASK;
                    v4l2_buf_.flags |= V4L2_BUF_FRAME_FLAG_SCALING_4;
                    break;
                default:
                    printf("didnot support this scale factor :%d",scale);
                    break;
            }
            return;
        }

        void resetVendorFlags()
        {
            v4l2_buf_.flags &= ~V4L2_BUF_FLAG_MVX_MASK;
        }

        void* const getDataPtr(unsigned int index)
        {
            return ptr_[index];
        }

    public:
        struct v4l2_buffer v4l2_buf_;
        struct v4l2_plane planes_[VIDEO_MAX_PLANES];
        std::vector<int> byte_per_planes_;
        void *ptr_[VIDEO_MAX_PLANES];
        v4l2_format format_;
        v4l2_buf_type type_;
        int index_;
        int height_;
        int width_;
        int length_;
    };

    using BufferMap = std::map<uint32_t, std::shared_ptr<Buffer>>;


    class Codec
    {

    public:
        Codec() = default;

        Codec(const char *dev, const unsigned int height, const unsigned int width, 
            const v4l2_buf_type in_type, uint32_t input_pix_fmt, 
            const v4l2_buf_type out_type, uint32_t output_pix_fmt)
            :height_(height), width_(width),
            input_buf_type_(in_type), input_pix_fmt_(input_pix_fmt),
            output_buf_type_(out_type), output_pix_fmt_(output_pix_fmt)
        {
            this->openDevice(dev);
        }

        ~Codec()
        {
            this->closeDevice();
        }

        void openDevice(const char *dev)
        {
            fd_ = open(dev, O_RDWR, 0);
            if (fd_ == -1)
            {
                throw VpuException(fmt::format("Failed to open video device: {}", dev));
            }
        }

        void closeDevice()
        {
            close(fd_);
            fd_ = -1;
        }

        void enumFormat()
        {
            vpu::enumFormat(fd_);
        }

        void setDownScale(int scale)
        {
            this->down_scale_ = scale;
        }

        void setOutputPrint(bool print)
        {
            this->output_print_ = print;
        }

        void setInputPrint(bool print)
        {
            this->input_print_ = print;
        }

        void allocateInputBuffer(uint16_t buffer_num)
        {
            struct v4l2_requestbuffers reqbuf;
            memset(&reqbuf, 0, sizeof(reqbuf));
        
            reqbuf.count = buffer_num;                    
            reqbuf.memory = V4L2_MEMORY_MMAP;
            reqbuf.type = input_buf_type_;
            std::cout << "[VPU] Allocate intput buffer for vpu, buffer numbers=" << buffer_num
                      << "\n";
            if ( 0 != ioctl(fd_, VIDIOC_REQBUFS, &reqbuf) && input_print_) {
                throw VpuException(fmt::format("Fail to request input buffer."));
            }
        
            
            for (int i = 0; i < buffer_num; ++i) {
                auto buffer = std::make_shared<Buffer>(input_buf_type_, fd_, i);
                input_buffers[i] = buffer;
            }
        }

        void allocateOutputBuffer(uint16_t buffer_num)
        {
            struct v4l2_requestbuffers reqbuf;
            memset(&reqbuf, 0, sizeof(reqbuf));
            reqbuf.count = buffer_num;                    
            reqbuf.memory = V4L2_MEMORY_MMAP;
            reqbuf.type = output_buf_type_;
            std::cout << "[VPU] Allocate output buffer for vpu, buffer numbers=" << buffer_num
                      << "\n";
            if ( 0 != ioctl(fd_, VIDIOC_REQBUFS, &reqbuf) && output_print_) {
                throw VpuException(fmt::format("Fail to request output buffer."));
            }
            
            for (int i = 0; i < buffer_num; ++i) {
                auto buffer = std::make_shared<Buffer>(output_buf_type_, fd_, i);
                buffer->setDownScale(down_scale_);
                output_buffers[i] = buffer;
            }
        }

        void streamon_()
        { 
            if (0 != ioctl(fd_, VIDIOC_STREAMON, &input_buf_type_)) {
                throw VpuException(fmt::format("Fail to turn on vpu input stream."));
            }
        
            if (0 != ioctl(fd_, VIDIOC_STREAMON, &output_buf_type_)) {
                throw VpuException(fmt::format("Fail to turn on vpu output stream."));
            }
        }

        void streamoff_()
        { 
            if (0 != ioctl(fd_, VIDIOC_STREAMOFF, &input_buf_type_)) {
                throw VpuException(fmt::format("Fail to turn off vpu input stream."));
            }
               
        
            if (0 != ioctl(fd_, VIDIOC_STREAMOFF, &output_buf_type_)) {
                throw VpuException(fmt::format("Fail to turn off vpu output stream."));
            }
        }
        


    protected:
        BufferMap  input_buffers;
        BufferMap  output_buffers;

        struct v4l2_format  input_format_;
        struct v4l2_format  output_format_;
        uint32_t input_pix_fmt_;
        uint32_t output_pix_fmt_;


        v4l2_buf_type input_buf_type_;
        v4l2_buf_type output_buf_type_;

        int fd_;
        unsigned int height_;
        unsigned int width_;

        bool eos_ = false;

        bool input_print_= false;
        bool output_print_ = false;

        int down_scale_ = 1;
        int rc_type_;
    };


    class Encoder : public Codec
    {
    public:
        // Encoder() = default; no default initializer

        Encoder(const char *dev, unsigned int height, unsigned width, 
                v4l2_buf_type in_type, uint32_t input_pix_fmt,  
                v4l2_buf_type out_type, uint32_t output_pix_fmt, 
                int buffer_num)
                : Codec(dev, height, width, in_type,input_pix_fmt, out_type, output_pix_fmt)
        {
            this->setFormat(input_pix_fmt, output_pix_fmt);
            this->allocateInputBuffer(buffer_num);
            this->allocateOutputBuffer(buffer_num);
            buffer_num_ = buffer_num;
            streamon_flag_.store(false);
            init_flag_.store(false);
            valid_buffer_index_.store(0);
        }  

        ~Encoder() {}

        void setFrameRate(uint32_t frame_rate)
        {
            std::cout << "[VPU] setEncFrameRate=" << frame_rate << "fps" << std::endl;
            struct v4l2_control control;
            memset(&control, 0, sizeof(control));
            control.id = V4L2_CID_MVE_VIDEO_FRAME_RATE;
            control.value = frame_rate << 16;
            if (-1 == ioctl(fd_, VIDIOC_S_CTRL, &control)) {
                throw VpuException(fmt::format("Failed to set frame_rate"));
            }
        
        }

        void setFormat(uint32_t input_fmt,  uint32_t output_fmt)
        { 
    
            int ret = 0;
        
            memset(&input_format_, 0, sizeof(input_format_));
            input_format_.type = input_buf_type_;      
            ret = ioctl(fd_, VIDIOC_G_FMT, &input_format_);
            if (ret != 0)
            {
                throw VpuException("Failed to get input format.");
            }
        
            if (V4L2_TYPE_IS_MULTIPLANAR(input_buf_type_)) {
                struct v4l2_pix_format_mplane *ip = &input_format_.fmt.pix_mp;    
                input_format_.type = input_buf_type_;       
                ip->width = width_;            
                ip->height = height_;
                ip->pixelformat = input_fmt;
                ip->field = V4L2_FIELD_NONE;   
                ip->num_planes = 3;    
                for (int i = 0; i < 3; ++i) {
                    ip->plane_fmt[i].bytesperline = 0;
                    ip->plane_fmt[i].sizeimage = 0;
                }     
                // field order - https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/field-order.html?highlight=v4l2_field_seq_tb
                
            } else {
                struct v4l2_pix_format *ip = &input_format_.fmt.pix;
                ip->width = width_;            
                ip->height = height_;
                ip->pixelformat = input_fmt;
                ip->field = V4L2_FIELD_NONE; 
                ip->bytesperline = 0;  
                ip->sizeimage = 5 * 1024 * 1024;         
                // field order - https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/field-order.html?highlight=v4l2_field_seq_tb
            }
            ret = ioctl(fd_, VIDIOC_TRY_FMT, &input_format_);
        
            if (0 != ret) {
                throw VpuException(fmt::format("Fail to try input format."));
            }
            
            ret = ioctl(fd_, VIDIOC_S_FMT, &input_format_);
            if (0 != ret) {
                throw VpuException(fmt::format("Fail to set up input format."));
            }
        
            printFormat(input_format_); 
        
        
            memset(&output_format_, 0, sizeof(output_format_));
            output_format_.type = output_buf_type_;       
            ret = ioctl(fd_, VIDIOC_G_FMT, &output_format_);
            if (ret != 0) {
                throw VpuException("Failed to get output format.");
            }
        
            if (V4L2_TYPE_IS_MULTIPLANAR(output_buf_type_)) {
                struct v4l2_pix_format_mplane *op = &output_format_.fmt.pix_mp;  
                output_format_.type = output_buf_type_;        
                op->width = width_;
                op->height = height_;
                op->pixelformat = output_fmt;
                op->field = V4L2_FIELD_NONE;
                for (int i = 0; i < 3; ++i) {
                    op->plane_fmt[i].bytesperline = 0;
                    op->plane_fmt[i].sizeimage = 0;
                }   
            } else {
                struct v4l2_pix_format *op = &output_format_.fmt.pix;  
                output_format_.type = output_buf_type_;        
                op->width = width_;
                op->height = height_;
                op->pixelformat = output_fmt;
                op->field = V4L2_FIELD_NONE;
                op->bytesperline = 0;
                op->sizeimage = 5 * 1024 * 1024;         
            }
        
            ret = ioctl(fd_, VIDIOC_TRY_FMT, &output_format_);
            if (0 != ret) {
                throw VpuException(fmt::format("Fail to try output format."));
            }
            
            ret = ioctl(fd_, VIDIOC_S_FMT, &output_format_);
            if (0 != ret) {
                throw VpuException(fmt::format("Fail to set up output format."));
            }
            printFormat(output_format_); 
        
        }


        void queueImage(cv::Mat &img)
        {

            if (!init_flag_)
            {   
                // dequeue output buffer first time
                for (int i = 0; i < output_buffers.size(); ++i) {
                    auto buffer_ptr_ca = output_buffers[i];
                    buffer_ptr_ca->resetVendorFlags();
                    if (0 != ioctl(fd_, VIDIOC_QBUF, &buffer_ptr_ca->getBuffer())) {
                        throw VpuException(fmt::format("Failed to queue output buffer."));
                    }
                    if (output_print_)  printBuffer(buffer_ptr_ca->getBuffer(), "[-> output]");
                }
                init_flag_.store(true);
            }
        
            if (valid_buffer_index_ < buffer_num_)
            {
                // queue image
                unsigned char* read_data = img.data;
                auto buffer_ptr = input_buffers[valid_buffer_index_];
                buffer_ptr->resetVendorFlags();
                if (V4L2_TYPE_IS_MULTIPLANAR(input_buf_type_)) {
                // multi-plane
                    for (int j = 0; j < buffer_ptr->length_; ++j) {
                        buffer_ptr->planes_[j].bytesused = buffer_ptr->byte_per_planes_[j];
                        memcpy((unsigned char*)buffer_ptr->ptr_[j], read_data, buffer_ptr->byte_per_planes_[j]);
                        read_data += buffer_ptr->byte_per_planes_[j];
                    }
                } else {
                    buffer_ptr->setByteUsed(buffer_ptr->byte_per_planes_[0]);
                    memcpy((unsigned char*)buffer_ptr->ptr_[0], read_data, buffer_ptr->byte_per_planes_[0]);
                }
        
                if (0 != ioctl(fd_, VIDIOC_QBUF, &buffer_ptr->getBuffer())) {
                    throw VpuException(fmt::format("Failed to queue intput buffer."));
                }
                if (input_print_) printBuffer(buffer_ptr->getBuffer(), "[-> input]");
        
                valid_buffer_index_.store(valid_buffer_index_+1);
                if (valid_buffer_index_ == buffer_num_)
                {
                    // stream on
                    streamon_();
                    streamon_flag_.store(true);
                }
            }
            else
            {
                // queue image
                unsigned char* read_data = img.data;
                auto buffer_ptr = getAvailableBuffer();
                buffer_ptr->resetVendorFlags();
                if (V4L2_TYPE_IS_MULTIPLANAR(input_buf_type_)) {
                // multi-plane
                    for (int j = 0; j < buffer_ptr->length_; ++j) {
                        buffer_ptr->planes_[j].bytesused = buffer_ptr->byte_per_planes_[j];
                        memcpy((unsigned char*)buffer_ptr->ptr_[j], read_data, buffer_ptr->byte_per_planes_[j]);
                        read_data += buffer_ptr->byte_per_planes_[j];
                    }
                } else {
                    buffer_ptr->setByteUsed(buffer_ptr->byte_per_planes_[0]);
                    memcpy((unsigned char*)buffer_ptr->ptr_[0], read_data, buffer_ptr->byte_per_planes_[0]);
                }
        
                if (0 != ioctl(fd_, VIDIOC_QBUF, &buffer_ptr->getBuffer())) {
                    throw VpuException(fmt::format("Failed to queue intput buffer."));
                }
                if (input_print_) printBuffer(buffer_ptr->getBuffer(), "[-> input]");
        
            }
        
        }

        void enqueueDataPtr(char* data_ptr)
        {
            if (!init_flag_)
            {   
                // dequeue output buffer first time
                for (int i = 0; i < output_buffers.size(); ++i) {
                    auto buffer_ptr_ca = output_buffers[i];
                    buffer_ptr_ca->resetVendorFlags();
                    if (0 != ioctl(fd_, VIDIOC_QBUF, &buffer_ptr_ca->getBuffer())) {
                        throw VpuException(fmt::format("Failed to queue output buffer."));
                    }
                    if (output_print_)  printBuffer(buffer_ptr_ca->getBuffer(), "[-> output]");
                }
                init_flag_.store(true);
            }
        
            if (valid_buffer_index_ < buffer_num_)
            {
                // queue image
                unsigned char* read_data = (unsigned char*)data_ptr;
                auto buffer_ptr = input_buffers[valid_buffer_index_];
                buffer_ptr->resetVendorFlags();
                if (V4L2_TYPE_IS_MULTIPLANAR(input_buf_type_)) {
                // multi-plane
                    for (int j = 0; j < buffer_ptr->length_; ++j) {
                        buffer_ptr->planes_[j].bytesused = buffer_ptr->byte_per_planes_[j];
                        memcpy((unsigned char*)buffer_ptr->ptr_[j], read_data, buffer_ptr->byte_per_planes_[j]);
                        read_data += buffer_ptr->byte_per_planes_[j];
                    }
                } else {
                    buffer_ptr->setByteUsed(buffer_ptr->byte_per_planes_[0]);
                    memcpy((unsigned char*)buffer_ptr->ptr_[0], read_data, buffer_ptr->byte_per_planes_[0]);
                }
        
                if (0 != ioctl(fd_, VIDIOC_QBUF, &buffer_ptr->getBuffer())) {
                    throw VpuException(fmt::format("Failed to queue intput buffer."));
                }
                if (input_print_) printBuffer(buffer_ptr->getBuffer(), "[-> input]");
        
                valid_buffer_index_.store(valid_buffer_index_+1);
                if (valid_buffer_index_ == buffer_num_)
                {
                    // stream on
                    streamon_();
                    streamon_flag_.store(true);
                }
            }
            else
            {
                // queue image
                unsigned char* read_data = (unsigned char*)data_ptr;
                auto buffer_ptr = getAvailableBuffer();
                buffer_ptr->resetVendorFlags();
                if (V4L2_TYPE_IS_MULTIPLANAR(input_buf_type_)) {
                // multi-plane
                    for (int j = 0; j < buffer_ptr->length_; ++j) {
                        buffer_ptr->planes_[j].bytesused = buffer_ptr->byte_per_planes_[j];
                        memcpy((unsigned char*)buffer_ptr->ptr_[j], read_data, buffer_ptr->byte_per_planes_[j]);
                        read_data += buffer_ptr->byte_per_planes_[j];
                    }
                } else {
                    buffer_ptr->setByteUsed(buffer_ptr->byte_per_planes_[0]);
                    memcpy((unsigned char*)buffer_ptr->ptr_[0], read_data, buffer_ptr->byte_per_planes_[0]);
                }
        
                if (0 != ioctl(fd_, VIDIOC_QBUF, &buffer_ptr->getBuffer())) {
                    throw VpuException(fmt::format("Failed to queue intput buffer."));
                }
                if (input_print_) printBuffer(buffer_ptr->getBuffer(), "[-> input]");
        
            }
        }

        bool getOutput(void* &ptr, int& size)
        {
            v4l2_plane planes[VIDEO_MAX_PLANES];
            v4l2_buffer buf_dq;
            memset(&planes, 0, sizeof(struct v4l2_plane) * VIDEO_MAX_PLANES);;
            memset(&buf_dq, 0, sizeof(buf_dq));
            buf_dq.m.planes = planes;
            buf_dq.type = output_buf_type_;
            buf_dq.memory = V4L2_MEMORY_MMAP;
            buf_dq.length = 3;
            int ret;
            ret = ioctl(fd_, VIDIOC_DQBUF, &buf_dq);
            if (0 != ret) {
                throw VpuException(fmt::format("in File:{}  Line:{} \n Failed to dequeue output buffer, errno={}", __FILE__, __LINE__, errno));
            }
        
            
            if ((buf_dq.flags & V4L2_BUF_FLAG_MVX_BUFFER_NEED_REALLOC) == V4L2_BUF_FLAG_MVX_BUFFER_NEED_REALLOC) {
                spdlog::error("need to reallocate buufer or res change!");
            }
        
            if (output_print_) printBuffer(buf_dq, "[<- output]");
        
            int index = buf_dq.index;
        
            if (!output_buffers.count(index)) {
                throw VpuException(fmt::format("Output buffer index error!"));
            }
        
            auto buffer_ptr = output_buffers[index];
            buffer_ptr->update(buf_dq);
            buffer_ptr->resetVendorFlags();
        
            ptr = (char *)buffer_ptr->ptr_[0] + (buffer_ptr->getBuffer().m.offset & ((1 << 12) - 1));
            size = buffer_ptr->getBuffer().bytesused;
        
            if (buf_dq.flags & V4L2_BUF_FLAG_LAST) {
                std::cout << "[VPU] Capture EOS." << std::endl;
                return false;
            }
        
            ret = ioctl(fd_, VIDIOC_QBUF, &buf_dq);
            if (0 != ret) {
                throw VpuException(fmt::format("in File:{}  Line: {} \n Failed to queue output buffer, errno={}", __FILE__, __LINE__, errno));
            }
            if (output_print_)  printBuffer(buffer_ptr->getBuffer(), "[-> output]");
          
            return true;
        }

        std::shared_ptr<Buffer> getAvailableBuffer()
        {
            v4l2_plane planes[VIDEO_MAX_PLANES];
            v4l2_buffer buf_dq;
            memset(&buf_dq, 0, sizeof(buf_dq));
            memset(&planes, 0, sizeof(struct v4l2_plane) * VIDEO_MAX_PLANES);
            buf_dq.m.planes = planes;
            buf_dq.type = input_buf_type_;
            buf_dq.memory = V4L2_MEMORY_MMAP;
            buf_dq.length = 3;
            // std::unique_lock get_buffer_lock(get_buffer_mutex_);
            int ret;
            ret = ioctl(fd_, VIDIOC_DQBUF, &buf_dq);
            if (0 != ret) {
                throw VpuException(fmt::format("in File:{}  Line: {} \n Failed to dequeue input buffer, errno={}", __FILE__, __LINE__, errno));
            }
        
            
            if ((buf_dq.flags & V4L2_BUF_FLAG_MVX_BUFFER_NEED_REALLOC) == V4L2_BUF_FLAG_MVX_BUFFER_NEED_REALLOC) {
                spdlog::error("need to reallocate buufer or res change!");
            }
        
            if (input_print_) printBuffer(buf_dq, "[<- input]");
            int index = buf_dq.index;
            if (!input_buffers.count(index)) {
                throw VpuException(fmt::format("Output buffer index error!"));
                return nullptr;
            }
        
            auto buffer_ptr = input_buffers[index];
            buffer_ptr->update(buf_dq);
            buffer_ptr->resetVendorFlags();
        
            return buffer_ptr;       
        }

        void returnBuffer(std::shared_ptr<Buffer>& buffer_ptr)
        {
            buffer_ptr->setByteUsed();
            int ret = ioctl(fd_, VIDIOC_QBUF, &buffer_ptr->getBuffer());
            if (0 != ret) {
                throw VpuException(fmt::format("in File:{}  Line: {} \n Failed to queue input buffer, errno={}", __FILE__, __LINE__, errno));
            }
            if (input_print_)  printBuffer(buffer_ptr->getBuffer(), "[-> input]");
        }

        void streamOff()
        { 
            streamoff_();
        }
    public:
        std::atomic<bool> streamon_flag_;
    protected:
        int buffer_num_;
        std::atomic<bool> init_flag_;
        std::atomic<int> valid_buffer_index_;
    };

    /**
     * @class Decoder
     * @brief Manages video decoding operations using the V4L2 (Video for Linux 2) API.
     *
     * This class inherits from the Codec class and provides a high-level interface
     * for setting up a V4L2 decoder, feeding it encoded data, and retrieving
     * decoded frames. It handles buffer management, format negotiation, and stream control.
     */
    class Decoder : public Codec
    {
    public:
        // Decoder() = default;
        
        /**
         * @brief Constructs a Decoder object.
         *
         * Initializes the VPU device, sets up formats, allocates input and output buffers,
         * and prepares the decoder for operation.
         *
         * @param dev The path to the V4L2 device file (e.g., "/dev/video1").
         * @param height The height of the video frames.
         * @param width The width of the video frames.
         * @param scale The downscaling factor to be applied.
         * @param in_type The V4L2 buffer type for the input queue (e.g., V4L2_BUF_TYPE_VIDEO_OUTPUT).
         * @param input_pix_fmt The pixel format of the encoded input data (e.g., V4L2_PIX_FMT_H264).
         * @param out_type The V4L2 buffer type for the output queue (e.g., V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE).
         * @param output_pix_fmt The pixel format for the decoded output data (e.g., V4L2_PIX_FMT_YUV420M).
         * @param buffer_num The number of buffers to allocate for both input and output queues.
         */
        Decoder(const char *dev, 
                unsigned int height, unsigned int width, unsigned int scale,
                v4l2_buf_type in_type, uint32_t input_pix_fmt,  
                v4l2_buf_type out_type, uint32_t output_pix_fmt, 
                int buffer_num)
                : Codec(dev, height, width, in_type,input_pix_fmt, out_type, output_pix_fmt),
                buffer_num_(buffer_num)
        {
            setDownScale(scale);
            setOutputPrint(false);
            setInputPrint(false);
            setFormat(input_pix_fmt, output_pix_fmt);
            allocateInputBuffer(buffer_num);
            allocateOutputBuffer(buffer_num);
            streamon_flag_.store(false);
            init_flag_.store(false);
            valid_buffer_index_.store(0);
        }

        /**
         * @brief Destroys the Decoder object.
         */
        ~Decoder() {}

        /**
         * @brief Sets the input and output formats for the VPU device.
         *
         * Configures the device using VIDIOC_S_FMT ioctl calls for both the
         * input (encoded) and output (decoded) streams.
         *
         * @param input_fmt The pixel format for the input stream.
         * @param output_fmt The pixel format for the output stream.
         * @throws VpuException if setting either format fails.
         */
        void setFormat(uint32_t input_fmt,  uint32_t output_fmt)
        {    
            memset(&input_format_, 0, sizeof(input_format_));
            struct v4l2_pix_format *ip = &input_format_.fmt.pix;    
            input_format_.type = input_buf_type_;       
            ip->width = width_;            
            ip->height = height_;
            ip->pixelformat = input_fmt;
            ip->field = V4L2_FIELD_NONE;   
            ip->sizeimage = 5 * 1024 * 1024;     
            ioctl(fd_, VIDIOC_TRY_FMT, input_format_);
            if ( 0 != ioctl(fd_, VIDIOC_S_FMT, input_format_)) {
                throw VpuException(fmt::format("Fail to set up input format."));
            }
        
            if (input_print_ || output_print_) printFormat(input_format_); 
            
            
            memset(&output_format_, 0, sizeof(output_format_));
            struct v4l2_pix_format_mplane *op = &output_format_.fmt.pix_mp; 
            output_format_.type = output_buf_type_;        
            op->width = width_;
            op->height = height_;
            op->pixelformat = output_fmt;
            op->field = V4L2_FIELD_NONE;
            op->num_planes = 3;
        
            // for yuv420M multi-plane
            for (int i = 0; i < 3; ++i) {
                if (i == 0) {
                    op->plane_fmt[i].bytesperline = width_;
                    op->plane_fmt[i].sizeimage = width_ * height_;
                } else {
                    op->plane_fmt[i].bytesperline = width_ * 0.5;
                    op->plane_fmt[i].sizeimage = width_ * height_ * 0.25;
                }
            }
            ioctl(fd_, VIDIOC_TRY_FMT, output_format_);
            if( 0 != ioctl(fd_, VIDIOC_S_FMT, output_format_)) {
                throw VpuException(fmt::format("Fail to set up output format."));
            }
        
            if (input_print_ || output_print_) printFormat(output_format_); 
        }

        /**
         * @brief Enqueues a buffer with encoded data for decoding.
         *
         * On the first run, it primes the pipeline by queueing all output buffers.
         * It then finds an available input buffer, copies the provided data into it,
         * and queues it for the hardware decoder. Once enough initial buffers are
         * queued, it starts the stream.
         *
         * @param data_ptr Pointer to the buffer containing the encoded video data.
         * @throws VpuException if queuing the input buffer fails.
         */
        void enqueueInput(const char* data_ptr)
        {
            if (!init_flag_)
            {
                // dequeue output buffer first time
                for (int i = 0; i < output_buffers.size(); ++i) {
                    auto buffer_ptr_ca = output_buffers[i];
                    buffer_ptr_ca->resetVendorFlags();
                    if (0 != ioctl(fd_, VIDIOC_QBUF, &buffer_ptr_ca->getBuffer())) {
                        throw VpuException(fmt::format("Failed to queue output buffer."));
                    }
                    if (output_print_)  printBuffer(buffer_ptr_ca->getBuffer(), "[-> output]");
                }
                init_flag_.store(true);
            }
        
            if (valid_buffer_index_ < buffer_num_)
            {
                auto buffer_ptr = input_buffers[valid_buffer_index_];
                std::memcpy(static_cast<char *>(buffer_ptr->ptr_[0]), data_ptr, V4L2_READ_LEN_BUFFER_ROI);
                buffer_ptr->setByteUsed(V4L2_READ_LEN_BUFFER_ROI);
                // queue buffer
                if (0 != ioctl(fd_, VIDIOC_QBUF, &buffer_ptr->getBuffer())) {
                    throw VpuException(fmt::format("in File:{}  Line: {} \n Failed to queue input buffer, errno={}", __FILE__, __LINE__, errno));
                }
                if (input_print_)  printBuffer(buffer_ptr->getBuffer(), "[-> input]");
        
        
                valid_buffer_index_.store(valid_buffer_index_+1);
                if (valid_buffer_index_ == buffer_num_)
                {
                    // stream on
                    streamon_();
                    streamon_flag_.store(true);
                }
            }
            else
            {   
                auto buffer_ptr = getAvailableBuffer();
                std::memcpy(static_cast<char *>(buffer_ptr->ptr_[0]), data_ptr, V4L2_READ_LEN_BUFFER_ROI);
                buffer_ptr->setByteUsed(V4L2_READ_LEN_BUFFER_ROI);
        
                // queue buffer
                if (0 != ioctl(fd_, VIDIOC_QBUF, &buffer_ptr->getBuffer())) {
                    throw VpuException(fmt::format("in File:{}  Line: {} \n Failed to queue input buffer, errno={}", __FILE__, __LINE__, errno));
                }
                if (input_print_)  printBuffer(buffer_ptr->getBuffer(), "[-> input]");
        
            }
        }

        /**
         * @brief Gets a decoded frame from the VPU.
         * 
         * Dequeues a completed buffer from the VPU's output queue, copies the
         * decoded frame data into the provided user buffer, and then re-queues
         * the VPU buffer for future use.
         *
         * @param ptr Output pointer to a user-allocated buffer where the decoded frame data will be copied.
         * @param size Output reference to an integer that will be filled with the size of the decoded data in bytes.
         * @param eos Output reference to a boolean that will be set to true if the end-of-stream is reached.
         * @throws VpuException if dequeuing the output buffer fails.
         */
        void getOutput(char* ptr, int& size, bool& eos)
        {
            eos = false;
            v4l2_plane planes[VIDEO_MAX_PLANES];
            v4l2_buffer buf_dq;
            memset(&planes, 0, sizeof(struct v4l2_plane) * VIDEO_MAX_PLANES);
            memset(&buf_dq, 0, sizeof(buf_dq));
            buf_dq.m.planes = planes;
            buf_dq.type = output_buf_type_;
            buf_dq.memory = V4L2_MEMORY_MMAP;
            buf_dq.length = 3;
            int ret;
            ret = ioctl(fd_, VIDIOC_DQBUF, &buf_dq);
            if (0 != ret) {
                throw VpuException(fmt::format("in File:{}  Line:{} \n Failed to dequeue output buffer, errno={}", __FILE__, __LINE__, errno));
            }
        
            if (output_print_) printBuffer(buf_dq, "[<- output]");
        
            int index = buf_dq.index;
        
            if (!output_buffers.count(index)) {
                throw VpuException(fmt::format("Output buffer index error!"));
            }
        
            auto buffer_ptr = output_buffers[index];
            buffer_ptr->update(buf_dq);
        
            if (buffer_ptr->getBuffer().flags & V4L2_BUF_FLAG_LAST) {
                eos = true;
                spdlog::info("Capture EOS");
        
            }   
        
            if (buf_dq.flags & V4L2_BUF_FLAG_LAST) {
                spdlog::info("get eos true");
                std::cout << "[VPU] Capture EOS." << std::endl;
                eos = true;
            }
        
        
            // copy data
            int data_bytesize;
            int offset = 0;
            size = 0;
            for (int i = 0; i < buffer_ptr->length_; ++i) {
                data_bytesize = buffer_ptr->planes_[i].bytesused;
                // spdlog::info("[output] size {} {} {}", i, data_bytesize, offset);
                size += data_bytesize;
                char* data_ptr = (char*)ptr+offset;
                std::memcpy(data_ptr, (char *)buffer_ptr->ptr_[i], data_bytesize);
                offset += data_bytesize;
            }
        
            returnBuffer(buffer_ptr);
        }

        /**
         * @brief Retrieves an available (processed) input buffer from the VPU.
         *
         * Dequeues a processed input buffer, making it available for reuse. This is
         * typically called internally by enqueueInput but can be used to manage
         * buffer flow manually.
         *
         * @return A shared pointer to the available Buffer object.
         * @throws VpuException if dequeuing the input buffer fails or an index error occurs.
         */
        std::shared_ptr<Buffer> getAvailableBuffer()
        {
            v4l2_plane planes[VIDEO_MAX_PLANES];
            v4l2_buffer buf_dq;
            memset(&buf_dq, 0, sizeof(buf_dq));
            memset(&planes, 0, sizeof(struct v4l2_plane) * VIDEO_MAX_PLANES);
            buf_dq.m.planes = planes;
            buf_dq.type = input_buf_type_;
            buf_dq.memory = V4L2_MEMORY_MMAP;
            buf_dq.length = 3;
            // std::unique_lock get_buffer_lock(get_buffer_mutex_);
            int ret;
            ret = ioctl(fd_, VIDIOC_DQBUF, &buf_dq);
            if (0 != ret) {
                throw VpuException(fmt::format("in File:{}  Line: {} \n Failed to dequeue input buffer, errno={}", __FILE__, __LINE__, errno));
            }
        
            
            if ((buf_dq.flags & V4L2_BUF_FLAG_MVX_BUFFER_NEED_REALLOC) == V4L2_BUF_FLAG_MVX_BUFFER_NEED_REALLOC) {
                spdlog::info("getAvailableBuffer error");
                spdlog::error("need to reallocate buffer or res change!");
                
            }
        
            if (input_print_) printBuffer(buf_dq, "[<- input]");
            int index = buf_dq.index;
            if (!input_buffers.count(index)) {
                throw VpuException(fmt::format("Output buffer index error!"));
                return nullptr;
            }
        
            auto buffer_ptr = input_buffers[index];
            buffer_ptr->update(buf_dq);
            buffer_ptr->resetVendorFlags();
        
            return buffer_ptr;       
        }
        
        /**
         * @brief Queues a buffer back to the VPU driver.
         *
         * This is typically used to re-queue an output buffer after its data has been consumed.
         *
         * @param buffer_ptr A shared pointer to the Buffer object to be queued.
         * @throws VpuException if the VIDIOC_QBUF ioctl call fails.
         */
        void returnBuffer(std::shared_ptr<Buffer>& buffer_ptr)
        {
            buffer_ptr->resetByteUsed();
            int ret = ioctl(fd_, VIDIOC_QBUF, &buffer_ptr->getBuffer());
            if (0 != ret) {
                throw VpuException(fmt::format("in File:{}  Line: {} \n Failed to queue input buffer, errno={}", __FILE__, __LINE__, errno));
            }
            if (input_print_)  printBuffer(buffer_ptr->getBuffer(), "[-> output]");
        }

        /**
         * @brief Stops the VPU video stream.
         *
         * Calls the VIDIOC_STREAMOFF ioctl to halt the streaming process.
         */
        void streamOff()
        { 
            streamoff_();
        }

    public:
        /// @brief Flag to indicate if the VPU stream is currently active.
        std::atomic<bool> streamon_flag_;

    protected:
        /// @brief The number of buffers allocated for input and output queues.
        int buffer_num_;
        /// @brief Flag to indicate if the initial output buffer queueing has been performed.
        std::atomic<bool> init_flag_;
        /// @brief Tracks the number of input buffers queued before the stream starts.
        std::atomic<int> valid_buffer_index_;
    };


}
