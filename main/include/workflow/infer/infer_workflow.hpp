#pragma once

#include "workflow/infer/infer_config.hpp"
#include "workflow/shared/run_control.hpp"

#include <filesystem>
#include <string>

namespace workflow::infer
{
    struct ManualFlightSettings
    {
        int manual_step_px = 128;
        int boost_step_px = 256;
        int trigger_distance_px = 128;
        int cache_grid_px = 64;
        bool path_overlay = true;
        std::string control_bindings = "W/A/S/D";
    };

    struct ManualFlightTelemetry
    {
        bool configured = false;
        bool active = false;
        bool paused = false;
        bool path_overlay = true;
        int position_x = 0;
        int position_y = 0;
        int velocity_x = 0;
        int velocity_y = 0;
        int requested_center_x = 0;
        int requested_center_y = 0;
        int last_inferred_center_x = 0;
        int last_inferred_center_y = 0;
        int path_points = 0;
        std::string active_keys;
    };

    void ConfigureManualFlight(const ManualFlightSettings &settings);
    void ResetManualFlight();
    void SetManualFlightPaused(bool paused);
    bool SubmitManualFlightKey(const std::string &key, bool pressed, std::string *message = nullptr);
    ManualFlightTelemetry GetManualFlightTelemetry();

    int Run(const std::filesystem::path &config_path);
    int Run(const AppConfig &config, shared::WorkflowRunControl *control = nullptr);
}
