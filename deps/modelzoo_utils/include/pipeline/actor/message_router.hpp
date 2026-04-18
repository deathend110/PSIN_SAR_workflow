#pragma once

#include "pipeline/base/thread_safe_queue.hpp"
#include "pipeline/base/messages.hpp"

#include <iostream>
#include <thread>
#include <mutex>
#include <atomic>
#include <map>
#include <functional>
#include <optional>    // 用于从队列中安全地获取值
#include <type_traits> // 需要包含此头文件

// header for MessageDemux
#include <variant>
#include <tuple>

/**
 * @class MessageRouter
 * @brief 一个通用的、线程安全的消息路由器。
 *
 * @tparam MessageType 消息的数据类型。该类型必须有一个名为 `source_id` 的公共整型成员。
 *
 * 工作流程:
 * 1. 从一个上游的输入队列 (input_queue) 中消费消息。
 * 2. 检查每条消息的 `source_id`。
 * 3. 根据预先注册的路由表，将消息转发到对应的下游队列 (destination_queue)。
 * 4. 在独立的后台线程中运行，不阻塞调用者。
 */
template <typename MessageType>
class MessageRouter
{
public:
    using QueueType = ThreadSafeQueue<MessageType>;
    /**
     * @brief 构造函数。
     * @param input_queue 指向唯一的上游消息队列的指针，路由器将从这里获取消息。
     */
    explicit MessageRouter(QueueType *input_queue)
        : m_input_queue(input_queue), m_running(false)
    {
        if (!m_input_queue)
        {
            throw std::invalid_argument("Input queue cannot be null.");
        }
    }

    /**
     * @brief 析构函数。
     *        如果路由器仍在运行，会自动调用 stop() 来确保线程安全退出。
     */
    ~MessageRouter()
    {
        if (m_running)
        {
            stop();
        }
    }

    // 禁止拷贝和移动构造，因为内含线程和独占资源。
    MessageRouter(const MessageRouter &) = delete;
    MessageRouter &operator=(const MessageRouter &) = delete;
    MessageRouter(MessageRouter &&) = delete;
    MessageRouter &operator=(MessageRouter &&) = delete;

    /**
     * @brief 注册一条路由规则。
     * @param source_id 消息源ID。
     * @param destination_queue 指向该ID对应的下游消息队列的指针。
     */
    void register_route(int source_id, QueueType *destination_queue)
    {
        if (!destination_queue)
        {
            // 或者可以抛出异常，取决于您的错误处理策略
            std::cerr << "Warning: Attempted to register a null destination queue for source_id: " << source_id << std::endl;
            return;
        }
        std::lock_guard<std::mutex> lock(m_routing_table_mutex);
        m_routing_table[source_id] = destination_queue;
        // std::cout << "Route registered: source_id " << source_id << " -> queue " << destination_queue << std::endl;
    }

    /**
     * @brief 启动路由器。
     *        创建一个后台线程并开始处理消息。如果已在运行，则此调用无效。
     */
    void start()
    {
        if (m_running.exchange(true))
        {
            std::cout << "Router is already running." << std::endl;
            return;
        }
        m_worker_thread = std::thread(&MessageRouter::run, this);
        std::cout << "MessageRouter started." << std::endl;
    }

    /**
     * @brief 停止路由器。
     *        向后台线程发送停止信号，并等待其安全退出。
     */
    void stop()
    {
        if (!m_running.exchange(false))
        {
            return;
        }
        if (m_input_queue)
        {
            m_input_queue->stop();
        }
        if (m_worker_thread.joinable())
        {
            m_worker_thread.join();
        }
        std::cout << "MessageRouter stopped." << std::endl;
    }

private:
    /**
     * @brief 路由器的工作循环，在后台线程中运行。
     */
    void run()
    {
        while (m_running)
        {
            // 从输入队列等待并弹出一个消息。
            MessageType message; // 为 unique_ptr 创建一个变量
            if (!m_input_queue->wait_and_pop(message))
            {
                break; // wait_and_pop 返回 false 意味着队列已停止
            }

            int msg_source_id = message.meta.source_id;

            QueueType *destination_queue = nullptr;
            {
                std::lock_guard<std::mutex> lock(m_routing_table_mutex);
                auto it = m_routing_table.find(msg_source_id);
                if (it != m_routing_table.end())
                {
                    destination_queue = it->second;
                }
            }

            if (destination_queue)
            {
                destination_queue->push(std::move(message));
            }
            else
            {
                std::cerr << "Warning: No route found for source_id: " << msg_source_id << ". Message dropped." << std::endl;
            }
        }
        std::cout << "Router worker thread finished." << std::endl;
    }

    // 上游消息队列
    QueueType *m_input_queue;

    // 路由表：source_id -> destination_queue
    std::map<int, QueueType *> m_routing_table;
    std::mutex m_routing_table_mutex;

    // 线程管理
    std::thread m_worker_thread;
    std::atomic<bool> m_running;
};

/**
 * @class MessageDemux (修正版)
 * @brief 根据消息的【类型】将 std::variant 中的消息分发到不同的下游队列。
 *
 * @tparam InputVariant 输入的消息类型，必须是 std::variant<...>
 * @tparam OutputQueues 一个包含所有下游队列指针的 std::tuple<Queue<T1>*, Queue<T2>*, ...>
 *
 * 工作流程:
 * 1. 从一个上游的输入队列 (input_queue) 中消费 `std::variant` 消息。
 * 2. 使用 `std::visit` 检查 `variant` 中包含的具体消息类型。
 * 3. 将消息转发到与该类型对应的下游队列。
 * 4. 在独立的后台线程中运行，不阻塞调用者。
 */
template <typename InputVariant, typename... OutputQueues>
class MessageDemux
{
public:
    using InputQueueType = ThreadSafeQueue<InputVariant>;
    using OutputQueueTuple = std::tuple<ThreadSafeQueue<OutputQueues> *...>;

    /**
     * @brief 构造函数。
     * @param input_queue 指向唯一的上游消息队列的指针。
     * @param output_queues 包含所有下游队列指针的元组。
     */
    explicit MessageDemux(InputQueueType *input_queue, OutputQueueTuple output_queues)
        : m_input_queue(input_queue), m_output_queues(output_queues), m_running(false)
    {
        if (!m_input_queue)
        {
            throw std::invalid_argument("Input queue cannot be null.");
        }
    }

    /**
     * @brief 析构函数。
     *        如果路由器仍在运行，会自动调用 stop() 来确保线程安全退出。
     */
    ~MessageDemux()
    {
        if (m_running)
        {
            stop();
        }
    }

    // 禁止拷贝和移动构造，因为内含线程和独占资源。
    MessageDemux(const MessageDemux &) = delete;
    MessageDemux &operator=(const MessageDemux &) = delete;
    MessageDemux(MessageDemux &&) = delete;
    MessageDemux &operator=(MessageDemux &&) = delete;

    /**
     * @brief 启动路由器。
     *        创建一个后台线程并开始处理消息。如果已在运行，则此调用无效。
     */
    void start()
    {
        if (m_running.exchange(true))
        {
            std::cout << "Router is already running." << std::endl;
            return;
        }
        m_worker_thread = std::thread(&MessageDemux::run, this);
        std::cout << "MessageDemux started." << std::endl;
    }

    /**
     * @brief 停止路由器。
     *        向后台线程发送停止信号，并等待其安全退出。
     */
    void stop()
    {
        if (!m_running.exchange(false))
        {
            return;
        }
        if (m_input_queue)
        {
            m_input_queue->stop();
        }
        if (m_worker_thread.joinable())
        {
            m_worker_thread.join();
        }
        std::cout << "MessageDemux stopped." << std::endl;
    }

private:
    /**
     * @brief 解复用器的工作循环，在后台线程中运行。
     */
    void run()
    {
        while (m_running)
        {
            // 从输入队列等待并弹出一个消息。
            InputVariant message_variant;
            if (!m_input_queue->wait_and_pop(message_variant))
            {
                break; // wait_and_pop 返回 false 意味着队列已停止
            }
            // 使用 std::visit 和 lambda 来处理每种可能的类型
            std::visit([this](auto &&concrete_message)
                       {
                // 获取具体消息的类型
                using ConcreteType = std::decay_t<decltype(concrete_message)>;
                // 在元组中查找匹配类型的队列
                auto& dest_queue_ptr = std::get<ThreadSafeQueue<ConcreteType>*>(m_output_queues);
                
                if (dest_queue_ptr)
                {
                    // 使用 std::forward 完美转发消息，保留其值类别（左值或右值）
                    dest_queue_ptr->push(std::forward<decltype(concrete_message)>(concrete_message));
                }
                else
                {
                    // 这种情况通常不应该发生，除非构造时传入了 nullptr
                    std::cerr << "Warning: No output queue registered for message type. Message dropped." << std::endl;
                } }, message_variant);
        }
    }

    InputQueueType *m_input_queue;
    OutputQueueTuple m_output_queues;

    // 线程管理
    std::thread m_worker_thread;
    std::atomic<bool> m_running;
};

/********************************************************************************
 *                                                                              *
 *                    --- 示例用法 (Example Usage) ---                          *
 *                                                                              *
 *  下面的代码是一个完整的、可编译的示例，演示了如何使用 MessageRouter。         *
 *  您可以在您的项目中移除这部分，或者用它来快速测试。                           *
 *                                                                              *
 *  要编译此示例: g++ -std=c++17 -o router_test your_file.cpp -pthread           *
 *                                                                              *
 ********************************************************************************/

#ifdef ENABLE_ROUTER_TEST_MAIN

#include <queue>
#include <chrono>
#include <condition_variable>
#include "pipeline/base/thread_safe_queue.hpp"
#include "pipeline/base/messages.hpp"

int main()
{
    std::cout << "--- MessageRouter Test ---" << std::endl;

    // 3. 创建队列实例
    ThreadSafeQueue<MyMessage> npu_output_queue;    // NPUActor的输出队列
    ThreadSafeQueue<MyMessage> vpu_encoder_queue_1; // VPUEncoderActor 1的输入队列
    ThreadSafeQueue<MyMessage> vpu_encoder_queue_2; // VPUEncoderActor 2的输入队列

    // 4. 创建并配置路由器
    MessageRouter<MyMessage, ThreadSafeQueue<MyMessage>> router(&npu_output_queue);
    router.register_route(1, &vpu_encoder_queue_1); // 来自source_id 1的消息进入队列1
    router.register_route(2, &vpu_encoder_queue_2); // 来自source_id 2的消息进入队列2

    // 5. 启动路由器
    router.start();

    // 6. 模拟消费者 (VPUEncoderActors)
    std::thread consumer1([&]()
                          {
        while (auto msg_opt = vpu_encoder_queue_1.wait_and_pop()) {
            std::cout << "[Consumer 1] Received message from source_id: " << msg_opt->source_id << " | data: " << msg_opt->data << std::endl;
        }
        std::cout << "[Consumer 1] Finished." << std::endl; });

    std::thread consumer2([&]()
                          {
        while (auto msg_opt = vpu_encoder_queue_2.wait_and_pop()) {
            std::cout << "[Consumer 2] Received message from source_id: " << msg_opt->source_id << " | data: " << msg_opt->data << std::endl;
        }
        std::cout << "[Consumer 2] Finished." << std::endl; });

    // 7. 模拟生产者 (NPUActor)
    std::thread producer([&]()
                         {
        for (int i = 0; i < 5; ++i) {
            npu_output_queue.push({1, "Frame " + std::to_string(i) + " for VPU 1"});
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            npu_output_queue.push({2, "Frame " + std::to_string(i) + " for VPU 2"});
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            npu_output_queue.push({3, "Frame " + std::to_string(i) + " for UNKNOWN"}); // 这条消息将被丢弃
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        } });

    producer.join();
    std::cout << "Producer finished." << std::endl;

    // 等待一小段时间，确保所有消息都被处理
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // 8. 停止所有组件
    std::cout << "Shutting down..." << std::endl;
    router.stop();
    vpu_encoder_queue_1.stop();
    vpu_encoder_queue_2.stop();

    consumer1.join();
    consumer2.join();

    std::cout << "--- Test Finished ---" << std::endl;

    return 0;
}

#endif // ENABLE_ROUTER_TEST_MAIN



/********************************************************************************
 *                                                                              *
 *            --- MessageDemu 示例用法 (Example Usage) ---                       *
 *                                                                              *
 ********************************************************************************/

#ifdef ENABLE_ROUTER_TEST_MAIN
// ... MessageRouter 的测试代码保持不变 ...
#endif // ENABLE_ROUTER_TEST_MAIN


#ifdef ENABLE_DEMUX_TEST_MAIN

#include <queue>
#include <chrono>
#include <condition_variable>
#include <string>
#include "pipeline/base/thread_safe_queue.hpp"

// 1. 定义几种不同的消息类型
struct ReportMessage {
    int report_id;
    std::string content;
};

struct AlertMessage {
    int severity;
    std::string source;
};

// 2. 定义一个 variant 来聚合这些类型
using UpstreamVariant = std::variant<ReportMessage, AlertMessage>;

int main()
{
    std::cout << "--- MessageDemux Test ---" << std::endl;

    // 3. 创建队列实例
    ThreadSafeQueue<UpstreamVariant> upstream_queue;
    ThreadSafeQueue<ReportMessage> report_queue;
    ThreadSafeQueue<AlertMessage> alert_queue;

    // 4. 创建并配置解复用器
    MessageDemux demux(
        &upstream_queue,
        std::make_tuple(&report_queue, &alert_queue)
    );

    // 5. 启动解复用器
    demux.start();

    // 6. 模拟消费者
    std::thread report_consumer([&]() {
        while (auto msg_opt = report_queue.wait_and_pop()) {
            std::cout << "[Report Consumer] Received Report ID: " << msg_opt->report_id << ", Content: " << msg_opt->content << std::endl;
        }
        std::cout << "[Report Consumer] Finished." << std::endl;
    });

    std::thread alert_consumer([&]() {
        while (auto msg_opt = alert_queue.wait_and_pop()) {
            std::cout << "[Alert Consumer] Received Alert! Severity: " << msg_opt->severity << ", Source: " << msg_opt->source << std::endl;
        }
        std::cout << "[Alert Consumer] Finished." << std::endl;
    });

    // 7. 模拟生产者
    std::thread producer([&]() {
        for (int i = 0; i < 3; ++i) {
            upstream_queue.push(ReportMessage{100 + i, "System status normal."});
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            upstream_queue.push(AlertMessage{9, "CPU temperature high!"});
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

    producer.join();
    std::cout << "Producer finished." << std::endl;

    // 等待一小段时间，确保所有消息都被处理
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // 8. 停止所有组件
    std::cout << "Shutting down..." << std::endl;
    demux.stop(); // 这会停止上游队列，进而让消费者退出循环
    report_queue.stop();
    alert_queue.stop();

    report_consumer.join();
    alert_consumer.join();

    std::cout << "--- Demux Test Finished ---" << std::endl;

    return 0;
}

#endif // ENABLE_DEMUX_TEST_MAIN