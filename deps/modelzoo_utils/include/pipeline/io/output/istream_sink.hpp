#pragma once
#include "pipeline/base/enums.hpp"
#include "pipeline/io/base/output_sink.hpp"
#include <memory>
#include <cstdint>
#include <string>

/**
 * @brief 编码后的数据包结构体
 */
struct SinkPacket {
    void* data;
    size_t size;
};

/**
 * @brief 流输出接口 (Stream Sink Interface)
 * 
 * 定义了处理编码后数据流的通用行为。
 * 任何希望接收VPU编码器输出的类（如文件写入器、RTMP推流器）都应实现此接口。
 */
class IStreamSink : public IOutputSink {
public:
    IStreamSink(int sink_id, OUTPUT_SINK sink_type)
        : IOutputSink(sink_id, sink_type, DATA_TYPE::STREAM) {}
    virtual ~IStreamSink() = default;

    /**
     * @brief 打开输出流。
     * @param uri 资源的统一资源标识符 (e.g., "file://path/to/output.h264", "rtmp://server/live/stream_key")
     * @return 如果成功打开则返回 true，否则返回 false。
     */
    virtual int open(const std::string& url) = 0;

    /**
     * @brief 处理一个编码后的数据包。
     * @param packet 包含编码数据的包。
     */
    virtual int handlePacket(const SinkPacket& packet) = 0;

    /**
     * @brief 关闭输出流并释放资源。
     */
    virtual void close() = 0;
};