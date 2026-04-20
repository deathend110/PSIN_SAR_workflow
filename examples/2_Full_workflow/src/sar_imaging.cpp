#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace
{
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
        cv::Mat data; // CV_64FC2, rows=range(nr), cols=azimuth(na)
        int rows = 0;
        int cols = 0;
    };

    struct BatchConfig
    {
        fs::path input_dir = "./io/echo";
        fs::path output_dir = "./io/sar_img";
        std::string input_ext = ".bin";
        std::string output_ext = ".png";
        bool recursive = false;
        bool overwrite = true;
    };

    std::int32_t readLittleEndianInt32(std::ifstream &ifs)
    {
        unsigned char bytes[4] = {0, 0, 0, 0};
        if (!ifs.read(reinterpret_cast<char *>(bytes), sizeof(bytes)))
        {
            throw std::runtime_error("Failed to read int32 header from echo bin.");
        }

        const std::uint32_t value =
            static_cast<std::uint32_t>(bytes[0]) |
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

        const std::uint32_t raw =
            static_cast<std::uint32_t>(bytes[0]) |
            (static_cast<std::uint32_t>(bytes[1]) << 8) |
            (static_cast<std::uint32_t>(bytes[2]) << 16) |
            (static_cast<std::uint32_t>(bytes[3]) << 24);

        float value = 0.0f;
        static_assert(sizeof(value) == sizeof(raw), "float32 must be 4 bytes.");
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
            throw std::runtime_error("Invalid echo shape in header: " +
                                     std::to_string(height) + "x" + std::to_string(width));
        }

        const std::uintmax_t expected_bytes =
            8ull + static_cast<std::uintmax_t>(height) *
                       static_cast<std::uintmax_t>(width) * 2ull * sizeof(float);
        const auto actual_bytes = fs::file_size(path);
        if (actual_bytes != expected_bytes)
        {
            throw std::runtime_error("Echo bin size mismatch. expected=" +
                                     std::to_string(expected_bytes) +
                                     ", actual=" + std::to_string(actual_bytes));
        }

        cv::Mat data(height, width, CV_64FC2);
        for (int r = 0; r < height; ++r)
        {
            auto *row = data.ptr<cv::Vec2d>(r);
            for (int c = 0; c < width; ++c)
            {
                const float real = readLittleEndianFloat32(ifs);
                const float imag = readLittleEndianFloat32(ifs);
                row[c] = cv::Vec2d(static_cast<double>(real), static_cast<double>(imag));
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
                const int src_r = (r - shift + src.rows) % src.rows;
                src.row(src_r).copyTo(dst.row(r));
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
                    const int src_c = (c - shift + src.cols) % src.cols;
                    dst_row[c] = src_row[src_c];
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
        return cv::Vec2d(a[0] * b[0] - a[1] * b[1],
                         a[0] * b[1] + a[1] * b[0]);
    }

    cv::Mat multiplyRowsByFilter(const cv::Mat &src, const std::vector<cv::Vec2d> &filter)
    {
        CV_Assert(src.type() == CV_64FC2);
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
        CV_Assert(src.type() == CV_64FC2);
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

    std::vector<cv::Vec2d> makeComplexExponential(const std::vector<double> &freq,
                                                  double coefficient)
    {
        std::vector<cv::Vec2d> out(freq.size());
        for (size_t i = 0; i < freq.size(); ++i)
        {
            const double phase = coefficient * freq[i] * freq[i];
            out[i] = cv::Vec2d(std::cos(phase), std::sin(phase));
        }
        return out;
    }

    cv::Mat vectorizedRcmc(const cv::Mat &data_fa,
                           const std::vector<double> &f_a,
                           const RadarConfig &cfg,
                           bool inverse = false)
    {
        CV_Assert(data_fa.type() == CV_64FC2);
        const int nr = data_fa.rows;
        const int na = data_fa.cols;
        const double delta_r = cfg.c / (2.0 * cfg.Fs);
        const double v2 = cfg.v_platform * cfg.v_platform;
        const double lam = cfg.lam();

        cv::Mat output = cv::Mat::zeros(data_fa.size(), data_fa.type());
        for (int a = 0; a < na; ++a)
        {
            const double delta_R = cfg.R0 * std::pow(lam * f_a[a], 2.0) / (8.0 * v2);
            const double signed_shift = (inverse ? -1.0 : 1.0) * (delta_R / delta_r);

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
                output.at<cv::Vec2d>(r, a) =
                    cv::Vec2d((1.0 - w) * v0[0] + w * v1[0],
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

        cv::Mat data = echo.clone();

        cv::Mat data_rc = fftshift(data, 0);
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

    cv::Mat magnitudeMinMaxToU8(const cv::Mat &complex_img)
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

        cv::Mat out(complex_img.rows, complex_img.cols, CV_8UC1, cv::Scalar(0));
        if (max_val == min_val)
        {
            return out;
        }

        const double scale = 255.0 / (max_val - min_val);
        for (int r = 0; r < mag.rows; ++r)
        {
            const auto *mag_row = mag.ptr<double>(r);
            auto *out_row = out.ptr<std::uint8_t>(r);
            for (int c = 0; c < mag.cols; ++c)
            {
                const double normalized = (mag_row[c] - min_val) * scale;
                out_row[c] = static_cast<std::uint8_t>(std::floor(normalized));
            }
        }
        return out;
    }

    void ensureParentDirectory(const fs::path &path)
    {
        const auto parent = path.parent_path();
        if (!parent.empty())
        {
            fs::create_directories(parent);
        }
    }

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

    std::string toLower(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return value;
    }

    bool parseBool(const std::string &value)
    {
        const auto lowered = toLower(trim(value));
        if (lowered == "true" || lowered == "1" || lowered == "yes" || lowered == "on")
        {
            return true;
        }
        if (lowered == "false" || lowered == "0" || lowered == "no" || lowered == "off")
        {
            return false;
        }
        throw std::runtime_error("Failed to parse bool config value: " + value);
    }

    std::string joinPath(const std::vector<std::string> &scopes)
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

    std::unordered_map<std::string, std::string> loadSimpleYaml(const fs::path &config_path)
    {
        std::ifstream ifs(config_path);
        if (!ifs)
        {
            throw std::runtime_error("Failed to open config file: " + config_path.string());
        }

        std::unordered_map<std::string, std::string> values;
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
                values[joinPath(scopes)] = stripQuotes(raw_value);
            }
        }
        return values;
    }

    std::string valueOr(const std::unordered_map<std::string, std::string> &values,
                        const std::string &key,
                        const std::string &default_value)
    {
        const auto it = values.find(key);
        return it == values.end() ? default_value : it->second;
    }

    BatchConfig loadBatchConfig(const fs::path &config_path)
    {
        const auto values = loadSimpleYaml(config_path);
        BatchConfig cfg;
        cfg.input_dir = valueOr(values,
                                "input.echo_dir",
                                valueOr(values, "input_dir", cfg.input_dir.string()));
        cfg.output_dir = valueOr(values,
                                 "output.sar_img_dir",
                                 valueOr(values, "output_dir", cfg.output_dir.string()));
        cfg.input_ext = valueOr(values, "input.ext", valueOr(values, "input_ext", cfg.input_ext));
        cfg.output_ext = valueOr(values, "output.ext", valueOr(values, "output_ext", cfg.output_ext));
        cfg.recursive = parseBool(valueOr(values, "runtime.recursive", valueOr(values, "recursive", "false")));
        cfg.overwrite = parseBool(valueOr(values, "runtime.overwrite", valueOr(values, "overwrite", "true")));

        if (!cfg.input_ext.empty() && cfg.input_ext.front() != '.')
        {
            cfg.input_ext = "." + cfg.input_ext;
        }
        if (!cfg.output_ext.empty() && cfg.output_ext.front() != '.')
        {
            cfg.output_ext = "." + cfg.output_ext;
        }
        return cfg;
    }

    void processEchoFile(const fs::path &input_path, const fs::path &output_path)
    {
        const auto start = std::chrono::steady_clock::now();
        const ComplexImage echo = loadEchoBin(input_path);
        std::cout << "Loaded echo: " << echo.rows << "x" << echo.cols << " from "
                  << input_path.string() << "\n";

        const RadarConfig cfg;
        const cv::Mat sar_complex = runImaging(echo.data, cfg);
        const cv::Mat sar_u8 = magnitudeMinMaxToU8(sar_complex);

        ensureParentDirectory(output_path);
        if (!cv::imwrite(output_path.string(), sar_u8))
        {
            throw std::runtime_error("Failed to write output png: " + output_path.string());
        }

        const auto end = std::chrono::steady_clock::now();
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        std::cout << "Saved SAR image: " << output_path.string() << "\n";
        std::cout << "C++ RD elapsed: " << elapsed_ms << " ms\n";
    }

    std::vector<fs::path> collectEchoBins(const BatchConfig &cfg)
    {
        if (!fs::exists(cfg.input_dir) || !fs::is_directory(cfg.input_dir))
        {
            throw std::runtime_error("Input echo directory does not exist: " + cfg.input_dir.string());
        }

        std::vector<fs::path> files;
        const auto wanted_ext = toLower(cfg.input_ext);
        if (cfg.recursive)
        {
            for (const auto &entry : fs::recursive_directory_iterator(cfg.input_dir))
            {
                if (entry.is_regular_file() && toLower(entry.path().extension().string()) == wanted_ext)
                {
                    files.push_back(entry.path());
                }
            }
        }
        else
        {
            for (const auto &entry : fs::directory_iterator(cfg.input_dir))
            {
                if (entry.is_regular_file() && toLower(entry.path().extension().string()) == wanted_ext)
                {
                    files.push_back(entry.path());
                }
            }
        }

        std::sort(files.begin(), files.end());
        return files;
    }

    int runBatch(const fs::path &config_path)
    {
        const BatchConfig cfg = loadBatchConfig(config_path);
        fs::create_directories(cfg.output_dir);
        const auto files = collectEchoBins(cfg);
        if (files.empty())
        {
            std::cout << "No echo bin files found in: " << cfg.input_dir.string() << "\n";
            return 0;
        }

        std::cout << "Batch SAR imaging config: " << config_path.string() << "\n";
        std::cout << "Input echo dir: " << cfg.input_dir.string() << "\n";
        std::cout << "Output SAR dir: " << cfg.output_dir.string() << "\n";
        std::cout << "Files: " << files.size() << "\n";

        int failed = 0;
        const auto batch_start = std::chrono::steady_clock::now();
        for (size_t i = 0; i < files.size(); ++i)
        {
            const auto &input_path = files[i];
            const auto output_path = cfg.output_dir / (input_path.stem().string() + cfg.output_ext);
            std::cout << "\n[" << (i + 1) << "/" << files.size() << "] "
                      << input_path.filename().string() << " -> "
                      << output_path.filename().string() << "\n";

            if (fs::exists(output_path) && !cfg.overwrite)
            {
                std::cout << "Skip existing output because overwrite=false: "
                          << output_path.string() << "\n";
                continue;
            }

            try
            {
                processEchoFile(input_path, output_path);
            }
            catch (const std::exception &e)
            {
                ++failed;
                std::cerr << "Failed to process " << input_path.string() << ": " << e.what() << "\n";
            }
        }

        const auto batch_end = std::chrono::steady_clock::now();
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(batch_end - batch_start).count();
        std::cout << "\nBatch finished. total=" << files.size()
                  << ", failed=" << failed
                  << ", elapsed=" << elapsed_ms << " ms\n";
        return failed == 0 ? 0 : 3;
    }
}

int main(int argc, char **argv)
{
    try
    {
        if (argc == 2)
        {
            return runBatch(argv[1]);
        }

        if (argc == 3)
        {
            processEchoFile(argv[1], argv[2]);
            return 0;
        }

        std::cerr << "Usage:\n"
                  << "  " << argv[0] << " <config.yaml>\n"
                  << "  " << argv[0] << " <input_echo.bin> <output_png>\n";
        return 1;
    }
    catch (const std::exception &e)
    {
        std::cerr << "sar_imaging failed: " << e.what() << "\n";
        return 2;
    }
}
