#pragma once

#include <icraft-xrt/core/tensor.h>

#include <vector>
#include <string>
#include <memory>

#include <array>

/**
 * @brief 公共元数据（组合优于继承）
 * 它包含了所有消息共享的通用元数据。
 */
struct MessageMeta
{
    long long timestamp = 0;
    int source_id = -1; // **极其重要**：标记数据来自哪个InputSource
    std::string chunk_group_id;
    int buffer_index = -1;    // 标记数据缓冲区索引
    bool error_input = false; // 硬件输入是否出错
    bool invalid_ps_frame = false; // 是否PL-PS版于帧为错误帧
};

// 基本消息结构体，用于调试数据通路
struct BasicMessage
{
    MessageMeta meta;
};

// 从输入Actor发往NPU Actor的消息
struct InputMessageForIcore
{
    MessageMeta meta;
    std::vector<icraft::xrt::Tensor> image_tensors; // 直接使用 vector，并通过 std::move 传递所有权
};

// 从NPU Actor发往后处理Actor的消息
struct IcoreMessageForPost
{
    MessageMeta meta;
    std::vector<icraft::xrt::Tensor> icore_tensors;
};

struct MultiSourceInputMessage
{
    MessageMeta meta;
    // 直接使用 vector，并通过 std::move 传递所有权
    std::vector<icraft::xrt::Tensor> icore_tensors;
    std::vector<std::string> chunk_group_ids; // 存储每个输入的 chunk_group_id
    std::vector<int> buffer_indices;          // 存储每个输入的 buffer_index
};


// 用于单核（single ICORE）多网络（multi_net）输出聚合的消息
template <size_t N>
struct MultiNetMessageForPost
{
    MessageMeta meta;
    std::array<std::vector<icraft::xrt::Tensor>, N> icore_tensor_group;
    std::array<bool, N> is_icore_executed;
    MultiNetMessageForPost()
    {
        is_icore_executed.fill(false);
    }
};