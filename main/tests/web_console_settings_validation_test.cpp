#include "workflow/web/web_console_controller.hpp"

#include <filesystem>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>

namespace
{
    void Fail(const std::string &message)
    {
        std::cerr << "web_console_settings_validation_test failed: " << message << '\n';
        std::exit(1);
    }

    void Expect(bool condition, const std::string &message)
    {
        if (!condition)
        {
            Fail(message);
        }
    }

    void ExpectContains(const std::string &text, const std::string &needle, const std::string &message)
    {
        if (text.find(needle) == std::string::npos)
        {
            Fail(message + " actual=" + text);
        }
    }

    std::filesystem::path FindRepoFile(const std::filesystem::path &relative_path)
    {
        std::filesystem::path current = std::filesystem::current_path();
        for (int depth = 0; depth < 8; ++depth)
        {
            const std::filesystem::path candidate = current / relative_path;
            if (std::filesystem::exists(candidate))
            {
                return candidate;
            }

            if (!current.has_parent_path())
            {
                break;
            }
            current = current.parent_path();
        }

        Fail("could not locate " + relative_path.generic_string());
        return {};
    }

    std::string ReadAll(const std::filesystem::path &path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            Fail("unable to open " + path.string());
        }

        std::ostringstream buffer;
        buffer << input.rdbuf();
        return buffer.str();
    }

    std::string ReadFileText(const std::filesystem::path &path)
    {
        return ReadAll(path);
    }
}

namespace workflow::infer
{
    namespace
    {
        ManualFlightSettings g_manual_settings;
        ManualFlightTelemetry g_manual_telemetry;
    }

    void ConfigureManualFlight(const ManualFlightSettings &settings)
    {
        g_manual_settings = settings;
        g_manual_telemetry.configured = true;
    }

    void ResetManualFlight()
    {
        g_manual_telemetry = ManualFlightTelemetry{};
        g_manual_telemetry.configured = true;
    }

    void SetManualFlightPaused(bool paused)
    {
        g_manual_telemetry.paused = paused;
    }

    bool SubmitManualFlightKey(const std::string &, bool, std::string *message)
    {
        if (message)
        {
            *message = "stub";
        }
        return true;
    }

    ManualFlightTelemetry GetManualFlightTelemetry()
    {
        return g_manual_telemetry;
    }

    int Run(const std::filesystem::path &)
    {
        return 0;
    }

    int Run(const AppConfig &, shared::WorkflowRunControl *)
    {
        return 0;
    }
}

namespace workflow::rd
{
    int Run(const std::filesystem::path &)
    {
        return 0;
    }

    int Run(const AppConfig &, shared::WorkflowRunControl *)
    {
        return 0;
    }
}

#include "../src/config_utils.cpp"
#include "../src/infer_config.cpp"
#include "../src/rd_config.cpp"
#include "../src/web_console_protocol.cpp"
#include "../src/web_console_controller.cpp"

namespace
{
    workflow::web::WebConsoleController MakeController()
    {
        return workflow::web::WebConsoleController(workflow::infer::AppConfig{}, workflow::rd::AppConfig{});
    }

    void TestRejectInvalidPatchSizeDoesNotMutateState()
    {
        auto controller = MakeController();
        const auto before = controller.inferConfig();

        const std::string response = controller.applySettings({
            {"infer.sys.device", "board-a"},
            {"infer.pipeline.patch.patch_size", "256"},
        });

        ExpectContains(response, "\"ok\":false", "invalid patch_size should fail");
        ExpectContains(response, "\"code\":\"invalid_settings\"", "invalid patch_size should return invalid_settings");

        const auto after = controller.inferConfig();
        Expect(after.device_url == before.device_url, "invalid batch must not partially update infer.sys.device");
        Expect(after.patch_size == before.patch_size, "invalid patch_size must not mutate controller state");
    }

    void TestRejectOverBudgetStrideDoesNotMutateState()
    {
        auto controller = MakeController();
        const auto before = controller.inferConfig();

        const std::string response = controller.applySettings({
            {"infer.pipeline.patch.stride", "32"},
        });

        ExpectContains(response, "\"ok\":false", "over-budget stride should fail");
        ExpectContains(response, "\"code\":\"board_budget_exceeded\"", "small stride should return board_budget_exceeded");

        const auto after = controller.inferConfig();
        Expect(after.stride == before.stride, "over-budget stride must not mutate controller state");
    }

    void TestRejectOverBudgetDisplaySettings()
    {
        auto controller = MakeController();
        const auto before = controller.inferConfig();

        const std::string response = controller.applySettings({
            {"infer.display.width", "1920"},
            {"infer.display.height", "1080"},
            {"infer.display.fps", "60"},
        });

        ExpectContains(response, "\"ok\":false", "over-budget display should fail");
        ExpectContains(response, "\"code\":\"board_budget_exceeded\"", "over-budget display should return board_budget_exceeded");

        const auto after = controller.inferConfig();
        Expect(after.display_width == before.display_width, "failed display batch must not mutate width");
        Expect(after.display_height == before.display_height, "failed display batch must not mutate height");
        Expect(after.display_fps == before.display_fps, "failed display batch must not mutate fps");
    }

    void TestRejectOverBudgetRdMemoryLimit()
    {
        auto controller = MakeController();
        const auto before = controller.rdConfig();

        const std::string response = controller.applySettings({
            {"rd.memory_limit_mb", "640"},
        });

        ExpectContains(response, "\"ok\":false", "over-budget memory limit should fail");
        ExpectContains(response, "\"code\":\"board_budget_exceeded\"", "over-budget memory limit should return board_budget_exceeded");

        const auto after = controller.rdConfig();
        Expect(after.memory_limit_mb == before.memory_limit_mb, "over-budget RD memory limit must not mutate state");
    }

    void TestRejectedBatchDoesNotPersistInvalidValues()
    {
        auto controller = MakeController();

        const std::string response = controller.applySettings({
            {"infer.sys.device", "board-a"},
            {"infer.pipeline.patch.patch_size", "256"},
        });

        ExpectContains(response, "\"code\":\"invalid_settings\"", "invalid batch should still fail before persistence");

        const auto temp_dir = std::filesystem::temp_directory_path() / "psin_task11_settings_validation";
        std::filesystem::create_directories(temp_dir);
        const auto infer_path = temp_dir / "infer_runtime.yaml";
        workflow::infer::SaveConfig(infer_path, controller.inferConfig());
        const std::string persisted = ReadAll(infer_path);

        Expect(persisted.find("device: board-a") == std::string::npos,
               "rejected settings must not leak into persisted infer config");
        ExpectContains(persisted, "patch_size: 512", "rejected batch must keep persisted patch_size at 512");
    }

    void TestAcceptValidBoardBudgetSettings()
    {
        auto controller = MakeController();

        const std::string response = controller.applySettings({
            {"infer.pipeline.patch.patch_size", "512"},
            {"infer.pipeline.patch.stride", "128"},
            {"infer.display.width", "1280"},
            {"infer.display.height", "720"},
            {"infer.display.fps", "30"},
            {"rd.memory_limit_mb", "500"},
        });

        ExpectContains(response, "\"ok\":true", "valid board-budget settings should succeed");

        const auto infer_cfg = controller.inferConfig();
        const auto rd_cfg = controller.rdConfig();
        Expect(infer_cfg.patch_size == 512, "valid patch_size should persist in memory");
        Expect(infer_cfg.stride == 128, "valid stride should persist in memory");
        Expect(infer_cfg.display_width == 1280, "valid display width should persist in memory");
        Expect(infer_cfg.display_height == 720, "valid display height should persist in memory");
        Expect(infer_cfg.display_fps == 30, "valid display fps should persist in memory");
        Expect(rd_cfg.memory_limit_mb == 500, "valid RD memory limit should persist in memory");
    }

    void TestWebConsoleSourcesExposeDebugRasterAndDebugStrideFields()
    {
        const std::string assets_source = ReadFileText(FindRepoFile("main/src/web_console_assets.cpp"));
        const std::string protocol_source = ReadFileText(FindRepoFile("main/src/web_console_protocol.cpp"));

        ExpectContains(assets_source,
                       "\"debug_raster\"",
                       "web console patch mode choices should expose debug_raster");
        ExpectContains(assets_source,
                       "infer.pipeline.debug.stride_x_px",
                       "web console settings page should expose debug stride_x_px");
        ExpectContains(assets_source,
                       "infer.pipeline.debug.stride_y_px",
                       "web console settings page should expose debug stride_y_px");
        ExpectContains(protocol_source,
                       "infer.pipeline.debug.stride_x_px",
                       "settings response should serialize debug stride_x_px");
        ExpectContains(protocol_source,
                       "infer.pipeline.debug.stride_y_px",
                       "settings response should serialize debug stride_y_px");
    }

    void TestDebugRasterSelectionAcceptsPngAndRejectsHdmi()
    {
        auto controller = MakeController();

        const std::string ok_response = controller.applySelection({
            {"patch_mode", "debug_raster"},
            {"output_mode", "png"},
        });

        ExpectContains(ok_response, "\"ok\":true", "debug_raster + png should be accepted");
        Expect(controller.inferConfig().patch_mode == "debug_raster",
               "debug_raster selection should propagate into infer config");
        Expect(controller.inferConfig().output_mode == "png",
               "debug_raster selection should keep png output mode");

        const std::string reject_response = controller.applySelection({
            {"patch_mode", "debug_raster"},
            {"output_mode", "hdmi"},
        });

        ExpectContains(reject_response, "\"ok\":false", "debug_raster + hdmi should be rejected");
        ExpectContains(reject_response,
                       "\"code\":\"invalid_selection\"",
                       "debug_raster + hdmi should fail as an invalid selection");
        ExpectContains(reject_response,
                       "debug_raster requires output_mode=png.",
                       "debug_raster should reject hdmi semantics with an explicit png-only message");
    }
}

int main()
{
    TestRejectInvalidPatchSizeDoesNotMutateState();
    TestRejectOverBudgetStrideDoesNotMutateState();
    TestRejectOverBudgetDisplaySettings();
    TestRejectOverBudgetRdMemoryLimit();
    TestRejectedBatchDoesNotPersistInvalidValues();
    TestAcceptValidBoardBudgetSettings();
    TestWebConsoleSourcesExposeDebugRasterAndDebugStrideFields();
    TestDebugRasterSelectionAcceptsPngAndRejectsHdmi();
    std::cout << "web_console_settings_validation_test passed\n";
    return 0;
}
