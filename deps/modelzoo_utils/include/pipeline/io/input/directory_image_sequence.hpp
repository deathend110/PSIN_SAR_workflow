#pragma once

#include "pipeline/io/base/input_source.hpp"
#include "file_utils.hpp"
#include "modelzoo_utils.hpp"  // For PicPre
#include "opencv2/opencv.hpp"
#include <string>
#include <vector>
#include <atomic>

// DirectoryImageSequence: An image source that reads sequentially from a directory.
class DirectoryImageSequence : public IInputSource
{
public:
    DirectoryImageSequence(int source_id, const std::string &dir_path, const std::string &filename_txt="")
        : IInputSource(source_id, INPUT_SOURCE::DISK, DATA_TYPE::IMAGE),
        filename_txt_(filename_txt),
        dir_path_(dir_path),
        current_index_(0)
    {
        if(dir_path_.empty())
        {
            throw std::runtime_error("Directory path is empty.");
        }
        if(!filename_txt_.empty())
        {
            image_paths_ = getFullFilePathsFromList(dir_path_, filename_txt_);
        }
        else {
            image_paths_ = listFilenames(dir_path_);
        }

        if (image_paths_.empty())
        {
            throw std::runtime_error("No images found in directory: " + dir_path_);
        }
    }

    // The 'take' operation now reads the next file, preprocesses it, and DMAs it to the given chunk.
    bool take(cv::Mat& out_mat, size_t& index)
    {
        if (current_index_ >= image_paths_.size())
        {
            spdlog::warn("Image sequence finished. No more images to take.");
            index = -1;
            // Or throw an exception if this is an error state
            return false;
        }
        std::string img_path = dir_path_ + "/" + image_paths_[current_index_];
        
        // This is a placeholder for the pre-processing and DMA logic
        // In a real implementation, this might be more complex.
        // For now, we just log it. The actual logic will be in the Actor.
        spdlog::debug("Taking image: {}", img_path);
        out_mat = cv::imread(img_path, cv::IMREAD_COLOR);
        if (out_mat.empty())
        {
            spdlog::error("Failed to read image: {}", img_path);
            return false;
        }
        // if(current_index_ == 1)
        //     cv::imwrite("debug_read_image"+std::to_string(source_id_) +".jpg", out_mat); // For debug
        index = current_index_;
        current_index_++;
        return true;
    }

    // This is a new method specific to this class to get the current image path
    std::string getCurrentImagePath() const
    {
        if (current_index_ > 0 && current_index_ <= image_paths_.size())
        {
            return dir_path_ + "/" + image_paths_[current_index_ - 1];
        }
        return "";
    }

    std::string getCurrentImageName() const
    {
        if (current_index_ > 0 && current_index_ <= image_paths_.size())
        {
            return image_paths_[current_index_ - 1];
        }
        return "";
    }

    bool isFinished() const
    {
        return current_index_ >= image_paths_.size();
    }

private:
    std::string dir_path_;
    std::vector<std::string> image_paths_;
    std::atomic<size_t> current_index_{0};
    const std::string filename_txt_;
};