#pragma once
#include "pipeline/base/enums.hpp"
#include "pipeline/io/output/istream_sink.hpp"

#include <spdlog/spdlog.h>
#include <fstream>
#include <string>

/**
*   Hdmi显示抽象类
 */

class StreamToFileSink : public IStreamSink
{
public:
    StreamToFileSink(int sink_id)
        : IStreamSink(sink_id, OUTPUT_SINK::DISK),
          file_size_(0),
          outstream_()
    {
        
    }


    uint64_t getFileByteSize() const
    {
        return file_size_;
    }

    
    int open(const std::string& file_name) override
    {
        outstream_ = std::ofstream(file_name.c_str(), std::ios_base::out);
        if (!outstream_.is_open()) {
            spdlog::error("StreamToFileSink: Failed to open output file: {}", file_name);
            return -1;
        }
        return 0;
    }

    int handlePacket(const SinkPacket& packet) override
    {
        if(!outstream_.is_open()) {
            spdlog::error("StreamToFileSink: Output file stream is not open.");
            return -1;
        }
        outstream_.write(static_cast<const char*>(packet.data), packet.size);
        outstream_.flush();
        file_size_+= packet.size;
        return 0;
    }

    void close() override
    {
        if(outstream_.is_open()) {
            outstream_.close();
        }
    }
    
private:
    uint64_t file_size_ = 0;
    std::ofstream outstream_;
};
