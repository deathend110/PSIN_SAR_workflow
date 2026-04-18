#pragma once
#include "pipeline/base/enums.hpp"
#include <string>

// "实现部分"的抽象基类
class IInputSource {
protected:
    IInputSource(int source_id, INPUT_SOURCE source_type, DATA_TYPE data_type)
        : source_id_(source_id),
        source_type_(source_type),
        data_type_(data_type)
        {

        }
public:
    virtual ~IInputSource() = default;

    // 提供 public 的 getter 方法来访问这些属性（如果需要）
    int getSourceId() const { return source_id_; }
    INPUT_SOURCE getSourceType() const { return source_type_; }
    DATA_TYPE getDataType() const { return data_type_; }
protected:
    int source_id_; // 标识不同的输入源
    INPUT_SOURCE source_type_; // 例如 "SDI", "HDMI", "File" 等
    DATA_TYPE data_type_; // 例如 "Image", "STREAM", "TIME_SERIES" 等
};