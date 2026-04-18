#pragma once

// enums
/// @brief 定义输入输出的枚举类型
// Use 'enum class' to scope the enumerators and prevent name collisions.
enum class INPUT_SOURCE {
    WEBCAM = 0,
    DISK,
    SDI, // PLIN from sdi camera
    PCIE, // PLIN from pcie
    RFU
};

enum class DATA_TYPE {
    IMAGE = 0, //frame as picture w/ or w/o draw box
    STREAM, // h264/h265/rtsp/rtmp w/ or w/o draw box
    TIME_SERIES, // user defined time series data
    TENSOR // tensor data, e.g feature maps or dump data
};

enum class OUTPUT_SINK {
    SOCKET = 0,
    DISK,
    HDMI,  // PLOUT to hdmi display
    PCIE, // PLOUT to pcie
    RFU
};