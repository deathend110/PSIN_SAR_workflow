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
            std::string sar_stem;
            int sar_index = 0;
            int sar_count = 0;
            int patch_count = 0;
            int frame_index = 0;
            PatchInfo patch;
            double fps = 0.0;
            double infer_ms = 0.0;
            double total_ms = 0.0;
            int stride = 256;
            bool manual_active = false;
            int manual_pos_x = 0;
            int manual_pos_y = 0;
            int manual_vel_x = 0;
            int manual_vel_y = 0;
            int manual_target_x = 0;
            int manual_target_y = 0;
            int manual_last_inferred_x = 0;
            int manual_last_inferred_y = 0;
            int manual_path_points = 0;
            std::string manual_keys;
        };

        struct MiniMapContext
        {
            cv::Mat sar_preview_bgr;
            int source_width = 0;
            int source_height = 0;
            int patch_size = 0;
            bool path_overlay = false;
            std::vector<cv::Point> path_points;
        };

        struct UiRenderContext
        {
            std::string status_label = "RUNNING";
            std::string mode_label = "INFERENCE ONLY";
            std::string output_label;
            std::string restore_label = "GRAY OUTPUT";
            std::string seg_label = "RGB MASK / 6 CLASS";
            MiniMapContext mini_map;
        };

        struct InferenceSnapshot
        {
            RuntimeState state;
            UiRenderContext ui_context;
            cv::Mat restore_bgr;
            cv::Mat mask_bgr;
        };

        struct ManualFlightSharedState
        {
            std::mutex mutex;
            std::condition_variable cv;
            ManualFlightSettings settings;
            bool configured = false;
            bool active = false;
            bool paused = false;
            int image_width = 0;
            int image_height = 0;
            int patch_size = kExpectedH;
            cv::Point2f position_px{0.0f, 0.0f};
            cv::Point2f velocity_px{0.0f, 0.0f};
            cv::Point requested_center{0, 0};
            cv::Point last_inferred_center{0, 0};
            std::uint64_t request_sequence = 0;
            std::uint64_t consumed_sequence = 0;
            bool key_w = false;
            bool key_a = false;
            bool key_s = false;
            bool key_d = false;
            bool key_shift = false;
            std::vector<cv::Point> path_points;
        };

        ManualFlightSharedState &manualFlightSharedState()
        {
            static ManualFlightSharedState state;
            return state;
        }

        cv::Point clampManualCenter(const cv::Point2f &point, int image_width, int image_height, int patch_size)
        {
            const int half = patch_size / 2;
            const int x = std::max(half, std::min(static_cast<int>(std::lround(point.x)), image_width - half));
            const int y = std::max(half, std::min(static_cast<int>(std::lround(point.y)), image_height - half));
            return cv::Point(x, y);
        }

        double pointDistance(const cv::Point &lhs, const cv::Point &rhs)
        {
            return cv::norm(cv::Point2f(static_cast<float>(lhs.x - rhs.x), static_cast<float>(lhs.y - rhs.y)));
        }

        std::string buildActiveKeysLabel(const ManualFlightSharedState &state)
        {
            std::vector<std::string> keys;
            if (state.key_w)
            {
                keys.push_back("W");
            }
            if (state.key_a)
            {
                keys.push_back("A");
            }
            if (state.key_s)
            {
                keys.push_back("S");
            }
            if (state.key_d)
            {
                keys.push_back("D");
            }
            if (state.key_shift)
            {
                keys.push_back("SHIFT");
            }
            if (keys.empty())
            {
                return "-";
            }

            std::ostringstream oss;
            for (size_t i = 0; i < keys.size(); ++i)
            {
                if (i > 0)
                {
                    oss << "+";
                }
                oss << keys[i];
            }
            return oss.str();
        }

        void trimManualPath(std::vector<cv::Point> &path_points)
        {
            constexpr size_t kMaxPathPoints = 256;
            if (path_points.size() > kMaxPathPoints)
            {
                path_points.erase(path_points.begin(), path_points.end() - static_cast<std::ptrdiff_t>(kMaxPathPoints));
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

    void ConfigureManualFlight(const ManualFlightSettings &settings)
    {
        auto &manual_shared = manualFlightSharedState();
        {
            std::lock_guard<std::mutex> lock(manual_shared.mutex);
            manual_shared.settings = settings;
            manual_shared.configured = true;
        }
        manual_shared.cv.notify_all();
    }

    void ResetManualFlight()
    {
        auto &manual_shared = manualFlightSharedState();
        {
            std::lock_guard<std::mutex> lock(manual_shared.mutex);
            manual_shared.active = false;
            manual_shared.paused = false;
            manual_shared.image_width = 0;
            manual_shared.image_height = 0;
            manual_shared.patch_size = kExpectedH;
            manual_shared.position_px = cv::Point2f(0.0f, 0.0f);
            manual_shared.velocity_px = cv::Point2f(0.0f, 0.0f);
            manual_shared.requested_center = cv::Point(0, 0);
            manual_shared.last_inferred_center = cv::Point(0, 0);
            manual_shared.request_sequence = 0;
            manual_shared.consumed_sequence = 0;
            manual_shared.key_w = false;
            manual_shared.key_a = false;
            manual_shared.key_s = false;
            manual_shared.key_d = false;
            manual_shared.key_shift = false;
            manual_shared.path_points.clear();
        }
        manual_shared.cv.notify_all();
    }

    void SetManualFlightPaused(bool paused)
    {
        auto &manual_shared = manualFlightSharedState();
        {
            std::lock_guard<std::mutex> lock(manual_shared.mutex);
            manual_shared.paused = paused;
            if (paused)
            {
                manual_shared.velocity_px = cv::Point2f(0.0f, 0.0f);
            }
        }
        manual_shared.cv.notify_all();
    }

    bool SubmitManualFlightKey(const std::string &key, bool pressed, std::string *message)
    {
        auto &manual_shared = manualFlightSharedState();
        const std::string lowered = shared::ToLower(shared::Trim(key));
        auto set_key = [&](bool &slot) {
            slot = pressed;
            return true;
        };

        std::lock_guard<std::mutex> lock(manual_shared.mutex);
        if (!manual_shared.active)
        {
            if (message != nullptr)
            {
                *message = "manual_flight is not active.";
            }
            return false;
        }

        bool accepted = false;
        if (lowered == "w")
        {
            accepted = set_key(manual_shared.key_w);
        }
        else if (lowered == "a")
        {
            accepted = set_key(manual_shared.key_a);
        }
        else if (lowered == "s")
        {
            accepted = set_key(manual_shared.key_s);
        }
        else if (lowered == "d")
        {
            accepted = set_key(manual_shared.key_d);
        }
        else if (lowered == "shift")
        {
            accepted = set_key(manual_shared.key_shift);
        }

        if (!accepted)
        {
            if (message != nullptr)
            {
                *message = "manual_flight only accepts W/A/S/D and optional Shift.";
            }
            return false;
        }

        if (message != nullptr)
        {
            *message = std::string("manual key ") + lowered + (pressed ? " down accepted." : " up accepted.");
        }
        manual_shared.cv.notify_all();
        return true;
    }

    ManualFlightTelemetry GetManualFlightTelemetry()
    {
        auto &manual_shared = manualFlightSharedState();
        std::lock_guard<std::mutex> lock(manual_shared.mutex);

        ManualFlightTelemetry telemetry;
        telemetry.configured = manual_shared.configured;
        telemetry.active = manual_shared.active;
        telemetry.paused = manual_shared.paused;
        telemetry.path_overlay = manual_shared.settings.path_overlay;
        telemetry.position_x = static_cast<int>(std::lround(manual_shared.position_px.x));
        telemetry.position_y = static_cast<int>(std::lround(manual_shared.position_px.y));
        telemetry.velocity_x = static_cast<int>(std::lround(manual_shared.velocity_px.x));
        telemetry.velocity_y = static_cast<int>(std::lround(manual_shared.velocity_px.y));
        telemetry.requested_center_x = manual_shared.requested_center.x;
        telemetry.requested_center_y = manual_shared.requested_center.y;
        telemetry.last_inferred_center_x = manual_shared.last_inferred_center.x;
        telemetry.last_inferred_center_y = manual_shared.last_inferred_center.y;
        telemetry.path_points = static_cast<int>(manual_shared.path_points.size());
        telemetry.active_keys = buildActiveKeysLabel(manual_shared);
        return telemetry;
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
        ManualFlightRuntime(cv::Mat image_norm, int patch_size)
            : image_norm_(std::move(image_norm)), patch_size_(patch_size)
        {
            if (image_norm_.cols < patch_size_ || image_norm_.rows < patch_size_)
            {
                throw std::runtime_error("ManualFlightRuntime image is smaller than patch size.");
            }
            initializeSharedState();
            simulation_thread_ = std::thread(&ManualFlightRuntime::simulationMain, this);
        }

        ~ManualFlightRuntime()
        {
            requestStop();
            join();
            ResetManualFlight();
        }

        bool waitNextPatch(PatchPacket &packet, shared::WorkflowRunControl *control, int patch_index)
        {
            auto &shared = manualFlightSharedState();
            for (;;)
            {
                std::unique_lock<std::mutex> lock(shared.mutex);
                shared.cv.wait_for(lock, std::chrono::milliseconds(40), [&] {
                    return stop_requested_ ||
                           shared.request_sequence > shared.consumed_sequence ||
                           (control != nullptr && control->shouldStop());
                });

                if (stop_requested_ || (control != nullptr && control->shouldStop()))
                {
                    return false;
                }
                if (shared.request_sequence <= shared.consumed_sequence)
                {
                    continue;
                }

                const cv::Point center = shared.requested_center;
                shared.consumed_sequence = shared.request_sequence;
                lock.unlock();

                packet = makePacket(center, patch_index);
                return true;
            }
        }

        void markInferenceCommitted(const PatchPacket &packet)
        {
            auto &shared = manualFlightSharedState();
            const cv::Point center(packet.info.x + packet.info.width / 2, packet.info.y + packet.info.height / 2);
            {
                std::lock_guard<std::mutex> lock(shared.mutex);
                shared.last_inferred_center = center;
            }
            shared.cv.notify_all();
        }

        void requestStop()
        {
            stop_requested_ = true;
            manualFlightSharedState().cv.notify_all();
        }

        void join()
        {
            if (simulation_thread_.joinable())
            {
                simulation_thread_.join();
            }
        }

    private:
        void initializeSharedState()
        {
            auto &shared = manualFlightSharedState();
            const cv::Point initial_center = clampManualCenter(cv::Point2f(static_cast<float>(image_norm_.cols / 2),
                                                                           static_cast<float>(image_norm_.rows / 2)),
                                                               image_norm_.cols,
                                                               image_norm_.rows,
                                                               patch_size_);
            std::lock_guard<std::mutex> lock(shared.mutex);
            shared.active = true;
            shared.paused = false;
            shared.image_width = image_norm_.cols;
            shared.image_height = image_norm_.rows;
            shared.patch_size = patch_size_;
            shared.position_px = cv::Point2f(static_cast<float>(initial_center.x), static_cast<float>(initial_center.y));
            shared.velocity_px = cv::Point2f(0.0f, 0.0f);
            shared.requested_center = initial_center;
            shared.last_inferred_center = initial_center;
            shared.request_sequence = 1;
            shared.consumed_sequence = 0;
            shared.key_w = false;
            shared.key_a = false;
            shared.key_s = false;
            shared.key_d = false;
            shared.key_shift = false;
            shared.path_points.clear();
            shared.path_points.push_back(initial_center);
        }

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

        void simulationMain()
        {
            auto &shared = manualFlightSharedState();
            auto last_tick = std::chrono::steady_clock::now();

            while (!stop_requested_)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
                const auto now = std::chrono::steady_clock::now();
                const double dt = std::min(0.10, std::chrono::duration<double>(now - last_tick).count());
                last_tick = now;

                std::lock_guard<std::mutex> lock(shared.mutex);
                if (!shared.active || shared.paused)
                {
                    shared.velocity_px = cv::Point2f(0.0f, 0.0f);
                    continue;
                }

                const int horizontal = (shared.key_d ? 1 : 0) - (shared.key_a ? 1 : 0);
                const int vertical = (shared.key_s ? 1 : 0) - (shared.key_w ? 1 : 0);

                cv::Point2f direction(0.0f, 0.0f);
                if (horizontal != 0 || vertical != 0)
                {
                    direction.x = static_cast<float>(horizontal);
                    direction.y = static_cast<float>(vertical);
                    const float magnitude = std::sqrt(direction.x * direction.x + direction.y * direction.y);
                    if (magnitude > 0.0f)
                    {
                        direction.x /= magnitude;
                        direction.y /= magnitude;
                    }
                }

                const double max_speed = static_cast<double>(std::max(1, shared.key_shift ? shared.settings.boost_step_px : shared.settings.manual_step_px));
                const double acceleration = max_speed * 6.0;
                if (direction.x != 0.0f || direction.y != 0.0f)
                {
                    shared.velocity_px.x += static_cast<float>(direction.x * acceleration * dt);
                    shared.velocity_px.y += static_cast<float>(direction.y * acceleration * dt);
                    const double velocity_norm = std::sqrt(shared.velocity_px.x * shared.velocity_px.x +
                                                           shared.velocity_px.y * shared.velocity_px.y);
                    if (velocity_norm > max_speed && velocity_norm > 0.0)
                    {
                        const double scale = max_speed / velocity_norm;
                        shared.velocity_px.x = static_cast<float>(shared.velocity_px.x * scale);
                        shared.velocity_px.y = static_cast<float>(shared.velocity_px.y * scale);
                    }
                }
                else
                {
                    const double damping = std::exp(-5.0 * dt);
                    shared.velocity_px.x = static_cast<float>(shared.velocity_px.x * damping);
                    shared.velocity_px.y = static_cast<float>(shared.velocity_px.y * damping);
                    if (std::abs(shared.velocity_px.x) < 0.5f)
                    {
                        shared.velocity_px.x = 0.0f;
                    }
                    if (std::abs(shared.velocity_px.y) < 0.5f)
                    {
                        shared.velocity_px.y = 0.0f;
                    }
                }

                shared.position_px.x += static_cast<float>(shared.velocity_px.x * dt);
                shared.position_px.y += static_cast<float>(shared.velocity_px.y * dt);

                const cv::Point clamped_center = clampManualCenter(shared.position_px,
                                                                   shared.image_width,
                                                                   shared.image_height,
                                                                   shared.patch_size);
                shared.position_px = cv::Point2f(static_cast<float>(clamped_center.x), static_cast<float>(clamped_center.y));

                const int path_step = std::max(1, shared.settings.cache_grid_px / 2);
                if (shared.path_points.empty() || pointDistance(shared.path_points.back(), clamped_center) >= path_step)
                {
                    shared.path_points.push_back(clamped_center);
                    trimManualPath(shared.path_points);
                }

                const int trigger_distance = std::max(1, shared.settings.trigger_distance_px);
                if (pointDistance(shared.requested_center, clamped_center) >= trigger_distance)
                {
                    shared.requested_center = clamped_center;
                    ++shared.request_sequence;
                    shared.cv.notify_all();
                }
            }
        }

        cv::Mat image_norm_;
        int patch_size_ = 512;
        bool stop_requested_ = false;
        std::thread simulation_thread_;
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

    std::string truncateToWidth(const std::string &text,
                                int max_width,
                                int font_face,
                                double font_scale,
                                int thickness)
    {
        if (text.empty() || max_width <= 0)
        {
            return std::string();
        }
        if (cv::getTextSize(text, font_face, font_scale, thickness, nullptr).width <= max_width)
        {
            return text;
        }

        static const std::string ellipsis = "...";
        if (cv::getTextSize(ellipsis, font_face, font_scale, thickness, nullptr).width > max_width)
        {
            return ellipsis;
        }

        std::string trimmed = text;
        while (!trimmed.empty())
        {
            trimmed.pop_back();
            const std::string candidate = trimmed + ellipsis;
            if (cv::getTextSize(candidate, font_face, font_scale, thickness, nullptr).width <= max_width)
            {
                return candidate;
            }
        }
        return ellipsis;
    }

    cv::Rect insetRect(const cv::Rect &rect, int dx, int dy)
    {
        const int width = std::max(1, rect.width - 2 * dx);
        const int height = std::max(1, rect.height - 2 * dy);
        return cv::Rect(rect.x + dx, rect.y + dy, width, height);
    }

    void drawGridTexture(cv::Mat &canvas, const cv::Rect &rect, int step, const cv::Scalar &color)
    {
        if (step <= 0)
        {
            return;
        }
        for (int x = rect.x; x <= rect.x + rect.width; x += step)
        {
            cv::line(canvas, cv::Point(x, rect.y), cv::Point(x, rect.y + rect.height), color, 1, cv::LINE_AA);
        }
        for (int y = rect.y; y <= rect.y + rect.height; y += step)
        {
            cv::line(canvas, cv::Point(rect.x, y), cv::Point(rect.x + rect.width, y), color, 1, cv::LINE_AA);
        }
    }

    cv::Rect drawPanel(cv::Mat &canvas,
                       const cv::Rect &rect,
                       const std::string &title,
                       const std::string &subtitle,
                       int header_height,
                       const cv::Scalar &panel_color,
                       const cv::Scalar &header_color,
                       const cv::Scalar &border_color,
                       const cv::Scalar &title_color,
                       const cv::Scalar &subtitle_color)
    {
        cv::rectangle(canvas, rect, panel_color, cv::FILLED);
        cv::rectangle(canvas, rect, border_color, 1, cv::LINE_AA);
        const cv::Rect header_rect(rect.x, rect.y, rect.width, std::min(rect.height, header_height));
        cv::rectangle(canvas, header_rect, header_color, cv::FILLED);
        cv::line(canvas,
                 cv::Point(rect.x, header_rect.br().y),
                 cv::Point(rect.x + rect.width, header_rect.br().y),
                 border_color,
                 1,
                 cv::LINE_AA);

        const int pad = std::max(8, header_height / 4);
        const int font_face = cv::FONT_HERSHEY_SIMPLEX;
        const double title_scale = std::max(0.45, header_height / 38.0);
        const double subtitle_scale = std::max(0.35, header_height / 52.0);
        const int title_thickness = std::max(1, header_height / 20);
        const int subtitle_thickness = std::max(1, title_thickness - 1);

        const std::string clipped_title = truncateToWidth(title, rect.width - 2 * pad, font_face, title_scale, title_thickness);
        const std::string clipped_subtitle = truncateToWidth(subtitle, rect.width - 2 * pad, font_face, subtitle_scale, subtitle_thickness);
        cv::putText(canvas,
                    clipped_title,
                    cv::Point(rect.x + pad, rect.y + pad + header_height / 2),
                    font_face,
                    title_scale,
                    title_color,
                    title_thickness,
                    cv::LINE_AA);
        cv::putText(canvas,
                    clipped_subtitle,
                    cv::Point(rect.x + rect.width - pad - cv::getTextSize(clipped_subtitle, font_face, subtitle_scale, subtitle_thickness, nullptr).width,
                              rect.y + pad + header_height / 2),
                    font_face,
                    subtitle_scale,
                    subtitle_color,
                    subtitle_thickness,
                    cv::LINE_AA);
        return insetRect(cv::Rect(rect.x, rect.y + header_rect.height, rect.width, rect.height - header_rect.height),
                         std::max(8, rect.width / 40),
                         std::max(8, rect.height / 40));
    }

    cv::Rect fitImageRect(const cv::Size &src_size, const cv::Rect &slot)
    {
        if (src_size.width <= 0 || src_size.height <= 0 || slot.width <= 0 || slot.height <= 0)
        {
            return cv::Rect(slot.x, slot.y, 1, 1);
        }
        const double scale = std::min(static_cast<double>(slot.width) / src_size.width,
                                      static_cast<double>(slot.height) / src_size.height);
        const int width = std::max(1, static_cast<int>(std::round(src_size.width * scale)));
        const int height = std::max(1, static_cast<int>(std::round(src_size.height * scale)));
        const int x = slot.x + (slot.width - width) / 2;
        const int y = slot.y + (slot.height - height) / 2;
        return cv::Rect(x, y, width, height);
    }

    cv::Rect drawFittedImage(const cv::Mat &src, cv::Mat &dst, const cv::Rect &slot, int interpolation)
    {
        if (src.empty())
        {
            return cv::Rect(slot.x, slot.y, 1, 1);
        }
        const cv::Rect target = fitImageRect(src.size(), slot);
        cv::Mat resized;
        cv::resize(src, resized, target.size(), 0.0, 0.0, interpolation);
        resized.copyTo(dst(target));
        return target;
    }

    cv::Mat normalizedSarToBgr(const cv::Mat &sar_norm)
    {
        cv::Mat sar_u8;
        sar_norm.convertTo(sar_u8, CV_8UC1, 255.0);
        cv::Mat sar_bgr;
        cv::cvtColor(sar_u8, sar_bgr, cv::COLOR_GRAY2BGR);
        return sar_bgr;
    }

    MiniMapContext buildMiniMapContext(const cv::Mat &sar_norm, int patch_size, int stride, int rows, int cols)
    {
        (void)stride;
        (void)rows;
        (void)cols;
        MiniMapContext context;
        context.sar_preview_bgr = normalizedSarToBgr(sar_norm);
        context.source_width = sar_norm.cols;
        context.source_height = sar_norm.rows;
        context.patch_size = patch_size;
        return context;
    }

    std::string formatCounter(int current, int total)
    {
        if (total > 0)
        {
            return std::to_string(current) + "/" + std::to_string(total);
        }
        return std::to_string(current);
    }

    std::string formatFrameCounter(int current)
    {
        return std::string("#") + std::to_string(current);
    }

    std::string formatMillis(double value)
    {
        std::ostringstream oss;
        oss.setf(std::ios::fixed);
        oss.precision(2);
        oss << value << " ms";
        return oss.str();
    }

    std::string formatFps(double value)
    {
        std::ostringstream oss;
        oss.setf(std::ios::fixed);
        oss.precision(1);
        oss << value;
        return oss.str();
    }

    void applyManualTelemetry(RuntimeState &state, UiRenderContext &ui_context)
    {
        const ManualFlightTelemetry telemetry = GetManualFlightTelemetry();
        if (!telemetry.active)
        {
            return;
        }

        state.manual_active = true;
        state.manual_pos_x = telemetry.position_x;
        state.manual_pos_y = telemetry.position_y;
        state.manual_vel_x = telemetry.velocity_x;
        state.manual_vel_y = telemetry.velocity_y;
        state.manual_target_x = telemetry.requested_center_x;
        state.manual_target_y = telemetry.requested_center_y;
        state.manual_last_inferred_x = telemetry.last_inferred_center_x;
        state.manual_last_inferred_y = telemetry.last_inferred_center_y;
        state.manual_path_points = telemetry.path_points;
        state.manual_keys = telemetry.active_keys;
        ui_context.mode_label = "MANUAL FLIGHT";
        ui_context.status_label = telemetry.paused ? "PAUSED" : "RUNNING";
        ui_context.mini_map.path_overlay = telemetry.path_overlay;

        auto &shared = manualFlightSharedState();
        std::lock_guard<std::mutex> lock(shared.mutex);
        ui_context.mini_map.path_points = shared.path_points;
    }

    cv::Point mapPointToRect(const cv::Point2f &point, const MiniMapContext &context, const cv::Rect &target_rect)
    {
        const double x_ratio = context.source_width > 0 ? point.x / static_cast<double>(context.source_width) : 0.0;
        const double y_ratio = context.source_height > 0 ? point.y / static_cast<double>(context.source_height) : 0.0;
        const int x = target_rect.x + static_cast<int>(std::round(x_ratio * target_rect.width));
        const int y = target_rect.y + static_cast<int>(std::round(y_ratio * target_rect.height));
        return cv::Point(x, y);
    }

    void drawMetricRows(cv::Mat &canvas,
                        const cv::Rect &body_rect,
                        const std::vector<std::pair<std::string, std::string>> &metrics,
                        const cv::Scalar &label_color,
                        const cv::Scalar &value_color,
                        const cv::Scalar &rule_color)
    {
        if (metrics.empty())
        {
            return;
        }

        const int font_face = cv::FONT_HERSHEY_SIMPLEX;
        const double font_scale = std::max(0.36, body_rect.height / 780.0);
        const int thickness = std::max(1, body_rect.height / 220);
        const int row_height = std::max(20, body_rect.height / static_cast<int>(metrics.size() + 1));
        const int label_width = static_cast<int>(body_rect.width * 0.46);

        int y = body_rect.y + row_height / 2;
        for (const auto &[label, value] : metrics)
        {
            if (y + row_height / 2 > body_rect.br().y)
            {
                break;
            }
            cv::line(canvas,
                     cv::Point(body_rect.x, y + row_height / 2),
                     cv::Point(body_rect.x + body_rect.width, y + row_height / 2),
                     rule_color,
                     1,
                     cv::LINE_AA);

            const std::string clipped_label = truncateToWidth(label, label_width, font_face, font_scale, thickness);
            const std::string clipped_value = truncateToWidth(value,
                                                              body_rect.width - label_width - 6,
                                                              font_face,
                                                              font_scale,
                                                              thickness);

            cv::putText(canvas,
                        clipped_label,
                        cv::Point(body_rect.x, y),
                        font_face,
                        font_scale,
                        label_color,
                        thickness,
                        cv::LINE_AA);
            const int value_width = cv::getTextSize(clipped_value, font_face, font_scale, thickness, nullptr).width;
            cv::putText(canvas,
                        clipped_value,
                        cv::Point(body_rect.x + body_rect.width - value_width, y),
                        font_face,
                        font_scale,
                        value_color,
                        thickness,
                        cv::LINE_AA);
            y += row_height;
        }
    }

    void drawMiniMap(cv::Mat &canvas,
                     const cv::Rect &body_rect,
                     const MiniMapContext &context,
                     const PatchInfo &patch,
                     const cv::Scalar &patch_color,
                     const cv::Scalar &current_point_color,
                     const cv::Scalar &border_color)
    {
        cv::rectangle(canvas, body_rect, cv::Scalar(242, 236, 231), cv::FILLED);
        cv::rectangle(canvas, body_rect, border_color, 1, cv::LINE_AA);
        drawGridTexture(canvas, body_rect, std::max(14, body_rect.width / 12), cv::Scalar(255, 255, 255));

        const cv::Rect image_rect = drawFittedImage(context.sar_preview_bgr, canvas, insetRect(body_rect, 8, 8), cv::INTER_LINEAR);

        if (context.path_overlay && context.path_points.size() >= 2)
        {
            std::vector<cv::Point> path_pixels;
            path_pixels.reserve(context.path_points.size());
            for (const auto &point : context.path_points)
            {
                path_pixels.push_back(mapPointToRect(cv::Point2f(static_cast<float>(point.x), static_cast<float>(point.y)),
                                                     context,
                                                     image_rect));
            }
            const cv::Point *points = path_pixels.data();
            const int point_count = static_cast<int>(path_pixels.size());
            cv::polylines(canvas, &points, &point_count, 1, false, cv::Scalar(66, 133, 244), std::max(1, body_rect.width / 180), cv::LINE_AA);
        }

        const double scale_x = context.source_width > 0 ? static_cast<double>(image_rect.width) / context.source_width : 1.0;
        const double scale_y = context.source_height > 0 ? static_cast<double>(image_rect.height) / context.source_height : 1.0;
        const cv::Rect patch_rect(image_rect.x + static_cast<int>(std::round(patch.x * scale_x)),
                                 image_rect.y + static_cast<int>(std::round(patch.y * scale_y)),
                                  std::max(1, static_cast<int>(std::round(patch.width * scale_x))),
                                  std::max(1, static_cast<int>(std::round(patch.height * scale_y))));
        cv::rectangle(canvas, patch_rect, patch_color, std::max(2, body_rect.width / 110), cv::LINE_AA);

        const cv::Point current_center = mapPointToRect(cv::Point2f(static_cast<float>(patch.x + patch.width / 2),
                                                                    static_cast<float>(patch.y + patch.height / 2)),
                                                        context,
                                                        image_rect);
        cv::circle(canvas, current_center, std::max(3, body_rect.width / 70), current_point_color, cv::FILLED, cv::LINE_AA);
        cv::circle(canvas, current_center, std::max(5, body_rect.width / 50), current_point_color, 1, cv::LINE_AA);

    }

    void drawLegend(cv::Mat &canvas, const cv::Rect &panel_rect)
    {
        static const std::array<const char *, SEG_CLASSES> names = {
            "Water",
            "Vegetation",
            "Bareland",
            "Road",
            "Building",
            "Mountain"};

        cv::rectangle(canvas, panel_rect, cv::Scalar(246, 242, 238), cv::FILLED);
        cv::rectangle(canvas, panel_rect, cv::Scalar(176, 163, 151), 1, cv::LINE_AA);

        const int font_face = cv::FONT_HERSHEY_SIMPLEX;
        const double title_scale = std::max(0.34, panel_rect.height / 140.0);
        const double row_scale = std::max(0.3, panel_rect.height / 170.0);
        const int title_thickness = std::max(1, panel_rect.height / 70);
        const int row_thickness = std::max(1, title_thickness - 1);
        const int pad = std::max(8, panel_rect.width / 18);
        cv::putText(canvas,
                    "LEGEND",
                    cv::Point(panel_rect.x + pad, panel_rect.y + pad + panel_rect.height / 10),
                    font_face,
                    title_scale,
                    cv::Scalar(42, 23, 15),
                    title_thickness,
                    cv::LINE_AA);

        const int columns = 2;
        const int cell_width = std::max(1, (panel_rect.width - pad * 2) / columns);
        const int cell_height = std::max(18, (panel_rect.height - pad * 2 - panel_rect.height / 5) / 3);
        for (int cls = 0; cls < SEG_CLASSES; ++cls)
        {
            const int col = cls % columns;
            const int row = cls / columns;
            const int x = panel_rect.x + pad + col * cell_width;
            const int y = panel_rect.y + pad + panel_rect.height / 5 + row * cell_height;
            const cv::Rect color_rect(x, y, std::max(10, panel_rect.width / 22), std::max(10, panel_rect.height / 10));
            cv::rectangle(canvas, color_rect, classColorBgr(cls), cv::FILLED);
            cv::rectangle(canvas, color_rect, cv::Scalar(0, 0, 0), 1, cv::LINE_AA);
            const std::string clipped_name = truncateToWidth(names[cls],
                                                             cell_width - color_rect.width - 10,
                                                             font_face,
                                                             row_scale,
                                                             row_thickness);
            cv::putText(canvas,
                        clipped_name,
                        cv::Point(color_rect.br().x + 6, y + color_rect.height - 1),
                        font_face,
                        row_scale,
                        cv::Scalar(55, 41, 31),
                        row_thickness,
                        cv::LINE_AA);
        }
    }

    cv::Mat composeIndustrialUiFrame(const UiRenderContext &ui_context,
                                     const RuntimeState &state,
                                     const cv::Mat &restore_bgr,
                                     const cv::Mat &mask_bgr,
                                     int width,
                                     int height)
    {
        const cv::Scalar shell_bg(53, 47, 42);
        const cv::Scalar frame_bg(132, 122, 115);
        const cv::Scalar panel_bg(246, 242, 238);
        const cv::Scalar header_bg(226, 219, 214);
        const cv::Scalar border_color(184, 172, 162);
        const cv::Scalar title_color(42, 23, 15);
        const cv::Scalar subtitle_color(105, 85, 71);
        const cv::Scalar success_color(94, 197, 34);
        const cv::Scalar badge_bg(251, 248, 246);
        const cv::Scalar badge_text_color(85, 65, 51);
        const cv::Scalar running_text_color(52, 101, 22);
        const cv::Scalar patch_color(172, 239, 134);
        const cv::Scalar current_point_color(68, 68, 239);
        const cv::Scalar outer_border_color(175, 163, 154);
        const cv::Scalar inner_frame_border_color(114, 103, 95);
        const cv::Scalar outer_grid_color(148, 138, 130);
        const cv::Scalar divider_color(217, 208, 200);
        const cv::Scalar restore_body_bg(245, 241, 237);
        const cv::Scalar seg_body_bg(234, 227, 221);
        const cv::Scalar seg_grid_color(255, 255, 255);

        cv::Mat canvas(height, width, CV_8UC3, shell_bg);
        const int margin = std::max(10, std::min(width, height) / 48);
        const int gap = std::max(10, std::min(width, height) / 54);
        const int header_height = std::max(68, height / 11);
        const int footer_height = std::max(42, height / 19);
        const cv::Rect shell_rect(margin, margin, width - margin * 2, height - margin * 2);

        cv::rectangle(canvas, shell_rect, frame_bg, cv::FILLED);
        cv::rectangle(canvas, shell_rect, outer_border_color, 1, cv::LINE_AA);
        cv::rectangle(canvas, shell_rect, inner_frame_border_color, std::max(2, margin / 4), cv::LINE_AA);
        drawGridTexture(canvas, insetRect(shell_rect, 2, 2), std::max(18, width / 72), outer_grid_color);

        const cv::Rect header_rect(shell_rect.x, shell_rect.y, shell_rect.width, std::min(header_height, shell_rect.height));
        cv::rectangle(canvas, header_rect, header_bg, cv::FILLED);
        cv::rectangle(canvas, header_rect, border_color, 1, cv::LINE_AA);

        const int header_pad = std::max(12, width / 96);
        const int font_face = cv::FONT_HERSHEY_SIMPLEX;
        const double eyebrow_scale = std::max(0.4, header_height / 74.0);
        const int eyebrow_thickness = std::max(1, header_height / 28);
        const int badge_height = std::max(30, header_height / 2);
        const int badge_gap = std::max(8, header_pad / 2);
        const int badge_width = std::max(120, shell_rect.width / 8);
        const int title_x = header_rect.x + header_pad * 2 + std::max(36, header_height / 2);
        cv::putText(canvas,
                    "UAV CONTROL TERMINAL",
                    cv::Point(title_x, header_rect.y + header_pad + header_rect.height / 4),
                    font_face,
                    eyebrow_scale,
                    subtitle_color,
                    eyebrow_thickness,
                    cv::LINE_AA);

        auto drawBadge = [&](int x, const std::string &label, const cv::Scalar &text_color, bool with_dot) {
            const cv::Rect badge_rect(x, header_rect.y + (header_rect.height - badge_height) / 2, badge_width, badge_height);
            cv::rectangle(canvas, badge_rect, badge_bg, cv::FILLED);
            cv::rectangle(canvas, badge_rect, border_color, 1, cv::LINE_AA);
            int text_x = badge_rect.x + 10;
            if (with_dot)
            {
                cv::circle(canvas,
                           cv::Point(badge_rect.x + 14, badge_rect.y + badge_rect.height / 2),
                           std::max(3, badge_rect.height / 9),
                           success_color,
                           cv::FILLED,
                           cv::LINE_AA);
                text_x = badge_rect.x + 28;
            }
            cv::putText(canvas,
                        truncateToWidth(label, badge_rect.width - (text_x - badge_rect.x) - 8, font_face, 0.5, 1),
                        cv::Point(text_x, badge_rect.y + badge_rect.height / 2 + badge_rect.height / 8),
                        font_face,
                        0.5,
                        text_color,
                        1,
                        cv::LINE_AA);
        };

        int badge_x = header_rect.br().x - header_pad - badge_width;
        drawBadge(badge_x, ui_context.output_label, badge_text_color, false);
        badge_x -= badge_gap + badge_width;
        drawBadge(badge_x, "MODE / " + ui_context.mode_label, badge_text_color, false);
        badge_x -= badge_gap + badge_width;
        drawBadge(badge_x, ui_context.status_label, running_text_color, true);

        const int content_top = header_rect.br().y + gap;
        const int content_bottom = shell_rect.br().y - footer_height - gap;
        const int content_height = std::max(1, content_bottom - content_top);
        const int left_width = std::max(260, shell_rect.width / 5);
        const cv::Rect left_column(shell_rect.x, content_top, left_width, content_height);
        const cv::Rect main_column(left_column.br().x + gap,
                                   content_top,
                                   shell_rect.br().x - (left_column.br().x + gap),
                                   content_height);

        const int panel_header = std::max(40, height / 25);
        const int left_map_height = std::max(180, content_height * 2 / 5);
        const cv::Rect map_panel(left_column.x, left_column.y, left_column.width, left_map_height);
        const cv::Rect telemetry_panel(left_column.x,
                                       map_panel.br().y + gap,
                                       left_column.width,
                                       std::max(1, left_column.br().y - (map_panel.br().y + gap)));
        const cv::Rect map_body = drawPanel(canvas,
                                            map_panel,
                                            "",
                                            "SCENE LOCATOR",
                                            panel_header,
                                            panel_bg,
                                            header_bg,
                                            border_color,
                                            title_color,
                                            subtitle_color);
        drawMiniMap(canvas, map_body, ui_context.mini_map, state.patch, patch_color, current_point_color, border_color);

        const cv::Rect telemetry_body = drawPanel(canvas,
                                                  telemetry_panel,
                                                  "",
                                                  "RUNTIME MONITOR",
                                                  panel_header,
                                                  panel_bg,
                                                  header_bg,
                                                  border_color,
                                                  title_color,
                                                  subtitle_color);
        // In inference-only mode each SAR image is the artifact produced from one upstream
        // echo capture, so the echo and SAR counters intentionally stay aligned here.
        const std::vector<std::pair<std::string, std::string>> metrics = {
            {"SYSTEM", ui_context.status_label},
            {"MODE", ui_context.mode_label},
            {"ECHO", formatCounter(state.sar_index, state.sar_count)},
            {"SAR", formatCounter(state.sar_index, state.sar_count)},
            {"PATCH", formatCounter(state.patch.index + 1, state.patch_count)},
            {"FRAME", formatFrameCounter(state.frame_index)},
            {"SAR_NAME", state.sar_stem},
            {"GRID_RC", state.manual_active ? "-" : (std::to_string(state.patch.grid_row) + ", " + std::to_string(state.patch.grid_col))},
            {"FPS", formatFps(state.fps)},
            {"NPU_MS", formatMillis(state.infer_ms)},
            {"TOTAL_MS", formatMillis(state.total_ms)}};
        std::vector<std::pair<std::string, std::string>> telemetry_metrics = metrics;
        if (state.manual_active)
        {
            telemetry_metrics.push_back({"POS_XY", std::to_string(state.manual_pos_x) + ", " + std::to_string(state.manual_pos_y)});
            telemetry_metrics.push_back({"VEL_XY", std::to_string(state.manual_vel_x) + ", " + std::to_string(state.manual_vel_y)});
            telemetry_metrics.push_back({"TARGET_XY", std::to_string(state.manual_target_x) + ", " + std::to_string(state.manual_target_y)});
            telemetry_metrics.push_back({"LAST_XY", std::to_string(state.manual_last_inferred_x) + ", " + std::to_string(state.manual_last_inferred_y)});
            telemetry_metrics.push_back({"KEYS", state.manual_keys});
            telemetry_metrics.push_back({"PATH_POINTS", std::to_string(state.manual_path_points)});
        }
        drawMetricRows(canvas,
                       telemetry_body,
                       telemetry_metrics,
                       subtitle_color,
                       title_color,
                       divider_color);

        const int status_height = std::max(92, main_column.height / 6);
        const cv::Rect status_panel(main_column.x, main_column.y, main_column.width, status_height);
        const cv::Rect status_body = drawPanel(canvas,
                                               status_panel,
                                               "SYSTEM STRIP",
                                               "PATCH SUMMARY",
                                               panel_header,
                                               panel_bg,
                                               header_bg,
                                               border_color,
                                               title_color,
                                               subtitle_color);

        const int status_gap = std::max(8, status_body.width / 60);
        const int cell_width = std::max(1, (status_body.width - status_gap * 2) / 3);
        const std::array<cv::Rect, 3> status_cells = {
            cv::Rect(status_body.x, status_body.y, cell_width, status_body.height),
            cv::Rect(status_body.x + cell_width + status_gap, status_body.y, cell_width, status_body.height),
            cv::Rect(status_body.x + (cell_width + status_gap) * 2, status_body.y, cell_width, status_body.height)};
        const std::array<std::pair<std::string, std::string>, 3> status_items = {
            std::make_pair(std::string("SYSTEM"), std::string("READY / LIVE")),
            std::make_pair(std::string("PATCH CENTER"),
                           std::to_string(state.patch.x + state.patch.width / 2) + ", " +
                               std::to_string(state.patch.y + state.patch.height / 2)),
            std::make_pair(std::string("PATCH RULE"),
                           std::to_string(state.patch.width) + "x" + std::to_string(state.patch.height) +
                               " / stride " + std::to_string(state.stride))};

        for (size_t i = 0; i < status_cells.size(); ++i)
        {
            if (i > 0)
            {
                cv::line(canvas,
                         cv::Point(status_cells[i].x - status_gap / 2, status_cells[i].y),
                         cv::Point(status_cells[i].x - status_gap / 2, status_cells[i].br().y),
                         border_color,
                         1,
                         cv::LINE_AA);
            }
            cv::putText(canvas,
                        status_items[i].first,
                        cv::Point(status_cells[i].x, status_cells[i].y + status_cells[i].height / 4),
                        font_face,
                        std::max(0.35, status_cells[i].height / 130.0),
                        subtitle_color,
                        1,
                        cv::LINE_AA);
            cv::putText(canvas,
                        truncateToWidth(status_items[i].second,
                                        status_cells[i].width - 8,
                                        font_face,
                                        std::max(0.52, status_cells[i].height / 72.0),
                                        1),
                        cv::Point(status_cells[i].x, status_cells[i].y + status_cells[i].height - status_cells[i].height / 5),
                        font_face,
                        std::max(0.52, status_cells[i].height / 72.0),
                        title_color,
                        1,
                        cv::LINE_AA);
        }

        const cv::Rect views_area(main_column.x,
                                  status_panel.br().y + gap,
                                  main_column.width,
                                  std::max(1, main_column.br().y - (status_panel.br().y + gap)));
        const int view_gap = std::max(10, views_area.width / 60);
        const int view_width = std::max(1, (views_area.width - view_gap) / 2);
        const cv::Rect restore_panel(views_area.x, views_area.y, view_width, views_area.height);
        const cv::Rect seg_panel(restore_panel.br().x + view_gap, views_area.y, view_width, views_area.height);

        const cv::Rect restore_body = drawPanel(canvas,
                                                restore_panel,
                                                "RESTORED SAR",
                                                "NET / RESTORE",
                                                panel_header,
                                                panel_bg,
                                                header_bg,
                                                border_color,
                                                title_color,
                                                subtitle_color);
        cv::rectangle(canvas, restore_body, restore_body_bg, cv::FILLED);
        drawGridTexture(canvas, restore_body, std::max(18, restore_body.width / 18), cv::Scalar(255, 255, 255));
        drawFittedImage(restore_bgr, canvas, insetRect(restore_body, 10, 10), cv::INTER_NEAREST);
        const cv::Rect restore_badge(restore_body.x + 12, restore_body.y + 12, std::max(180, restore_body.width / 2), std::max(28, restore_body.height / 16));
        cv::rectangle(canvas, restore_badge, panel_bg, cv::FILLED);
        cv::rectangle(canvas, restore_badge, border_color, 1, cv::LINE_AA);
        cv::putText(canvas,
                    truncateToWidth("PATCH / " + state.sar_stem, restore_badge.width - 12, font_face, 0.42, 1),
                    cv::Point(restore_badge.x + 6, restore_badge.y + restore_badge.height / 2 + restore_badge.height / 8),
                    font_face,
                    0.42,
                    badge_text_color,
                    1,
                    cv::LINE_AA);
        const cv::Rect restore_footer(restore_body.br().x - std::max(120, restore_body.width / 4) - 12,
                                      restore_body.br().y - std::max(28, restore_body.height / 16) - 12,
                                      std::max(120, restore_body.width / 4),
                                      std::max(28, restore_body.height / 16));
        cv::rectangle(canvas, restore_footer, panel_bg, cv::FILLED);
        cv::rectangle(canvas, restore_footer, border_color, 1, cv::LINE_AA);
        cv::putText(canvas,
                    ui_context.restore_label,
                    cv::Point(restore_footer.x + 8, restore_footer.y + restore_footer.height / 2 + restore_footer.height / 8),
                    font_face,
                    0.42,
                    badge_text_color,
                    1,
                    cv::LINE_AA);

        const cv::Rect seg_body = drawPanel(canvas,
                                            seg_panel,
                                            "SEGMENTATION RGB",
                                            "NET / SEGMENT",
                                            panel_header,
                                            panel_bg,
                                            header_bg,
                                            border_color,
                                            title_color,
                                            subtitle_color);
        cv::rectangle(canvas, seg_body, seg_body_bg, cv::FILLED);
        drawGridTexture(canvas, seg_body, std::max(18, seg_body.width / 18), seg_grid_color);
        drawFittedImage(mask_bgr, canvas, insetRect(seg_body, 10, 10), cv::INTER_NEAREST);
        const cv::Rect seg_badge(seg_body.x + 12, seg_body.y + 12, std::max(180, seg_body.width / 3), std::max(28, seg_body.height / 16));
        cv::rectangle(canvas, seg_badge, panel_bg, cv::FILLED);
        cv::rectangle(canvas, seg_badge, border_color, 1, cv::LINE_AA);
        cv::putText(canvas,
                    ui_context.seg_label,
                    cv::Point(seg_badge.x + 6, seg_badge.y + seg_badge.height / 2 + seg_badge.height / 8),
                    font_face,
                    0.42,
                    badge_text_color,
                    1,
                    cv::LINE_AA);

        const cv::Rect legend_panel(seg_body.br().x - std::max(220, seg_body.width / 3) - 12,
                                    seg_body.br().y - std::max(126, seg_body.height / 4) - 12,
                                    std::max(220, seg_body.width / 3),
                                    std::max(126, seg_body.height / 4));
        drawLegend(canvas, legend_panel);

        const cv::Rect footer_rect(shell_rect.x, content_bottom + gap, shell_rect.width, footer_height);
        cv::rectangle(canvas, footer_rect, panel_bg, cv::FILLED);
        cv::rectangle(canvas, footer_rect, border_color, 1, cv::LINE_AA);
        const double footer_scale = std::max(0.38, footer_rect.height / 70.0);
        const std::string left_footer = truncateToWidth("AUTO_SNAKE / PATCH 512x512 / STRIDE " + std::to_string(state.stride),
                                                        footer_rect.width / 2,
                                                        font_face,
                                                        footer_scale,
                                                        1);
        const std::string right_footer = truncateToWidth(ui_context.output_label,
                                                         footer_rect.width / 2,
                                                         font_face,
                                                         footer_scale,
                                                         1);
        cv::putText(canvas,
                    left_footer,
                    cv::Point(footer_rect.x + header_pad, footer_rect.y + footer_rect.height / 2 + footer_rect.height / 8),
                    font_face,
                    footer_scale,
                    badge_text_color,
                    1,
                    cv::LINE_AA);
        const int footer_width = cv::getTextSize(right_footer, font_face, footer_scale, 1, nullptr).width;
        cv::putText(canvas,
                    right_footer,
                    cv::Point(footer_rect.br().x - header_pad - footer_width, footer_rect.y + footer_rect.height / 2 + footer_rect.height / 8),
                    font_face,
                    footer_scale,
                    title_color,
                    1,
                    cv::LINE_AA);

        return canvas;
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
                ManualFlightRuntime manual_runtime(sar_norm, cfg.patch_size);
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
