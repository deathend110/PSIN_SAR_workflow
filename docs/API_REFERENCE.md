# FPAI Pipeline & Actor API Reference

本文档介绍 FPAI Pipeline 框架的核心 API。

---

## 核心类

### BaseActor

所有 Actor 的基类。

```cpp
template <typename DeviceType>
class BaseActor {
public:
    void start();  // 启动 Actor 线程
    void stop();   // 停止 Actor 线程
    
protected:
    virtual void loop() = 0;  // 子类实现的主循环
    
    DeviceType& device_;
    BufferManager& buffer_manager_;
    std::atomic<bool> stop_flag_;
};
```

---

### SDICameraInputActor

SDI 摄像头输入 Actor。

```cpp
template <typename DeviceType, typename BackendType>
class SDICameraInputActor : public BaseActor<DeviceType> {
public:
    SDICameraInputActor(
        int id,
        std::unique_ptr<SDICamera<DeviceType>> camera,
        ThreadSafeQueue<std::unique_ptr<IQueueMessage>>& output,
        DeviceType& device,
        BufferManager& buffer_manager,
        std::vector<icraft::xrt::Session>& imk_sessions = {},
        uint64_t imagemake_reg_base = 0
    );
    
    bool hasImageMake() const;
    std::string getChunkGroupId() const;
};
```

---

### BufferManager

零拷贝内存管理器。

```cpp
class BufferManager {
public:
    // 创建 Chunk 组（在 PS-DDR 上）
    void createChunkGroup(
        const std::string& group_id,
        icraft::xrt::Device device,
        size_t chunk_size
    );
    
    // 创建 PL-DDR Chunk 组
    void createPLDDRChunkGroup(
        const std::string& group_id,
        icraft::xrt::Device device,
        size_t chunk_size
    );
    
    // 请求可用的 buffer index（阻塞）
    int requestIndex(const std::string& group_id);
    
    // 归还 buffer
    void returnIndex(const std::string& group_id, int index);
    
    // 获取 Chunk
    icraft::xrt::MemChunk& getChunk(const std::string& group_id, int index);
};
```

---

## 使用示例

### 创建简单的 Pipeline

```cpp
#include "pipeline/actor/sdicamera_input_actor.hpp"
#include "pipeline/actor/base_actors.hpp"
#include "pipeline/memory/buffer_manager.hpp"

int main() {
    // 1. 初始化设备
    auto device = icraft::xrt::Device::Open("/dev/fpai0");
    auto fpai_device = device.cast<FPAIDevice>();
    
    // 2. 创建 BufferManager
    BufferManager buffer_mgr(3);  // 3 个 buffer
    
    // 3. 创建消息队列
    ThreadSafeQueue<std::unique_ptr<IQueueMessage>> queue(10);
    
    // 4. 创建摄像头
    auto camera = std::make_unique<GenericSDICamera>(
        0, fpai_device, 1920, 1080, /* ... */
    );
    
    // 5. 创建 InputActor
    SDICameraInputActor<FPAIDevice, FPAIBackend> input_actor(
        0, std::move(camera), queue, fpai_device, buffer_mgr
    );
    
    // 6. 启动
    input_actor.start();
    
    // 7. 停止
    std::cin.get();
    input_actor.stop();
    
    icraft::xrt::Device::Close(device);
    return 0;
}
```

---

更多详细信息请参考 `deps/modelzoo_utils/C++_API_reference.md`。
