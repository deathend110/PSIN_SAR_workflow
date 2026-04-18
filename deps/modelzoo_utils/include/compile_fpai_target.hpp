#pragma once
namespace fpai
{
#if defined(USE_BUYI_BACKEND)
#include <icraft-xrt/dev/buyi_device.h>
#include <icraft-backends/buyibackend/buyibackend.h>
    // 定义一个通用的FPGA设备类型
    using FPAIDevice = icraft::xrt::BuyiDevice;
    using FPAIBackend = icraft::xrt::BuyiBackend;
#elif defined(USE_ZG330_BACKEND)
// 假设ZG330的头文件路径如下
#include <icraft-xrt/dev/zg330_device.h>
#include <icraft-backends/zg330backend/zg330backend.h>
    // 定义一个通用的FPGA设备类型
    using FPAIDevice = icraft::xrt::ZG330Device;
    using FPAIBackend = icraft::xrt::zg330::ZG330Backend;
#else
#error "No backend selected! Please define USE_BUYI_BACKEND or USE_ZG330_BACKEND."
#endif

}
