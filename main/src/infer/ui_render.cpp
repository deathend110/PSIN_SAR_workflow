#include "workflow/infer/infer_config.hpp"
#include "workflow/infer/infer_workflow_internal.hpp"
#include "workflow/infer/manual_flight_runtime.hpp"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <sstream>
#include <utility>
#include <vector>

namespace workflow::infer
{
    namespace
    {
        struct StatusBadgeStyle
        {
            cv::Scalar dot_color;
            cv::Scalar text_color;
        };

        bool isOperationalStatusLabel(const std::string &status_label)
        {
            return status_label == "PAUSED" ||
                   status_label == "EDGE HOLD" ||
                   status_label == "RUNNING";
        }

        StatusBadgeStyle resolveStatusBadgeStyle(const std::string &status_label)
        {
            const cv::Scalar running_dot_color(94, 197, 34);
            const cv::Scalar running_text_color(52, 101, 22);
            const cv::Scalar stopped_dot_color(52, 52, 235);
            const cv::Scalar stopped_text_color(36, 36, 182);

            if (status_label == "STOPPED")
            {
                return {stopped_dot_color, stopped_text_color};
            }
            if (isOperationalStatusLabel(status_label))
            {
                return {running_dot_color, running_text_color};
            }
            return {running_dot_color, running_text_color};
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
            static const std::array<const char *, kSegClasses> names = {
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
            for (int cls = 0; cls < kSegClasses; ++cls)
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
    }

    cv::Vec3b classColorBgr(int cls)
    {
        static const cv::Vec3b colors[kSegClasses] = {
            cv::Vec3b(255, 0, 0),
            cv::Vec3b(0, 255, 0),
            cv::Vec3b(0, 0, 255),
            cv::Vec3b(255, 255, 0),
            cv::Vec3b(0, 255, 255),
            cv::Vec3b(255, 0, 255)};
        return colors[std::max(0, std::min(cls, kSegClasses - 1))];
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
        state.manual_last_inferred_x = telemetry.last_inferred_center_x;
        state.manual_last_inferred_y = telemetry.last_inferred_center_y;
        state.manual_path_points = telemetry.path_points;
        state.manual_patch_count = telemetry.patch_count;
        state.manual_edge_blocked = telemetry.edge_blocked;
        state.manual_direction = telemetry.current_direction;
        state.manual_pending_direction = telemetry.pending_direction;
        ui_context.mode_label = "MANUAL CURSOR";
        ui_context.status_label = telemetry.paused ? "PAUSED" : (telemetry.edge_blocked ? "EDGE HOLD" : "RUNNING");
        ui_context.mini_map.path_overlay = telemetry.path_overlay;

        const auto runtime = activeManualFlightRuntimeState();
        if (runtime == nullptr)
        {
            return;
        }
        ui_context.mini_map.path_points = runtime->pathPoints();
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
        const cv::Scalar badge_bg(251, 248, 246);
        const cv::Scalar badge_text_color(85, 65, 51);
        const cv::Scalar patch_color(172, 239, 134);
        const cv::Scalar current_point_color(68, 68, 239);
        const cv::Scalar outer_border_color(175, 163, 154);
        const cv::Scalar inner_frame_border_color(114, 103, 95);
        const cv::Scalar outer_grid_color(148, 138, 130);
        const cv::Scalar divider_color(217, 208, 200);
        const cv::Scalar restore_body_bg(245, 241, 237);
        const cv::Scalar seg_body_bg(234, 227, 221);
        const cv::Scalar seg_grid_color(255, 255, 255);
        const StatusBadgeStyle status_badge_style = resolveStatusBadgeStyle(ui_context.status_label);

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
                           status_badge_style.dot_color,
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
        drawBadge(badge_x, ui_context.status_label, status_badge_style.text_color, true);

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
            telemetry_metrics.push_back({"DIR", state.manual_direction.empty() ? "-" : state.manual_direction});
            telemetry_metrics.push_back({"NEXT_DIR", state.manual_pending_direction.empty() ? "-" : state.manual_pending_direction});
            telemetry_metrics.push_back({"EDGE_BLOCK", state.manual_edge_blocked ? "true" : "false"});
            telemetry_metrics.push_back({"LAST_XY", std::to_string(state.manual_last_inferred_x) + ", " + std::to_string(state.manual_last_inferred_y)});
            telemetry_metrics.push_back({"PATCH_COUNT", std::to_string(state.manual_patch_count)});
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
}
