#include <chrono>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <opencv2/opencv.hpp>
#include <spdlog/spdlog.h>

#include <icraft-xrt/core/session.h>
#include <icraft-xrt/dev/host_device.h>
#include <icraft-xrt/dev/zg330_device.h>
#include <icraft-backends/buyibackend/buyibackend.h>
#include <icraft-backends/hostbackend/backend.h>
#include <icraft-backends/hostbackend/utils.h>
#include <icraft-backends/zg330backend/zg330backend.h>
#include <icraft-xir/serialize/json.h>

using namespace icraft::xrt;
using namespace icraft::xir;

namespace fs = std::filesystem;

namespace
{
    constexpr int EXPECTED_N = 1;
    constexpr int EXPECTED_H = 512;
    constexpr int EXPECTED_W = 512;
    constexpr int EXPECTED_C = 1;

    struct AppConfig
    {
        std::string device_url;
        std::string run_backend;
        bool mmu_mode = true;
        bool speed_mode = false;
        bool compress_ftmp = false;
        int ocm_option = 1;
        bool enable_profile = false;

        std::string image_path;
        int patch_size = 512;
        double overlap_ratio = 0.5;

        std::string json_path;
        std::string raw_path;

        bool dump_outputs = true;
        std::string dump_format = "SFB";
        std::string output_dir = "./io/output/imgpatch";
        int output_wait_ms = 20000;
    };

    struct PatchInfo
    {
        int index = -1;
        int grid_row = -1;
        int grid_col = -1;
        int x = -1;
        int y = -1;
        int width = 0;
        int height = 0;
        bool right_to_left = false;
    };

    struct PatchPacket
    {
        PatchInfo info;
        cv::Mat patch_u8;
    };

    using PatchPostprocessFunc =
        std::function<void(const PatchInfo &, const cv::Mat &, const std::vector<Tensor> &)>;

    std::string trim(std::string value)
    {
        const auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
        value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
        return value;
    }

    std::string stripQuotes(std::string value)
    {
        value = trim(std::move(value));
        if (value.size() >= 2 &&
            ((value.front() == '"' && value.back() == '"') ||
             (value.front() == '\'' && value.back() == '\'')))
        {
            return value.substr(1, value.size() - 2);
        }
        return value;
    }

    class SimpleYaml
    {
    public:
        explicit SimpleYaml(const std::string &path)
        {
            std::ifstream ifs(path);
            if (!ifs)
            {
                throw std::runtime_error("Failed to open config yaml: " + path);
            }

            std::vector<std::string> scopes;
            std::string line;
            while (std::getline(ifs, line))
            {
                const auto comment_pos = line.find('#');
                if (comment_pos != std::string::npos)
                {
                    line = line.substr(0, comment_pos);
                }
                if (trim(line).empty())
                {
                    continue;
                }

                const auto indent = line.find_first_not_of(' ');
                const int level = static_cast<int>((indent == std::string::npos ? 0 : indent) / 2);
                const auto content = trim(line);
                const auto colon_pos = content.find(':');
                if (colon_pos == std::string::npos)
                {
                    continue;
                }

                const auto key = trim(content.substr(0, colon_pos));
                const auto raw_value = trim(content.substr(colon_pos + 1));
                if (static_cast<int>(scopes.size()) <= level)
                {
                    scopes.resize(level + 1);
                }
                scopes[level] = key;
                scopes.resize(level + 1);

                if (!raw_value.empty())
                {
                    values_[joinPath(scopes)] = stripQuotes(raw_value);
                }
            }
        }

        template <typename T>
        T get(const std::string &key) const
        {
            const auto it = values_.find(key);
            if (it == values_.end())
            {
                throw std::runtime_error("Missing required config key: " + key);
            }
            return convert<T>(it->second);
        }

        template <typename T>
        T valueOr(const std::string &key, const T &default_value) const
        {
            const auto it = values_.find(key);
            return it == values_.end() ? default_value : convert<T>(it->second);
        }

    private:
        static std::string joinPath(const std::vector<std::string> &scopes)
        {
            std::string path;
            for (const auto &scope : scopes)
            {
                if (!path.empty())
                {
                    path += ".";
                }
                path += scope;
            }
            return path;
        }

        template <typename T>
        static T convert(const std::string &value)
        {
            std::istringstream iss(value);
            T result{};
            iss >> result;
            if (iss.fail())
            {
                throw std::runtime_error("Failed to convert config value: " + value);
            }
            return result;
        }

        std::unordered_map<std::string, std::string> values_;
    };

    template <>
    inline std::string SimpleYaml::convert<std::string>(const std::string &value)
    {
        return value;
    }

    template <>
    inline bool SimpleYaml::convert<bool>(const std::string &value)
    {
        std::string lowered = value;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (lowered == "true" || lowered == "1" || lowered == "yes")
        {
            return true;
        }
        if (lowered == "false" || lowered == "0" || lowered == "no")
        {
            return false;
        }
        throw std::runtime_error("Failed to convert bool config value: " + value);
    }

    template <typename ShapeType>
    std::string shapeToString(const ShapeType &shape)
    {
        std::ostringstream oss;
        oss << "[";
        for (size_t i = 0; i < shape.size(); ++i)
        {
            if (i > 0)
            {
                oss << ",";
            }
            oss << shape[i];
        }
        oss << "]";
        return oss.str();
    }

    std::string directionString(const PatchInfo &info)
    {
        return info.right_to_left ? "R->L" : "L->R";
    }

    Network loadNetwork(const std::string &json_path, const std::string &raw_path)
    {
        Network network = Network::CreateFromJsonFile(json_path);
        network.lazyLoadParamsFromFile(raw_path);
        return network;
    }

    Session initSession(const std::string &run_backend,
                        const NetworkView &network_view,
                        Device &device,
                        int ocm_option,
                        bool mmu_mode,
                        bool speed_mode,
                        bool compress_ftmp)
    {
        if (run_backend == "host")
        {
            return Session::Create<HostBackend>(network_view, {device});
        }

        if (run_backend == "buyi")
        {
#if defined(USE_BUYI_BACKEND)
            auto session = Session::Create<BuyiBackend, HostBackend>(
                network_view,
                {device, HostDevice::Default()});
            if (mmu_mode)
            {
                return session;
            }

            auto buyi_backend = session->backends[0].cast<BuyiBackend>();
            if (compress_ftmp)
            {
                buyi_backend.compressFtmp();
            }
            if (speed_mode)
            {
                buyi_backend.speedMode();
            }
            return session;
#else
            throw std::runtime_error("This binary was built without Buyi backend support.");
#endif
        }

        if (run_backend == "zg330")
        {
#if defined(USE_ZG330_BACKEND)
            auto session = Session::Create<icraft::xrt::zg330::ZG330Backend, HostBackend>(
                network_view,
                {device, HostDevice::Default()});

            auto zg_backend = session->backends[0].cast<icraft::xrt::zg330::ZG330Backend>();
            if (!compress_ftmp)
            {
                zg_backend.disableEtmOptimize();
            }
            if (!speed_mode)
            {
                zg_backend.disableMergeHardOp();
            }

            if (ocm_option == 0)
            {
                zg_backend.ocmOptimize(icraft::xrt::zg330::OcmOpt::None);
            }
            else if (ocm_option == 1)
            {
                zg_backend.ocmOptimize(icraft::xrt::zg330::OcmOpt::OPTION1);
            }
            else if (ocm_option == 2)
            {
                zg_backend.ocmOptimize(icraft::xrt::zg330::OcmOpt::OPTION2);
            }
            else if (ocm_option == 3)
            {
                zg_backend.ocmOptimize(icraft::xrt::zg330::OcmOpt::OPTION3);
            }
            else if (ocm_option != 4)
            {
                throw std::runtime_error("Unsupported ocm_option. Expected 0, 1, 2, 3, or 4.");
            }

            return session;
#else
            throw std::runtime_error("This binary was built without ZG330 backend support.");
#endif
        }

        throw std::runtime_error("Unsupported run_backend. Expected host, buyi, or zg330.");
    }

    Tensor dataToUInt8Tensor(const uint8_t *input_data, const Value &input_value)
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
        auto param_chunk = HostDevice::MemRegion().malloc(size * sizeof(uint8_t));
        auto *dst = reinterpret_cast<uint8_t *>(param_chunk->begin.cptr());
        std::memcpy(dst, input_data, size * sizeof(uint8_t));
        return Tensor(out_dtype, param_chunk);
    }

    AppConfig loadConfig(const std::string &config_path)
    {
        const SimpleYaml config(config_path);

        AppConfig cfg;
        cfg.device_url = config.get<std::string>("sys.device");
        cfg.run_backend = config.valueOr<std::string>("sys.run_backend", "zg330");
        cfg.mmu_mode = config.valueOr<bool>("sys.mmuMode", true);
        cfg.speed_mode = config.valueOr<bool>("sys.speedMode", false);
        cfg.compress_ftmp = config.valueOr<bool>("sys.compressFtmp", false);
        cfg.ocm_option = config.valueOr<int>("sys.ocm_option", 1);
        cfg.enable_profile = config.valueOr<bool>("sys.profile", false);

        cfg.image_path = config.get<std::string>("pipeline.input.image_path");
        cfg.patch_size = config.valueOr<int>("pipeline.patch.patch_size", 512);
        cfg.overlap_ratio = config.valueOr<double>("pipeline.patch.overlap_ratio", 0.5);

        cfg.json_path = config.get<std::string>("pipeline.icore.json");
        cfg.raw_path = config.get<std::string>("pipeline.icore.raw");

        cfg.dump_outputs = config.valueOr<bool>("debug.dump_outputs", true);
        cfg.dump_format = config.valueOr<std::string>("debug.dump_format", "SFB");
        cfg.output_dir = config.valueOr<std::string>("debug.output_dir", "./io/output/imgpatch");
        cfg.output_wait_ms = config.valueOr<int>("debug.output_wait_ms", 20000);
        return cfg;
    }

    class SnakePatchSource
    {
    public:
        SnakePatchSource(std::string image_path, int patch_size, double overlap_ratio)
            : image_path_(std::move(image_path)),
              patch_size_(patch_size),
              overlap_ratio_(overlap_ratio)
        {
            if (patch_size_ <= 0)
            {
                throw std::invalid_argument("patch_size must be positive.");
            }
            if (overlap_ratio_ < 0.0 || overlap_ratio_ >= 1.0)
            {
                throw std::invalid_argument("overlap_ratio must satisfy 0 <= overlap_ratio < 1.");
            }

            image_u8_ = cv::imread(image_path_, cv::IMREAD_GRAYSCALE);
            if (image_u8_.empty())
            {
                throw std::runtime_error("Failed to read grayscale image: " + image_path_);
            }
            if (image_u8_.type() != CV_8UC1)
            {
                throw std::runtime_error("Image must be CV_8UC1 after grayscale read.");
            }

            stride_ = static_cast<int>(std::lround(patch_size_ * (1.0 - overlap_ratio_)));
            if (stride_ <= 0)
            {
                throw std::runtime_error("Computed stride must be positive.");
            }

            if (image_u8_.cols >= patch_size_ && image_u8_.rows >= patch_size_)
            {
                cols_ = (image_u8_.cols - patch_size_) / stride_ + 1;
                rows_ = (image_u8_.rows - patch_size_) / stride_ + 1;
                total_ = rows_ * cols_;
            }
        }

        bool next(PatchPacket &packet)
        {
            if (cursor_ >= total_)
            {
                return false;
            }

            const int row = cursor_ / cols_;
            const int order_in_row = cursor_ % cols_;
            const bool right_to_left = (row % 2) == 1;
            const int col = right_to_left ? (cols_ - 1 - order_in_row) : order_in_row;
            const int x = col * stride_;
            const int y = row * stride_;

            packet.info.index = cursor_;
            packet.info.grid_row = row;
            packet.info.grid_col = col;
            packet.info.x = x;
            packet.info.y = y;
            packet.info.width = patch_size_;
            packet.info.height = patch_size_;
            packet.info.right_to_left = right_to_left;
            packet.patch_u8 = image_u8_(cv::Rect(x, y, patch_size_, patch_size_)).clone();

            ++cursor_;
            return true;
        }

        int imageWidth() const { return image_u8_.cols; }
        int imageHeight() const { return image_u8_.rows; }
        int patchSize() const { return patch_size_; }
        int stride() const { return stride_; }
        int rows() const { return rows_; }
        int cols() const { return cols_; }
        int totalPatches() const { return total_; }

    private:
        std::string image_path_;
        cv::Mat image_u8_;
        int patch_size_ = 512;
        double overlap_ratio_ = 0.5;
        int stride_ = 256;
        int rows_ = 0;
        int cols_ = 0;
        int total_ = 0;
        int cursor_ = 0;
    };

    class PatchTensorBuilder
    {
    public:
        explicit PatchTensorBuilder(const NetworkView &network_view)
            : input_value_(network_view.inputs()[0])
        {
            const auto shape = input_value_.tensorType()->shape;
            if (shape.size() != 4 ||
                shape[0] != EXPECTED_N ||
                shape[1] != EXPECTED_H ||
                shape[2] != EXPECTED_W ||
                shape[3] != EXPECTED_C)
            {
                throw std::runtime_error("Model input must be NHWC [1,512,512,1], actual shape=" +
                                         shapeToString(shape));
            }

            const auto storage_type = input_value_.tensorType()->element_dtype.getStorageType();
            if (!storage_type.is<icraft::xir::IntegerType>() ||
                !storage_type.cast<icraft::xir::IntegerType>().isUInt8())
            {
                throw std::runtime_error("Model input dtype must be UINT8 for raw 0-255 patch input.");
            }
        }

        Tensor build(const cv::Mat &patch_u8) const
        {
            if (patch_u8.empty())
            {
                throw std::runtime_error("Patch is empty.");
            }
            if (patch_u8.type() != CV_8UC1)
            {
                throw std::runtime_error("Patch must be CV_8UC1.");
            }
            if (patch_u8.rows != EXPECTED_H || patch_u8.cols != EXPECTED_W)
            {
                throw std::runtime_error("Patch size must be 512x512.");
            }

            cv::Mat continuous = patch_u8.isContinuous() ? patch_u8 : patch_u8.clone();
            return dataToUInt8Tensor(continuous.data, input_value_);
        }

    private:
        icraft::xir::Value input_value_;
    };

    class DebugOutputDumper
    {
    public:
        DebugOutputDumper(bool enabled, std::string output_dir, std::string dump_format)
            : enabled_(enabled),
              output_dir_(std::move(output_dir)),
              dump_format_(std::move(dump_format))
        {
            if (enabled_)
            {
                fs::create_directories(output_dir_);
                index_csv_.open(fs::path(output_dir_) / "patch_index.csv", std::ios::out);
                index_csv_ << "patch_index,row,col,x,y,width,height,direction,output_dir\n";
            }
        }

        void dump(const PatchInfo &info, std::vector<Tensor> &host_outputs)
        {
            if (!enabled_)
            {
                return;
            }

            const auto patch_dir = fs::path(output_dir_) / ("patch_" + formatPatchIndex(info.index));
            fs::create_directories(patch_dir);
            for (size_t i = 0; i < host_outputs.size(); ++i)
            {
                const auto out_path = patch_dir / ("out_" + std::to_string(i) + ".ftmp");
                std::ofstream ofs(out_path, std::ios::binary);
                if (!ofs)
                {
                    throw std::runtime_error("Failed to open output dump file: " + out_path.string());
                }
                host_outputs[i].dump(ofs, dump_format_);
            }

            index_csv_ << info.index << ","
                       << info.grid_row << ","
                       << info.grid_col << ","
                       << info.x << ","
                       << info.y << ","
                       << info.width << ","
                       << info.height << ","
                       << directionString(info) << ","
                       << patch_dir.string() << "\n";
        }

    private:
        static std::string formatPatchIndex(int index)
        {
            char buf[32] = {0};
            std::snprintf(buf, sizeof(buf), "%06d", index);
            return std::string(buf);
        }

        bool enabled_ = false;
        std::string output_dir_;
        std::string dump_format_;
        std::ofstream index_csv_;
    };

    class PatchInferenceRunner
    {
    public:
        PatchInferenceRunner(Session &session, Device &device, int output_wait_ms)
            : session_(session),
              device_(device),
              output_wait_ms_(output_wait_ms)
        {
        }

        std::vector<Tensor> forward(const Tensor &input_tensor)
        {
            auto outputs = session_.forward({input_tensor});
            std::vector<Tensor> host_outputs;
            host_outputs.reserve(outputs.size());

            for (auto &output : outputs)
            {
                output.waitForReady(std::chrono::milliseconds(output_wait_ms_));
                host_outputs.push_back(output.to(HostDevice::MemRegion()));
            }

            device_.reset(1);
            return host_outputs;
        }

    private:
        Session &session_;
        Device &device_;
        int output_wait_ms_ = 20000;
    };

    void defaultPatchPostprocess(const PatchInfo &, const cv::Mat &, const std::vector<Tensor> &)
    {
        // 预留接口：后续正式流程会在这里接模型输出 -> 后处理 -> HDMI 显示。
    }

    void logNetworkIO(const NetworkView &network_view)
    {
        const auto inputs = network_view.inputs();
        const auto outputs = network_view.outputs();
        spdlog::info("Model inputs: {}", inputs.size());
        for (size_t i = 0; i < inputs.size(); ++i)
        {
            spdlog::info("  input[{}] shape={}", i, shapeToString(inputs[i].tensorType()->shape));
        }
        spdlog::info("Model outputs: {}", outputs.size());
        for (size_t i = 0; i < outputs.size(); ++i)
        {
            spdlog::info("  output[{}] shape={}", i, shapeToString(outputs[i].tensorType()->shape));
        }
    }
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        std::cerr << "Usage: " << argv[0] << " <config yaml>" << std::endl;
        return 1;
    }

    try
    {
        spdlog::set_level(spdlog::level::debug);
        const AppConfig cfg = loadConfig(argv[1]);

        auto device = Device::Open(cfg.device_url.c_str());
        auto network = loadNetwork(cfg.json_path, cfg.raw_path);
        auto network_view = network.view(0);

        if (network_view.inputs().size() != 1)
        {
            throw std::runtime_error("This example supports exactly one model input.");
        }

        logNetworkIO(network_view);
        PatchTensorBuilder tensor_builder(network_view);

        auto session = initSession(cfg.run_backend,
                                   network_view,
                                   device,
                                   cfg.ocm_option,
                                   cfg.mmu_mode,
                                   cfg.speed_mode,
                                   cfg.compress_ftmp);
        session.enableTimeProfile(cfg.enable_profile);
        session.apply();

        SnakePatchSource patch_source(cfg.image_path, cfg.patch_size, cfg.overlap_ratio);
        spdlog::info("Loaded image {}: {}x{}", cfg.image_path, patch_source.imageWidth(), patch_source.imageHeight());
        spdlog::info("Patch grid: rows={}, cols={}, total={}, patch_size={}, stride={}",
                     patch_source.rows(),
                     patch_source.cols(),
                     patch_source.totalPatches(),
                     patch_source.patchSize(),
                     patch_source.stride());

        DebugOutputDumper dumper(cfg.dump_outputs, cfg.output_dir, cfg.dump_format);
        PatchInferenceRunner runner(session, device, cfg.output_wait_ms);
        PatchPostprocessFunc postprocess = defaultPatchPostprocess;

        PatchPacket packet;
        const auto start = std::chrono::steady_clock::now();
        while (patch_source.next(packet))
        {
            spdlog::info("Patch #{} row={} col={} x={} y={} dir={}",
                         packet.info.index,
                         packet.info.grid_row,
                         packet.info.grid_col,
                         packet.info.x,
                         packet.info.y,
                         directionString(packet.info));

            Tensor input_tensor = tensor_builder.build(packet.patch_u8);
            std::vector<Tensor> host_outputs = runner.forward(input_tensor);
            dumper.dump(packet.info, host_outputs);
            postprocess(packet.info, packet.patch_u8, host_outputs);
        }

        const auto end = std::chrono::steady_clock::now();
        const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        spdlog::info("Finished {} patches in {} ms.", patch_source.totalPatches(), duration_ms);

        Device::Close(device);
        return 0;
    }
    catch (const std::exception &e)
    {
        spdlog::error("imgpatch failed: {}", e.what());
        return -1;
    }
}
