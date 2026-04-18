#pragma once

#include <vector>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <chrono>

/**
 * @brief 一个线程安全的、有界环形队列模板类。
 * 
 * @tparam T 队列中存储的元素类型。推荐使用智能指针（如 std::unique_ptr）来管理对象生命周期。
 */
template <typename T>
class RingQueue
{
public:
    /**
     * @brief 构造一个环形队列。
     * @param capacity 队列的最大容量。
     */
    explicit RingQueue(size_t capacity)
        : capacity_(capacity),
          buffer_(capacity),
          head_(0),
          tail_(0),
          count_(0)
    {
        if (capacity == 0)
        {
            throw std::invalid_argument("RingQueue capacity must be positive.");
        }
    }

    // 禁用拷贝和赋值，因为队列状态（特别是mutex和condvar）不应被复制。
    RingQueue(const RingQueue &) = delete;
    RingQueue &operator=(const RingQueue &) = delete;

    /**
     * @brief 将一个元素推入队列（阻塞操作）。
     * 如果队列已满，此方法将阻塞，直到有空间可用。
     * @param item 要移动到队列中的元素。
     */
    void push(T &&item)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        // 等待直到队列不满
        not_full_cond_.wait(lock, [this] { return count_ < capacity_; });

        buffer_[tail_] = std::move(item);
        tail_ = (tail_ + 1) % capacity_;
        count_++;

        // 通知一个可能在等待的消费者
        not_empty_cond_.notify_one();
    }

    /**
     * @brief 从队列中弹出一个元素（阻塞操作）。
     * 如果队列为空，此方法将阻塞，直到有元素可用。
     * @return 队列头部的元素。
     */
    T pop()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        // 等待直到队列不空
        not_empty_cond_.wait(lock, [this] { return count_ > 0; });

        T item = std::move(buffer_[head_]);
        head_ = (head_ + 1) % capacity_;
        count_--;

        // 通知一个可能在等待的生产者
        not_full_cond_.notify_one();
        return item;
    }

    /**
     * @brief 尝试在指定时间内从队列中弹出一个元素。
     * @param timeout 等待的超时时间。
     * @return 如果成功，返回包含元素的 std::optional；如果超时，返回 std::nullopt。
     */
    std::optional<T> try_pop_for(const std::chrono::milliseconds& timeout)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!not_empty_cond_.wait_for(lock, timeout, [this] { return count_ > 0; }))
        {
            return std::nullopt; // 超时
        }

        T item = std::move(buffer_[head_]);
        head_ = (head_ + 1) % capacity_;
        count_--;

        not_full_cond_.notify_one();
        return item;
    }

    /**
     * @brief 获取队列当前的元素数量。
     * @return 队列中的元素数量。
     */
    size_t size() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return count_;
    }

    /**
     * @brief 检查队列是否为空。
     * @return 如果队列为空则返回 true，否则返回 false。
     */
    bool empty() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return count_ == 0;
    }

    /**
     * @brief 检查队列是否已满。
     * @return 如果队列已满则返回 true，否则返回 false。
     */
    bool full() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return count_ == capacity_;
    }

private:
    const size_t capacity_;
    std::vector<T> buffer_;
    size_t head_;
    size_t tail_;
    size_t count_;

    mutable std::mutex mutex_;
    std::condition_variable not_full_cond_;
    std::condition_variable not_empty_cond_;
};