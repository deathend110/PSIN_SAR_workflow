#include "workflow/rd/rd_config.hpp"
#include "workflow/rd/rd_workflow.hpp"
#include "workflow/shared/config_utils.hpp"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace workflow::rd
{
    namespace fs = std::filesystem;

    namespace
    {
        // 通过异常把“协作式停止”从深层 tile 处理循环快速抛回顶层主循环。
        struct StopRequested : public std::exception
        {
            const char *what() const noexcept override
            {
                return "stop requested";
            }
        };

        // 当前 RD 成像使用的一组固定雷达参数。
        // 这些参数直接参与距离压缩、方位压缩和 RCMC 的滤波器推导。
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

        // echo bin 文件头里声明的二维复数矩阵尺寸。
        struct EchoShape
        {
            int rows = 0;
            int cols = 0;
        };

    // 把 RD 配置映射成统一的运行时选择快照，便于 Web Console 展示同一套状态协议。
    shared::WorkflowSelection makeSelection(const AppConfig &cfg)
    {
        shared::WorkflowSelection selection;
        selection.workflow = shared::SelectedWorkflow::RdOnly;
        selection.patch_mode = shared::SelectedPatchMode::AutoSnake;
        selection.output_mode = "png";
        selection.selected_source = cfg.echo_dir.string();
        return selection;
    }

    // 向外部控制面发布 RD 当前所处阶段。
    // RD 本身不感知 Web，二者只通过 WorkflowRunControl 交换快照。
    void publishSnapshot(shared::WorkflowRunControl *control,
                         const AppConfig &cfg,
                         shared::ControlState state,
                         const std::string &stage,
                         const std::string &current_item,
                         int current_index,
                         int total_count,
                         const std::string &last_error = {})
    {
        if (control == nullptr)
        {
            return;
        }

        shared::WorkflowRuntimeSnapshot snapshot;
        snapshot.state = state;
        snapshot.selection = makeSelection(cfg);
        snapshot.current_stage = stage;
        snapshot.current_item = current_item;
        snapshot.last_error = last_error;
        snapshot.current_index = current_index;
        snapshot.total_count = total_count;
        control->publish(snapshot);
    }

    // 在每个安全点检查暂停/停止请求。
    // RD 是 tile 驱动的大循环，因此这里是所有长耗时步骤的统一协作式控制入口。
    void checkControl(shared::WorkflowRunControl *control)
    {
        if (control == nullptr)
        {
            return;
        }
        control->waitIfPaused();
        if (control->shouldStop())
        {
            throw StopRequested{};
        }
    }

    // 统一输出带时间戳的控制台日志，便于串联多阶段 scratch / memory 管线的执行轨迹。
    void logLine(const std::string &message)
    {
        const auto now = std::chrono::system_clock::now();
        const auto t = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        std::cout << "[" << std::put_time(&tm, "%F %T") << "] " << message << std::endl;
    }

    // echo bin 文件头使用 little-endian int32 存储尺寸，这里显式按字节解析避免平台歧义。
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

    // 读取 echo 文件头并校验“声明尺寸”和“实际文件字节数”是否一致。
    EchoShape readEchoShapeAndValidate(const fs::path &path)
    {
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs)
        {
            throw std::runtime_error("Failed to open echo bin: " + path.string());
        }
        const int rows = readLittleEndianInt32(ifs);
        const int cols = readLittleEndianInt32(ifs);
        if (rows <= 0 || cols <= 0)
        {
            throw std::runtime_error("Invalid echo shape: " + std::to_string(rows) + "x" + std::to_string(cols));
        }

        const std::uintmax_t expected_bytes =
            8ull + static_cast<std::uintmax_t>(rows) * static_cast<std::uintmax_t>(cols) * 2ull * sizeof(float);
        const auto actual_bytes = fs::file_size(path);
        if (actual_bytes != expected_bytes)
        {
            throw std::runtime_error("Echo bin size mismatch. expected=" + std::to_string(expected_bytes) +
                                     ", actual=" + std::to_string(actual_bytes));
        }
        return EchoShape{rows, cols};
    }

    // 生成和 numpy `fftshift(fftfreq(...))` 语义一致的频率轴，供滤波器设计使用。
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

    // 根据二次相位项生成复指数滤波器，double 版本用于高精度 scratch / memory 管线。
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

    // float32 版本滤波器，供 memory_float32 管线减少内存占用。
    std::vector<cv::Vec2f> makeComplexExponentialF32(const std::vector<double> &freq, double coefficient)
    {
        std::vector<cv::Vec2f> out(freq.size());
        for (size_t i = 0; i < freq.size(); ++i)
        {
            const double phase = coefficient * freq[i] * freq[i];
            out[i] = cv::Vec2f(static_cast<float>(std::cos(phase)), static_cast<float>(std::sin(phase)));
        }
        return out;
    }

    // 复数乘法的小工具，避免在热点循环里重复写实部/虚部公式。
    cv::Vec2d complexMultiply(const cv::Vec2d &a, const cv::Vec2d &b)
    {
        return cv::Vec2d(a[0] * b[0] - a[1] * b[1], a[0] * b[1] + a[1] * b[0]);
    }

    // float32 版本复数乘法。
    cv::Vec2f complexMultiply(const cv::Vec2f &a, const cv::Vec2f &b)
    {
        return cv::Vec2f(a[0] * b[0] - a[1] * b[1], a[0] * b[1] + a[1] * b[0]);
    }

    // 对指定轴执行循环位移，是 fftshift/ifftshift 的底层实现。
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

    // double 复数矩阵版本 fftshift。
    cv::Mat fftshift(const cv::Mat &src, int axis)
    {
        const int n = (axis == 0) ? src.rows : src.cols;
        return rollAxis(src, axis, n / 2);
    }

    // double 复数矩阵版本 ifftshift。
    cv::Mat ifftshift(const cv::Mat &src, int axis)
    {
        const int n = (axis == 0) ? src.rows : src.cols;
        return rollAxis(src, axis, -(n / 2));
    }

    // float32 复数矩阵版本的循环位移。
    cv::Mat rollAxisF32(const cv::Mat &src, int axis, int shift)
    {
        CV_Assert(src.type() == CV_32FC2);
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
                const auto *src_row = src.ptr<cv::Vec2f>(r);
                auto *dst_row = dst.ptr<cv::Vec2f>(r);
                for (int c = 0; c < src.cols; ++c)
                {
                    dst_row[c] = src_row[(c - shift + src.cols) % src.cols];
                }
            }
        }
        return dst;
    }

    // float32 复数矩阵版本 fftshift。
    cv::Mat fftshiftF32(const cv::Mat &src, int axis)
    {
        const int n = (axis == 0) ? src.rows : src.cols;
        return rollAxisF32(src, axis, n / 2);
    }

    // float32 复数矩阵版本 ifftshift。
    cv::Mat ifftshiftF32(const cv::Mat &src, int axis)
    {
        const int n = (axis == 0) ? src.rows : src.cols;
        return rollAxisF32(src, axis, -(n / 2));
    }

    // 沿单个轴做 DFT / IDFT。
    // OpenCV 原生更擅长按行处理，这里对 axis=0 通过转置绕过去。
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

    // float32 版本单轴 DFT / IDFT。
    cv::Mat dftAxisF32(const cv::Mat &src, int axis, bool inverse)
    {
        CV_Assert(src.type() == CV_32FC2);
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

    // 将二维复数矩阵中的某个元素位置映射成 scratch 文件字节偏移。
    std::uint64_t complexOffsetBytes(int row, int col, int total_cols)
    {
        return static_cast<std::uint64_t>(row) * static_cast<std::uint64_t>(total_cols) * sizeof(cv::Vec2d) +
               static_cast<std::uint64_t>(col) * sizeof(cv::Vec2d);
    }

    // 幅度 scratch 是 double 标量矩阵，这里计算其字节偏移。
    std::uint64_t doubleOffsetBytes(int row, int col, int total_cols)
    {
        return static_cast<std::uint64_t>(row) * static_cast<std::uint64_t>(total_cols) * sizeof(double) +
               static_cast<std::uint64_t>(col) * sizeof(double);
    }

    // 带错误检查的 seekg，避免文件随机访问失败后静默继续。
    void checkedSeekg(std::ifstream &ifs, std::uint64_t offset)
    {
        ifs.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!ifs)
        {
            throw std::runtime_error("Failed to seek input file.");
        }
    }

    // 带错误检查的 seekp，用于对 scratch 文件执行按 tile 的随机写入。
    void checkedSeekp(std::fstream &fs_out, std::uint64_t offset)
    {
        fs_out.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!fs_out)
        {
            throw std::runtime_error("Failed to seek scratch file.");
        }
    }

    // 从原始 echo bin 中按“列块”读取复数数据。
    // 距离压缩阶段按列处理，因此这种访问模式能减少峰值内存。
    cv::Mat readEchoColumnTile(const fs::path &path, int rows, int cols, int col0, int tile_cols)
    {
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs)
        {
            throw std::runtime_error("Failed to open echo bin: " + path.string());
        }

        cv::Mat tile(rows, tile_cols, CV_64FC2);
        std::vector<float> packed(static_cast<size_t>(tile_cols) * 2u);
        for (int r = 0; r < rows; ++r)
        {
            const std::uint64_t offset = 8ull +
                                         (static_cast<std::uint64_t>(r) * static_cast<std::uint64_t>(cols) +
                                          static_cast<std::uint64_t>(col0)) *
                                             2ull * sizeof(float);
            checkedSeekg(ifs, offset);
            if (!ifs.read(reinterpret_cast<char *>(packed.data()),
                          static_cast<std::streamsize>(packed.size() * sizeof(float))))
            {
                throw std::runtime_error("Failed to read echo column tile.");
            }

            auto *dst_row = tile.ptr<cv::Vec2d>(r);
            for (int c = 0; c < tile_cols; ++c)
            {
                dst_row[c] = cv::Vec2d(static_cast<double>(packed[2 * c]),
                                       static_cast<double>(packed[2 * c + 1]));
            }
        }
        return tile;
    }

    // 一次性把整个 echo 读入 float32 复数矩阵，供 memory_float32 路径使用。
    cv::Mat loadEchoF32(const fs::path &path, const EchoShape &shape)
    {
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs)
        {
            throw std::runtime_error("Failed to open echo bin: " + path.string());
        }
        checkedSeekg(ifs, 8);

        cv::Mat data(shape.rows, shape.cols, CV_32FC2);
        for (int r = 0; r < shape.rows; ++r)
        {
            if (!ifs.read(reinterpret_cast<char *>(data.ptr<cv::Vec2f>(r)),
                          static_cast<std::streamsize>(shape.cols * sizeof(cv::Vec2f))))
            {
                throw std::runtime_error("Failed to read echo row into memory.");
            }
        }
        return data;
    }

    // 从整图内存矩阵中切出一块列 tile。
    cv::Mat gatherColumnTileF32(const cv::Mat &src, int col0, int tile_cols)
    {
        CV_Assert(src.type() == CV_32FC2);
        cv::Mat tile(src.rows, tile_cols, CV_32FC2);
        for (int r = 0; r < src.rows; ++r)
        {
            std::memcpy(tile.ptr<cv::Vec2f>(r),
                        src.ptr<cv::Vec2f>(r) + col0,
                        static_cast<size_t>(tile_cols) * sizeof(cv::Vec2f));
        }
        return tile;
    }

    // 将处理完的 float32 列 tile 写回整图。
    void scatterColumnTileF32(const cv::Mat &tile, cv::Mat &dst, int col0)
    {
        CV_Assert(tile.type() == CV_32FC2);
        CV_Assert(dst.type() == CV_32FC2);
        for (int r = 0; r < dst.rows; ++r)
        {
            std::memcpy(dst.ptr<cv::Vec2f>(r) + col0,
                        tile.ptr<cv::Vec2f>(r),
                        static_cast<size_t>(tile.cols) * sizeof(cv::Vec2f));
        }
    }

    // 从 scratch 复数文件中按“行块”读回一段数据，供方位向相关阶段处理。
    cv::Mat readComplexRowTile(const fs::path &path, int cols, int row0, int tile_rows)
    {
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs)
        {
            throw std::runtime_error("Failed to open scratch file: " + path.string());
        }

        cv::Mat tile(tile_rows, cols, CV_64FC2);
        checkedSeekg(ifs, complexOffsetBytes(row0, 0, cols));
        if (!ifs.read(reinterpret_cast<char *>(tile.ptr<cv::Vec2d>(0)),
                      static_cast<std::streamsize>(static_cast<std::uint64_t>(tile_rows) *
                                                   static_cast<std::uint64_t>(cols) *
                                                   sizeof(cv::Vec2d))))
        {
            throw std::runtime_error("Failed to read complex row tile.");
        }
        return tile;
    }

    // 把按列处理得到的复数 tile 写到 scratch 对应列位置。
    void writeComplexColumnTile(std::fstream &ofs, const cv::Mat &tile, int total_cols, int col0)
    {
        CV_Assert(tile.type() == CV_64FC2);
        for (int r = 0; r < tile.rows; ++r)
        {
            checkedSeekp(ofs, complexOffsetBytes(r, col0, total_cols));
            if (!ofs.write(reinterpret_cast<const char *>(tile.ptr<cv::Vec2d>(r)),
                           static_cast<std::streamsize>(tile.cols * sizeof(cv::Vec2d))))
            {
                throw std::runtime_error("Failed to write complex column tile.");
            }
        }
    }

    // 把按行处理得到的复数 tile 顺序写回 scratch。
    void writeComplexRowTile(std::fstream &ofs, const cv::Mat &tile, int total_cols, int row0)
    {
        CV_Assert(tile.type() == CV_64FC2);
        checkedSeekp(ofs, complexOffsetBytes(row0, 0, total_cols));
        if (!ofs.write(reinterpret_cast<const char *>(tile.ptr<cv::Vec2d>(0)),
                       static_cast<std::streamsize>(static_cast<std::uint64_t>(tile.rows) *
                                                    static_cast<std::uint64_t>(tile.cols) *
                                                    sizeof(cv::Vec2d))))
        {
            throw std::runtime_error("Failed to write complex row tile.");
        }
    }

    // 把幅度结果写入单独的 magnitude scratch，供最终归一化输出阶段使用。
    void writeMagnitudeRowTile(std::fstream &ofs, const cv::Mat &mag_tile, int total_cols, int row0)
    {
        CV_Assert(mag_tile.type() == CV_64FC1);
        checkedSeekp(ofs, doubleOffsetBytes(row0, 0, total_cols));
        if (!ofs.write(reinterpret_cast<const char *>(mag_tile.ptr<double>(0)),
                       static_cast<std::streamsize>(static_cast<std::uint64_t>(mag_tile.rows) *
                                                    static_cast<std::uint64_t>(mag_tile.cols) *
                                                    sizeof(double))))
        {
            throw std::runtime_error("Failed to write magnitude row tile.");
        }
    }

    // 从 magnitude scratch 中按行块回读，用于最终 PNG 归一化落盘。
    cv::Mat readMagnitudeRowTile(const fs::path &path, int cols, int row0, int tile_rows)
    {
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs)
        {
            throw std::runtime_error("Failed to open magnitude scratch: " + path.string());
        }

        cv::Mat tile(tile_rows, cols, CV_64FC1);
        checkedSeekg(ifs, doubleOffsetBytes(row0, 0, cols));
        if (!ifs.read(reinterpret_cast<char *>(tile.ptr<double>(0)),
                      static_cast<std::streamsize>(static_cast<std::uint64_t>(tile_rows) *
                                                   static_cast<std::uint64_t>(cols) *
                                                   sizeof(double))))
        {
            throw std::runtime_error("Failed to read magnitude row tile.");
        }
        return tile;
    }

    // 以“可随机读写、创建即清空”的方式打开 scratch 文件。
    std::fstream openScratchForRandomWrite(const fs::path &path)
    {
        std::fstream stream(path, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
        if (!stream)
        {
            throw std::runtime_error("Failed to create scratch file: " + path.string());
        }
        return stream;
    }

    // 将距离向匹配滤波器逐行乘到 tile 上。
    void multiplyRowsByFilterInPlace(cv::Mat &tile, const std::vector<cv::Vec2d> &filter)
    {
        CV_Assert(tile.type() == CV_64FC2);
        CV_Assert(static_cast<int>(filter.size()) >= tile.rows);
        for (int r = 0; r < tile.rows; ++r)
        {
            auto *row = tile.ptr<cv::Vec2d>(r);
            for (int c = 0; c < tile.cols; ++c)
            {
                row[c] = complexMultiply(row[c], filter[r]);
            }
        }
    }

    // 将方位向滤波器逐列乘到 tile 上。
    void multiplyColsByFilterInPlace(cv::Mat &tile, const std::vector<cv::Vec2d> &filter)
    {
        CV_Assert(tile.type() == CV_64FC2);
        CV_Assert(static_cast<int>(filter.size()) == tile.cols);
        for (int r = 0; r < tile.rows; ++r)
        {
            auto *row = tile.ptr<cv::Vec2d>(r);
            for (int c = 0; c < tile.cols; ++c)
            {
                row[c] = complexMultiply(row[c], filter[c]);
            }
        }
    }

    // float32 版本距离向逐行滤波。
    void multiplyRowsByFilterInPlaceF32(cv::Mat &tile, const std::vector<cv::Vec2f> &filter)
    {
        CV_Assert(tile.type() == CV_32FC2);
        CV_Assert(static_cast<int>(filter.size()) >= tile.rows);
        for (int r = 0; r < tile.rows; ++r)
        {
            auto *row = tile.ptr<cv::Vec2f>(r);
            for (int c = 0; c < tile.cols; ++c)
            {
                row[c] = complexMultiply(row[c], filter[r]);
            }
        }
    }

    // float32 版本方位向逐列滤波。
    void multiplyColsByFilterInPlaceF32(cv::Mat &tile, const std::vector<cv::Vec2f> &filter)
    {
        CV_Assert(tile.type() == CV_32FC2);
        CV_Assert(static_cast<int>(filter.size()) == tile.cols);
        for (int r = 0; r < tile.rows; ++r)
        {
            auto *row = tile.ptr<cv::Vec2f>(r);
            for (int c = 0; c < tile.cols; ++c)
            {
                row[c] = complexMultiply(row[c], filter[c]);
            }
        }
    }

    // 根据方位频率轴计算每一列需要补偿的距离徙动样本偏移量。
    std::vector<double> makeRcmcShifts(const std::vector<double> &f_a, const RadarConfig &cfg)
    {
        const double delta_r = cfg.c / (2.0 * cfg.Fs);
        const double v2 = cfg.v_platform * cfg.v_platform;
        const double lam = cfg.lam();

        std::vector<double> shifts(f_a.size(), 0.0);
        for (size_t c = 0; c < f_a.size(); ++c)
        {
            const double delta_R = cfg.R0 * std::pow(lam * f_a[c], 2.0) / (8.0 * v2);
            shifts[c] = delta_R / delta_r;
        }
        return shifts;
    }

    // 对一个行窗口执行 RCMC 插值，并只输出本 tile 负责的行段。
    // 之所以额外传 input_row0/output_row0，是因为输入窗口通常包含“超出本 tile 的补边”。
    cv::Mat rcmcRowTile(const cv::Mat &input_window,
                        int input_row0,
                        int output_row0,
                        int output_rows,
                        int total_rows,
                        const std::vector<double> &shifts)
    {
        CV_Assert(input_window.type() == CV_64FC2);
        const int cols = input_window.cols;
        cv::Mat output = cv::Mat::zeros(output_rows, cols, CV_64FC2);

        for (int local_r = 0; local_r < output_rows; ++local_r)
        {
            const int global_r = output_row0 + local_r;
            auto *dst_row = output.ptr<cv::Vec2d>(local_r);
            for (int c = 0; c < cols; ++c)
            {
                const double map_r = static_cast<double>(global_r) + shifts[c];
                if (map_r < 0.0 || map_r >= static_cast<double>(total_rows - 1))
                {
                    continue;
                }

                const int idx0 = static_cast<int>(std::floor(map_r));
                const double w = map_r - static_cast<double>(idx0);
                const int local_idx0 = idx0 - input_row0;
                const auto *row0 = input_window.ptr<cv::Vec2d>(local_idx0);
                const auto *row1 = input_window.ptr<cv::Vec2d>(local_idx0 + 1);
                const cv::Vec2d v0 = row0[c];
                const cv::Vec2d v1 = row1[c];
                dst_row[c] = cv::Vec2d((1.0 - w) * v0[0] + w * v1[0],
                                       (1.0 - w) * v0[1] + w * v1[1]);
            }
        }
        return output;
    }

    // float32 内存管线的原地 RCMC。
    // 为了避免当前位置被自己覆盖，需要先保存当前行再做插值。
    void rcmcInPlaceF32(cv::Mat &data, const std::vector<double> &shifts)
    {
        CV_Assert(data.type() == CV_32FC2);
        cv::Mat prev_row(1, data.cols, CV_32FC2);

        for (int r = 0; r < data.rows; ++r)
        {
            data.row(r).copyTo(prev_row);
            auto *dst_row = data.ptr<cv::Vec2f>(r);
            const auto *saved_row = prev_row.ptr<cv::Vec2f>(0);

            for (int c = 0; c < data.cols; ++c)
            {
                const double map_r = static_cast<double>(r) + shifts[c];
                if (map_r < 0.0 || map_r >= static_cast<double>(data.rows - 1))
                {
                    dst_row[c] = cv::Vec2f(0.0f, 0.0f);
                    continue;
                }

                const int idx0 = static_cast<int>(std::floor(map_r));
                const float w = static_cast<float>(map_r - static_cast<double>(idx0));
                const cv::Vec2f v0 = (idx0 == r) ? saved_row[c] : data.at<cv::Vec2f>(idx0, c);
                const cv::Vec2f v1 = data.at<cv::Vec2f>(idx0 + 1, c);
                dst_row[c] = cv::Vec2f((1.0f - w) * v0[0] + w * v1[0],
                                       (1.0f - w) * v0[1] + w * v1[1]);
            }
        }
    }

    // 收集待处理 echo 文件列表。
    // 输入既可以是单个 bin 文件，也可以是包含多个 bin 的目录。
    std::vector<fs::path> collectEchoBins(const AppConfig &cfg)
    {
        if (!fs::exists(cfg.echo_dir))
        {
            throw std::runtime_error("Echo path does not exist: " + cfg.echo_dir.string());
        }
        if (fs::is_regular_file(cfg.echo_dir))
        {
            if (shared::ToLower(cfg.echo_dir.extension().string()) != cfg.echo_ext)
            {
                throw std::runtime_error("Selected echo file does not match configured extension: " + cfg.echo_dir.string());
            }
            return {cfg.echo_dir};
        }
        if (!fs::is_directory(cfg.echo_dir))
        {
            throw std::runtime_error("Echo directory does not exist: " + cfg.echo_dir.string());
        }

        std::vector<fs::path> files;
        for (const auto &entry : fs::directory_iterator(cfg.echo_dir))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }
            if (shared::ToLower(entry.path().extension().string()) == cfg.echo_ext)
            {
                files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end());
        return files;
    }

    // Scratch 管线的第一阶段：距离压缩。
    // 读取 echo 的列 tile，沿距离向做 FFT -> 匹配滤波 -> IFFT，并写入 stage_a。
    void runRangeCompression(const fs::path &echo_path,
                             const fs::path &stage_a,
                             const EchoShape &shape,
                             const std::vector<cv::Vec2d> &Hr,
                             int column_tile,
                             shared::WorkflowRunControl *control = nullptr)
    {
        logLine("[stage] range compression -> " + stage_a.string());
        auto out = openScratchForRandomWrite(stage_a);
        for (int col0 = 0; col0 < shape.cols; col0 += column_tile)
        {
            checkControl(control);
            const int tile_cols = std::min(column_tile, shape.cols - col0);
            cv::Mat tile = readEchoColumnTile(echo_path, shape.rows, shape.cols, col0, tile_cols);
            tile = fftshift(tile, 0);
            tile = dftAxis(tile, 0, false);
            tile = fftshift(tile, 0);
            multiplyRowsByFilterInPlace(tile, Hr);
            tile = dftAxis(tile, 0, true);
            writeComplexColumnTile(out, tile, shape.cols, col0);
        }
    }

    // 与 `runRangeCompression` 等价，但把结果直接拼到内存矩阵里，避免 stage_a scratch。
    cv::Mat runRangeCompressionToMemory(const fs::path &echo_path,
                                        const EchoShape &shape,
                                        const std::vector<cv::Vec2d> &Hr,
                                        int column_tile,
                                        shared::WorkflowRunControl *control = nullptr)
    {
        logLine("[stage] range compression -> memory");
        cv::Mat data_rc(shape.rows, shape.cols, CV_64FC2);
        for (int col0 = 0; col0 < shape.cols; col0 += column_tile)
        {
            checkControl(control);
            const int tile_cols = std::min(column_tile, shape.cols - col0);
            cv::Mat tile = readEchoColumnTile(echo_path, shape.rows, shape.cols, col0, tile_cols);
            tile = fftshift(tile, 0);
            tile = dftAxis(tile, 0, false);
            tile = fftshift(tile, 0);
            multiplyRowsByFilterInPlace(tile, Hr);
            tile = dftAxis(tile, 0, true);

            for (int r = 0; r < shape.rows; ++r)
            {
                std::memcpy(data_rc.ptr<cv::Vec2d>(r) + col0,
                            tile.ptr<cv::Vec2d>(r),
                            static_cast<size_t>(tile_cols) * sizeof(cv::Vec2d));
            }
        }
        return data_rc;
    }

    // Scratch 管线第二阶段：对 stage_a 做方位向 FFT，结果写入 stage_b。
    void runAzimuthFft(const fs::path &stage_a,
                       const fs::path &stage_b,
                       const EchoShape &shape,
                       int row_tile,
                       shared::WorkflowRunControl *control = nullptr)
    {
        logLine("[stage] azimuth FFT -> " + stage_b.string());
        auto out = openScratchForRandomWrite(stage_b);
        for (int row0 = 0; row0 < shape.rows; row0 += row_tile)
        {
            checkControl(control);
            const int tile_rows = std::min(row_tile, shape.rows - row0);
            cv::Mat tile = readComplexRowTile(stage_a, shape.cols, row0, tile_rows);
            tile = fftshift(tile, 1);
            tile = dftAxis(tile, 1, false);
            tile = fftshift(tile, 1);
            writeComplexRowTile(out, tile, shape.cols, row0);
        }
    }

    // 将 RCMC 单独拆成 scratch 阶段的版本。
    // 当前主路径已经更多使用 fused 版本，但保留这个实现便于理解和调试。
    void runRcmc(const fs::path &stage_b,
                 const fs::path &stage_a,
                 const EchoShape &shape,
                 const std::vector<double> &f_a,
                 const RadarConfig &radar,
                 int row_tile,
                 shared::WorkflowRunControl *control = nullptr)
    {
        logLine("[stage] RCMC -> " + stage_a.string());
        auto out = openScratchForRandomWrite(stage_a);
        const auto shifts = makeRcmcShifts(f_a, radar);
        const auto max_shift_it = std::max_element(shifts.begin(), shifts.end());
        const double max_shift = max_shift_it == shifts.end() ? 0.0 : *max_shift_it;
        logLine("RCMC row-tiled mode, max range shift=" + std::to_string(max_shift) + " sample(s)");

        for (int row0 = 0; row0 < shape.rows; row0 += row_tile)
        {
            checkControl(control);
            const int tile_rows = std::min(row_tile, shape.rows - row0);
            const int input_row0 = row0;
            const int input_row1 = std::min(shape.rows - 1,
                                            row0 + tile_rows - 1 + static_cast<int>(std::ceil(max_shift)) + 1);
            const int input_rows = input_row1 - input_row0 + 1;
            cv::Mat input_window = readComplexRowTile(stage_b, shape.cols, input_row0, input_rows);
            cv::Mat corrected = rcmcRowTile(input_window, input_row0, row0, tile_rows, shape.rows, shifts);
            writeComplexRowTile(out, corrected, shape.cols, row0);
        }
    }

    // 将方位压缩、IFFT 和幅度计算串起来，并把结果写成 magnitude scratch。
    // 返回全局 min/max，供最终 PNG 归一化使用。
    std::pair<double, double> runAzimuthCompressionAndMagnitude(const fs::path &stage_a,
                                                                const fs::path &magnitude_path,
                                                                const EchoShape &shape,
                                                                const std::vector<cv::Vec2d> &Ha,
                                                                int row_tile,
                                                                shared::WorkflowRunControl *control = nullptr)
    {
        logLine("[stage] azimuth compression + IFFT -> magnitude scratch");
        auto out = openScratchForRandomWrite(magnitude_path);
        double min_val = std::numeric_limits<double>::infinity();
        double max_val = -std::numeric_limits<double>::infinity();

        for (int row0 = 0; row0 < shape.rows; row0 += row_tile)
        {
            checkControl(control);
            const int tile_rows = std::min(row_tile, shape.rows - row0);
            cv::Mat tile = readComplexRowTile(stage_a, shape.cols, row0, tile_rows);
            multiplyColsByFilterInPlace(tile, Ha);
            tile = ifftshift(tile, 1);
            tile = dftAxis(tile, 1, true);

            cv::Mat mag(tile.rows, tile.cols, CV_64FC1);
            for (int r = 0; r < tile.rows; ++r)
            {
                const auto *src_row = tile.ptr<cv::Vec2d>(r);
                auto *mag_row = mag.ptr<double>(r);
                for (int c = 0; c < tile.cols; ++c)
                {
                    const double value = std::hypot(src_row[c][0], src_row[c][1]);
                    mag_row[c] = value;
                    min_val = std::min(min_val, value);
                    max_val = std::max(max_val, value);
                }
            }
            writeMagnitudeRowTile(out, mag, shape.cols, row0);
        }

        return {min_val, max_val};
    }

    // 当前 scratch 主路径使用的融合阶段：
    // 读取方位频域数据 -> 对当前 row tile 做 RCMC -> 方位压缩 -> IFFT -> 幅度提取。
    // 这样可以少落一次中间 scratch，并顺手统计全图幅度范围。
    std::pair<double, double> runFusedRcmcAzimuthCompressionAndMagnitude(const fs::path &stage_b,
                                                                         const fs::path &magnitude_path,
                                                                         const EchoShape &shape,
                                                                         const std::vector<double> &f_a,
                                                                         const std::vector<cv::Vec2d> &Ha,
                                                                         const RadarConfig &radar,
                                                                         int row_tile,
                                                                         shared::WorkflowRunControl *control = nullptr)
    {
        logLine("[stage] fused RCMC + azimuth compression + IFFT -> magnitude scratch");
        auto out = openScratchForRandomWrite(magnitude_path);
        const auto shifts = makeRcmcShifts(f_a, radar);
        const auto max_shift_it = std::max_element(shifts.begin(), shifts.end());
        const double max_shift = max_shift_it == shifts.end() ? 0.0 : *max_shift_it;
        logLine("RCMC fused row-tiled mode, max range shift=" + std::to_string(max_shift) + " sample(s)");

        double min_val = std::numeric_limits<double>::infinity();
        double max_val = -std::numeric_limits<double>::infinity();

        for (int row0 = 0; row0 < shape.rows; row0 += row_tile)
        {
            checkControl(control);
            const int tile_rows = std::min(row_tile, shape.rows - row0);
            const int input_row0 = row0;
            const int input_row1 = std::min(shape.rows - 1,
                                            row0 + tile_rows - 1 + static_cast<int>(std::ceil(max_shift)) + 1);
            const int input_rows = input_row1 - input_row0 + 1;

            cv::Mat input_window = readComplexRowTile(stage_b, shape.cols, input_row0, input_rows);
            cv::Mat tile = rcmcRowTile(input_window, input_row0, row0, tile_rows, shape.rows, shifts);
            input_window.release();

            multiplyColsByFilterInPlace(tile, Ha);
            tile = ifftshift(tile, 1);
            tile = dftAxis(tile, 1, true);

            cv::Mat mag(tile.rows, tile.cols, CV_64FC1);
            for (int r = 0; r < tile.rows; ++r)
            {
                const auto *src_row = tile.ptr<cv::Vec2d>(r);
                auto *mag_row = mag.ptr<double>(r);
                for (int c = 0; c < tile.cols; ++c)
                {
                    const double value = std::hypot(src_row[c][0], src_row[c][1]);
                    mag_row[c] = value;
                    min_val = std::min(min_val, value);
                    max_val = std::max(max_val, value);
                }
            }
            writeMagnitudeRowTile(out, mag, shape.cols, row0);
        }

        return {min_val, max_val};
    }

    // double 精度的纯内存管线：
    // 已完成距离压缩的数据直接在内存中走方位 FFT、RCMC、方位压缩和 IFFT。
    cv::Mat runMemoryPipeline(cv::Mat data_rc,
                              const EchoShape &shape,
                              const std::vector<double> &f_a,
                              const std::vector<cv::Vec2d> &Ha,
                              const RadarConfig &radar,
                              shared::WorkflowRunControl *control = nullptr)
    {
        logLine("[stage] azimuth FFT -> memory");
        cv::Mat data_fa = fftshift(data_rc, 1);
        data_rc.release();
        data_fa = dftAxis(data_fa, 1, false);
        data_fa = fftshift(data_fa, 1);

        logLine("[stage] RCMC -> memory");
        const auto shifts = makeRcmcShifts(f_a, radar);
        cv::Mat data_rcmc = cv::Mat::zeros(shape.rows, shape.cols, CV_64FC2);
        for (int r = 0; r < shape.rows; ++r)
        {
            checkControl(control);
            auto *dst_row = data_rcmc.ptr<cv::Vec2d>(r);
            for (int c = 0; c < shape.cols; ++c)
            {
                const double map_r = static_cast<double>(r) + shifts[c];
                if (map_r < 0.0 || map_r >= static_cast<double>(shape.rows - 1))
                {
                    continue;
                }
                const int idx0 = static_cast<int>(std::floor(map_r));
                const double w = map_r - static_cast<double>(idx0);
                const cv::Vec2d v0 = data_fa.at<cv::Vec2d>(idx0, c);
                const cv::Vec2d v1 = data_fa.at<cv::Vec2d>(idx0 + 1, c);
                dst_row[c] = cv::Vec2d((1.0 - w) * v0[0] + w * v1[0],
                                       (1.0 - w) * v0[1] + w * v1[1]);
            }
        }
        data_fa.release();

        logLine("[stage] azimuth compression + IFFT -> memory");
        multiplyColsByFilterInPlace(data_rcmc, Ha);
        data_rcmc = ifftshift(data_rcmc, 1);
        return dftAxis(data_rcmc, 1, true);
    }

    // 直接从复数图像统计幅度范围并归一化成 8-bit PNG。
    void writeNormalizedPngFromComplex(const cv::Mat &complex_img, const fs::path &output_path)
    {
        CV_Assert(complex_img.type() == CV_64FC2);
        logLine("[stage] normalize + write PNG -> " + output_path.string());
        cv::Mat out_img(complex_img.rows, complex_img.cols, CV_8UC1, cv::Scalar(0));

        double min_val = std::numeric_limits<double>::infinity();
        double max_val = -std::numeric_limits<double>::infinity();
        for (int r = 0; r < complex_img.rows; ++r)
        {
            const auto *src_row = complex_img.ptr<cv::Vec2d>(r);
            for (int c = 0; c < complex_img.cols; ++c)
            {
                const double value = std::hypot(src_row[c][0], src_row[c][1]);
                min_val = std::min(min_val, value);
                max_val = std::max(max_val, value);
            }
        }

        if (std::isfinite(min_val) && std::isfinite(max_val) && max_val != min_val)
        {
            const double scale = 255.0 / (max_val - min_val);
            for (int r = 0; r < complex_img.rows; ++r)
            {
                const auto *src_row = complex_img.ptr<cv::Vec2d>(r);
                auto *dst_row = out_img.ptr<std::uint8_t>(r);
                for (int c = 0; c < complex_img.cols; ++c)
                {
                    const double value = std::hypot(src_row[c][0], src_row[c][1]);
                    const double normalized = (value - min_val) * scale;
                    const double clamped = std::max(0.0, std::min(255.0, normalized));
                    dst_row[c] = static_cast<std::uint8_t>(clamped);
                }
            }
        }

        fs::create_directories(output_path.parent_path());
        if (!cv::imwrite(output_path.string(), out_img))
        {
            throw std::runtime_error("Failed to write output PNG: " + output_path.string());
        }
    }

    // float32 复数图像版本的 PNG 归一化输出。
    void writeNormalizedPngFromComplexF32(const cv::Mat &complex_img, const fs::path &output_path)
    {
        CV_Assert(complex_img.type() == CV_32FC2);
        logLine("[stage] normalize + write PNG -> " + output_path.string());
        cv::Mat out_img(complex_img.rows, complex_img.cols, CV_8UC1, cv::Scalar(0));

        float min_val = std::numeric_limits<float>::infinity();
        float max_val = -std::numeric_limits<float>::infinity();
        for (int r = 0; r < complex_img.rows; ++r)
        {
            const auto *src_row = complex_img.ptr<cv::Vec2f>(r);
            for (int c = 0; c < complex_img.cols; ++c)
            {
                const float value = std::hypot(src_row[c][0], src_row[c][1]);
                min_val = std::min(min_val, value);
                max_val = std::max(max_val, value);
            }
        }

        if (std::isfinite(min_val) && std::isfinite(max_val) && max_val != min_val)
        {
            const float scale = 255.0f / (max_val - min_val);
            for (int r = 0; r < complex_img.rows; ++r)
            {
                const auto *src_row = complex_img.ptr<cv::Vec2f>(r);
                auto *dst_row = out_img.ptr<std::uint8_t>(r);
                for (int c = 0; c < complex_img.cols; ++c)
                {
                    const float value = std::hypot(src_row[c][0], src_row[c][1]);
                    const float normalized = (value - min_val) * scale;
                    const float clamped = std::max(0.0f, std::min(255.0f, normalized));
                    dst_row[c] = static_cast<std::uint8_t>(clamped);
                }
            }
        }

        fs::create_directories(output_path.parent_path());
        if (!cv::imwrite(output_path.string(), out_img))
        {
            throw std::runtime_error("Failed to write output PNG: " + output_path.string());
        }
    }

    // 从 magnitude scratch 逐 tile 回读，按全局 min/max 归一化成最终 PNG。
    void writeNormalizedPng(const fs::path &magnitude_path,
                            const fs::path &output_path,
                            const EchoShape &shape,
                            double min_val,
                            double max_val,
                            int row_tile,
                            shared::WorkflowRunControl *control = nullptr)
    {
        logLine("[stage] normalize + write PNG -> " + output_path.string());
        cv::Mat out_img(shape.rows, shape.cols, CV_8UC1, cv::Scalar(0));

        if (std::isfinite(min_val) && std::isfinite(max_val) && max_val != min_val)
        {
            const double scale = 255.0 / (max_val - min_val);
            for (int row0 = 0; row0 < shape.rows; row0 += row_tile)
            {
                checkControl(control);
                const int tile_rows = std::min(row_tile, shape.rows - row0);
                cv::Mat mag = readMagnitudeRowTile(magnitude_path, shape.cols, row0, tile_rows);
                for (int r = 0; r < tile_rows; ++r)
                {
                    const auto *mag_row = mag.ptr<double>(r);
                    auto *dst_row = out_img.ptr<std::uint8_t>(row0 + r);
                    for (int c = 0; c < shape.cols; ++c)
                    {
                        const double normalized = (mag_row[c] - min_val) * scale;
                        const double clamped = std::max(0.0, std::min(255.0, normalized));
                        dst_row[c] = static_cast<std::uint8_t>(clamped);
                    }
                }
            }
        }

        fs::create_directories(output_path.parent_path());
        if (!cv::imwrite(output_path.string(), out_img))
        {
            throw std::runtime_error("Failed to write output PNG: " + output_path.string());
        }
    }

    // 板端优先路径：在 float32 内存里完成整条 RD 管线，尽量压低峰值内存与 scratch I/O。
    void runMemoryFloat32Pipeline(const fs::path &echo_path,
                                  const fs::path &output_path,
                                  const EchoShape &shape,
                                  const std::vector<double> &f_r,
                                  const std::vector<double> &f_a,
                                  const RadarConfig &radar,
                                  int column_tile,
                                  int row_tile,
                                  shared::WorkflowRunControl *control = nullptr)
    {
        logLine("[stage] load echo -> CV_32FC2 memory");
        cv::Mat data = loadEchoF32(echo_path, shape);

        const auto Hr = makeComplexExponentialF32(f_r, -CV_PI / radar.gamma());
        const double Ka = 2.0 * radar.v_platform * radar.v_platform / (radar.lam() * radar.R0);
        const auto Ha = makeComplexExponentialF32(f_a, -CV_PI / Ka);

        logLine("[stage] range compression -> memory_float32");
        for (int col0 = 0; col0 < shape.cols; col0 += column_tile)
        {
            checkControl(control);
            const int tile_cols = std::min(column_tile, shape.cols - col0);
            cv::Mat tile = gatherColumnTileF32(data, col0, tile_cols);
            tile = fftshiftF32(tile, 0);
            tile = dftAxisF32(tile, 0, false);
            tile = fftshiftF32(tile, 0);
            multiplyRowsByFilterInPlaceF32(tile, Hr);
            tile = dftAxisF32(tile, 0, true);
            scatterColumnTileF32(tile, data, col0);
        }

        logLine("[stage] azimuth FFT -> memory_float32");
        for (int row0 = 0; row0 < shape.rows; row0 += row_tile)
        {
            checkControl(control);
            const int tile_rows = std::min(row_tile, shape.rows - row0);
            cv::Mat tile = data.rowRange(row0, row0 + tile_rows).clone();
            tile = fftshiftF32(tile, 1);
            tile = dftAxisF32(tile, 1, false);
            tile = fftshiftF32(tile, 1);
            tile.copyTo(data.rowRange(row0, row0 + tile_rows));
        }

        logLine("[stage] RCMC -> memory_float32 in-place");
        const auto shifts = makeRcmcShifts(f_a, radar);
        rcmcInPlaceF32(data, shifts);

        logLine("[stage] azimuth compression + IFFT -> memory_float32");
        for (int row0 = 0; row0 < shape.rows; row0 += row_tile)
        {
            checkControl(control);
            const int tile_rows = std::min(row_tile, shape.rows - row0);
            cv::Mat tile = data.rowRange(row0, row0 + tile_rows).clone();
            multiplyColsByFilterInPlaceF32(tile, Ha);
            tile = ifftshiftF32(tile, 1);
            tile = dftAxisF32(tile, 1, true);
            tile.copyTo(data.rowRange(row0, row0 + tile_rows));
        }

        writeNormalizedPngFromComplexF32(data, output_path);
    }

    // 处理单个 echo bin 的总控函数。
    // 它负责：输入校验、输出命名、scratch 生命周期、管线模式选择以及阶段快照发布。
    void processOneEcho(const fs::path &echo_path,
                        const AppConfig &cfg,
                        shared::WorkflowRunControl *control = nullptr,
                        int current_index = 0,
                        int total_count = 0)
    {
        const auto start = std::chrono::steady_clock::now();
        logLine("Processing echo: " + echo_path.string());
        publishSnapshot(control, cfg, shared::ControlState::Running, "process echo", echo_path.string(), current_index, total_count);

        const EchoShape shape = readEchoShapeAndValidate(echo_path);
        logLine("Echo shape: " + std::to_string(shape.rows) + "x" + std::to_string(shape.cols));

        const fs::path output_path = cfg.output_dir / (echo_path.stem().string() + ".png");
        if (fs::exists(output_path) && !cfg.overwrite)
        {
            logLine("Skip existing output: " + output_path.string());
            publishSnapshot(control, cfg, shared::ControlState::Running, "skip existing output", output_path.string(), current_index, total_count);
            return;
        }

        const fs::path scratch_root = cfg.scratch_dir / echo_path.stem();
        fs::create_directories(scratch_root);
        const fs::path stage_a = scratch_root / "stage_a.bin";
        const fs::path stage_b = scratch_root / "stage_b.bin";
        const fs::path magnitude_path = scratch_root / "magnitude.bin";
        const std::uint64_t complex_bytes = static_cast<std::uint64_t>(shape.rows) *
                                            static_cast<std::uint64_t>(shape.cols) *
                                            sizeof(cv::Vec2d);
        const std::uint64_t complex_f32_bytes = static_cast<std::uint64_t>(shape.rows) *
                                                static_cast<std::uint64_t>(shape.cols) *
                                                sizeof(cv::Vec2f);
        const std::uint64_t estimated_memory_peak = complex_bytes * 4ull;
        const std::uint64_t f32_memory_peak = complex_f32_bytes +
                                              std::max<std::uint64_t>(
                                                  static_cast<std::uint64_t>(shape.rows) *
                                                      static_cast<std::uint64_t>(cfg.column_tile) *
                                                      sizeof(cv::Vec2f) * 4ull,
                                                  static_cast<std::uint64_t>(cfg.row_tile) *
                                                      static_cast<std::uint64_t>(shape.cols) *
                                                      sizeof(cv::Vec2f) * 4ull) +
                                              static_cast<std::uint64_t>(shape.rows) *
                                                  static_cast<std::uint64_t>(shape.cols);
        const bool use_memory_float32 =
            cfg.execution_mode == "memory_float32" ||
            (cfg.execution_mode == "auto" &&
             f32_memory_peak <= static_cast<std::uint64_t>(cfg.memory_limit_mb) * 1024ull * 1024ull);
        const bool use_memory_pipeline =
            cfg.execution_mode != "scratch_double" &&
            cfg.prefer_memory_pipeline &&
            estimated_memory_peak <= static_cast<std::uint64_t>(cfg.memory_limit_mb) * 1024ull * 1024ull;
        logLine(std::string("RD pipeline mode: ") +
                (use_memory_float32 ? "memory_float32" : (use_memory_pipeline ? "memory_double" : "scratch_double")) +
                ", one complex image=" + std::to_string(complex_bytes / 1024ull / 1024ull) +
                " MiB, f32 estimate=" + std::to_string(f32_memory_peak / 1024ull / 1024ull) +
                " MiB, double estimate=" + std::to_string(estimated_memory_peak / 1024ull / 1024ull) + " MiB");

        try
        {
            const RadarConfig radar;
            const auto f_r = fftfreqShifted(shape.rows, 1.0 / radar.Fs);
            const auto f_a = fftfreqShifted(shape.cols, 1.0 / radar.PRF);
            const auto Hr = makeComplexExponential(f_r, -CV_PI / radar.gamma());
            const double Ka = 2.0 * radar.v_platform * radar.v_platform / (radar.lam() * radar.R0);
            const auto Ha = makeComplexExponential(f_a, -CV_PI / Ka);

            if (use_memory_float32)
            {
                publishSnapshot(control, cfg, shared::ControlState::Running, "memory_float32 pipeline", echo_path.string(), current_index, total_count);
                runMemoryFloat32Pipeline(echo_path, output_path, shape, f_r, f_a, radar, cfg.column_tile, cfg.row_tile, control);
            }
            else if (use_memory_pipeline)
            {
                publishSnapshot(control, cfg, shared::ControlState::Running, "memory pipeline", echo_path.string(), current_index, total_count);
                cv::Mat data_rc = runRangeCompressionToMemory(echo_path, shape, Hr, cfg.column_tile, control);
                cv::Mat sar_complex = runMemoryPipeline(std::move(data_rc), shape, f_a, Ha, radar, control);
                writeNormalizedPngFromComplex(sar_complex, output_path);
            }
            else
            {
                publishSnapshot(control, cfg, shared::ControlState::Running, "scratch range compression", echo_path.string(), current_index, total_count);
                runRangeCompression(echo_path, stage_a, shape, Hr, cfg.column_tile, control);
                publishSnapshot(control, cfg, shared::ControlState::Running, "scratch azimuth fft", echo_path.string(), current_index, total_count);
                runAzimuthFft(stage_a, stage_b, shape, cfg.row_tile, control);

                if (!cfg.keep_scratch)
                {
                    std::error_code ec;
                    fs::remove(stage_a, ec);
                }

                const auto [min_val, max_val] =
                    runFusedRcmcAzimuthCompressionAndMagnitude(stage_b,
                                                               magnitude_path,
                                                               shape,
                                                               f_a,
                                                               Ha,
                                                               radar,
                                                               cfg.row_tile,
                                                               control);
                logLine("Magnitude min=" + std::to_string(min_val) + ", max=" + std::to_string(max_val));
                if (!cfg.keep_scratch)
                {
                    std::error_code ec;
                    fs::remove(stage_b, ec);
                }
                writeNormalizedPng(magnitude_path, output_path, shape, min_val, max_val, cfg.row_tile, control);
            }

            if (!cfg.keep_scratch)
            {
                std::error_code ec;
                fs::remove_all(scratch_root, ec);
                if (ec)
                {
                    logLine("Warning: failed to remove scratch dir: " + scratch_root.string() + ", " + ec.message());
                }
            }
        }
        catch (...)
        {
            if (!cfg.keep_scratch)
            {
                std::error_code ec;
                fs::remove_all(scratch_root, ec);
            }
            throw;
        }

        const auto end = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(end - start).count();
        logLine("Saved: " + output_path.string() + ", elapsed=" + std::to_string(seconds) + "s");
        publishSnapshot(control, cfg, shared::ControlState::Running, "echo complete", output_path.string(), current_index, total_count);
    }
    }

// 从配置文件路径启动 RD workflow 的便捷入口。
int Run(const std::filesystem::path &config_path)
{
    return Run(LoadConfig(config_path), nullptr);
}

// RD workflow 顶层入口。
// 顶层只负责编排文件列表和失败统计；单个文件内部失败会被记录并继续处理后续文件。
int Run(const AppConfig &cfg, shared::WorkflowRunControl *control)
{
    try
    {
        fs::create_directories(cfg.output_dir);
        fs::create_directories(cfg.scratch_dir);
        publishSnapshot(control, cfg, shared::ControlState::Starting, "load config", cfg.echo_dir.string(), 0, 0);

        logLine("Config loaded: echo_dir=" + cfg.echo_dir.string() +
                ", output_dir=" + cfg.output_dir.string() +
                ", scratch_dir=" + cfg.scratch_dir.string());

        const auto files = collectEchoBins(cfg);
        if (files.empty())
        {
            throw std::runtime_error("No echo bin files found in: " + cfg.echo_dir.string());
        }
        logLine("Found " + std::to_string(files.size()) + " echo bin file(s).");
        publishSnapshot(control, cfg, shared::ControlState::Running, "collect echo bins", cfg.echo_dir.string(), 0, static_cast<int>(files.size()));

        int failed = 0;
        bool stop_requested = false;
        for (size_t i = 0; i < files.size(); ++i)
        {
            checkControl(control);
            logLine("========== [" + std::to_string(i + 1) + "/" + std::to_string(files.size()) + "] ==========");
            try
            {
                processOneEcho(files[i], cfg, control, static_cast<int>(i + 1), static_cast<int>(files.size()));
            }
            catch (const StopRequested &)
            {
                stop_requested = true;
                logLine("Stop requested. Finishing RD workflow at safe point.");
                break;
            }
            catch (const std::exception &e)
            {
                ++failed;
                logLine(std::string("Error: ") + e.what());
                publishSnapshot(control,
                                cfg,
                                shared::ControlState::Running,
                                "echo error",
                                files[i].string(),
                                static_cast<int>(i + 1),
                                static_cast<int>(files.size()),
                                e.what());
            }
        }

        if (stop_requested)
        {
            publishSnapshot(control, cfg, shared::ControlState::Idle, "stopped", cfg.echo_dir.string(), 0, static_cast<int>(files.size()));
            return 0;
        }
        if (failed > 0)
        {
            logLine("Finished with " + std::to_string(failed) + " failed file(s).");
            publishSnapshot(control,
                            cfg,
                            shared::ControlState::Error,
                            "finished with errors",
                            cfg.echo_dir.string(),
                            failed,
                            static_cast<int>(files.size()),
                            std::to_string(failed) + " file(s) failed");
            return 2;
        }
        logLine("All echo files processed successfully.");
        publishSnapshot(control, cfg, shared::ControlState::Finished, "finished", cfg.echo_dir.string(), static_cast<int>(files.size()), static_cast<int>(files.size()));
        return 0;
    }
    catch (const StopRequested &)
    {
        publishSnapshot(control, cfg, shared::ControlState::Idle, "stopped", cfg.echo_dir.string(), 0, 0);
        return 0;
    }
    catch (const std::exception &e)
    {
        publishSnapshot(control, cfg, shared::ControlState::Error, "error", cfg.echo_dir.string(), 0, 0, e.what());
        std::cerr << "rd_imaging_stream failed: " << e.what() << std::endl;
        return 1;
    }
}
}
