#pragma once

#include "unistd.h"
#include "file_utils.hpp"
#include "pipeline/actor/base_actors.hpp"
#include "pipeline/base/messages.hpp"
#include "pipeline/base/thread_safe_queue.hpp"
#include "pipeline/memory/buffer_manager.hpp"

#include <opencv2/opencv.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fpai
{
    // 双目录帧源：
    // 负责从灰度图目录和 RGB 图目录中按排序后的相同索引读取一对图片，
    // 再把它们合成为一张用于显示的整帧 BGR 图像。
    class DoubleDirectoryFrameSource
    {
    public:
        static constexpr std::string_view LogP = "[DoubleDirSource]";

        DoubleDirectoryFrameSource(std::string gray_dir,
                                   std::string rgb_dir,
                                   int display_width,
                                   int display_height,
                                   bool loop)
            : gray_dir_(std::move(gray_dir)),
              rgb_dir_(std::move(rgb_dir)),
              display_width_(display_width),
              display_height_(display_height),
              loop_(loop),
              current_index_(0),
              pair_count_(0),
              finished_(false)
        {
            // 这里先把所有不会变的输入条件校验掉，避免线程跑起来后才发现配置错误。
            if (display_width_ <= 0 || display_height_ <= 0)
            {
                throw std::invalid_argument("Display width/height must be positive.");
            }
            if (gray_dir_.empty() || rgb_dir_.empty())
            {
                throw std::invalid_argument("Input directories must not be empty.");
            }

            gray_paths_ = collectSortedImagePaths(gray_dir_);
            rgb_paths_ = collectSortedImagePaths(rgb_dir_);

            if (gray_paths_.empty())
            {
                throw std::runtime_error("No readable images found in gray_dir: " + gray_dir_);
            }
            if (rgb_paths_.empty())
            {
                throw std::runtime_error("No readable images found in rgb_dir: " + rgb_dir_);
            }

            pair_count_ = std::min(gray_paths_.size(), rgb_paths_.size());
            if (gray_paths_.size() != rgb_paths_.size())
            {
                spdlog::warn("{} gray_dir has {} files, rgb_dir has {} files; only the first {} pairs will be used.",
                             LogP,
                             gray_paths_.size(),
                             rgb_paths_.size(),
                             pair_count_);
            }
            if (pair_count_ == 0)
            {
                throw std::runtime_error("No valid image pairs found across the two directories.");
            }
        }

        // 读取下一对图像并合成为一帧。
        // 返回 true 表示成功得到一帧；
        // 返回 false 表示没有更多有效帧了（例如播完且不循环）。
        bool nextFrame(cv::Mat &composed_bgr, size_t &pair_index)
        {
            while (true)
            {
                if (pair_count_ == 0)
                {
                    finished_ = true;
                    return false;
                }

                if (current_index_ >= pair_count_)
                {
                    if (loop_)
                    {
                        current_index_ = 0;
                    }
                    else
                    {
                        finished_ = true;
                        return false;
                    }
                }

                const size_t active_index = current_index_;
                cv::Mat gray_mat = cv::imread(gray_paths_[active_index], cv::IMREAD_GRAYSCALE);
                cv::Mat rgb_mat = cv::imread(rgb_paths_[active_index], cv::IMREAD_COLOR);
                ++current_index_;

                if (gray_mat.empty() || rgb_mat.empty())
                {
                    spdlog::error("{} Failed to read pair index {}. gray='{}' rgb='{}'",
                                  LogP,
                                  active_index,
                                  gray_paths_[active_index],
                                  rgb_paths_[active_index]);

                    if (!loop_ && current_index_ >= pair_count_)
                    {
                        finished_ = true;
                        return false;
                    }
                    continue;
                }

                // 左侧目录固定按灰度图读取，再扩成 3 通道，方便和右侧彩图走同一套显示流程。
                cv::Mat gray_bgr;
                cv::cvtColor(gray_mat, gray_bgr, cv::COLOR_GRAY2BGR);

                composed_bgr = composeFrame(gray_bgr, rgb_mat);
                pair_index = active_index;
                return true;
            }
        }

        bool isFinished() const
        {
            return finished_;
        }

        size_t getPairCount() const
        {
            return pair_count_;
        }

    private:
        // 只接收常见静态图片格式，避免把目录里的其他文件误当成输入。
        static bool isSupportedImageFile(const std::filesystem::path &path)
        {
            if (!path.has_extension())
            {
                return false;
            }
            std::string ext = path.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch)
                           { return static_cast<char>(std::tolower(ch)); });
            return ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".bmp" ||
                   ext == ".tif" || ext == ".tiff" || ext == ".webp";
        }

        // 收集目录下的所有图片文件，并按 numericCompare 规则排序。
        // 这样 1.png, 2.png, 10.png 会按更符合直觉的顺序排列。
        static std::vector<std::string> collectSortedImagePaths(const std::string &dir_path)
        {
            std::filesystem::path directory(dir_path);
            if (!std::filesystem::exists(directory))
            {
                throw std::runtime_error("Directory does not exist: " + dir_path);
            }

            std::vector<std::string> filenames;
            for (const auto &entry : std::filesystem::directory_iterator(directory))
            {
                if (entry.is_regular_file() && isSupportedImageFile(entry.path()))
                {
                    filenames.push_back(entry.path().filename().string());
                }
            }

            std::sort(filenames.begin(), filenames.end(), numericCompare);

            std::vector<std::string> full_paths;
            full_paths.reserve(filenames.size());
            for (const auto &filename : filenames)
            {
                full_paths.push_back((directory / filename).string());
            }
            return full_paths;
        }

        // 生成一张完整的 HDMI 输出帧：
        // 左半屏放灰度图，右半屏放 RGB 图，背景补黑。
        cv::Mat composeFrame(const cv::Mat &left_bgr, const cv::Mat &right_bgr) const
        {
            cv::Mat canvas(display_height_, display_width_, CV_8UC3, cv::Scalar(0, 0, 0));

            const int left_slot_width = display_width_ / 2;
            const int right_slot_width = display_width_ - left_slot_width;
            const cv::Rect left_slot(0, 0, left_slot_width, display_height_);
            const cv::Rect right_slot(left_slot_width, 0, right_slot_width, display_height_);

            blitCentered(left_bgr, canvas, left_slot);
            blitCentered(right_bgr, canvas, right_slot);
            return canvas;
        }

        // 将一张图按比例缩放后贴到指定区域中央。
        // 不裁剪内容，宁可留黑边，也保持原始宽高比。
        static void blitCentered(const cv::Mat &src, cv::Mat &dst, const cv::Rect &slot)
        {
            if (src.empty() || slot.width <= 0 || slot.height <= 0)
            {
                return;
            }

            const double scale_w = static_cast<double>(slot.width) / static_cast<double>(src.cols);
            const double scale_h = static_cast<double>(slot.height) / static_cast<double>(src.rows);
            const double scale = std::min(scale_w, scale_h);

            const int resized_w = std::max(1, static_cast<int>(std::round(src.cols * scale)));
            const int resized_h = std::max(1, static_cast<int>(std::round(src.rows * scale)));

            cv::Mat resized;
            cv::resize(src, resized, cv::Size(resized_w, resized_h), 0.0, 0.0, cv::INTER_LINEAR);

            const int x = slot.x + (slot.width - resized_w) / 2;
            const int y = slot.y + (slot.height - resized_h) / 2;
            resized.copyTo(dst(cv::Rect(x, y, resized_w, resized_h)));
        }

        std::string gray_dir_;
        std::string rgb_dir_;
        int display_width_;
        int display_height_;
        bool loop_;
        size_t current_index_;
        size_t pair_count_;
        bool finished_;
        std::vector<std::string> gray_paths_;
        std::vector<std::string> rgb_paths_;
    };

    // 输入 actor：
    // 负责持续从 DoubleDirectoryFrameSource 取帧，
    // 转成 HDMI 需要的 RGB565，并写入 BufferManager 管理的 chunk，
    // 最后把 buffer 索引通过队列发给后面的 HDMIDisplayActor。
    template <typename DeviceType>
    class DoubleDirectoryInputActor : public BaseActor<void, InputMessageForIcore>
    {
    public:
        static constexpr std::string_view LogP = "[DoubleDirInput]";

        DoubleDirectoryInputActor(int source_id,
                                  DeviceType &device,
                                  BufferManager &buffer_manager,
                                  std::string gray_dir,
                                  std::string rgb_dir,
                                  int display_width,
                                  int display_height,
                                  int target_fps,
                                  bool loop)
            : BaseActor<void, InputMessageForIcore>(buffer_manager),
              device_(device),
              source_id_(source_id),
              target_fps_(target_fps),
              frame_interval_us_(0),
              chunk_group_id_("double_dir_input_" + std::to_string(source_id_)),
              buffer_bytes_(static_cast<uint64_t>(display_width) * static_cast<uint64_t>(display_height) * 2),
              finished_(false),
              frame_source_(std::move(gray_dir), std::move(rgb_dir), display_width, display_height, loop)
        {
            // 按目标帧率计算每一帧之间至少要间隔多久。
            if (target_fps_ > 0)
            {
                frame_interval_us_ = std::chrono::microseconds(1000000 / target_fps_);
            }

            // HDMI 最终吃的是 RGB565，所以每个像素占 2 字节。
            this->buffer_manager_.createChunkGroup(chunk_group_id_, device_, buffer_bytes_);
            spdlog::info("{} initialized. pair_count={}, chunk_group='{}'",
                         LogP,
                         frame_source_.getPairCount(),
                         chunk_group_id_);
        }

        ~DoubleDirectoryInputActor()
        {
            this->stop();
        }

        std::string getChunkGroupId() const
        {
            return chunk_group_id_;
        }

        bool isFinished() const
        {
            return finished_.load();
        }

        size_t getPairCount() const
        {
            return frame_source_.getPairCount();
        }

    protected:
        void loop() override
        {
            spdlog::info("{} actor thread started.", LogP);

            // 主循环的职责很单一：
            // 1. 申请一个空闲 buffer
            // 2. 从双目录读出并合成一帧
            // 3. 转成 RGB565 写入 buffer
            // 4. 把 buffer_index 发给显示 actor
            while (!this->stop_flag_)
            {
                const auto loop_start = std::chrono::steady_clock::now();
                int buffer_index = this->buffer_manager_.requestIndex(chunk_group_id_);
                if (buffer_index < 0)
                {
                    break;
                }

                InputMessageForIcore msg;
                msg.meta.source_id = source_id_;
                msg.meta.chunk_group_id = chunk_group_id_;
                msg.meta.buffer_index = buffer_index;
                msg.meta.error_input = false;
                msg.meta.invalid_ps_frame = false;

                cv::Mat composed_bgr;
                size_t pair_index = 0;
                if (!frame_source_.nextFrame(composed_bgr, pair_index))
                {
                    this->buffer_manager_.returnIndex(chunk_group_id_, buffer_index);
                    finished_.store(true);
                    break;
                }

                // timestamp 这里直接复用图片对索引，方便后续调试和串联流程。
                msg.meta.timestamp = static_cast<long long>(pair_index);

                // HDMI sink 使用 RGB565，因此在写入 chunk 之前统一转换一次。
                cv::Mat composed_rgb565;
                cv::cvtColor(composed_bgr, composed_rgb565, cv::COLOR_BGR2BGR565);

                auto &chunk = this->buffer_manager_.getChunk(chunk_group_id_, buffer_index);
                chunk.write(0,
                            reinterpret_cast<char *>(composed_rgb565.data),
                            static_cast<uint64_t>(composed_rgb565.total() * composed_rgb565.elemSize()));

                this->output_queue_->push(std::move(msg));
                sleepToTargetFps(loop_start);
            }

            finished_.store(true);
            spdlog::info("{} actor thread finished.", LogP);
        }

    private:
        // 如果用户配置了输入帧率，这里会主动补 sleep，避免读图和显示跑得过快。
        void sleepToTargetFps(const std::chrono::steady_clock::time_point &loop_start) const
        {
            if (frame_interval_us_.count() <= 0)
            {
                return;
            }
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - loop_start);
            if (elapsed < frame_interval_us_)
            {
                std::this_thread::sleep_for(frame_interval_us_ - elapsed);
            }
        }

        DeviceType &device_;
        int source_id_;
        int target_fps_;
        std::chrono::microseconds frame_interval_us_;
        std::string chunk_group_id_;
        uint64_t buffer_bytes_;
        std::atomic<bool> finished_;
        DoubleDirectoryFrameSource frame_source_;
    };
}
