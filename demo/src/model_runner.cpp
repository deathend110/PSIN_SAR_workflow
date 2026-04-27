#include "model_runner.hpp"

#define WISE_ENUM_OPTIONAL_TYPE std::optional

#include <icraft-backends/hostbackend/backend.h>
#include <icraft-backends/zg330backend/zg330backend.h>
#include <icraft-xir/serialize/json.h>
#include <icraft-xrt/core/session.h>
#include <icraft-xrt/dev/host_device.h>
#include <icraft-xrt/dev/zg330_device.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <iostream>

using namespace icraft::xrt;
using namespace icraft::xir;

namespace demo
{
    namespace
    {
        constexpr int kExpectedN = 1;
        constexpr int kExpectedH = 512;
        constexpr int kExpectedW = 512;
        constexpr int kExpectedC = 1;
        constexpr int kSegClasses = 6;
        constexpr int kOutputWaitMs = 20000;
        constexpr const char *kBackend = "zg330";
        constexpr const char *kDeviceUrl = "axi://zg330aiu?npu=0x40000000&dma=0x80000000";

        std::string ShapeToString(const std::vector<int64_t> &shape)
        {
            std::ostringstream oss;
            oss << "[";
            for (std::size_t i = 0; i < shape.size(); ++i)
            {
                if (i != 0)
                {
                    oss << ",";
                }
                oss << shape[i];
            }
            oss << "]";
            return oss.str();
        }

        Network LoadNetwork(const std::filesystem::path &json_path, const std::filesystem::path &raw_path)
        {
            Network network = Network::CreateFromJsonFile(json_path.string());
            network.lazyLoadParamsFromFile(raw_path.string());
            return network;
        }

        Session CreateSession(const NetworkView &network_view, Device &device)
        {
            auto session = Session::Create<icraft::xrt::zg330::ZG330Backend, HostBackend>(
                network_view, {device, HostDevice::Default()});
            return session;
        }

        Tensor DataToFp32Tensor(const float *input_data, const Value &input_value)
        {
            TensorType out_dtype;
            if (input_value.tensorType()->shape[0] == -1)
            {
                out_dtype = input_value.getUsesOp()[0]->outputs[0].tensorType().clone();
            }
            else
            {
                out_dtype = input_value.tensorType().clone();
            }
            const auto size = out_dtype.numElements();
            auto param_chunk = HostDevice::MemRegion().malloc(size * sizeof(float));
            auto *dst = reinterpret_cast<float *>(param_chunk->begin.cptr());
            std::memcpy(dst, input_data, size * sizeof(float));
            return Tensor(out_dtype, param_chunk);
        }

        void ValidateInputShape(const Value &input_value)
        {
            const auto shape = input_value.tensorType()->shape;
            if (shape.size() != 4 || shape[0] != kExpectedN || shape[1] != kExpectedH ||
                shape[2] != kExpectedW || shape[3] != kExpectedC)
            {
                throw std::runtime_error("model input must be NHWC [1,512,512,1], actual=" + ShapeToString(shape));
            }
            if (!input_value.tensorType()->element_dtype.getStorageType().isFP32())
            {
                throw std::runtime_error("model input storage dtype must be FP32");
            }
        }

        void ValidateOutputShape(const NetworkView &network_view)
        {
            if (network_view.outputs().size() != 2)
            {
                throw std::runtime_error("model must provide exactly two outputs: restore and seg logits");
            }

            const auto restore_shape = network_view.outputs()[0].tensorType()->shape;
            const auto seg_shape = network_view.outputs()[1].tensorType()->shape;
            if (restore_shape.size() != 4 || restore_shape[0] != 1 || restore_shape[1] != 512 ||
                restore_shape[2] != 512 || restore_shape[3] != 1)
            {
                throw std::runtime_error("restore output must be [1,512,512,1], actual=" + ShapeToString(restore_shape));
            }
            if (seg_shape.size() != 4 || seg_shape[0] != 1 || seg_shape[1] != 512 ||
                seg_shape[2] != 512 || seg_shape[3] != kSegClasses)
            {
                throw std::runtime_error("seg output must be [1,512,512,6], actual=" + ShapeToString(seg_shape));
            }

            if (!network_view.outputs()[0].tensorType()->element_dtype.isFP32() ||
                !network_view.outputs()[1].tensorType()->element_dtype.isFP32())
            {
                throw std::runtime_error("model outputs must be FP32");
            }
        }

        void ValidateInputImage(const cv::Mat &input)
        {
            if (input.empty())
            {
                throw std::runtime_error("input image is empty");
            }
            if (input.type() != CV_8UC1)
            {
                throw std::runtime_error("input image must be CV_8UC1");
            }
            if (input.rows != kExpectedH || input.cols != kExpectedW)
            {
                throw std::runtime_error("input image must be 512x512");
            }
        }

        cv::Mat NormalizeToFloat32(const cv::Mat &gray_u8)
        {
            ValidateInputImage(gray_u8);
            cv::Mat normalized;
            gray_u8.convertTo(normalized, CV_32FC1, 1.0 / 255.0);
            return normalized;
        }

        cv::Mat RestoreToGrayU8(const Tensor &tensor)
        {
            const auto *data = reinterpret_cast<const float *>(tensor.data().cptr());
            cv::Mat gray(kExpectedH, kExpectedW, CV_8UC1);
            for (int y = 0; y < kExpectedH; ++y)
            {
                auto *row = gray.ptr<std::uint8_t>(y);
                for (int x = 0; x < kExpectedW; ++x)
                {
                    const float v = std::clamp(data[y * kExpectedW + x], 0.0f, 1.0f);
                    row[x] = static_cast<std::uint8_t>(std::round(v * 255.0f));
                }
            }
            return gray;
        }

        cv::Mat LogitsToMaskClassU8(const Tensor &tensor)
        {
            const auto *data = reinterpret_cast<const float *>(tensor.data().cptr());
            cv::Mat mask(kExpectedH, kExpectedW, CV_8UC1);
            for (int y = 0; y < kExpectedH; ++y)
            {
                auto *row = mask.ptr<std::uint8_t>(y);
                for (int x = 0; x < kExpectedW; ++x)
                {
                    const int base = (y * kExpectedW + x) * kSegClasses;
                    int best_cls = 0;
                    float best_score = data[base];
                    for (int cls = 1; cls < kSegClasses; ++cls)
                    {
                        const float score = data[base + cls];
                        if (score > best_score)
                        {
                            best_cls = cls;
                            best_score = score;
                        }
                    }
                    row[x] = static_cast<std::uint8_t>(best_cls + 1);
                }
            }
            return mask;
        }
    }

    struct ModelRunner::Impl
    {
        Device device;
        bool device_open = false;
        Network network;
        Session session;
        Value input_value;

        Impl(const std::filesystem::path &json_path, const std::filesystem::path &raw_path)
        {
            std::cerr << "[demo] open device\n";
            device = Device::Open(kDeviceUrl);
            device_open = true;

            std::cerr << "[demo] load network\n";
            network = LoadNetwork(json_path, raw_path);
            const auto network_view = network.view(0);
            if (network_view.inputs().size() != 1)
            {
                throw std::runtime_error("model must provide exactly one input");
            }
            input_value = network_view.inputs()[0];
            ValidateInputShape(input_value);
            ValidateOutputShape(network_view);

            std::cerr << "[demo] create session\n";
            session = CreateSession(network_view, device);
            std::cerr << "[demo] session apply start\n";
            session.apply();
            std::cerr << "[demo] session apply done\n";
        }

        ~Impl()
        {
            if (device_open)
            {
                Device::Close(device);
                device_open = false;
            }
        }
    };

    ModelRunner::ModelRunner(const std::filesystem::path &json_path, const std::filesystem::path &raw_path)
        : impl_(std::make_unique<Impl>(json_path, raw_path))
    {
    }

    ModelRunner::~ModelRunner() = default;

    ModelRunner::ModelRunner(ModelRunner &&other) noexcept = default;

    ModelRunner &ModelRunner::operator=(ModelRunner &&other) noexcept = default;

    InferenceResult ModelRunner::Run(const cv::Mat &input_gray_512)
    {
        ValidateInputImage(input_gray_512);

        std::cerr << "[demo] prepare tensor\n";
        cv::Mat normalized = NormalizeToFloat32(input_gray_512);
        cv::Mat continuous = normalized.isContinuous() ? normalized : normalized.clone();
        Tensor input_tensor = DataToFp32Tensor(reinterpret_cast<const float *>(continuous.data), impl_->input_value);

        std::cerr << "[demo] forward start\n";
        auto outputs = impl_->session.forward({input_tensor});
        if (outputs.size() != 2)
        {
            throw std::runtime_error("session forward returned unexpected number of outputs");
        }

        std::vector<Tensor> host_outputs;
        host_outputs.reserve(outputs.size());
        for (auto &output : outputs)
        {
            output.waitForReady(std::chrono::milliseconds(kOutputWaitMs));
            host_outputs.push_back(output.to(HostDevice::MemRegion()));
        }
        std::cerr << "[demo] forward done\n";
        impl_->device.reset(1);

        InferenceResult result;
        result.restore = RestoreToGrayU8(host_outputs[0]);
        result.mask_class = LogitsToMaskClassU8(host_outputs[1]);
        return result;
    }
}
