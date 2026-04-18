#pragma once

#include "pipeline/base/enums.hpp"

// 输入类型的基类
class IOutputSink {
protected:
    IOutputSink(int sink_id, OUTPUT_SINK sink_type, DATA_TYPE data_type)
        : sink_id_(sink_id),
        sink_type_(sink_type),
        data_type_(data_type)
        {

        }
public:
    // 虚析构函数，允许被继承
    virtual ~IOutputSink() = default;

    int getSinkId() const { return sink_id_; }
    OUTPUT_SINK getSinkType() const { return sink_type_; }
    DATA_TYPE getDataType() const { return data_type_; }
private:
    int sink_id_; // 标识不同的输入源
    OUTPUT_SINK sink_type_;
    DATA_TYPE data_type_;
};
