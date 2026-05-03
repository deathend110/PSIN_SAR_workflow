#include "workflow/infer/infer_workflow_internal.hpp"

#include <stdexcept>
#include <utility>

namespace workflow::infer
{
    // 按蛇形扫描规则初始化 patch 网格：
    // 第 0 行从左到右，第 1 行从右到左，依此往复。
    SnakePatchSource::SnakePatchSource(cv::Mat image_norm, int patch_size, int stride)
        : image_norm_(std::move(image_norm)), patch_size_(patch_size), stride_(stride)
    {
        if (image_norm_.empty() || image_norm_.type() != CV_32FC1)
        {
            throw std::runtime_error("SnakePatchSource requires CV_32FC1 normalized SAR image.");
        }
        if (image_norm_.cols >= patch_size_ && image_norm_.rows >= patch_size_)
        {
            cols_ = (image_norm_.cols - patch_size_) / stride_ + 1;
            rows_ = (image_norm_.rows - patch_size_) / stride_ + 1;
            total_ = rows_ * cols_;
        }
    }

    // 生成下一个蛇形扫描 patch，并返回它在整图中的坐标信息。
    bool SnakePatchSource::next(PatchPacket &packet)
    {
        if (cursor_ >= total_)
        {
            return false;
        }
        const int row = cursor_ / cols_;
        const int order_in_row = cursor_ % cols_;
        const bool right_to_left = (row % 2) == 1;
        const int col = right_to_left ? (cols_ - 1 - order_in_row) : order_in_row;
        const int x = col * stride_;
        const int y = row * stride_;

        packet.info.index = cursor_;
        packet.info.grid_row = row;
        packet.info.grid_col = col;
        packet.info.x = x;
        packet.info.y = y;
        packet.info.width = patch_size_;
        packet.info.height = patch_size_;
        packet.info.right_to_left = right_to_left;
        packet.patch_norm = image_norm_(cv::Rect(x, y, patch_size_, patch_size_)).clone();
        ++cursor_;
        return true;
    }

    // 返回蛇形扫描总 patch 数。
    int SnakePatchSource::totalPatches() const
    {
        return total_;
    }

    // 返回蛇形扫描网格行数。
    int SnakePatchSource::rows() const
    {
        return rows_;
    }

    // 返回蛇形扫描网格列数。
    int SnakePatchSource::cols() const
    {
        return cols_;
    }

    // 按固定 X/Y 步长初始化调试栅格。
    DebugRasterPatchSource::DebugRasterPatchSource(cv::Mat image_norm, int patch_size, int stride_x_px, int stride_y_px)
        : image_norm_(std::move(image_norm)),
          patch_size_(patch_size),
          stride_x_px_(stride_x_px),
          stride_y_px_(stride_y_px)
    {
        if (image_norm_.empty() || image_norm_.type() != CV_32FC1)
        {
            throw std::runtime_error("DebugRasterPatchSource requires CV_32FC1 normalized SAR image.");
        }
        if (stride_x_px_ <= 0 || stride_y_px_ <= 0)
        {
            throw std::runtime_error("DebugRasterPatchSource requires positive X/Y stride.");
        }
        if (image_norm_.cols >= patch_size_ && image_norm_.rows >= patch_size_)
        {
            cols_ = (image_norm_.cols - patch_size_) / stride_x_px_ + 1;
            rows_ = (image_norm_.rows - patch_size_) / stride_y_px_ + 1;
            total_ = rows_ * cols_;
        }
    }

    // 生成下一个调试栅格 patch；顺序为行优先，从左到右、从上到下。
    bool DebugRasterPatchSource::next(PatchPacket &packet)
    {
        if (cursor_ >= total_)
        {
            return false;
        }
        const int row = cursor_ / cols_;
        const int col = cursor_ % cols_;
        const int x = col * stride_x_px_;
        const int y = row * stride_y_px_;

        packet.info.index = cursor_;
        packet.info.grid_row = row;
        packet.info.grid_col = col;
        packet.info.x = x;
        packet.info.y = y;
        packet.info.width = patch_size_;
        packet.info.height = patch_size_;
        packet.info.right_to_left = false;
        packet.patch_norm = image_norm_(cv::Rect(x, y, patch_size_, patch_size_)).clone();
        ++cursor_;
        return true;
    }

    // 返回调试栅格总 patch 数。
    int DebugRasterPatchSource::totalPatches() const
    {
        return total_;
    }

    // 返回调试栅格行数。
    int DebugRasterPatchSource::rows() const
    {
        return rows_;
    }

    // 返回调试栅格列数。
    int DebugRasterPatchSource::cols() const
    {
        return cols_;
    }
}
