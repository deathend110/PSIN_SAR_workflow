#pragma once

#include <opencv2/core.hpp>

#include <memory>
#include <string>
#include <vector>

namespace workflow::infer
{
    class ManualFlightRuntimeState;

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
        int manual_last_inferred_x = 0;
        int manual_last_inferred_y = 0;
        int manual_path_points = 0;
        int manual_patch_count = 0;
        bool manual_edge_blocked = false;
        std::string manual_direction;
        std::string manual_pending_direction;
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

    cv::Vec3b classColorBgr(int cls);

    std::shared_ptr<ManualFlightRuntimeState> activeManualFlightRuntimeState();

    MiniMapContext buildMiniMapContext(const cv::Mat &sar_norm, int patch_size, int stride, int rows, int cols);

    void applyManualTelemetry(RuntimeState &state, UiRenderContext &ui_context);

    cv::Mat composeIndustrialUiFrame(const UiRenderContext &ui_context,
                                     const RuntimeState &state,
                                     const cv::Mat &restore_bgr,
                                     const cv::Mat &mask_bgr,
                                     int width,
                                     int height);
}
