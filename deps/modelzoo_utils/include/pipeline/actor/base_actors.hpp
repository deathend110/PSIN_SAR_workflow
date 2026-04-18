#pragma once

#include "pipeline/base/messages.hpp"
#include "pipeline/memory/buffer_manager.hpp"
#include "pipeline/base/thread_safe_queue.hpp"

#include <memory>
#include <thread>
#include <atomic>

namespace fpai
{
    // 前向声明
    template <typename InputMsg, typename OutputMsg>
    class BaseActor;

    // Primary template: For intermediate actors with both input and output.
    template <typename InputMsg, typename OutputMsg>
    class BaseActor
    {
    public:
        BaseActor(BufferManager &buffer_manager)
            : buffer_manager_(buffer_manager),
              stop_flag_{false},
              input_queue_(nullptr),
              output_queue_(nullptr)
        {
        }
        virtual ~BaseActor()
        {
            // 确保线程在对象销毁前停止并清理
            // std::cout << "BaseActor destructor called, stopping actor..." << std::endl;
            stop();
        }
        // 禁止拷贝和移动
        BaseActor(const BaseActor &) = delete;
        BaseActor &operator=(const BaseActor &) = delete;
        BaseActor(BaseActor &&) = delete;
        BaseActor &operator=(BaseActor &&) = delete;

        void bindInputQueue(ThreadSafeQueue<InputMsg> *q) { input_queue_ = q; }
        void bindOutputQueue(ThreadSafeQueue<OutputMsg> *q) { output_queue_ = q; }

    protected:
        // **模板方法**：定义了算法的骨架
        virtual void loop() = 0; // 改为纯虚函数，强制子类必须实现它
    public:
        virtual void start()
        {
            stop_flag_.store(false);
            worker_ = std::thread(&BaseActor::loop, this);
        }
        virtual void stop()
        {
            stop_flag_.store(true);
            if (input_queue_) input_queue_->stop();
            if (output_queue_) output_queue_->stop(); // 通知可能阻塞在输出队列的线程
            if (worker_.joinable())
            {
                worker_.join(); // 等待线程结束
            }
        }

    protected:
        BufferManager &buffer_manager_;
        ThreadSafeQueue<InputMsg> *input_queue_;
        ThreadSafeQueue<OutputMsg> *output_queue_;
        std::thread worker_;
        std::atomic<bool> stop_flag_;
    };

    // Specialization for Source Actors (InputMsg = void)
    template <typename OutputMsg>
    class BaseActor<void, OutputMsg>
    {
    public:
        BaseActor(BufferManager &buffer_manager)
            : buffer_manager_(buffer_manager),
              stop_flag_{false},
              output_queue_(nullptr)
        {
        }
        virtual ~BaseActor()
        {
            // 确保线程在对象销毁前停止并清理
            // std::cout << "BaseActor destructor called, stopping actor..." << std::endl;
            stop();
        }
        // 禁止拷贝和移动
        BaseActor(const BaseActor &) = delete;
        BaseActor &operator=(const BaseActor &) = delete;
        BaseActor(BaseActor &&) = delete;
        BaseActor &operator=(BaseActor &&) = delete;

        void bindOutputQueue(ThreadSafeQueue<OutputMsg> *q) { output_queue_ = q; }

    protected:
        // **模板方法**：定义了算法的骨架
        virtual void loop() = 0; // 改为纯虚函数，强制子类必须实现它
    public:
        virtual void start()
        {
            stop_flag_.store(false);
            worker_ = std::thread(&BaseActor::loop, this);
        }
        virtual void stop()
        {
            stop_flag_.store(true);
            if (output_queue_) output_queue_->stop(); // 通知可能阻塞在输出队列的线程
            if (worker_.joinable())
            {
                worker_.join(); // 等待线程结束
            }
        }

    protected:
        BufferManager &buffer_manager_;
        ThreadSafeQueue<OutputMsg> *output_queue_;
        std::thread worker_;
        std::atomic<bool> stop_flag_;
    };

    // Specialization for Sink Actors (OutputMsg = void)
    template <typename InputMsg>
    class BaseActor<InputMsg, void>
    {
    public:
        BaseActor(BufferManager &buffer_manager)
            : buffer_manager_(buffer_manager),
              stop_flag_{false},
              input_queue_(nullptr)
        {
        }
        virtual ~BaseActor()
        {
            // 确保线程在对象销毁前停止并清理
            // std::cout << "BaseActor destructor called, stopping actor..." << std::endl;
            stop();
        }
        // 禁止拷贝和移动
        BaseActor(const BaseActor &) = delete;
        BaseActor &operator=(const BaseActor &) = delete;
        BaseActor(BaseActor &&) = delete;
        BaseActor &operator=(BaseActor &&) = delete;

        void bindInputQueue(ThreadSafeQueue<InputMsg> *q) { input_queue_ = q; }

    protected:
        // **模板方法**：定义了算法的骨架
        virtual void loop() = 0; // 改为纯虚函数，强制子类必须实现它
    public:
        virtual void start()
        {
            stop_flag_.store(false);
            worker_ = std::thread(&BaseActor::loop, this);
        }
        virtual void stop()
        {
            stop_flag_.store(true);
            if (input_queue_) input_queue_->stop();
            if (worker_.joinable())
            {
                worker_.join(); // 等待线程结束
            }
        }

    protected:
        BufferManager &buffer_manager_;
        ThreadSafeQueue<InputMsg> *input_queue_;
        std::thread worker_;
        std::atomic<bool> stop_flag_;
    };
}
