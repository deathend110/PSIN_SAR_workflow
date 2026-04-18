#pragma once

#include <linux/videodev2.h> // V4L2_PIX_FMT_*
#include <opencv2/opencv.hpp>
#include <string>

/****************************************************************************************
*  需要注意：YUYV和YUV420的帧大小计算
*  YUYV：h*w*2
*  YUV420: h*w*3/2
*  size(YUV420) = size(YUYV)*3/4
*  如果帧大小差异被忽略，导致花屏。
****************************************************************************************/
int yuv422_to_yuv420(uint8_t yuv422[], uint8_t yuv420[], int width, int height)
{
    int ynum=width*height;
    int i,j,k=0;
    //得到Y分量  
    for(i=0;i<ynum;i++){

        yuv420[i]=yuv422[i*2];
    }
    //得到U分量  
    for(i=0;i<height;i++){

        if((i%2)!=0)continue;
        for(j=0;j<(width/2);j++){

            if((4*j+1)>(2*width))break;
            yuv420[ynum+k*2*width/4+j]=yuv422[i*2*width+4*j+1];
                    }
        k++;
    }
    k=0;
    //得到V分量  
    for(i=0;i<height;i++){

        if((i%2)==0) continue;
        for(j=0;j<(width/2);j++){

            if((4*j+3)>(2*width))break;
            yuv420[ynum+ynum/4+k*2*width/4+j]=yuv422[i*2*width+4*j+3];

        }
        k++;
    }

    return 1;
}


// Function to check if a url string is a RTMP or RTSP link
// returns:
// - 0 local file
// - 1 RTMP
// - 2 RTSP
int parse_stream_sink(const std::string& url) {
    const std::string rtmpPrefix = "rtmp://";
    const std::string rtspPrefix = "rtsp";
    if (url.compare(0, rtmpPrefix.size(), rtmpPrefix) == 0)   return 1;
    else if(url.compare(0, rtspPrefix.size(), rtspPrefix) == 0)   return 2;
    else   return 0;
}

int get_spspps_from_h264(uint8_t* buf, int len)
{
    int i = 0;
    for (i = 0; i < len; i++) {
        if (buf[i+0] == 0x00 
            && buf[i + 1] == 0x00
            && buf[i + 2] == 0x00
            && buf[i + 3] == 0x01
            && buf[i + 4] == 0x68) {
            break;
        }
    }
    if (i == len) {
        printf("get_spspps_from_h264 error...");
        return -1;
    }
    
    printf("h264(i=%d):", i);
    for (int j = 0; j < i; j++) {
        printf("%x ", buf[j]);
    }
    return i;
}

static bool is_idr_frame2(uint8_t* buf)
{
    printf("isIDRFrame2: %x\n", buf[0] & 0x1f);
    switch (buf[0] & 0x1f) {
        case 0x7: // SPS
            return true;
        case 8: // PPS
            return true;
        case 5: // IDR
            return true;
        case 1: // P
            return false;
        
        default:
            return false;
            break;
    }
    return false;
}

static bool is_idr_frame1(uint8_t* buf, int size)
{
    int last = 0;
    for (int i = 2; i <= size; ++i) {
        if (i == size) {
            if (last) {
                bool ret = is_idr_frame2(buf + last);//, i - last);
                if (ret) {
                    return true;
                }
            }
        }
        else if (buf[i - 2] == 0x00 && buf[i - 1] == 0x00 && buf[i] == 0x01) {
            if (last) {
                int size = i - last - 3;
                if (buf[i - 3]) ++size;
                bool ret = is_idr_frame2(buf + last);//, size);
                if (ret) {
                    return true;
                }
            }
            last = i + 1;
        }
    }
    return false;
}

static int convert_UYVY422_to_rgb565(const cv::Mat& input, cv::Mat& output)
{
    cv::Mat bgr;
    cv::cvtColor(input, bgr, cv::COLOR_YUV2BGR_Y422);
    cv::cvtColor(bgr, output, cv::COLOR_BGR2BGR565);
    return 0;
}

static int convertBGR2UYVY(const cv::Mat& bgr, cv::Mat& uyvy)
{
    // 检查输入图像是否为 BGR 格式
    if (bgr.type() != CV_8UC3)
    {
        std::cerr << "Input image must be of type CV_8UC3 (BGR format)." << std::endl;
        return -1;
    }

    // 检查输出图像是否为 UYVY 格式
    if (uyvy.type() != CV_8UC2)
    {
        std::cerr << "Output image must be of type CV_8UC2 (UYVY format)." << std::endl;
        return -1;
    }

    for (int row = 0; row < bgr.rows; ++row)
    {
        // 读取 BGR 原图每行指针
        const cv::Vec3b* bgrPtr = bgr.ptr<cv::Vec3b>(row);
        // 输出UYVY每行指针：每个像素对会占用4字节
        uchar* uyvyPtr = uyvy.ptr<uchar>(row);

        for (int col = 0; col < bgr.cols; col += 2)
        {
            // ---- 取第一个像素 (p0) ----
            uchar B0 = bgrPtr[col][0];
            uchar G0 = bgrPtr[col][1];
            uchar R0 = bgrPtr[col][2];

            // 防止输入图宽度是奇数，做一下越界保护
            uchar B1 = 0, G1 = 0, R1 = 0;
            if (col + 1 < bgr.cols)
            {
                B1 = bgrPtr[col + 1][0];
                G1 = bgrPtr[col + 1][1];
                R1 = bgrPtr[col + 1][2];
            }

            // 计算Y0, U0, V0
            int Y0 = static_cast<int>(  0.299 * R0 + 0.587 * G0 + 0.114 * B0);
            int U0 = static_cast<int>(- 0.147 * R0 - 0.289 * G0 + 0.436 * B0 + 128);
            int V0 = static_cast<int>(  0.615 * R0 - 0.515 * G0 - 0.100 * B0 + 128);

            // 计算Y1
            int Y1 = static_cast<int>(  0.299 * R1 + 0.587 * G1 + 0.114 * B1);
            // 对于 UYVY 4:2:2，这两个像素 (p0, p1) 的 U、V 是共用的
            // 如果想更精细，每两个像素取平均，但通常直接用 p0 的 U、V 或 p1 的 U、V 也常见

            // 裁剪到 0~255 避免溢出
            Y0 = std::clamp(Y0, 0, 255);
            U0 = std::clamp(U0, 0, 255);
            V0 = std::clamp(V0, 0, 255);
            Y1 = std::clamp(Y1, 0, 255);

            // 将结果写入 UYVY: U, Y0, V, Y1
            int outIndex = col * 2; // col走2步，对应4字节
            uyvyPtr[outIndex + 0] = static_cast<uchar>(U0);
            uyvyPtr[outIndex + 1] = static_cast<uchar>(Y0);
            uyvyPtr[outIndex + 2] = static_cast<uchar>(V0);
            uyvyPtr[outIndex + 3] = static_cast<uchar>(Y1);
        }
    }
    return 0;
}

static uint64_t V4L2format2datasize(uint32_t fmt)
{
    // Calculate the frame size for 1080p (1920x1080) for a given V4L2 format
    const int WIDTH_1080P = 1920;
    const int HEIGHT_1080P = 1080;

    switch (fmt) {
        case V4L2_PIX_FMT_YUYV:
        case V4L2_PIX_FMT_UYVY:
        case V4L2_PIX_FMT_RGB565:
            return WIDTH_1080P * HEIGHT_1080P * 2;  // 16 bits per pixel

        case V4L2_PIX_FMT_P010:
        case V4L2_PIX_FMT_Y0L2:
        case V4L2_PIX_FMT_RGB24:
        case V4L2_PIX_FMT_BGR24:
            return WIDTH_1080P * HEIGHT_1080P * 3;  // 24 bits per pixel
            
        case V4L2_PIX_FMT_RGB32:
        case V4L2_PIX_FMT_BGR32:
        case V4L2_PIX_FMT_XRGB32:
        case V4L2_PIX_FMT_XBGR32:
        case V4L2_PIX_FMT_ARGB32:
        case V4L2_PIX_FMT_ABGR32:
            return WIDTH_1080P * HEIGHT_1080P * 4;  // 32 bits per pixel
            
        case V4L2_PIX_FMT_YUV420:
        case V4L2_PIX_FMT_YVU420:
        case V4L2_PIX_FMT_YUV420M:
        case V4L2_PIX_FMT_NV12:
        case V4L2_PIX_FMT_NV21:
            return WIDTH_1080P * HEIGHT_1080P * 3 / 2;  // 12 bits per pixel
            
        case V4L2_PIX_FMT_GREY:
        case V4L2_PIX_FMT_Y10:
        case V4L2_PIX_FMT_Y12:
        case V4L2_PIX_FMT_Y16:
            return WIDTH_1080P * HEIGHT_1080P;  // 8 bits per pixel
            
        default:
            return 0;  // Unsupported format
    }
}

/**
 * @brief 根据传入的 V4L2 fourcc 格式返回在1080p下的
 *        - 占用字节数(sz)
 *        - OpenCV 常用 mat 类型(cv_type)
 */
static void V4L2format2params(uint32_t fmt, uint64_t& sz, uint16_t& cv_type)
{
    // 先设为默认值
    sz = 0;
    cv_type = -1; // -1 表示不支持或未知
    auto WIDTH_1080P = 1920;
    auto HEIGHT_1080P = 1080;
    switch (fmt)
    {
    // -------------------------
    // 16 bits per pixel (2 bytes)
    // -------------------------
    case V4L2_PIX_FMT_YUYV:
    case V4L2_PIX_FMT_UYVY:
    case V4L2_PIX_FMT_RGB565:
        // 大小
        sz = static_cast<uint64_t>(WIDTH_1080P) 
             * static_cast<uint64_t>(HEIGHT_1080P) 
             * 2;
        // OpenCV 通常用 CV_8UC2 来存放 2-Byte Packed 像素
        cv_type = CV_8UC2; 
        break;

    // -------------------------
    // 24 bits per pixel (3 bytes)
    // 这里把 P010 / Y0L2 (10-bit) 也简化地一起处理
    // -------------------------
    case V4L2_PIX_FMT_P010:
    case V4L2_PIX_FMT_Y0L2:
    case V4L2_PIX_FMT_RGB24:
    case V4L2_PIX_FMT_BGR24:
        // 大小
        sz = static_cast<uint64_t>(WIDTH_1080P)
             * static_cast<uint64_t>(HEIGHT_1080P)
             * 3;
        // OpenCV 通常用 CV_8UC3 表示 3 通道 8 位
        cv_type = CV_8UC3;
        break;

    // -------------------------
    // 32 bits per pixel (4 bytes)
    // -------------------------
    case V4L2_PIX_FMT_RGB32:
    case V4L2_PIX_FMT_BGR32:
    case V4L2_PIX_FMT_XRGB32:
    case V4L2_PIX_FMT_XBGR32:
    case V4L2_PIX_FMT_ARGB32:
    case V4L2_PIX_FMT_ABGR32:
        sz = static_cast<uint64_t>(WIDTH_1080P)
             * static_cast<uint64_t>(HEIGHT_1080P)
             * 4;
        // OpenCV 常用 CV_8UC4 表示 4 通道 8 位
        cv_type = CV_8UC4;
        break;

    // -------------------------
    // 12 bits per pixel (1.5 bytes/像素)
    // 典型的 YUV420 / NV12 / NV21 同一类
    // -------------------------
    case V4L2_PIX_FMT_YUV420:
    case V4L2_PIX_FMT_YVU420:
    case V4L2_PIX_FMT_YUV420M:
    case V4L2_PIX_FMT_NV12:
    case V4L2_PIX_FMT_NV21:
        sz = (static_cast<uint64_t>(WIDTH_1080P)
              * static_cast<uint64_t>(HEIGHT_1080P)
              * 3) / 2; // 1.5 × width × height
        // 如果想用单平面保存，可用 CV_8UC1
        // (后续再区分 Y/U/V 平面或分配 UV 需自行实现)
        cv_type = CV_8UC1;
        break;

    // -------------------------
    // 8 bits per pixel (1 byte)
    // 注：这里把 Y16 等也放在一起仅做示例
    // -------------------------
    case V4L2_PIX_FMT_GREY:  // 8-bit
    case V4L2_PIX_FMT_Y10:
    case V4L2_PIX_FMT_Y12:
    case V4L2_PIX_FMT_Y16:
        sz = static_cast<uint64_t>(WIDTH_1080P)
             * static_cast<uint64_t>(HEIGHT_1080P);
        // 这里简单映射为 CV_8UC1
        // 实际上 Y16 应当是 CV_16UC1
        cv_type = CV_8UC1;
        break;

    default:
        // 不支持或未知的格式
        sz = 0;
        cv_type = -1;
        break;
    }
}

/**
 * @brief 根据传入的 V4L2 fourcc 格式、图像尺寸 (rows x cols)，
 *        以及可选的数据指针 data_ptr，创建对应的 cv::Mat。
 * 
 * @param fmt      V4L2 fourcc format (如 V4L2_PIX_FMT_YUYV, V4L2_PIX_FMT_NV12, ...)
 * @param rows     图像的高 (行数)
 * @param cols     图像的宽 (列数)
 * @param data_ptr 指向图像数据的指针，可为空。如果不为空，则 Mat 将使用该外部数据。
 * @return         返回对应的 cv::Mat。如无法识别格式或出错，则返回空 Mat (type = -1)。
 */
static cv::Mat V4L2format2cvmat(uint32_t fmt, int rows, int cols, void* data_ptr=nullptr)
{
    // 默认返回空矩阵
    cv::Mat result;
    // 准备一个存储 opencv 类型 (CV_8UC1, CV_8UC2, CV_8UC3, CV_8UC4, CV_16UC1等)
    int cv_type = -1; // 初始化为无效类型
    int adjustedRows = rows;  // 默认与传入相同

    switch (fmt)
    {
    // 16 bits per pixel (2 bytes)
    case V4L2_PIX_FMT_YUYV:
    case V4L2_PIX_FMT_UYVY:
    case V4L2_PIX_FMT_RGB565:
        cv_type = CV_8UC2;
        break;

    // 24 bits per pixel (3 bytes)
    case V4L2_PIX_FMT_P010:
    case V4L2_PIX_FMT_Y0L2:
    case V4L2_PIX_FMT_RGB24:
    case V4L2_PIX_FMT_BGR24:
        cv_type = CV_8UC3;
        break;

    // 32 bits per pixel (4 bytes)
    case V4L2_PIX_FMT_RGB32:
    case V4L2_PIX_FMT_BGR32:
    case V4L2_PIX_FMT_XRGB32:
    case V4L2_PIX_FMT_XBGR32:
    case V4L2_PIX_FMT_ARGB32:
    case V4L2_PIX_FMT_ABGR32:
        cv_type = CV_8UC4;
        break;

    // 12 bits per pixel (1.5 bytes/像素)
    // YUV420 / NV12 / NV21 等 4:2:0 格式，简化为单平面
    case V4L2_PIX_FMT_YUV420:
    case V4L2_PIX_FMT_YVU420:
    case V4L2_PIX_FMT_YUV420M:
    case V4L2_PIX_FMT_NV12:
    case V4L2_PIX_FMT_NV21:
        // 注意：YUV420格式实际上是多个平面的，这里简化处理为单平面
        cv_type = CV_8UC1;
        // 将行数按1.5倍存储所有平面 (Y + UV)
        adjustedRows = static_cast<int>(rows * 3 / 2);
        break;

    // 8 bits per pixel (1 byte)
    case V4L2_PIX_FMT_GREY:  // 8-bit
        cv_type = CV_8UC1;
        break;
        
    // 其他位深格式，需要特殊处理
    case V4L2_PIX_FMT_Y10:
    case V4L2_PIX_FMT_Y12:
        cv_type = CV_16UC1; // 应该使用更高位深
        break;
        
    case V4L2_PIX_FMT_Y16:
        cv_type = CV_16UC1;
        break;

    default:
        // 不支持的格式，返回空矩阵
        return result;
    }

    // 检查类型和数据指针是否有效
    if (cv_type != -1) {
        // 创建引用外部数据的cv::Mat
        // 注意：这不会复制数据，而是引用传入的数据指针
        if(data_ptr)
            result = cv::Mat(adjustedRows, cols, cv_type, data_ptr);
        else
            result = cv::Mat(adjustedRows, cols, cv_type);
    }

    return result;
}