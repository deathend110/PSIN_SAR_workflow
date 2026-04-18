#pragma once

#include <iostream>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <memory>
#include <vector>

/**
 * @brief A robust, thread-safe queue supporting move-only types and clean shutdown.
 * @tparam T The type of elements in the queue.
 */
template<typename T>
class ThreadSafeQueue {
public:
    explicit ThreadSafeQueue(size_t max_size)
        : max_size_(max_size),
        stop_flag_(false),
        mtx_(), q_(),
        empty_cv_(), full_cv_()
        {}
    // Delete the default constructor to force users to specify a size.
    ThreadSafeQueue() = delete;

    // An overload for lvalues (like int) that copies the value.
    void push(const T& value) {
        std::unique_lock<std::mutex> lock(mtx_);
        // Wait until there is space in the queue or it's stopped.
        full_cv_.wait(lock, [this]{ return q_.size() < max_size_ || stop_flag_; });
        
        if (stop_flag_) return;

        q_.push(value);
        // Notify one waiting consumer that an item is available.
        empty_cv_.notify_one();
    }

    // Overload for movable types (like std::unique_ptr) using rvalue reference.
    void push(T&& value) {
        std::unique_lock<std::mutex> lock(mtx_);
        // [FIX] Wait until there is space in the queue or it's stopped.
        full_cv_.wait(lock, [this]{ return q_.size() < max_size_ || stop_flag_; });

        if (stop_flag_) return;

        q_.push(std::move(value));
        // Notify one waiting consumer that an item is available.
        empty_cv_.notify_one();
    }

    /**
     * @brief Blocks until an item is available or stop() is called.
     * @param value Reference to store the popped value.
     * @return True if an item was successfully popped, false if the queue was stopped and is empty.
     */
    bool wait_and_pop(T& value) {
        std::unique_lock<std::mutex> lock(mtx_);
        empty_cv_.wait(lock, [this]{ return !q_.empty() || stop_flag_; });

        if (q_.empty() && stop_flag_) {
            return false;
        }

        value = std::move(q_.front());
        q_.pop();

        // Notify one waiting producer that space is now available.
        full_cv_.notify_one();
        return true;
    }

    /**
     * @brief Stops the queue and wakes up all waiting threads.
     * No more items can be pushed after this is called.
     */
    void stop() {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            stop_flag_ = true;
        }
        // Wake up all waiting producers AND consumers.
        empty_cv_.notify_all();
        full_cv_.notify_all();
    }

    void restart() {
        std::lock_guard<std::mutex> lock(mtx_);
        stop_flag_ = false;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return q_.size();
    }

private:
    std::atomic<bool> stop_flag_;
    std::queue<T> q_;
    mutable std::mutex mtx_;
    // Use two condition variables: one for empty, one for full.
    std::condition_variable empty_cv_;
    std::condition_variable full_cv_;
    size_t max_size_;
};