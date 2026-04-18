#pragma once

#include "pipeline/base/thread_safe_queue.hpp" // 需要一个阻塞队列实现

#include "et_device.hpp"
#include "pcie_arm_utils.hpp"

#include <icraft-xrt/core/tensor.h>
#include <icraft-xrt/dev/buyi_device.h>
#include <icraft-backends/buyibackend/buyibackend.h>
#include <icraft-backends/hostbackend/backend.h>

#include <numeric>
#include <vector>
#include <atomic>
#include <memory>
#include <mutex>
#include <condition_variable>

// Simple thread-safe buffer manager for managing PS and PL reusable udmabuf
class BufferManager
{
public:
    // Default constructor. The manager starts empty.
    explicit BufferManager(size_t chunk_count)
        : chunk_count_(chunk_count),
          map_mutex_{},
          available_indices_per_group_{},
          buffer_pools_{}
    {
    }
    // 禁止拷贝和移动，因为它管理着底层资源
    BufferManager(const BufferManager &) = delete;
    BufferManager &operator=(const BufferManager &) = delete;

    // InputActor会调用这个方法来获取一个可用的buffer
    int requestIndex(const std::string &chunk_group_id)
    {
        // 访问队列本身是线程安全的，但查找map需要保护
        // std::shared_lock<std::shared_mutex> lock(map_mutex_); 初始化后只读
        auto it = available_indices_per_group_.find(chunk_group_id);
        if (it == available_indices_per_group_.end())
        {
            throw std::runtime_error("Requesting index from unregistered group '" + chunk_group_id + "'.");
        }
        int index;
        if (it->second->wait_and_pop(index))
            return index;
        return -1;
    }

    // 当buffer的生命周期结束时，由最后一个使用者调用
    void returnIndex(const std::string &chunk_group_id, int index)
    {
        // 访问队列本身是线程安全的，但查找map需要保护
        // std::shared_lock<std::shared_mutex> lock(map_mutex_); 初始化后只读
        auto it = available_indices_per_group_.find(chunk_group_id);
        if (it == available_indices_per_group_.end())
        {
            fprintf(stderr, "Warning: Returning index to unregistered group '%s'.\n", chunk_group_id.c_str());
            return;
        }
        it->second->push(index);
    }

    /**
     * @brief 根据池名称和索引，获取具体的缓冲区对象引用 (非const版本，用于读写)
     */
    icraft::xrt::MemChunk &getChunk(const std::string &id, int index)
    {
        auto it = buffer_pools_.find(id);
        if (it == buffer_pools_.end())
        {
            throw std::runtime_error("Pool with id '" + id + "' not found.");
        }
        if (index < 0 || index >= it->second.size())
        {
            throw std::out_of_range("Index is out of range for pool '" + id + "'.");
        }
        return it->second[index];
    }

    /**
     * @brief 创建一个新的、具名的缓冲池
     * @param id 池的唯一ID
     * @param device 用于分配内存的设备对象
     * @param buffer_size 池中每个缓冲区的大小
     * @param region 分配内存的区域名称，默认为"udma", 为PS端DDR
     * @return 如果名称已存在则抛出异常
     */
    void createChunkGroup(const std::string &id, const icraft::xrt::Device &device, uint64_t buffer_size, const std::string &region = "udma")
    {
        std::lock_guard<std::shared_mutex> lock(map_mutex_);
        spdlog::debug("Creating PS udma chunk group '{}' with buffer size {} bytes", id, buffer_size);
        if (buffer_pools_.count(id))
        {
            throw std::runtime_error("Buffer group '" + id + "' is already registered.");
        }
        // 1. 创建并存储内存块
        auto chunks = std::vector<icraft::xrt::MemChunk>(chunk_count_);
        for (size_t i = 0; i < chunk_count_; ++i)
        {
            chunks[i] = device.getMemRegion(region).malloc(buffer_size, false);
        }
        buffer_pools_[id] = std::move(chunks);
        // 2. 初始化该组的可用索引队列
        initializeIndexQueue(id, chunk_count_);
    }

    /**
     * @brief 创建一个新的、具名的缓冲池，分配在默认的PL内存区域
     * @param id 池的唯一ID
     * @param device 用于分配内存的设备对象
     * @param buffer_size 池中每个缓冲区的大小
     * @return 如果名称已存在则抛出异常
     */
    void createPLDDRChunkGroup(const std::string &id, const icraft::xrt::Device &device, uint64_t buffer_size)
    {
        std::lock_guard<std::shared_mutex> lock(map_mutex_);
        spdlog::debug("Creating device chunk group '{}' with buffer size {} bytes", id, buffer_size);
        if (buffer_pools_.count(id))
        {
            throw std::runtime_error("Buffer group '" + id + "' is already registered.");
        }
        // 1. 创建并存储内存块
        auto chunks = std::vector<icraft::xrt::MemChunk>(chunk_count_);
        for (size_t i = 0; i < chunk_count_; ++i)
        {
            chunks[i] = device.defaultMemRegion().malloc(buffer_size);
        }
        buffer_pools_[id] = std::move(chunks);
        // 2. 初始化该组的可用索引队列
        initializeIndexQueue(id, chunk_count_);
    }

    /**
     * @brief 创建一个新的、具名的缓冲池，分配在默认的PL内存区域
     * @param id 池的唯一ID
     * @param device 用于分配内存的设备对象
     * @param buffer_size 池中每个缓冲区的大小
     * @return 成功返回true，如果名称已存在则返回false
     */
    bool createChunkGroup_pcie(const std::string &id, const icraft::xrt::Device &device, uint64_t buffer_size, const std::string &region = "udma")
    {
        spdlog::info("Creating device {} chunk pcie group '{}' with buffer size {} bytes", region, id, buffer_size);
        if (buffer_pools_.count(id))
        {
            // 防止重复创建同名池
            return false;
        }
        auto chunks = initHostIn_chunk(device, chunk_count_, buffer_size, region);
        buffer_pools_[id] = std::move(chunks);
        return true;
    }

    /**
     * @brief 创建一个新的、具名的空缓冲池
     * @param id 池的唯一ID
     * @param device 用于分配内存的设备对象
     * @param region 分配内存的区域名称，默认为"udma", 为PS端DDR，如果是"plddr"则为PL端DDR
     * @return 如果名称已存在则抛出异常
     */
    void createEmptyChunkGroup(const std::string &id, const icraft::xrt::Device &device, const std::string &region = "udma")
    {
        std::lock_guard<std::shared_mutex> lock(map_mutex_);
        spdlog::debug("Creating region@{} empty chunk group '{}'", region, id);
        if (buffer_pools_.count(id))
        {
            throw std::runtime_error("Buffer group '" + id + "' is already registered.");
        }
        // 1. 创建并存储内存块
        auto chunks = std::vector<icraft::xrt::MemChunk>(chunk_count_);
        buffer_pools_[id] = std::move(chunks);
        // 2. 初始化该组的可用索引队列
        initializeIndexQueue(id, chunk_count_);
    }

    /**
     * @brief 将一个已存在的 MemChunk 分配到指定池的指定索引位置
     * @param id 池的唯一ID
     * @param index 需要覆写的 MemChunk 索引
     * @param memchunk 要分配的 MemChunk 对象
     * @return 如果名称已存在则抛出异常
     */
    void assignChunkToGroup(const std::string &id, int index, const icraft::xrt::MemChunk &memchunk)
    {
        std::lock_guard<std::shared_mutex> lock(map_mutex_);
        spdlog::debug("Assigning chunk to group '{}', index {}", id, index);

        auto it = buffer_pools_.find(id);
        if (it == buffer_pools_.end())
        {
            throw std::runtime_error("Pool with id '" + id + "' not found.");
        }
        if (index < 0 || index >= it->second.size())
        {
            throw std::out_of_range("Index is out of range for pool '" + id + "'.");
        }
        it->second[index] = memchunk;
    }


    size_t getChunkCount() const
    {
        return chunk_count_;
    }

    void read(void *frame_data, const icraft::xrt::MemChunk &memchunk, uint64_t buffer_size) const
    {
        memchunk.read((char *)frame_data, 0, buffer_size);
    }

    std::string getStatusString(const std::string &id) const
    {
        // 使用共享锁安全地读取 map
        std::shared_lock<std::shared_mutex> lock(map_mutex_);
        auto pool_it = buffer_pools_.find(id);
        if (pool_it == buffer_pools_.end())
        {
            return "Pool['" + id + "' not found]";
        }
        const size_t total_count = pool_it->second.size();
        auto queue_it = available_indices_per_group_.find(id);
        if (queue_it == available_indices_per_group_.end())
        {
            // 这种情况理论上不应发生，如果池存在，队列也应存在
            return "Pool['" + id + "' queue missing]";
        }
        // FIX: 使用 ->size() 访问智能指针指向的对象的成员
        const size_t available_count = queue_it->second->size();

        // FIX: 使用从 buffer_pools_ 获取的特定于该池的总数
        return "Usage[" + std::to_string(available_count) + "/" + std::to_string(total_count) + "]";
    }

    std::string listAllStatus() const
    {
        std::shared_lock<std::shared_mutex> lock(map_mutex_);
        std::string status;
        for (const auto &pair : buffer_pools_)
        {
            const std::string &id = pair.first;
            status += "{" + id + ":" + getStatusString(id) + "}";
        }
        return status;
    }

    void plDDR_ChunkGroupSync(const std::string &src_pool_id, int buffer_index,
                              int start_offset, int end_offset, icraft::xrt::Device &device)
    {
        auto it = buffer_pools_.find(src_pool_id);
        if (it == buffer_pools_.end())
        {
            // 增加健壮性检查
            throw std::runtime_error("Pool with id '" + src_pool_id + "' not found for sync.");
        }
        auto &chunk_group = it->second; // 使用引用避免拷贝
        auto src_base_addr = chunk_group[buffer_index]->begin.addr() + start_offset;
        auto src_end_addr = chunk_group[buffer_index]->begin.addr() + end_offset;

        for (int i = 1; i < chunk_count_; i++)
        {
            int idx = (buffer_index + i) % chunk_count_;
            // std::cout << "memcpy buffer_index from: " << post_msg.buffer_index << " to: " << idx << std::endl;
            auto dest_base_addr = chunk_group[idx]->begin.addr() + start_offset;
            auto dest_end_addr = chunk_group[idx]->begin.addr() + end_offset;
            PLDDRMemRegion::Plddr_memcpy(src_base_addr, src_end_addr, dest_base_addr, dest_end_addr, device);
        }
    }

private:
    void initializeIndexQueue(const std::string &id, size_t chunk_count)
    {
        auto index_queue = std::make_unique<ThreadSafeQueue<int>>(chunk_count);
        for (size_t i = 0; i < chunk_count; ++i)
        {
            index_queue->push(i);
        }
        available_indices_per_group_[id] = std::move(index_queue);
    }
    const size_t chunk_count_;
    mutable std::shared_mutex map_mutex_; // 用于保护 map 结构的互斥锁
    std::map<std::string, std::unique_ptr<ThreadSafeQueue<int>>> available_indices_per_group_;
    std::map<std::string, std::vector<icraft::xrt::MemChunk>> buffer_pools_;
};
