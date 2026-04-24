#include "infer_workflow_hdmi_display.hpp"
#include "workflow/infer/infer_config.hpp"
#include "workflow/infer/infer_workflow.hpp"
#include "workflow/shared/config_utils.hpp"
#include <icraft-backends/hostbackend/backend.h>
#include <icraft-backends/hostbackend/utils.h>
#include <icraft-backends/zg330backend/zg330backend.h>
#include <icraft-xir/serialize/json.h>
#include <icraft-xrt/core/session.h>
#include <icraft-xrt/dev/host_device.h>
#include <icraft-xrt/dev/zg330_device.h>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace icraft::xrt;
using namespace icraft::xir;
using FPAIDevice = icraft::xrt::ZG330Device;

namespace workflow::infer
{
    namespace fs = std::filesystem;

    namespace
    {
        constexpr int EXPECTED_N = kExpectedN;
        constexpr int EXPECTED_H = kExpectedH;
        constexpr int EXPECTED_W = kExpectedW;
        constexpr int EXPECTED_C = kExpectedC;
        constexpr int SEG_CLASSES = kSegClasses;

        const char *g_runtime_stage = "startup";

        void setStage(const char *stage)
        {
            g_runtime_stage = stage;
            spdlog::info("[stage] {}", stage);
        }

        void handleSegfault(int)
        {
            std::fprintf(stderr, "\n[fatal] Segmentation fault near stage: %s\n", g_runtime_stage);
            std::_Exit(139);
        }

        struct RadarConfig
        {
            double c = 3e8;
            double fc = 36.01e9;
            double B = 30e6;
            double Tp = 1e-6;
            double Fs = 4.0 * 30e6;
            double PRF = 480.0;
            double v_platform = 10.0;
            double R0 = 400.0;

            double lam() const { return c / fc; }
            double gamma() const { return B / Tp; }
        };

        struct ComplexImage
        {
            cv::Mat data;
            int rows = 0;
            int cols = 0;
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
            cv::Mat patch_norm;
        };

        struct RuntimeState
        {
            std::string echo_stem;
            int echo_index = 0;
            int echo_count = 0;
            PatchInfo patch;
            double rd_ms = 0.0;
            double infer_ms = 0.0;
            double total_ms = 0.0;
        };
    }

    namespace
    {
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

    std::int32_t readLittleEndianInt32(std::ifstream &ifs)
    {
        unsigned char bytes[4] = {0, 0, 0, 0};
        if (!ifs.read(reinterpret_cast<char *>(bytes), sizeof(bytes)))
        {
            throw std::runtime_error("Failed to read int32 header from echo bin.");
        }
        const std::uint32_t value = static_cast<std::uint32_t>(bytes[0]) |
                                    (static_cast<std::uint32_t>(bytes[1]) << 8) |
                                    (static_cast<std::uint32_t>(bytes[2]) << 16) |
                                    (static_cast<std::uint32_t>(bytes[3]) << 24);
        return static_cast<std::int32_t>(value);
    }

    float readLittleEndianFloat32(std::ifstream &ifs)
    {
        unsigned char bytes[4] = {0, 0, 0, 0};
        if (!ifs.read(reinterpret_cast<char *>(bytes), sizeof(bytes)))
        {
            throw std::runtime_error("Unexpected end of file while reading complex samples.");
        }
        const std::uint32_t raw = static_cast<std::uint32_t>(bytes[0]) |
                                  (static_cast<std::uint32_t>(bytes[1]) << 8) |
                                  (static_cast<std::uint32_t>(bytes[2]) << 16) |
                                  (static_cast<std::uint32_t>(bytes[3]) << 24);
        float value = 0.0f;
        std::memcpy(&value, &raw, sizeof(value));
        return value;
    }

    ComplexImage loadEchoBin(const fs::path &path)
    {
        if (!fs::exists(path))
        {
            throw std::runtime_error("Input echo bin does not exist: " + path.string());
        }
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs)
        {
            throw std::runtime_error("Failed to open input echo bin: " + path.string());
        }

        const std::int32_t height = readLittleEndianInt32(ifs);
        const std::int32_t width = readLittleEndianInt32(ifs);
        if (height <= 0 || width <= 0)
        {
            throw std::runtime_error("Invalid echo shape: " + std::to_string(height) + "x" + std::to_string(width));
        }

        const std::uintmax_t expected_bytes =
            8ull + static_cast<std::uintmax_t>(height) * static_cast<std::uintmax_t>(width) * 2ull * sizeof(float);
        const auto actual_bytes = fs::file_size(path);
        if (actual_bytes != expected_bytes)
        {
            throw std::runtime_error("Echo bin size mismatch. expected=" + std::to_string(expected_bytes) +
                                     ", actual=" + std::to_string(actual_bytes));
        }

        cv::Mat data(height, width, CV_64FC2);
        for (int r = 0; r < height; ++r)
        {
            auto *row = data.ptr<cv::Vec2d>(r);
            for (int c = 0; c < width; ++c)
            {
                row[c] = cv::Vec2d(static_cast<double>(readLittleEndianFloat32(ifs)),
                                   static_cast<double>(readLittleEndianFloat32(ifs)));
            }
        }
        return ComplexImage{data, height, width};
    }

    std::vector<double> fftfreqShifted(int n, double d)
    {
        std::vector<double> freq(n);
        for (int i = 0; i < n; ++i)
        {
            const int k = (i <= (n - 1) / 2) ? i : i - n;
            freq[i] = static_cast<double>(k) / (static_cast<double>(n) * d);
        }

        std::vector<double> shifted(n);
        const int shift = n / 2;
        for (int i = 0; i < n; ++i)
        {
            shifted[i] = freq[(i - shift + n) % n];
        }
        return shifted;
    }

    cv::Mat rollAxis(const cv::Mat &src, int axis, int shift)
    {
        CV_Assert(src.type() == CV_64FC2);
        const int n = (axis == 0) ? src.rows : src.cols;
        if (n == 0)
        {
            return src.clone();
        }
        shift %= n;
        if (shift < 0)
        {
            shift += n;
        }

        cv::Mat dst(src.size(), src.type());
        if (axis == 0)
        {
            for (int r = 0; r < src.rows; ++r)
            {
                src.row((r - shift + src.rows) % src.rows).copyTo(dst.row(r));
            }
        }
        else
        {
            for (int r = 0; r < src.rows; ++r)
            {
                const auto *src_row = src.ptr<cv::Vec2d>(r);
                auto *dst_row = dst.ptr<cv::Vec2d>(r);
                for (int c = 0; c < src.cols; ++c)
                {
                    dst_row[c] = src_row[(c - shift + src.cols) % src.cols];
                }
            }
        }
        return dst;
    }

    cv::Mat fftshift(const cv::Mat &src, int axis)
    {
        const int n = (axis == 0) ? src.rows : src.cols;
        return rollAxis(src, axis, n / 2);
    }

    cv::Mat ifftshift(const cv::Mat &src, int axis)
    {
        const int n = (axis == 0) ? src.rows : src.cols;
        return rollAxis(src, axis, -(n / 2));
    }

    cv::Mat dftAxis(const cv::Mat &src, int axis, bool inverse)
    {
        CV_Assert(src.type() == CV_64FC2);
        int flags = cv::DFT_COMPLEX_OUTPUT | cv::DFT_ROWS;
        if (inverse)
        {
            flags |= cv::DFT_INVERSE | cv::DFT_SCALE;
        }

        cv::Mat dst;
        if (axis == 1)
        {
            cv::dft(src, dst, flags);
            return dst;
        }

        cv::Mat transposed;
        cv::transpose(src, transposed);
        cv::Mat transformed;
        cv::dft(transposed, transformed, flags);
        cv::transpose(transformed, dst);
        return dst;
    }

    cv::Vec2d complexMultiply(const cv::Vec2d &a, const cv::Vec2d &b)
    {
        return cv::Vec2d(a[0] * b[0] - a[1] * b[1], a[0] * b[1] + a[1] * b[0]);
    }

    cv::Mat multiplyRowsByFilter(const cv::Mat &src, const std::vector<cv::Vec2d> &filter)
    {
        CV_Assert(static_cast<int>(filter.size()) == src.rows);
        cv::Mat dst(src.size(), src.type());
        for (int r = 0; r < src.rows; ++r)
        {
            const auto *src_row = src.ptr<cv::Vec2d>(r);
            auto *dst_row = dst.ptr<cv::Vec2d>(r);
            for (int c = 0; c < src.cols; ++c)
            {
                dst_row[c] = complexMultiply(src_row[c], filter[r]);
            }
        }
        return dst;
    }

    cv::Mat multiplyColsByFilter(const cv::Mat &src, const std::vector<cv::Vec2d> &filter)
    {
        CV_Assert(static_cast<int>(filter.size()) == src.cols);
        cv::Mat dst(src.size(), src.type());
        for (int r = 0; r < src.rows; ++r)
        {
            const auto *src_row = src.ptr<cv::Vec2d>(r);
            auto *dst_row = dst.ptr<cv::Vec2d>(r);
            for (int c = 0; c < src.cols; ++c)
            {
                dst_row[c] = complexMultiply(src_row[c], filter[c]);
            }
        }
        return dst;
    }

    std::vector<cv::Vec2d> makeComplexExponential(const std::vector<double> &freq, double coefficient)
    {
        std::vector<cv::Vec2d> out(freq.size());
        for (size_t i = 0; i < freq.size(); ++i)
        {
            const double phase = coefficient * freq[i] * freq[i];
            out[i] = cv::Vec2d(std::cos(phase), std::sin(phase));
        }
        return out;
    }

    cv::Mat vectorizedRcmc(const cv::Mat &data_fa, const std::vector<double> &f_a, const RadarConfig &cfg)
    {
        const int nr = data_fa.rows;
        const int na = data_fa.cols;
        const double delta_r = cfg.c / (2.0 * cfg.Fs);
        const double v2 = cfg.v_platform * cfg.v_platform;
        const double lam = cfg.lam();

        cv::Mat output = cv::Mat::zeros(data_fa.size(), data_fa.type());
        for (int a = 0; a < na; ++a)
        {
            const double delta_R = cfg.R0 * std::pow(lam * f_a[a], 2.0) / (8.0 * v2);
            const double signed_shift = delta_R / delta_r;
            for (int r = 0; r < nr; ++r)
            {
                const double map_r = static_cast<double>(r) + signed_shift;
                if (map_r < 0.0 || map_r >= static_cast<double>(nr - 1))
                {
                    continue;
                }
                const int idx0 = static_cast<int>(std::floor(map_r));
                const double w = map_r - static_cast<double>(idx0);
                const cv::Vec2d v0 = data_fa.at<cv::Vec2d>(idx0, a);
                const cv::Vec2d v1 = data_fa.at<cv::Vec2d>(idx0 + 1, a);
                output.at<cv::Vec2d>(r, a) = cv::Vec2d((1.0 - w) * v0[0] + w * v1[0],
                                                       (1.0 - w) * v0[1] + w * v1[1]);
            }
        }
        return output;
    }

    cv::Mat runImaging(const cv::Mat &echo, const RadarConfig &cfg)
    {
        const int nr = echo.rows;
        const int na = echo.cols;
        const auto f_r = fftfreqShifted(nr, 1.0 / cfg.Fs);
        const auto f_a = fftfreqShifted(na, 1.0 / cfg.PRF);

        const auto Hr = makeComplexExponential(f_r, -CV_PI / cfg.gamma());
        const double Ka = 2.0 * cfg.v_platform * cfg.v_platform / (cfg.lam() * cfg.R0);
        const auto Ha = makeComplexExponential(f_a, -CV_PI / Ka);

        cv::Mat data_rc = fftshift(echo.clone(), 0);
        data_rc = dftAxis(data_rc, 0, false);
        data_rc = fftshift(data_rc, 0);
        data_rc = multiplyRowsByFilter(data_rc, Hr);
        data_rc = dftAxis(data_rc, 0, true);

        cv::Mat data_fa = fftshift(data_rc, 1);
        data_fa = dftAxis(data_fa, 1, false);
        data_fa = fftshift(data_fa, 1);

        cv::Mat data_rcmc = vectorizedRcmc(data_fa, f_a, cfg);
        cv::Mat compressed = multiplyColsByFilter(data_rcmc, Ha);
        compressed = ifftshift(compressed, 1);
        return dftAxis(compressed, 1, true);
    }

    cv::Mat magnitudeMinMaxToNormF32(const cv::Mat &complex_img)
    {
        CV_Assert(complex_img.type() == CV_64FC2);
        cv::Mat mag(complex_img.rows, complex_img.cols, CV_64FC1);
        double min_val = std::numeric_limits<double>::infinity();
        double max_val = -std::numeric_limits<double>::infinity();
        for (int r = 0; r < complex_img.rows; ++r)
        {
            const auto *src_row = complex_img.ptr<cv::Vec2d>(r);
            auto *mag_row = mag.ptr<double>(r);
            for (int c = 0; c < complex_img.cols; ++c)
            {
                const double value = std::hypot(src_row[c][0], src_row[c][1]);
                mag_row[c] = value;
                min_val = std::min(min_val, value);
                max_val = std::max(max_val, value);
            }
        }

        cv::Mat out(complex_img.rows, complex_img.cols, CV_32FC1, cv::Scalar(0.0f));
        if (max_val == min_val)
        {
            return out;
        }
        const double scale = 1.0 / (max_val - min_val);
        for (int r = 0; r < mag.rows; ++r)
        {
            const auto *mag_row = mag.ptr<double>(r);
            auto *out_row = out.ptr<float>(r);
            for (int c = 0; c < mag.cols; ++c)
            {
                out_row[c] = static_cast<float>((mag_row[c] - min_val) * scale);
            }
        }
        return out;
    }

    std::vector<fs::path> collectSarImages(const AppConfig &cfg)
    {
        if (!fs::exists(cfg.sar_img_dir) || !fs::is_directory(cfg.sar_img_dir))
        {
            throw std::runtime_error("Input SAR image directory does not exist: " + cfg.sar_img_dir.string());
        }

        std::vector<fs::path> files;
        const auto wanted_ext = shared::ToLower(cfg.sar_img_ext);
        if (cfg.recursive)
        {
            for (const auto &entry : fs::recursive_directory_iterator(cfg.sar_img_dir))
            {
                if (entry.is_regular_file() && shared::ToLower(entry.path().extension().string()) == wanted_ext)
                {
                    files.push_back(entry.path());
                }
            }
        }
        else
        {
            for (const auto &entry : fs::directory_iterator(cfg.sar_img_dir))
            {
                if (entry.is_regular_file() && shared::ToLower(entry.path().extension().string()) == wanted_ext)
                {
                    files.push_back(entry.path());
                }
            }
        }
        std::sort(files.begin(), files.end());
        return files;
    }

    cv::Mat loadSarImageNorm(const fs::path &path)
    {
        cv::Mat gray = cv::imread(path.string(), cv::IMREAD_GRAYSCALE);
        if (gray.empty())
        {
            throw std::runtime_error("Failed to read SAR image: " + path.string());
        }
        cv::Mat norm;
        gray.convertTo(norm, CV_32FC1, 1.0 / 255.0);
        return norm;
    }

    class SnakePatchSource
    {
    public:
        SnakePatchSource(cv::Mat image_norm, int patch_size, int stride)
            : image_norm_(std::move(image_norm)), patch_size_(patch_size), stride_(stride)
        {
            if (image_norm_.empty() || image_norm_.type() != CV_32FC1)
            {
                throw std::runtime_error("SnakePatchSource requires CV_32FC1 normalized SAR image.");
            }
            if (image_norm_.cols >= patch_size_ && image_norm_.rows >= patch_size_)
            {
                cols_ = (image_norm_.cols - patch_size_) / stride_ + 1;
                rows_ = (image_norm_.rows - patch_size_) / stride_ + 1;
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
            packet.patch_norm = image_norm_(cv::Rect(x, y, patch_size_, patch_size_)).clone();
            ++cursor_;
            return true;
        }

        int totalPatches() const { return total_; }
        int rows() const { return rows_; }
        int cols() const { return cols_; }

    private:
        cv::Mat image_norm_;
        int patch_size_ = 512;
        int stride_ = 256;
        int rows_ = 0;
        int cols_ = 0;
        int total_ = 0;
        int cursor_ = 0;
    };

    class ManualFlightPatchSource
    {
    public:
        ManualFlightPatchSource(cv::Mat image_norm, int patch_size)
            : image_norm_(std::move(image_norm)), patch_size_(patch_size)
        {
            if (image_norm_.cols < patch_size_ || image_norm_.rows < patch_size_)
            {
                throw std::runtime_error("ManualFlightPatchSource image is smaller than patch size.");
            }
            cx_ = image_norm_.cols / 2;
            cy_ = image_norm_.rows / 2;
            clampCenter();
        }

        void moveBy(int dx, int dy)
        {
            cx_ += dx;
            cy_ += dy;
            clampCenter();
        }

        PatchPacket current() const
        {
            const int half = patch_size_ / 2;
            PatchPacket packet;
            packet.info.index = 0;
            packet.info.x = cx_ - half;
            packet.info.y = cy_ - half;
            packet.info.width = patch_size_;
            packet.info.height = patch_size_;
            packet.patch_norm = image_norm_(cv::Rect(packet.info.x, packet.info.y, patch_size_, patch_size_)).clone();
            return packet;
        }

    private:
        void clampCenter()
        {
            const int half = patch_size_ / 2;
            cx_ = std::max(half, std::min(cx_, image_norm_.cols - half));
            cy_ = std::max(half, std::min(cy_, image_norm_.rows - half));
        }

        cv::Mat image_norm_;
        int patch_size_ = 512;
        int cx_ = 0;
        int cy_ = 0;
    };

    Network loadNetwork(const std::string &json_path, const std::string &raw_path)
    {
        Network network = Network::CreateFromJsonFile(json_path);
        network.lazyLoadParamsFromFile(raw_path);
        return network;
    }

    Session initSession(const AppConfig &cfg, const NetworkView &network_view, Device &device)
    {
        if (cfg.run_backend == "host")
        {
            return Session::Create<HostBackend>(network_view, {device});
        }
        if (cfg.run_backend != "zg330")
        {
            throw std::runtime_error("Only run_backend=zg330 or host is supported in infer_workflow.");
        }

        auto session = Session::Create<icraft::xrt::zg330::ZG330Backend, HostBackend>(
            network_view, {device, HostDevice::Default()});
        auto zg_backend = session->backends[0].cast<icraft::xrt::zg330::ZG330Backend>();

        if (!cfg.compress_ftmp)
        {
            zg_backend.disableEtmOptimize();
        }
        if (!cfg.speed_mode)
        {
            zg_backend.disableMergeHardOp();
        }

        if (cfg.ocm_option == 0)
        {
            zg_backend.ocmOptimize(icraft::xrt::zg330::OcmOpt::None);
        }
        else if (cfg.ocm_option == 1)
        {
            zg_backend.ocmOptimize(icraft::xrt::zg330::OcmOpt::OPTION1);
        }
        else if (cfg.ocm_option == 2)
        {
            zg_backend.ocmOptimize(icraft::xrt::zg330::OcmOpt::OPTION2);
        }
        else if (cfg.ocm_option == 3)
        {
            zg_backend.ocmOptimize(icraft::xrt::zg330::OcmOpt::OPTION3);
        }
        else if (cfg.ocm_option != 4)
        {
            throw std::runtime_error("Unsupported sys.ocm_option. Expected 0, 1, 2, 3, or 4.");
        }
        return session;
    }

    Tensor dataToFp32Tensor(const float *input_data, const Value &input_value)
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

    class PatchTensorBuilder
    {
    public:
        explicit PatchTensorBuilder(const NetworkView &network_view)
            : input_value_(network_view.inputs()[0])
        {
            const auto shape = input_value_.tensorType()->shape;
            if (shape.size() != 4 || shape[0] != EXPECTED_N || shape[1] != EXPECTED_H ||
                shape[2] != EXPECTED_W || shape[3] != EXPECTED_C)
            {
                throw std::runtime_error("Model input must be NHWC [1,512,512,1], actual=" + shapeToString(shape));
            }

            const auto storage_type = input_value_.tensorType()->element_dtype.getStorageType();
            if (!storage_type.isFP32())
            {
                throw std::runtime_error("Model input storage dtype must be FP32 for normalized 0-1 patch input.");
            }
        }

        Tensor build(const cv::Mat &patch_norm) const
        {
            if (patch_norm.empty() || patch_norm.type() != CV_32FC1 || patch_norm.rows != EXPECTED_H || patch_norm.cols != EXPECTED_W)
            {
                throw std::runtime_error("Patch tensor input must be CV_32FC1 512x512 normalized to 0-1.");
            }
            cv::Mat continuous = patch_norm.isContinuous() ? patch_norm : patch_norm.clone();
            return dataToFp32Tensor(reinterpret_cast<const float *>(continuous.data), input_value_);
        }

    private:
        Value input_value_;
    };

    void validateNetworkIO(const NetworkView &network_view)
    {
        if (network_view.inputs().size() != 1)
        {
            throw std::runtime_error("infer_workflow supports exactly one model input.");
        }
        if (network_view.outputs().size() != 2)
        {
            throw std::runtime_error("infer_workflow expects exactly two model outputs: restore and seg logits.");
        }

        const auto restore_shape = network_view.outputs()[0].tensorType()->shape;
        const auto seg_shape = network_view.outputs()[1].tensorType()->shape;
        if (restore_shape.size() != 4 || restore_shape[0] != 1 || restore_shape[1] != 512 ||
            restore_shape[2] != 512 || restore_shape[3] != 1)
        {
            throw std::runtime_error("output[0] must be [1,512,512,1], actual=" + shapeToString(restore_shape));
        }
        if (seg_shape.size() != 4 || seg_shape[0] != 1 || seg_shape[1] != 512 ||
            seg_shape[2] != 512 || seg_shape[3] != SEG_CLASSES)
        {
            throw std::runtime_error("output[1] must be [1,512,512,6], actual=" + shapeToString(seg_shape));
        }
        if (!network_view.outputs()[0].tensorType()->element_dtype.isFP32() ||
            !network_view.outputs()[1].tensorType()->element_dtype.isFP32())
        {
            throw std::runtime_error("Stage 0 postprocess expects FP32 model outputs.");
        }
    }

    class PatchInferenceRunner
    {
    public:
        PatchInferenceRunner(Session &session, Device &device, int output_wait_ms)
            : session_(session), device_(device), output_wait_ms_(output_wait_ms)
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

    cv::Mat restoreToGrayU8(const Tensor &tensor)
    {
        const auto *data = reinterpret_cast<const float *>(tensor.data().cptr());
        cv::Mat gray(EXPECTED_H, EXPECTED_W, CV_8UC1);
        for (int y = 0; y < EXPECTED_H; ++y)
        {
            auto *row = gray.ptr<std::uint8_t>(y);
            for (int x = 0; x < EXPECTED_W; ++x)
            {
                const float v = std::max(0.0f, std::min(1.0f, data[y * EXPECTED_W + x]));
                row[x] = static_cast<std::uint8_t>(std::round(v * 255.0f));
            }
        }
        return gray;
    }

    cv::Vec3b classColorBgr(int cls)
    {
        static const cv::Vec3b colors[SEG_CLASSES] = {
            cv::Vec3b(255, 0, 0),
            cv::Vec3b(0, 255, 0),
            cv::Vec3b(0, 0, 255),
            cv::Vec3b(255, 255, 0),
            cv::Vec3b(0, 255, 255),
            cv::Vec3b(255, 0, 255)};
        return colors[std::max(0, std::min(cls, SEG_CLASSES - 1))];
    }

    cv::Mat logitsToMaskBgr(const Tensor &tensor)
    {
        const auto *data = reinterpret_cast<const float *>(tensor.data().cptr());
        cv::Mat mask_bgr(EXPECTED_H, EXPECTED_W, CV_8UC3);
        for (int y = 0; y < EXPECTED_H; ++y)
        {
            auto *row = mask_bgr.ptr<cv::Vec3b>(y);
            for (int x = 0; x < EXPECTED_W; ++x)
            {
                const int base = (y * EXPECTED_W + x) * SEG_CLASSES;
                int best_cls = 0;
                float best_score = data[base];
                for (int cls = 1; cls < SEG_CLASSES; ++cls)
                {
                    const float score = data[base + cls];
                    if (score > best_score)
                    {
                        best_score = score;
                        best_cls = cls;
                    }
                }
                row[x] = classColorBgr(best_cls);
            }
        }
        return mask_bgr;
    }

    cv::Mat composeSideBySide(const cv::Mat &left_bgr, const cv::Mat &right_bgr, int width, int height)
    {
        cv::Mat canvas(height, width, CV_8UC3, cv::Scalar(0, 0, 0));
        const int left_width = width / 2;
        const cv::Rect left_slot(0, 0, left_width, height);
        const cv::Rect right_slot(left_width, 0, width - left_width, height);

        const auto blitCentered = [](const cv::Mat &src, cv::Mat &dst, const cv::Rect &slot) {
            if (src.empty())
            {
                return;
            }
            const double scale = std::min(static_cast<double>(slot.width) / src.cols,
                                          static_cast<double>(slot.height) / src.rows);
            const int resized_w = std::max(1, static_cast<int>(std::round(src.cols * scale)));
            const int resized_h = std::max(1, static_cast<int>(std::round(src.rows * scale)));
            cv::Mat resized;
            cv::resize(src, resized, cv::Size(resized_w, resized_h), 0.0, 0.0, cv::INTER_NEAREST);
            const int x = slot.x + (slot.width - resized_w) / 2;
            const int y = slot.y + (slot.height - resized_h) / 2;
            resized.copyTo(dst(cv::Rect(x, y, resized_w, resized_h)));
        };

        blitCentered(left_bgr, canvas, left_slot);
        blitCentered(right_bgr, canvas, right_slot);
        return canvas;
    }

    class IFrameSink
    {
    public:
        virtual ~IFrameSink() = default;
        virtual void write(const RuntimeState &state, const cv::Mat &frame_bgr) = 0;
    };

    class PngFrameSink : public IFrameSink
    {
    public:
        PngFrameSink(fs::path output_dir, bool overwrite)
            : output_dir_(std::move(output_dir)), overwrite_(overwrite)
        {
            fs::create_directories(output_dir_);
        }

        void write(const RuntimeState &state, const cv::Mat &frame_bgr) override
        {
            const auto echo_dir = output_dir_ / state.echo_stem;
            fs::create_directories(echo_dir);
            char name[64] = {0};
            std::snprintf(name, sizeof(name), "patch_%06d.png", state.patch.index);
            const auto path = echo_dir / name;
            if (fs::exists(path) && !overwrite_)
            {
                return;
            }
            if (!cv::imwrite(path.string(), frame_bgr))
            {
                throw std::runtime_error("Failed to write PNG output: " + path.string());
            }
        }

    private:
        fs::path output_dir_;
        bool overwrite_ = true;
    };

    class HdmiFrameSink : public IFrameSink
    {
    public:
        HdmiFrameSink(FPAIDevice &device, int width, int height, int fps)
            : display_(0, device, width, height), fps_(fps)
        {
            if (fps_ > 0)
            {
                frame_interval_ = std::chrono::microseconds(1000000 / fps_);
            }
        }

        void write(const RuntimeState &, const cv::Mat &frame_bgr) override
        {
            const auto start = std::chrono::steady_clock::now();
            cv::Mat rgb565;
            cv::cvtColor(frame_bgr, rgb565, cv::COLOR_BGR2BGR565);
            display_.show(reinterpret_cast<int8_t *>(rgb565.data));
            if (frame_interval_.count() > 0)
            {
                const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - start);
                if (elapsed < frame_interval_)
                {
                    std::this_thread::sleep_for(frame_interval_ - elapsed);
                }
            }
        }

    private:
        infer_workflow::RGB565HDMIDisplay<FPAIDevice> display_;
        int fps_ = 0;
        std::chrono::microseconds frame_interval_{0};
    };

    void emitBackendLogIfRequested(const AppConfig &cfg, Session &session)
    {
        if (!cfg.dump_backend_log || cfg.run_backend != "zg330")
        {
            return;
        }
        if (session->backends.empty())
        {
            throw std::runtime_error("Cannot dump backend log because session has no backend.");
        }
        setStage("dump ZG330 backend log");
        auto zg_backend = session->backends[0].cast<icraft::xrt::zg330::ZG330Backend>();
        zg_backend.log();
        spdlog::info("ZG330 backend log requested. Check .icraft/logs for generate_memopt.log.");
    }

    void processPatch(const PatchPacket &packet,
                      const RuntimeState &base_state,
                      PatchTensorBuilder &tensor_builder,
                      PatchInferenceRunner &runner,
                      IFrameSink &sink,
                      const AppConfig &cfg)
    {
        RuntimeState state = base_state;
        state.patch = packet.info;
        const auto patch_start = std::chrono::steady_clock::now();

        Tensor input_tensor = tensor_builder.build(packet.patch_norm);
        const auto infer_start = std::chrono::steady_clock::now();
        auto host_outputs = runner.forward(input_tensor);
        const auto infer_end = std::chrono::steady_clock::now();

        cv::Mat restore_gray = restoreToGrayU8(host_outputs[0]);
        cv::Mat restore_bgr;
        cv::cvtColor(restore_gray, restore_bgr, cv::COLOR_GRAY2BGR);
        cv::Mat mask_bgr = logitsToMaskBgr(host_outputs[1]);
        cv::Mat frame_bgr = composeSideBySide(restore_bgr, mask_bgr, cfg.display_width, cfg.display_height);

        state.infer_ms = std::chrono::duration<double, std::milli>(infer_end - infer_start).count();
        state.total_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - patch_start).count();
        sink.write(state, frame_bgr);

        spdlog::info("[{}/{}] {} patch #{} row={} col={} x={} y={} infer={:.2f}ms total={:.2f}ms",
                     state.echo_index,
                     state.echo_count,
                     state.echo_stem,
                     state.patch.index,
                     state.patch.grid_row,
                     state.patch.grid_col,
                     state.patch.x,
                     state.patch.y,
                     state.infer_ms,
                     state.total_ms);
    }
    }

int Run(const std::filesystem::path &config_path)
{
    Device device;
    bool device_open = false;
    try
    {
        std::signal(SIGSEGV, handleSegfault);
        spdlog::set_level(spdlog::level::info);

        setStage("load config");
        const AppConfig cfg = LoadConfig(config_path);
        spdlog::info("Config loaded: output.mode={}, sar_img_dir={}, model_json={}",
                     cfg.output_mode,
                     cfg.sar_img_dir.string(),
                     cfg.json_path);

        setStage("collect SAR images");
        const auto sar_files = collectSarImages(cfg);
        if (sar_files.empty())
        {
            throw std::runtime_error("No SAR image files found in: " + cfg.sar_img_dir.string());
        }
        spdlog::info("Found {} SAR image file(s).", sar_files.size());

        setStage("open device");
        device = Device::Open(cfg.device_url.c_str());
        device_open = true;

        setStage("load network json/raw");
        auto network = loadNetwork(cfg.json_path, cfg.raw_path);

        setStage("create network view");
        auto network_view = network.view(0);

        setStage("validate network IO");
        validateNetworkIO(network_view);
        PatchTensorBuilder tensor_builder(network_view);

        setStage("init session");
        auto session = initSession(cfg, network_view, device);
        session.enableTimeProfile(cfg.enable_profile);

        setStage("session apply");
        session.apply();
        spdlog::info("Session apply finished.");

        emitBackendLogIfRequested(cfg, session);

        setStage("create output sink");
        std::unique_ptr<IFrameSink> sink;
        std::optional<FPAIDevice> fpai_device;
        if (cfg.output_mode == "hdmi")
        {
            fpai_device.emplace(device.cast<FPAIDevice>());
            sink = std::make_unique<HdmiFrameSink>(*fpai_device, cfg.display_width, cfg.display_height, cfg.display_fps);
        }
        else
        {
            sink = std::make_unique<PngFrameSink>(cfg.output_dir, cfg.overwrite);
        }

        setStage("create inference runner");
        PatchInferenceRunner runner(session, device, cfg.output_wait_ms);

        for (size_t sar_idx = 0; sar_idx < sar_files.size(); ++sar_idx)
        {
            const auto &sar_path = sar_files[sar_idx];
            RuntimeState base_state;
            base_state.echo_stem = sar_path.stem().string();
            base_state.echo_index = static_cast<int>(sar_idx + 1);
            base_state.echo_count = static_cast<int>(sar_files.size());

            spdlog::info("Processing SAR image [{}/{}]: {}", base_state.echo_index, base_state.echo_count, sar_path.string());
            const cv::Mat sar_norm = loadSarImageNorm(sar_path);
            spdlog::info("Loaded SAR image {}, size={}x{}, dtype=CV_32FC1, range=0-1", base_state.echo_stem, sar_norm.cols, sar_norm.rows);

            if (sar_norm.cols < cfg.patch_size || sar_norm.rows < cfg.patch_size)
            {
                spdlog::warn("Skip {} because SAR image is smaller than 512x512.", base_state.echo_stem);
                continue;
            }
            if (cfg.patch_mode != "auto_snake")
            {
                throw std::runtime_error("Only pipeline.patch.mode=auto_snake is implemented in stage 0.");
            }

            SnakePatchSource patch_source(sar_norm, cfg.patch_size, cfg.stride);
            spdlog::info("Patch grid for {}: rows={}, cols={}, total={}",
                         base_state.echo_stem,
                         patch_source.rows(),
                         patch_source.cols(),
                         patch_source.totalPatches());

            PatchPacket packet;
            while (patch_source.next(packet))
            {
                processPatch(packet, base_state, tensor_builder, runner, *sink, cfg);
            }
        }

        if (device_open)
        {
            Device::Close(device);
            device_open = false;
        }
        return 0;
    }
    catch (const std::exception &e)
    {
        spdlog::error("infer_workflow failed: {}", e.what());
        try
        {
            if (device_open)
            {
                Device::Close(device);
            }
        }
        catch (...)
        {
        }
        return 2;
    }
}
}
