#include <opencv2/opencv.hpp>

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// icraft
#include <icraft-xrt/core/session.h>
#include <icraft-xrt/dev/host_device.h>
#include <icraft-xrt/dev/zg330_device.h>
#include <icraft-backends/zg330backend/zg330backend.h>
#include <icraft-backends/hostbackend/backend.h>
#include <icraft-backends/hostbackend/utils.h>

// 这里假设你工程里已经接了 modelzoo_utils / icraft_utils.hpp
// 文档里推荐的 C++ 自定义输入构造接口就是 CvMat2Tensor / Data2Tensor
#include "icraft_utils.hpp"

enum class InputMode {
    RAW_U8,      // 保持 uint8，不做 /255
    FLOAT32_01   // 转 float32，并归一化到 [0,1]
};

struct PatchInfo {
    int index = -1;
    int grid_row = -1;
    int grid_col = -1;
    int x = -1;
    int y = -1;
    int width = 0;
    int height = 0;
    bool right_to_left = false;
};

struct PatchPacket {
    PatchInfo info;
    icraft::xrt::Tensor tensor;  // 可直接 session.forward({tensor})
};

class SnakePatchIcraftFeeder {
public:
    SnakePatchIcraftFeeder(const std::string& image_path,
                           const icraft::xir::NetworkView& network_view,
                           int patch_size = 512,
                           double overlap_ratio = 0.5,
                           InputMode input_mode = InputMode::FLOAT32_01,
                           bool clone_patch = true)
        : network_view_(network_view),
          patch_size_(patch_size),
          overlap_ratio_(overlap_ratio),
          input_mode_(input_mode),
          clone_patch_(clone_patch) {
        if (patch_size_ <= 0) {
            throw std::invalid_argument("patch_size must be > 0");
        }
        if (overlap_ratio_ < 0.0 || overlap_ratio_ >= 1.0) {
            throw std::invalid_argument("overlap_ratio must satisfy 0 <= overlap_ratio < 1");
        }

        image_u8_ = cv::imread(image_path, cv::IMREAD_GRAYSCALE);
        if (image_u8_.empty()) {
            throw std::runtime_error("failed to read image: " + image_path);
        }
        if (image_u8_.type() != CV_8UC1) {
            throw std::runtime_error("input image is not grayscale uint8");
        }
        if (!image_u8_.isContinuous()) {
            image_u8_ = image_u8_.clone();
        }

        stride_ = static_cast<int>(std::lround(patch_size_ * (1.0 - overlap_ratio_)));
        if (stride_ <= 0) {
            throw std::runtime_error("computed stride <= 0");
        }

        if (image_u8_.cols < patch_size_ || image_u8_.rows < patch_size_) {
            rows_ = 0;
            cols_ = 0;
            total_ = 0;
            return;
        }

        // 边缘直接丢弃
        cols_ = (image_u8_.cols - patch_size_) / stride_ + 1;
        rows_ = (image_u8_.rows - patch_size_) / stride_ + 1;
        total_ = rows_ * cols_;
    }

    bool next(PatchPacket& packet) {
        if (cursor_ >= total_) {
            return false;
        }

        const int row = cursor_ / cols_;
        const int order_in_row = cursor_ % cols_;

        const bool rtl = (row % 2 == 1);
        const int col = rtl ? (cols_ - 1 - order_in_row) : order_in_row;

        const int x = col * stride_;
        const int y = row * stride_;

        const cv::Rect roi(x, y, patch_size_, patch_size_);
        cv::Mat patch_u8_view = image_u8_(roi);
        cv::Mat patch = clone_patch_ ? patch_u8_view.clone() : patch_u8_view;

        packet.info.index = cursor_;
        packet.info.grid_row = row;
        packet.info.grid_col = col;
        packet.info.x = x;
        packet.info.y = y;
        packet.info.width = patch_size_;
        packet.info.height = patch_size_;
        packet.info.right_to_left = rtl;

        packet.tensor = buildTensorFromPatch(patch);

        ++cursor_;
        return true;
    }

    void reset() { cursor_ = 0; }

    int totalPatches() const { return total_; }
    int rows() const { return rows_; }
    int cols() const { return cols_; }
    int stride() const { return stride_; }

    int imageWidth() const { return image_u8_.cols; }
    int imageHeight() const { return image_u8_.rows; }

private:
    icraft::xrt::Tensor buildTensorFromPatch(const cv::Mat& patch_u8) const {
        if (patch_u8.empty()) {
            throw std::runtime_error("empty patch");
        }
        if (patch_u8.type() != CV_8UC1) {
            throw std::runtime_error("patch must be CV_8UC1");
        }

        if (input_mode_ == InputMode::RAW_U8) {
            cv::Mat input_mat = patch_u8;
            return CvMat2Tensor(input_mat, network_view_);
        }

        // FLOAT32_01
        cv::Mat patch_f32;
        patch_u8.convertTo(patch_f32, CV_32FC1, 1.0 / 255.0);
        if (!patch_f32.isContinuous()) {
            patch_f32 = patch_f32.clone();
        }
        return CvMat2Tensor(patch_f32, network_view_);
    }

private:
    const icraft::xir::NetworkView& network_view_;

    cv::Mat image_u8_;

    int patch_size_ = 512;
    double overlap_ratio_ = 0.5;
    int stride_ = 256;

    int rows_ = 0;
    int cols_ = 0;
    int total_ = 0;
    int cursor_ = 0;

    InputMode input_mode_ = InputMode::FLOAT32_01;
    bool clone_patch_ = true;
};


#include <iostream>

int main() {
    // 1. 加载网络
    std::string json_path = "./model_ZG.json";
    std::string raw_path  = "./model_ZG.raw";

    auto network = icraft::xir::Network::CreateFromJsonFile(json_path);
    network.lazyLoadParamsFromFile(raw_path);

    // 这里直接取完整视图
    auto net_view = network.view(0);

    // 2. 创建设备
    icraft::xrt::Device host_device = icraft::xrt::HostDevice::Default();
    icraft::xrt::Device zg_device =
        icraft::xrt::Device::Open("axi://zg330aiu?npu=0x40000000&dma=0x80000000");

    // 3. 创建 session
    auto sess = icraft::xrt::Session::Create<
        icraft::backends::ZG330Backend,
        icraft::backends::HostBackend
    >(net_view, {zg_device, host_device});

    sess->apply();

    // 4. 创建 patch feeder
    // 如果你的编译配置/运行时输入是 float32 且需要 /255，就用 FLOAT32_01
    // 如果你的 parse 里 inputs_dtype=UInt8，并且 pre_scale/pre_mean 在图里做了，就用 RAW_U8
    SnakePatchIcraftFeeder feeder(
        "big_gray.png",
        net_view,
        512,
        0.5,
        InputMode::FLOAT32_01,   // 或 InputMode::RAW_U8
        true
    );

    std::cout << "image: " << feeder.imageWidth() << " x " << feeder.imageHeight() << "\n";
    std::cout << "grid : " << feeder.rows() << " x " << feeder.cols()
              << ", total = " << feeder.totalPatches()
              << ", stride = " << feeder.stride() << "\n";

    PatchPacket packet;
    while (feeder.next(packet)) {
        auto outputs = sess->forward({packet.tensor});

        std::cout
            << "patch #" << packet.info.index
            << " row=" << packet.info.grid_row
            << " col=" << packet.info.grid_col
            << " x=" << packet.info.x
            << " y=" << packet.info.y
            << " dir=" << (packet.info.right_to_left ? "R->L" : "L->R")
            << "\n";

        // 后续：
        // auto host_out = outputs[0].to(icraft::xrt::HostDevice::MemRegion());
        // 然后做后处理 / 拼图 / HDMI 显示
    }

    return 0;
}