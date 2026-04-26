#include "infer_workflow_hdmi_display.hpp"
#include "workflow/infer/infer_config.hpp"
#include "workflow/infer/infer_workflow.hpp"
#include "workflow/infer/infer_workflow_internal.hpp"
#include "workflow/infer/manual_flight_runtime.hpp"
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
#include <array>
#include <chrono>
#include <csignal>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
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

        struct PatchPacket
        {
            PatchInfo info;
            cv::Mat patch_norm;
        };

        struct InferenceSnapshot
        {
            RuntimeState state;
            UiRenderContext ui_context;
            cv::Mat restore_bgr;
            cv::Mat mask_bgr;
        };

        struct ManualFlightCoordinatorState
        {
            std::mutex mutex;
            ManualFlightSettings settings;
            bool configured = false;
            std::shared_ptr<ManualFlightRuntimeState> runtime;
        };

        ManualFlightCoordinatorState &manualFlightCoordinatorState()
        {
            static ManualFlightCoordinatorState state;
            return state;
        }

        void syncManualFlightRuntimeConfiguration(const std::shared_ptr<ManualFlightRuntimeState> &runtime)
        {
            if (runtime == nullptr)
            {
                return;
            }

            ManualFlightSettings settings;
            bool configured = false;
            {
                auto &coordinator = manualFlightCoordinatorState();
                std::lock_guard<std::mutex> lock(coordinator.mutex);
                settings = coordinator.settings;
                configured = coordinator.configured;
            }

            runtime->setConfiguration(settings, configured);
        }

        void registerManualFlightRuntimeState(const std::shared_ptr<ManualFlightRuntimeState> &runtime)
        {
            auto &coordinator = manualFlightCoordinatorState();
            std::lock_guard<std::mutex> lock(coordinator.mutex);
            coordinator.runtime = runtime;
        }

        void unregisterManualFlightRuntimeState(const std::shared_ptr<ManualFlightRuntimeState> &runtime)
        {
            auto &coordinator = manualFlightCoordinatorState();
            std::lock_guard<std::mutex> lock(coordinator.mutex);
            if (coordinator.runtime == runtime)
            {
                coordinator.runtime.reset();
            }
        }

        shared::SelectedPatchMode parsePatchMode(const std::string &mode)
        {
            return shared::ToLower(mode) == "manual_flight"
                       ? shared::SelectedPatchMode::ManualFlight
                       : shared::SelectedPatchMode::AutoSnake;
        }

        shared::WorkflowSelection makeSelection(const AppConfig &cfg)
        {
            shared::WorkflowSelection selection;
            selection.workflow = shared::SelectedWorkflow::InferOnly;
            selection.patch_mode = parsePatchMode(cfg.patch_mode);
            selection.output_mode = cfg.output_mode;
            selection.selected_source = cfg.sar_img_dir.string();
            return selection;
        }

        void publishSnapshot(shared::WorkflowRunControl *control,
                             const AppConfig &cfg,
                             const RuntimeState &state,
                             const std::string &stage,
                             shared::ControlState control_state,
                             const std::string &current_item = {},
                             const std::string &last_error = {})
        {
            if (control == nullptr)
            {
                return;
            }

            shared::WorkflowRuntimeSnapshot snapshot;
            snapshot.state = control_state;
            snapshot.selection = makeSelection(cfg);
            snapshot.current_stage = stage;
            snapshot.current_item = current_item.empty() ? state.sar_stem : current_item;
            snapshot.last_error = last_error;
            snapshot.current_index = state.patch.index >= 0 ? (state.patch.index + 1) : state.sar_index;
            snapshot.total_count = state.patch_count > 0 ? state.patch_count : state.sar_count;
            snapshot.infer_ms = state.infer_ms;
            snapshot.total_ms = state.total_ms;
            snapshot.fps = state.fps;
            control->publish(snapshot);
        }
    }

    std::shared_ptr<ManualFlightRuntimeState> activeManualFlightRuntimeState()
    {
        auto &coordinator = manualFlightCoordinatorState();
        std::lock_guard<std::mutex> lock(coordinator.mutex);
        return coordinator.runtime;
    }

    void ConfigureManualFlight(const ManualFlightSettings &settings)
    {
        std::shared_ptr<ManualFlightRuntimeState> runtime;
        {
            auto &coordinator = manualFlightCoordinatorState();
            std::lock_guard<std::mutex> lock(coordinator.mutex);
            coordinator.settings = settings;
            coordinator.configured = true;
            runtime = coordinator.runtime;
        }
        if (runtime != nullptr)
        {
            runtime->configure(settings);
        }
    }

    void ResetManualFlight()
    {
        const auto runtime = activeManualFlightRuntimeState();
        if (runtime == nullptr)
        {
            return;
        }
        runtime->reset();
    }

    void SetManualFlightPaused(bool paused)
    {
        const auto runtime = activeManualFlightRuntimeState();
        if (runtime == nullptr)
        {
            return;
        }
        runtime->setPaused(paused);
    }

    bool SubmitManualFlightKey(const std::string &key, bool pressed, std::string *message)
    {
        const auto runtime = activeManualFlightRuntimeState();
        if (runtime == nullptr)
        {
            if (message != nullptr)
            {
                *message = "manual_flight is not active.";
            }
            return false;
        }
        return runtime->submitDirectionKey(key, pressed, message);
    }

    ManualFlightTelemetry GetManualFlightTelemetry()
    {
        const auto runtime = activeManualFlightRuntimeState();
        if (runtime == nullptr)
        {
            auto &coordinator = manualFlightCoordinatorState();
            std::lock_guard<std::mutex> lock(coordinator.mutex);
            ManualFlightRuntimeState idle_runtime;
            idle_runtime.setConfiguration(coordinator.settings, coordinator.configured);
            return idle_runtime.telemetry();
        }
        return runtime->telemetry();
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
        if (!fs::exists(cfg.sar_img_dir))
        {
            throw std::runtime_error("Input SAR image path does not exist: " + cfg.sar_img_dir.string());
        }

        if (fs::is_regular_file(cfg.sar_img_dir))
        {
            const auto wanted_ext = shared::ToLower(cfg.sar_img_ext);
            if (shared::ToLower(cfg.sar_img_dir.extension().string()) != wanted_ext)
            {
                throw std::runtime_error("Selected SAR image does not match configured extension: " + cfg.sar_img_dir.string());
            }
            return {cfg.sar_img_dir};
        }

        if (!fs::is_directory(cfg.sar_img_dir))
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

    class ManualFlightRuntime
    {
    public:
        ManualFlightRuntime(cv::Mat image_norm, int patch_size, int stride)
            : image_norm_(std::move(image_norm)),
              patch_size_(patch_size),
              stride_(std::max(1, stride)),
              state_(std::make_shared<ManualFlightRuntimeState>())
        {
            if (image_norm_.cols < patch_size_ || image_norm_.rows < patch_size_)
            {
                throw std::runtime_error("ManualFlightRuntime image is smaller than patch size.");
            }
            syncManualFlightRuntimeConfiguration(state_);
            state_->activate(image_norm_.cols, image_norm_.rows, patch_size_, stride_);
            registerManualFlightRuntimeState(state_);
            syncManualFlightRuntimeConfiguration(state_);
        }

        ~ManualFlightRuntime()
        {
            requestStop();
            state_->reset();
            unregisterManualFlightRuntimeState(state_);
        }

        bool waitNextPatch(PatchPacket &packet, shared::WorkflowRunControl *control, int patch_index)
        {
            cv::Point center;
            if (!state_->waitNextCenter([&] {
                    return stop_requested_ || (control != nullptr && control->shouldStop());
                },
                center))
            {
                return false;
            }
            packet = makePacket(center, patch_index);
            return true;
        }

        void markInferenceCommitted(const PatchPacket &packet)
        {
            const cv::Point center(packet.info.x + packet.info.width / 2, packet.info.y + packet.info.height / 2);
            state_->markInferenceCommitted(center);
        }

        void requestStop()
        {
            stop_requested_ = true;
            if (state_ != nullptr)
            {
                state_->requestStop();
            }
        }

    private:
        PatchPacket makePacket(const cv::Point &center, int patch_index) const
        {
            const int half = patch_size_ / 2;
            PatchPacket packet;
            packet.info.index = patch_index;
            packet.info.grid_row = -1;
            packet.info.grid_col = -1;
            packet.info.x = center.x - half;
            packet.info.y = center.y - half;
            packet.info.width = patch_size_;
            packet.info.height = patch_size_;
            packet.info.right_to_left = false;
            packet.patch_norm = image_norm_(cv::Rect(packet.info.x, packet.info.y, patch_size_, patch_size_)).clone();
            return packet;
        }

        cv::Mat image_norm_;
        int patch_size_ = 512;
        int stride_ = 256;
        bool stop_requested_ = false;
        std::shared_ptr<ManualFlightRuntimeState> state_;
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

    std::string buildOutputLabel(const AppConfig &cfg)
    {
        if (cfg.output_mode == "hdmi")
        {
            return "HDMI / " + std::to_string(cfg.display_width) + "x" + std::to_string(cfg.display_height) +
                   "@" + std::to_string(cfg.display_fps);
        }
        return "PNG / " + cfg.output_dir.string();
    }

    class LatestSnapshotMailbox
    {
    public:
        // Phase 2 intentionally uses latest-state handoff rather than FIFO buffering:
        // the producer always overwrites the previous snapshot and the consumer always
        // renders the newest complete snapshot available at display time.
        enum class WakeReason
        {
            Timeout,
            NewSnapshot,
            InputClosed,
            StopRequested,
        };

        void publish(InferenceSnapshot &&snapshot)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            latest_snapshot_ = std::move(snapshot);
            ++published_sequence_;
            cv_.notify_all();
        }

        bool loadLatest(InferenceSnapshot &snapshot, std::uint64_t &sequence)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!latest_snapshot_.has_value())
            {
                return false;
            }
            snapshot = *latest_snapshot_;
            sequence = published_sequence_;
            return true;
        }

        WakeReason waitForChangeOrStop(std::uint64_t known_sequence, std::chrono::microseconds timeout)
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!cv_.wait_for(lock, timeout, [&] {
                    return stop_requested_ || input_closed_ || published_sequence_ != known_sequence;
                }))
            {
                return WakeReason::Timeout;
            }
            if (stop_requested_)
            {
                return WakeReason::StopRequested;
            }
            if (published_sequence_ != known_sequence)
            {
                return WakeReason::NewSnapshot;
            }
            return WakeReason::InputClosed;
        }

        void markInputClosed()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            input_closed_ = true;
            cv_.notify_all();
        }

        void requestStop()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_requested_ = true;
            cv_.notify_all();
        }

    private:
        std::mutex mutex_;
        std::condition_variable cv_;
        std::optional<InferenceSnapshot> latest_snapshot_;
        std::uint64_t published_sequence_ = 0;
        bool input_closed_ = false;
        bool stop_requested_ = false;
    };

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
            const auto sar_dir = output_dir_ / state.sar_stem;
            fs::create_directories(sar_dir);
            char name[64] = {0};
            std::snprintf(name, sizeof(name), "patch_%06d.png", state.patch.index);
            const auto path = sar_dir / name;
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
        }

        void write(const RuntimeState &, const cv::Mat &frame_bgr) override
        {
            cv::Mat rgb565;
            cv::cvtColor(frame_bgr, rgb565, cv::COLOR_BGR2BGR565);
            display_.show(reinterpret_cast<int8_t *>(rgb565.data));
        }

    private:
        infer_workflow::RGB565HDMIDisplay<FPAIDevice> display_;
        int fps_ = 0;
    };

    class HdmiRenderWorker
    {
    public:
        HdmiRenderWorker(IFrameSink &sink,
                         LatestSnapshotMailbox &mailbox,
                         const UiRenderContext &placeholder_ui_context,
                         int display_width,
                         int display_height,
                         int display_fps,
                         std::mutex &device_access_mutex)
            : sink_(sink),
              mailbox_(mailbox),
              placeholder_ui_context_(placeholder_ui_context),
              display_width_(display_width),
              display_height_(display_height),
              display_interval_(display_fps > 0 ? std::chrono::microseconds(1000000 / display_fps)
                                                : std::chrono::microseconds(33333)),
              device_access_mutex_(device_access_mutex)
        {
        }

        void start()
        {
            worker_ = std::thread(&HdmiRenderWorker::run, this);
        }

        void requestStop()
        {
            mailbox_.requestStop();
        }

        void join()
        {
            if (worker_.joinable())
            {
                worker_.join();
            }
            rethrowIfFailed();
        }

        void rethrowIfFailed()
        {
            std::lock_guard<std::mutex> lock(error_mutex_);
            if (worker_error_ != nullptr)
            {
                std::rethrow_exception(worker_error_);
            }
        }

    private:
        void run()
        {
            try
            {
                RuntimeState placeholder_state;
                placeholder_state.sar_stem = "WAITING";
                placeholder_state.patch.width = EXPECTED_W;
                placeholder_state.patch.height = EXPECTED_H;
                placeholder_state.stride = EXPECTED_W / 2;

                UiRenderContext waiting_ui = placeholder_ui_context_;
                waiting_ui.status_label = "WAITING";

                cv::Mat empty_restore(EXPECTED_H, EXPECTED_W, CV_8UC3, cv::Scalar(0, 0, 0));
                cv::Mat empty_mask(EXPECTED_H, EXPECTED_W, CV_8UC3, cv::Scalar(0, 0, 0));

                cv::Mat current_frame = composeIndustrialUiFrame(waiting_ui,
                                                                 placeholder_state,
                                                                 empty_restore,
                                                                 empty_mask,
                                                                 display_width_,
                                                                 display_height_);
                RuntimeState current_state = placeholder_state;
                InferenceSnapshot current_snapshot;
                std::uint64_t current_sequence = 0;
                auto next_present_time = std::chrono::steady_clock::now();
                auto emitStoppedFrame = [&]() {
                    UiRenderContext stopped_ui = current_snapshot.ui_context;
                    RuntimeState stopped_state = current_state;
                    cv::Mat stopped_restore = current_snapshot.restore_bgr;
                    cv::Mat stopped_mask = current_snapshot.mask_bgr;

                    if (stopped_ui.output_label.empty())
                    {
                        stopped_ui = placeholder_ui_context_;
                    }
                    if (stopped_restore.empty())
                    {
                        stopped_restore = empty_restore;
                    }
                    if (stopped_mask.empty())
                    {
                        stopped_mask = empty_mask;
                    }

                    stopped_ui.status_label = "STOPPED";
                    cv::Mat stopped_frame = composeIndustrialUiFrame(stopped_ui,
                                                                     stopped_state,
                                                                     stopped_restore,
                                                                     stopped_mask,
                                                                     display_width_,
                                                                     display_height_);
                    writeFrame(stopped_state, stopped_frame);
                };

                while (true)
                {
                    const auto now = std::chrono::steady_clock::now();
                    if (now >= next_present_time)
                    {
                        writeFrame(current_state, current_frame);
                        next_present_time += display_interval_;

                        const auto after_present = std::chrono::steady_clock::now();
                        while (next_present_time <= after_present)
                        {
                            next_present_time += display_interval_;
                        }
                    }

                    const auto wait_now = std::chrono::steady_clock::now();
                    const auto wait_timeout = next_present_time > wait_now
                                                  ? std::chrono::duration_cast<std::chrono::microseconds>(next_present_time - wait_now)
                                                  : std::chrono::microseconds(0);
                    const auto wake_reason = mailbox_.waitForChangeOrStop(current_sequence, wait_timeout);
                    if (wake_reason == LatestSnapshotMailbox::WakeReason::StopRequested)
                    {
                        emitStoppedFrame();
                        return;
                    }

                    InferenceSnapshot latest_snapshot;
                    std::uint64_t latest_sequence = current_sequence;
                    if (mailbox_.loadLatest(latest_snapshot, latest_sequence) && latest_sequence != current_sequence)
                    {
                        current_snapshot = std::move(latest_snapshot);
                        current_sequence = latest_sequence;
                        current_frame = composeIndustrialUiFrame(current_snapshot.ui_context,
                                                                 current_snapshot.state,
                                                                 current_snapshot.restore_bgr,
                                                                 current_snapshot.mask_bgr,
                                                                 display_width_,
                                                                 display_height_);
                        current_state = current_snapshot.state;
                        continue;
                    }

                    if (wake_reason == LatestSnapshotMailbox::WakeReason::InputClosed)
                    {
                        emitStoppedFrame();
                        return;
                    }
                }
            }
            catch (...)
            {
                {
                    std::lock_guard<std::mutex> lock(error_mutex_);
                    worker_error_ = std::current_exception();
                }
                mailbox_.requestStop();
            }
        }

        void writeFrame(const RuntimeState &state, const cv::Mat &frame_bgr)
        {
            // Phase 2 keeps HDMI and inference device access serialized on purpose.
            // This means HDMI is not strictly real-time under heavy inference load, but
            // it removes direct producer-side sleep blocking without risking unsafe
            // concurrent access to the underlying device/driver.
            std::lock_guard<std::mutex> device_lock(device_access_mutex_);
            sink_.write(state, frame_bgr);
        }

        IFrameSink &sink_;
        LatestSnapshotMailbox &mailbox_;
        UiRenderContext placeholder_ui_context_;
        int display_width_ = 1920;
        int display_height_ = 1080;
        std::chrono::microseconds display_interval_{0};
        std::mutex &device_access_mutex_;
        std::thread worker_;
        std::mutex error_mutex_;
        std::exception_ptr worker_error_;
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

    RuntimeState processPatchToPng(const PatchPacket &packet,
                                   const RuntimeState &base_state,
                                   const UiRenderContext &ui_context,
                                   PatchTensorBuilder &tensor_builder,
                                   PatchInferenceRunner &runner,
                                   IFrameSink &sink,
                                   const AppConfig &cfg,
                                   int &frame_counter)
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

        state.infer_ms = std::chrono::duration<double, std::milli>(infer_end - infer_start).count();
        state.frame_index = ++frame_counter;
        state.total_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - patch_start).count();
        state.fps = state.total_ms > 0.0 ? (1000.0 / state.total_ms) : 0.0;
        cv::Mat frame_bgr = composeIndustrialUiFrame(ui_context, state, restore_bgr, mask_bgr, cfg.display_width, cfg.display_height);
        state.total_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - patch_start).count();
        state.fps = state.total_ms > 0.0 ? (1000.0 / state.total_ms) : 0.0;
        frame_bgr = composeIndustrialUiFrame(ui_context, state, restore_bgr, mask_bgr, cfg.display_width, cfg.display_height);
        sink.write(state, frame_bgr);

        spdlog::info("[{}/{}] {} patch #{}/{} frame={} row={} col={} x={} y={} infer={:.2f}ms total={:.2f}ms fps={:.1f}",
                     state.sar_index,
                     state.sar_count,
                     state.sar_stem,
                     state.patch.index + 1,
                     state.patch_count,
                     state.frame_index,
                     state.patch.grid_row,
                     state.patch.grid_col,
                     state.patch.x,
                     state.patch.y,
                     state.infer_ms,
                     state.total_ms,
                     state.fps);
        return state;
    }

    RuntimeState processPatchToHdmi(const PatchPacket &packet,
                                    const RuntimeState &base_state,
                                    const UiRenderContext &ui_context,
                                    PatchTensorBuilder &tensor_builder,
                                    PatchInferenceRunner &runner,
                                    std::mutex &device_access_mutex,
                                    LatestSnapshotMailbox &mailbox,
                                    HdmiRenderWorker &render_worker,
                                    int &frame_counter)
    {
        render_worker.rethrowIfFailed();

        RuntimeState state = base_state;
        state.patch = packet.info;
        const auto patch_start = std::chrono::steady_clock::now();

        Tensor input_tensor = tensor_builder.build(packet.patch_norm);
        const auto infer_start = std::chrono::steady_clock::now();
        std::vector<Tensor> host_outputs;
        {
            // Phase 2 deliberately serializes the whole forward/wait/copy/reset window
            // against HDMI writes. The accepted tradeoff is "non-strict-real-time HDMI"
            // rather than unsafely overlapping display_.show(...) with device.reset(1).
            std::lock_guard<std::mutex> device_lock(device_access_mutex);
            host_outputs = runner.forward(input_tensor);
        }
        const auto infer_end = std::chrono::steady_clock::now();

        cv::Mat restore_gray = restoreToGrayU8(host_outputs[0]);
        cv::Mat restore_bgr;
        cv::cvtColor(restore_gray, restore_bgr, cv::COLOR_GRAY2BGR);
        cv::Mat mask_bgr = logitsToMaskBgr(host_outputs[1]);

        state.infer_ms = std::chrono::duration<double, std::milli>(infer_end - infer_start).count();

        // Phase 2 metrics stop at the producer-side snapshot handoff boundary so HDMI
        // display cadence, BGR565 conversion, and register writes do not distort infer fps.
        const int next_frame_index = frame_counter + 1;
        state.frame_index = next_frame_index;
        state.total_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - patch_start).count();
        state.fps = state.total_ms > 0.0 ? (1000.0 / state.total_ms) : 0.0;

        InferenceSnapshot snapshot;
        snapshot.state = state;
        snapshot.ui_context = ui_context;
        snapshot.restore_bgr = std::move(restore_bgr);
        snapshot.mask_bgr = std::move(mask_bgr);
        mailbox.publish(std::move(snapshot));

        frame_counter = next_frame_index;
        render_worker.rethrowIfFailed();

        spdlog::info("[{}/{}] {} patch #{}/{} frame={} row={} col={} x={} y={} infer={:.2f}ms total={:.2f}ms fps={:.1f} [snapshot->hdmi]",
                     state.sar_index,
                     state.sar_count,
                     state.sar_stem,
                     state.patch.index + 1,
                     state.patch_count,
                     state.frame_index,
                     state.patch.grid_row,
                     state.patch.grid_col,
                     state.patch.x,
                     state.patch.y,
                     state.infer_ms,
                     state.total_ms,
                     state.fps);
        return state;
    }
    }

int Run(const std::filesystem::path &config_path)
{
    return Run(LoadConfig(config_path), nullptr);
}

int Run(const AppConfig &cfg, shared::WorkflowRunControl *control)
{
    Device device;
    bool device_open = false;
    std::optional<LatestSnapshotMailbox> hdmi_mailbox;
    std::unique_ptr<HdmiRenderWorker> hdmi_render_worker;
    try
    {
        std::signal(SIGSEGV, handleSegfault);
        spdlog::set_level(spdlog::level::info);

        setStage("load config");
        RuntimeState published_state;
        publishSnapshot(control, cfg, published_state, "load config", shared::ControlState::Starting, cfg.sar_img_dir.string());
        spdlog::info("Config loaded: output.mode={}, sar_img_dir={}, model_json={}",
                     cfg.output_mode,
                     cfg.sar_img_dir.string(),
                     cfg.json_path);

        setStage("collect SAR images");
        publishSnapshot(control, cfg, published_state, "collect SAR images", shared::ControlState::Starting, cfg.sar_img_dir.string());
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
        publishSnapshot(control, cfg, published_state, "load network", shared::ControlState::Starting, cfg.json_path);
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
        std::mutex device_access_mutex;
        if (cfg.output_mode == "hdmi")
        {
            fpai_device.emplace(device.cast<FPAIDevice>());
            sink = std::make_unique<HdmiFrameSink>(*fpai_device, cfg.display_width, cfg.display_height, cfg.display_fps);
            UiRenderContext placeholder_ui_context;
            placeholder_ui_context.output_label = buildOutputLabel(cfg);
            placeholder_ui_context.mini_map.source_width = EXPECTED_W;
            placeholder_ui_context.mini_map.source_height = EXPECTED_H;
            placeholder_ui_context.mini_map.patch_size = EXPECTED_W;
            placeholder_ui_context.mini_map.sar_preview_bgr = cv::Mat(EXPECTED_H, EXPECTED_W, CV_8UC3, cv::Scalar(0, 0, 0));
            hdmi_mailbox.emplace();
            hdmi_render_worker = std::make_unique<HdmiRenderWorker>(*sink,
                                                                    *hdmi_mailbox,
                                                                    placeholder_ui_context,
                                                                    cfg.display_width,
                                                                    cfg.display_height,
                                                                    cfg.display_fps,
                                                                    device_access_mutex);
            hdmi_render_worker->start();
        }
        else
        {
            sink = std::make_unique<PngFrameSink>(cfg.output_dir, cfg.overwrite);
        }

        setStage("create inference runner");
        PatchInferenceRunner runner(session,
                                    device,
                                    cfg.output_wait_ms);
        int frame_counter = 0;
        bool stop_requested = false;
        const shared::SelectedPatchMode patch_mode = parsePatchMode(cfg.patch_mode);
        if (patch_mode == shared::SelectedPatchMode::ManualFlight && sar_files.size() != 1)
        {
            throw std::runtime_error("manual_flight requires exactly one selected SAR image.");
        }

        for (size_t sar_idx = 0; sar_idx < sar_files.size(); ++sar_idx)
        {
            const auto &sar_path = sar_files[sar_idx];
            RuntimeState base_state;
            base_state.sar_stem = sar_path.stem().string();
            base_state.sar_index = static_cast<int>(sar_idx + 1);
            base_state.sar_count = static_cast<int>(sar_files.size());

            spdlog::info("Processing SAR image [{}/{}]: {}", base_state.sar_index, base_state.sar_count, sar_path.string());
            const cv::Mat sar_norm = loadSarImageNorm(sar_path);
            spdlog::info("Loaded SAR image {}, size={}x{}, dtype=CV_32FC1, range=0-1", base_state.sar_stem, sar_norm.cols, sar_norm.rows);

            if (sar_norm.cols < cfg.patch_size || sar_norm.rows < cfg.patch_size)
            {
                spdlog::warn("Skip {} because SAR image is smaller than 512x512.", base_state.sar_stem);
                continue;
            }

            UiRenderContext ui_context;
            ui_context.output_label = buildOutputLabel(cfg);
            ui_context.mini_map = buildMiniMapContext(sar_norm, cfg.patch_size, cfg.stride, 0, 0);
            publishSnapshot(control, cfg, base_state, "sar loaded", shared::ControlState::Running, sar_path.string());

            if (patch_mode == shared::SelectedPatchMode::ManualFlight)
            {
                ManualFlightRuntime manual_runtime(sar_norm, cfg.patch_size, cfg.stride);
                base_state.patch_count = 0;
                base_state.stride = cfg.stride;

                PatchPacket packet;
                int patch_index = 0;
                while (manual_runtime.waitNextPatch(packet, control, patch_index))
                {
                    if (control != nullptr)
                    {
                        control->waitIfPaused();
                        if (control->shouldStop())
                        {
                            publishSnapshot(control, cfg, base_state, "stop requested", shared::ControlState::Stopping, sar_path.string());
                            stop_requested = true;
                            break;
                        }
                    }

                    RuntimeState patch_base_state = base_state;
                    patch_base_state.patch_count = patch_index + 1;
                    UiRenderContext patch_ui_context = ui_context;
                    applyManualTelemetry(patch_base_state, patch_ui_context);

                    RuntimeState patch_state;
                    if (cfg.output_mode == "hdmi")
                    {
                        patch_state = processPatchToHdmi(packet,
                                                         patch_base_state,
                                                         patch_ui_context,
                                                         tensor_builder,
                                                         runner,
                                                         device_access_mutex,
                                                         *hdmi_mailbox,
                                                         *hdmi_render_worker,
                                                         frame_counter);
                    }
                    else
                    {
                        patch_state = processPatchToPng(packet,
                                                        patch_base_state,
                                                        patch_ui_context,
                                                        tensor_builder,
                                                        runner,
                                                        *sink,
                                                        cfg,
                                                        frame_counter);
                    }
                    manual_runtime.markInferenceCommitted(packet);
                    applyManualTelemetry(patch_state, patch_ui_context);
                    publishSnapshot(control, cfg, patch_state, "manual patch processed", shared::ControlState::Running, sar_path.string());
                    base_state = patch_state;
                    ++patch_index;
                }
            }
            else
            {
                SnakePatchSource patch_source(sar_norm, cfg.patch_size, cfg.stride);
                base_state.patch_count = patch_source.totalPatches();
                base_state.stride = cfg.stride;
                spdlog::info("Patch grid for {}: rows={}, cols={}, total={}",
                             base_state.sar_stem,
                             patch_source.rows(),
                             patch_source.cols(),
                             patch_source.totalPatches());
                ui_context.mini_map = buildMiniMapContext(sar_norm, cfg.patch_size, cfg.stride, patch_source.rows(), patch_source.cols());

                PatchPacket packet;
                while (patch_source.next(packet))
                {
                    if (control != nullptr)
                    {
                        control->waitIfPaused();
                        if (control->shouldStop())
                        {
                            publishSnapshot(control, cfg, base_state, "stop requested", shared::ControlState::Stopping, sar_path.string());
                            stop_requested = true;
                            break;
                        }
                    }

                    RuntimeState patch_state;
                    if (cfg.output_mode == "hdmi")
                    {
                        patch_state = processPatchToHdmi(packet,
                                                         base_state,
                                                         ui_context,
                                                         tensor_builder,
                                                         runner,
                                                         device_access_mutex,
                                                         *hdmi_mailbox,
                                                         *hdmi_render_worker,
                                                         frame_counter);
                    }
                    else
                    {
                        patch_state = processPatchToPng(packet,
                                                        base_state,
                                                        ui_context,
                                                        tensor_builder,
                                                        runner,
                                                        *sink,
                                                        cfg,
                                                        frame_counter);
                    }
                    publishSnapshot(control, cfg, patch_state, "patch processed", shared::ControlState::Running, sar_path.string());
                }

                if (stop_requested)
                {
                    break;
                }
            }

            if (stop_requested)
            {
                break;
            }
        }

        if (hdmi_mailbox.has_value())
        {
            hdmi_mailbox->markInputClosed();
        }
        if (hdmi_render_worker != nullptr)
        {
            hdmi_render_worker->join();
        }

        if (device_open)
        {
            Device::Close(device);
            device_open = false;
        }
        publishSnapshot(control,
                        cfg,
                        RuntimeState{},
                        stop_requested ? "stopped" : "finished",
                        stop_requested ? shared::ControlState::Idle : shared::ControlState::Finished,
                        cfg.sar_img_dir.string());
        if (patch_mode == shared::SelectedPatchMode::ManualFlight)
        {
            ResetManualFlight();
        }
        return 0;
    }
    catch (const std::exception &e)
    {
        publishSnapshot(control,
                        cfg,
                        RuntimeState{},
                        "error",
                        shared::ControlState::Error,
                        cfg.sar_img_dir.string(),
                        e.what());
        spdlog::error("infer_workflow failed: {}", e.what());
        try
        {
            if (hdmi_mailbox.has_value())
            {
                hdmi_mailbox->requestStop();
            }
            if (hdmi_render_worker != nullptr)
            {
                hdmi_render_worker->join();
            }
            if (device_open)
            {
                Device::Close(device);
            }
            if (parsePatchMode(cfg.patch_mode) == shared::SelectedPatchMode::ManualFlight)
            {
                ResetManualFlight();
            }
        }
        catch (...)
        {
        }
        return 2;
    }
}
}
