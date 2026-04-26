#include "workflow/infer/infer_workflow_internal.hpp"

#include <stdexcept>
#include <utility>

namespace workflow::infer
{
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

    int SnakePatchSource::totalPatches() const
    {
        return total_;
    }

    int SnakePatchSource::rows() const
    {
        return rows_;
    }

    int SnakePatchSource::cols() const
    {
        return cols_;
    }

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

    int DebugRasterPatchSource::totalPatches() const
    {
        return total_;
    }

    int DebugRasterPatchSource::rows() const
    {
        return rows_;
    }

    int DebugRasterPatchSource::cols() const
    {
        return cols_;
    }
}
