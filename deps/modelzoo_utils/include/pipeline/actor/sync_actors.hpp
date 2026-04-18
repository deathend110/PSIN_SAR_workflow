/* Brief

                                +-------------------+
                                |                   |
                                |                   |
                                |   Sync Actor      |   DualInputMessageForIcore
--Msg(IR)--Msg(RGB)--Msg(IR)--> | (Timestamp-based) |----Msg(RGB+IR)----Msg(RGB+IR)----->
                                |                   |
                                |                   |
                                +-------------------+
Brief: This actor synchronizes multi-modal input messages (e.g., RGB and IR)
        based on their timestamps. It waits until it receives messages from all
        input streams that have matching timestamps before forwarding them as a
        combined message to the next stage in the pipeline.

*/
#include "pipeline/base/messages.hpp"
#include "pipeline/base/thread_safe_queue.hpp"
#include "pipeline/actor/base_actors.hpp"

#include "log_utils.hpp"
#include "fps_calculator.hpp"
#include "compile_fpai_target.hpp"
#include <memory>
#include <chrono>
#include <map>

// 根据SDICamera输入触发的时间戳进行多模态消息同步
namespace fpai
{
    template <typename DeviceType>
    class EventSyncActor : public BaseActor<InputMessageForIcore, MultiSourceInputMessage>
    {
    public:
        const long long MAX_TIME_DIFFERENCE_NS_ = 8 * 1000 * 1000; // 8ms for 60fps
        const long long MAX_TIME_DELAY_NS_ = 20 * 1000 * 1000;     // 1 frames for 60fps

    public:
        EventSyncActor(DeviceType &device,
                       BufferManager &buffer_manager,
                       int display_source = 0,
                       long long time_difference_ns = 8 * 1000 * 1000,
                       long long time_delay_ns = 20 * 1000 * 1000)
            : BaseActor<InputMessageForIcore, MultiSourceInputMessage>(buffer_manager),
              device_(device),
              rgb_cache_(),
              ir_cache_(),
              last_received_timestamp_(0),
              fps_calculator_(),
              DISPLAY_SOURCE_(display_source),
              MAX_TIME_DIFFERENCE_NS_(time_difference_ns),
              MAX_TIME_DELAY_NS_(time_delay_ns)
        {
        }

    protected:
        void loop() override
        {
            fps_calculator_.tick();
            while (!this->stop_flag_)
            {
                auto start_ts = std::chrono::steady_clock::now();
                InputMessageForIcore input_msg;
                if (!this->input_queue_->wait_and_pop(input_msg))
                {
                    LOG_ERROR(LogP, "Input queue is closed, EventSyncActor loop is stopping.");
                    break;
                }
                auto wait_end_ts = std::chrono::steady_clock::now();
                auto wait_duration = std::chrono::duration_cast<std::chrono::microseconds>(wait_end_ts - start_ts);
                // 1. 根据 source_id 存入对应缓存
                {
                    std::lock_guard<std::mutex> lock(cache_mutex_);
                    last_received_timestamp_ = input_msg.meta.timestamp;

                    if (input_msg.meta.source_id == 0)
                    { // 假设 0 是 RGB
                        rgb_cache_[input_msg.meta.timestamp] = std::move(input_msg);
                    }
                    else
                    { // 假设 1 是 IR
                        ir_cache_[input_msg.meta.timestamp] = std::move(input_msg);
                    }
                    LOG_DEBUG("[SYNC]", "Cached message with ts={}ms from source {}", input_msg.meta.timestamp / 1000000, input_msg.meta.source_id);
                }
                auto cache_end_ts = std::chrono::steady_clock::now();
                auto cache_duration = std::chrono::duration_cast<std::chrono::microseconds>(cache_end_ts - wait_end_ts);
                // 2. 尝试配对
                auto res = findAndPushPairs();
                auto find_end_ts = std::chrono::steady_clock::now();
                auto find_duration = std::chrono::duration_cast<std::chrono::microseconds>(find_end_ts - cache_end_ts);
                // 3. 清理旧缓存
                cleanupOldMessages();
                auto end_ts = std::chrono::steady_clock::now();
                auto clean_duration = std::chrono::duration_cast<std::chrono::microseconds>(end_ts - find_end_ts);
                auto loop_duration = std::chrono::duration_cast<std::chrono::microseconds>(end_ts - start_ts);
                LOG_INFO("[SYNC]", "T({}ms)=wait({}ms)+cache({}ms)+find({}ms)+clean({}ms), matched={}, fps={:.2f}, \nbuffers={}",
                         float(loop_duration.count() / 1000),
                         float(wait_duration.count() / 1000),
                         float(cache_duration.count() / 1000),
                         float(find_duration.count() / 1000),
                         float(clean_duration.count() / 1000),
                         res, fps_calculator_.getFPS(),
                         this->buffer_manager_.listAllStatus());
            }
        }

        bool findAndPushPairs()
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            bool matched = false;
            for (auto it_rgb = rgb_cache_.rbegin(); it_rgb != rgb_cache_.rend();) // 从最新的开始
            {
                auto rgb_ts = it_rgb->first;
                auto best_match_it = ir_cache_.rend(); // 反向迭代器的 "end"
                long long min_diff = MAX_TIME_DIFFERENCE_NS_;

                // 在IR缓存中寻找最佳匹配
                for (auto it_ir = ir_cache_.rbegin(); it_ir != ir_cache_.rend(); ++it_ir)
                {
                    long long diff = std::abs(it_ir->first - rgb_ts);
                    if (diff < min_diff)
                    {
                        min_diff = diff;
                        best_match_it = it_ir;
                    }
                }

                if (best_match_it != ir_cache_.rend())
                {
                    LOG_DEBUG("[SYNC]", "Found matching IR ts={}ms for RGB ts={}ms, diff={}ns", best_match_it->first / 1000000, rgb_ts / 1000000, min_diff);
                    // 找到了配对！
                    // 注意：it_rgb 和 best_match_it 都是反向迭代器。
                    // 要访问它们指向的元素或删除它们，需要先转换为正向迭代器。
                    // rbegin() 指向最后一个元素，其 base() 是 end()。
                    // rend() 指向第一个元素之前，其 base() 是 begin()。
                    // 反向迭代器 it 指向的元素，其对应的正向迭代器是 std::next(it).base()
                    auto forward_it_rgb = std::next(it_rgb).base();
                    auto forward_it_ir = std::next(best_match_it).base();

                    auto &rgb_msg = it_rgb->second;
                    auto &ir_msg = best_match_it->second;

                    MultiSourceInputMessage dual_msg;
                    dual_msg.meta.timestamp = (rgb_msg.meta.timestamp + ir_msg.meta.timestamp) / 2; // 使用平均时间戳
                    dual_msg.meta.source_id = rgb_msg.meta.source_id;                               // 选取可将光RGB的一路作为主ID
                    dual_msg.meta.error_input = rgb_msg.meta.error_input || ir_msg.meta.error_input;
                    if (DISPLAY_SOURCE == 0)
                    {
                        dual_msg.meta.invalid_ps_frame = rgb_msg.meta.invalid_ps_frame;
                        dual_msg.meta.chunk_group_id = rgb_msg.meta.chunk_group_id;
                    }
                    else
                    {
                        dual_msg.meta.invalid_ps_frame = ir_msg.meta.invalid_ps_frame;
                        dual_msg.meta.chunk_group_id = ir_msg.meta.chunk_group_id;
                    }
                    // std::cout << "[SYNC] EventSyncActor: Pushing paired message with timestamps RGB: " << rgb_msg->timestamp / 1000000
                    //           << "ms, IR: " << ir_msg->timestamp / 1000000 << "ms" << std::endl;
                    // **重要**: BufferManager需要知道两个buffer都在使用中
                    // 这里需要仔细设计BufferManager的生命周期管理
                    // 一个简单的策略是让下游Actor负责归还两个buffer index
                    dual_msg.buffer_indices.resize(2);
                    dual_msg.buffer_indices[1] = ir_msg.meta.buffer_index;  // 次buffer
                    dual_msg.buffer_indices[0] = rgb_msg.meta.buffer_index; // 主buffer

                    dual_msg.icore_tensors.resize(2);
                    dual_msg.icore_tensors[0] = std::move(rgb_msg.image_tensors[0]);
                    dual_msg.icore_tensors[1] = std::move(ir_msg.image_tensors[0]);

                    dual_msg.chunk_group_ids.resize(2);
                    dual_msg.chunk_group_ids[0] = rgb_msg.meta.chunk_group_id;
                    dual_msg.chunk_group_ids[1] = ir_msg.meta.chunk_group_id;

                    this->output_queue_->push(std::move(dual_msg));
                    // 从缓存中移除已配对的项
                    // erase() 需要正向迭代器，并返回下一个有效的正向迭代器
                    // 我们需要将其转换回反向迭代器以继续循环
                    it_rgb = std::make_reverse_iterator(rgb_cache_.erase(forward_it_rgb));
                    ir_cache_.erase(forward_it_ir); // IMPORTANT: 记得在CFTIcoreActor内部释放
                    fps_calculator_.tick();
                    LOG_INFO("[SYNC]", "Current FPS: {:.2f}", fps_calculator_.getFPS());
                    matched = true;
                }
                else
                {
                    ++it_rgb;
                    LOG_DEBUG("[SYNC]", "No matching for rgb ts={}ms[rgb_cache size={}, ir_cache size={}]", rgb_ts / 1000000, rgb_cache_.size(), ir_cache_.size());
                }
            }
            return matched;
        }
        void cleanupOldMessages()
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            long long last_ts = last_received_timestamp_.load();
            LOG_DEBUG("[SYNC]", "Before clean RGB cache[{}], IR cache[{}]", rgb_cache_.size(), ir_cache_.size());
            // 清理过时的RGB消息
            while (!rgb_cache_.empty())
            {
                auto it = rgb_cache_.begin();
                if (last_ts - it->first > MAX_TIME_DELAY_NS_)
                {
                    LOG_WARN("[SYNC]", "Removing stale RGB message with ts={}ms from cache (delay > {}ms)", it->first / 1000000, MAX_TIME_DELAY_NS_ / 1000000);
                    this->buffer_manager_.returnIndex(it->second->chunk_group_id, it->second->buffer_index);
                    rgb_cache_.erase(it);
                }
                else
                {
                    // 因为map是按时间戳排序的，如果最早的消息都不过时，那么后面的肯定也不过时
                    break;
                }
            }
            // 清理过时的IR消息
            while (!ir_cache_.empty())
            {
                auto it = ir_cache_.begin();
                if (last_ts - it->first > MAX_TIME_DELAY_NS_)
                {
                    LOG_WARN("[SYNC]", "Removing stale IR message with ts={}ms from cache (delay > {}ms)", it->first / 1000000, MAX_TIME_DELAY_NS_ / 1000000);
                    this->buffer_manager_.returnIndex(it->second->chunk_group_id, it->second->buffer_index);
                    ir_cache_.erase(it);
                }
                else
                {
                    // 同上
                    break;
                }
            }
            LOG_DEBUG("[SYNC]", "After clean RGB cache[{}], IR cache[{}]", rgb_cache_.size(), ir_cache_.size());
        }

    private:
        // --- 成员变量 ---
        std::mutex cache_mutex_;
        DeviceType &device_;
        // 使用时间戳作为key，自动排序
        std::map<long long, InputMessageForIcore> rgb_cache_;
        std::map<long long, InputMessageForIcore> ir_cache_;
        std::atomic<long long> last_received_timestamp_;
        FPSCalculator fps_calculator_;
        const int DISPLAY_SOURCE_;
    };

    // 在PSin的情况下，msg->timestamp存储的是id值，这个情况只要匹配相同的count即可
    template <typename DeviceType>
    class CounterSyncActor : public BaseActor<InputMessageForIcore, MultiSourceInputMessage>
    {
    public:
        CounterSyncActor(DeviceType &device,
                         BufferManager &buffer_manager,
                         int display_source = 0)
            : BaseActor<InputMessageForIcore, MultiSourceInputMessage>(buffer_manager),
              device_(device),
              rgb_cache_(),
              ir_cache_(),
              fps_calculator_(),
              DISPLAY_SOURCE_(display_source)
        {
        }

    protected:
        void loop() override
        {
            fps_calculator_.tick();
            while (!this->stop_flag_)
            {
                auto start_ts = std::chrono::steady_clock::now();
                InputMessageForIcore input_msg;
                if (!this->input_queue_->wait_and_pop(input_msg))
                {
                    LOG_ERROR(LogP, "Input queue is closed, CounterSyncActor loop is stopping.");
                    break;
                }
                auto wait_end_ts = std::chrono::steady_clock::now();
                auto wait_duration = std::chrono::duration_cast<std::chrono::microseconds>(wait_end_ts - start_ts);
                // 1. 根据 source_id 存入对应缓存
                {
                    std::lock_guard<std::mutex> lock(cache_mutex_);
                    last_received_timestamp_ = input_msg.meta.timestamp;

                    if (input_msg.meta.source_id == 0)
                    { // 假设 0 是 RGB
                        rgb_cache_[input_msg.meta.timestamp] = std::move(input_msg);
                    }
                    else
                    { // 假设 1 是 IR
                        ir_cache_[input_msg.meta.timestamp] = std::move(input_msg);
                    }
                    LOG_DEBUG("[SYNC]", "Cached message with ts={}ms from source {}", input_msg.meta.timestamp / 1000000, input_msg.meta.source_id);
                }
                auto cache_end_ts = std::chrono::steady_clock::now();
                auto cache_duration = std::chrono::duration_cast<std::chrono::microseconds>(cache_end_ts - wait_end_ts);
                // 2. 尝试配对
                auto res = findAndPushPairs();
                auto find_end_ts = std::chrono::steady_clock::now();
                auto find_duration = std::chrono::duration_cast<std::chrono::microseconds>(find_end_ts - cache_end_ts);
                // 3. 清理旧缓存
                cleanupOldMessages();
                auto end_ts = std::chrono::steady_clock::now();
                auto clean_duration = std::chrono::duration_cast<std::chrono::microseconds>(end_ts - find_end_ts);
                auto loop_duration = std::chrono::duration_cast<std::chrono::microseconds>(end_ts - start_ts);
                LOG_INFO("[SYNC]", "T({}ms)=wait({}ms)+cache({}ms)+find({}ms)+clean({}ms), matched={}, fps={:.2f}",
                         float(loop_duration.count() / 1000),
                         float(wait_duration.count() / 1000),
                         float(cache_duration.count() / 1000),
                         float(find_duration.count() / 1000),
                         float(clean_duration.count() / 1000),
                         res, fps_calculator_.getFPS());
            }
        }

        bool findAndPushPairs()
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            bool matched = false;
            for (auto it_rgb = rgb_cache_.rbegin(); it_rgb != rgb_cache_.rend();) // 从最新的开始
            {
                auto rgb_ts = it_rgb->first;
                auto best_match_it = ir_cache_.rend(); // 反向迭代器的 "end"
                long long min_diff = 0;

                // 在IR缓存中寻找最佳匹配
                for (auto it_ir = ir_cache_.rbegin(); it_ir != ir_cache_.rend(); ++it_ir)
                {
                    if (it_ir->first == rgb_ts)
                    {
                        best_match_it = it_ir; // counter value match
                    }
                }

                if (best_match_it != ir_cache_.rend())
                {
                    LOG_DEBUG("[SYNC]", "Found matching IR ts={}ms for RGB ts={}ms, diff={}ns", best_match_it->first / 1000000, rgb_ts / 1000000, min_diff);
                    // 找到了配对！
                    // 注意：it_rgb 和 best_match_it 都是反向迭代器。
                    // 要访问它们指向的元素或删除它们，需要先转换为正向迭代器。
                    // rbegin() 指向最后一个元素，其 base() 是 end()。
                    // rend() 指向第一个元素之前，其 base() 是 begin()。
                    // 反向迭代器 it 指向的元素，其对应的正向迭代器是 std::next(it).base()
                    auto forward_it_rgb = std::next(it_rgb).base();
                    auto forward_it_ir = std::next(best_match_it).base();

                    InputMessageForIcore &rgb_msg = it_rgb->second;
                    InputMessageForIcore &ir_msg = best_match_it->second;

                    MultiSourceInputMessage dual_msg;
                    dual_msg.meta.timestamp = (rgb_msg.meta.timestamp + ir_msg.meta.timestamp) / 2; // 使用平均时间戳
                    dual_msg.meta.source_id = rgb_msg.meta.source_id;                                // 选取可将光RGB的一路作为主ID
                    dual_msg.meta.error_input = rgb_msg.meta.error_input || ir_msg.meta.error_input;
                    if (DISPLAY_SOURCE == 0)
                    {
                        dual_msg.meta.invalid_ps_frame = rgb_msg.meta.invalid_ps_frame;
                        dual_msg.meta.chunk_group_id = rgb_msg.meta.chunk_group_id;
                    }
                    else
                    {
                        dual_msg.meta.invalid_ps_frame = ir_msg.meta.invalid_ps_frame;
                        dual_msg.meta.chunk_group_id = ir_msg.meta.chunk_group_id;
                    }
                    // std::cout << "[SYNC] EventSyncActor: Pushing paired message with timestamps RGB: " << rgb_msg->timestamp / 1000000
                    //           << "ms, IR: " << ir_msg->timestamp / 1000000 << "ms" << std::endl;
                    // **重要**: BufferManager需要知道两个buffer都在使用中
                    // 这里需要仔细设计BufferManager的生命周期管理
                    // 一个简单的策略是让下游Actor负责归还两个buffer index
                    dual_msg.buffer_indices.resize(2);
                    dual_msg.buffer_indices[0] = rgb_msg.meta.buffer_index; // 主buffer
                    dual_msg.buffer_indices[1] = ir_msg.meta.buffer_index;  // 次buffer

                    dual_msg.icore_tensors.resize(2);
                    dual_msg.icore_tensors[0] = std::move(rgb_msg.image_tensors[0]);
                    dual_msg.icore_tensors[1] = std::move(ir_msg.image_tensors[0]);

                    dual_msg.chunk_group_ids.resize(2);
                    dual_msg.chunk_group_ids[0] = rgb_msg.meta.chunk_group_id;
                    dual_msg.chunk_group_ids[1] = ir_msg.meta.chunk_group_id;

                    this->output_queue_->push(std::move(dual_msg));

                    // 从缓存中移除已配对的项
                    // erase() 需要正向迭代器，并返回下一个有效的正向迭代器
                    // 我们需要将其转换回反向迭代器以继续循环
                    it_rgb = std::make_reverse_iterator(rgb_cache_.erase(forward_it_rgb));
                    ir_cache_.erase(forward_it_ir);
                    fps_calculator_.tick();
                    LOG_INFO("[SYNC]", "Send message to downstream, current FPS: {:.2f}", fps_calculator_.getFPS());
                    matched = true;
                }
                else
                {
                    ++it_rgb;
                    LOG_DEBUG("[SYNC]", "No matching for rgb ts={}ms[rgb_cache size={}, ir_cache size={}]", rgb_ts / 1000000, rgb_cache_.size(), ir_cache_.size());
                }
            }
            return matched;
        }
        void cleanupOldMessages()
        {
            // no cleaning because psin count value is continuous and always match
        }

    private:
        // --- 成员变量 ---
        std::mutex cache_mutex_;
        Device &device_;
        // 使用时间戳作为key，自动排序
        std::map<long long, InputMessageForIcore> rgb_cache_;
        std::map<long long, InputMessageForIcore> ir_cache_;
        std::atomic<long long> last_received_timestamp_;
        FPSCalculator fps_calculator_;
        const int DISPLAY_SOURCE_;
    };
} // namespace fpai