#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    void Fail(const std::string &message)
    {
        std::cerr << "infer_workflow_regression_test failed: " << message << '\n';
        std::exit(1);
    }

    void Expect(bool condition, const std::string &message)
    {
        if (!condition)
        {
            Fail(message);
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

        Fail("could not locate " + relative_path.generic_string() + " from " + std::filesystem::current_path().generic_string());
        return {};
    }

    std::string ReadFileText(const std::filesystem::path &path)
    {
        std::ifstream input(path, std::ios::binary);
        Expect(static_cast<bool>(input), "unable to open " + path.generic_string());

        std::ostringstream buffer;
        buffer << input.rdbuf();
        return buffer.str();
    }

    void ExpectContains(const std::string &text, const std::string &needle, const std::string &message)
    {
        Expect(text.find(needle) != std::string::npos, message + " missing=" + needle);
    }

    void ExpectContainsAll(const std::string &text, const std::vector<std::string> &needles, const std::string &message)
    {
        for (const auto &needle : needles)
        {
            ExpectContains(text, needle, message);
        }
    }

    void TestUiRenderHelperWasSplitIntoAnImplementationFile()
    {
        const auto source_path = FindRepoFile("main/src/infer/ui_render.cpp");
        const std::string source = ReadFileText(source_path);

        ExpectContains(source, "composeIndustrialUiFrame", "ui render implementation should own composeIndustrialUiFrame()");
        ExpectContains(source, "resolveStatusBadgeStyle", "ui render implementation should own status badge styling");
    }

    void TestStoppedAndManualTelemetrySemanticsRemainVisible()
    {
        const auto source_path = FindRepoFile("main/src/infer/ui_render.cpp");
        const std::string source = ReadFileText(source_path);

        ExpectContainsAll(source,
                          {"STOPPED", "PAUSED", "EDGE HOLD", "RUNNING"},
                          "ui render implementation should keep the terminal and manual status labels");
        ExpectContains(source,
                       "ui_context.status_label",
                       "ui render implementation should still render from the status label boundary");
        ExpectContains(source,
                       "ui_context.mode_label",
                       "ui render implementation should still render from the mode label boundary");
    }

    void TestManualTelemetryStillMapsIntoObservableModeAndStatusLabels()
    {
        const auto source_path = FindRepoFile("main/src/infer/ui_render.cpp");
        const std::string source = ReadFileText(source_path);

        ExpectContains(source, "applyManualTelemetry(", "manual telemetry helper should still exist");
        ExpectContains(source, "MANUAL CURSOR", "manual telemetry should still expose the manual mode label");
        ExpectContains(source, "telemetry.paused ? \"PAUSED\" : (telemetry.edge_blocked ? \"EDGE HOLD\" : \"RUNNING\")",
                       "manual telemetry should still map pause and edge conditions into explicit status text");
    }

    void TestSnakePatchSourceWasMovedIntoAPatchPlannerImplementationFile()
    {
        const auto planner_path = FindRepoFile("main/src/infer/patch_planner.cpp");
        const std::string planner_source = ReadFileText(planner_path);

        ExpectContains(planner_source,
                       "SnakePatchSource::SnakePatchSource",
                       "patch planner implementation should own SnakePatchSource construction");
        ExpectContains(planner_source,
                       "SnakePatchSource::next(PatchPacket &packet)",
                       "patch planner implementation should still expose the next(PatchPacket&) boundary");
        ExpectContainsAll(planner_source,
                          {"SnakePatchSource::rows() const", "SnakePatchSource::cols() const", "SnakePatchSource::totalPatches() const"},
                          "patch planner implementation should preserve planner size accessors");
        ExpectContains(planner_source,
                       "const bool right_to_left = (row % 2) == 1;",
                       "patch planner implementation should preserve snake traversal direction");
    }

    void TestInferWorkflowNoLongerContainsTheFullSnakePatchSourceImplementation()
    {
        const auto workflow_path = FindRepoFile("main/src/infer_workflow.cpp");
        const std::string workflow_source = ReadFileText(workflow_path);

        Expect(workflow_source.find("class SnakePatchSource") == std::string::npos,
               "infer_workflow.cpp should no longer own the full SnakePatchSource implementation");
        Expect(workflow_source.find("bool next(PatchPacket &packet)") == std::string::npos,
               "infer_workflow.cpp should no longer define SnakePatchSource::next()");
        Expect(workflow_source.find("const bool right_to_left = (row % 2) == 1;") == std::string::npos,
               "infer_workflow.cpp should no longer keep the snake traversal algorithm inline");
    }

    void TestInferWorkflowNoLongerContainsTheLegacyImagingHelpers()
    {
        const auto workflow_path = FindRepoFile("main/src/infer_workflow.cpp");
        const std::string workflow_source = ReadFileText(workflow_path);

        Expect(workflow_source.find("RadarConfig") == std::string::npos,
               "infer_workflow.cpp should no longer keep the legacy radar imaging config helper");
        Expect(workflow_source.find("ComplexImage") == std::string::npos,
               "infer_workflow.cpp should no longer keep the legacy complex image wrapper");
        Expect(workflow_source.find("loadEchoBin(") == std::string::npos,
               "infer_workflow.cpp should no longer define the echo-bin loader helper");
        Expect(workflow_source.find("runImaging(") == std::string::npos,
               "infer_workflow.cpp should no longer define the legacy imaging pipeline helper");
        Expect(workflow_source.find("magnitudeMinMaxToNormF32(") == std::string::npos,
               "infer_workflow.cpp should no longer define the legacy magnitude normalization helper");
        Expect(workflow_source.find("vectorizedRcmc(") == std::string::npos,
               "infer_workflow.cpp should no longer keep the legacy RCMC helper inline");
        Expect(workflow_source.find("makeComplexExponential(") == std::string::npos,
               "infer_workflow.cpp should no longer keep the legacy complex exponential helper inline");
        Expect(workflow_source.find("fftfreqShifted(") == std::string::npos,
               "infer_workflow.cpp should no longer keep the legacy frequency grid helper inline");
        Expect(workflow_source.find("readLittleEndianInt32(") == std::string::npos,
               "infer_workflow.cpp should no longer keep the binary echo reader helper inline");
        Expect(workflow_source.find("readLittleEndianFloat32(") == std::string::npos,
               "infer_workflow.cpp should no longer keep the binary float reader helper inline");
    }

    void TestInferWorkflowInternalHeaderDeclaresThePatchPlannerBoundary()
    {
        const auto header_path = FindRepoFile("main/include/workflow/infer/infer_workflow_internal.hpp");
        const std::string header_source = ReadFileText(header_path);

        ExpectContains(header_source,
                       "struct PatchPacket",
                       "infer internal header should carry the planner packet boundary");
        ExpectContains(header_source,
                       "class SnakePatchSource",
                       "infer internal header should declare the planner class boundary");
        ExpectContainsAll(header_source,
                          {"next(PatchPacket &packet)", "rows() const", "cols() const", "totalPatches() const"},
                          "infer internal header should keep the planner interface visible to infer orchestration");
    }

    void TestOutputSinkImplementationFileExistsAndOwnsTheHdmiOutputBoundary()
    {
        const auto sink_path = FindRepoFile("main/src/infer/output_sink.cpp");
        const std::string sink_source = ReadFileText(sink_path);

        ExpectContains(sink_source,
                       "LatestSnapshotMailbox::publish",
                       "output sink implementation should own the latest-wins mailbox boundary");
        ExpectContains(sink_source,
                       "HdmiRenderWorker::run",
                       "output sink implementation should own the HDMI render worker boundary");
        ExpectContainsAll(sink_source,
                          {"RuntimeState processPatchToPng(", "RuntimeState processPatchToHdmi("},
                          "output sink implementation should own the PNG and HDMI patch sinks");
        ExpectContainsAll(sink_source,
                          {"STOPPED", "WAITING", "LatestSnapshotMailbox::markInputClosed", "HdmiRenderWorker::requestStop"},
                          "output sink implementation should keep the terminal frame and shutdown semantics visible");
    }

    void TestInferWorkflowNoLongerContainsTheFullOutputSinkImplementation()
    {
        const auto workflow_path = FindRepoFile("main/src/infer_workflow.cpp");
        const std::string workflow_source = ReadFileText(workflow_path);

        Expect(workflow_source.find("class LatestSnapshotMailbox") == std::string::npos,
               "infer_workflow.cpp should no longer own the LatestSnapshotMailbox implementation");
        Expect(workflow_source.find("class HdmiRenderWorker") == std::string::npos,
               "infer_workflow.cpp should no longer own the HdmiRenderWorker implementation");
        Expect(workflow_source.find("RuntimeState processPatchToPng(") == std::string::npos,
               "infer_workflow.cpp should no longer define processPatchToPng()");
        Expect(workflow_source.find("RuntimeState processPatchToHdmi(") == std::string::npos,
               "infer_workflow.cpp should no longer define processPatchToHdmi()");
        Expect(workflow_source.find("LatestSnapshotMailbox::") == std::string::npos,
               "infer_workflow.cpp should no longer embed mailbox implementation details");
        Expect(workflow_source.find("HdmiRenderWorker::") == std::string::npos,
               "infer_workflow.cpp should no longer embed HDMI worker implementation details");
    }

    void TestInferWorkflowKeepsTheTopLevelOrchestrationSkeleton()
    {
        const auto workflow_path = FindRepoFile("main/src/infer_workflow.cpp");
        const std::string workflow_source = ReadFileText(workflow_path);

        ExpectContainsAll(workflow_source,
                          {"setStage(\"load config\")",
                           "setStage(\"collect SAR images\")",
                           "setStage(\"open device\")",
                           "setStage(\"load network json/raw\")",
                           "setStage(\"create output sink\")",
                           "setStage(\"create inference runner\")",
                           "publishSnapshot(control, cfg, published_state, \"load config\"",
                           "publishSnapshot(control, cfg, published_state, \"collect SAR images\"",
                           "publishSnapshot(control, cfg, base_state, \"sar loaded\"",
                           "publishSnapshot(control,\n                        cfg,\n                        RuntimeState{}",
                           "stop_requested ? \"stopped\" : \"finished\"",
                           "shared::ControlState::Error"},
                          "infer_workflow.cpp should keep the top-level orchestration stages and terminal snapshots");
    }

    void TestInferWorkflowStillSplitsManualAndAutoSnakePaths()
    {
        const auto workflow_path = FindRepoFile("main/src/infer_workflow.cpp");
        const std::string workflow_source = ReadFileText(workflow_path);

        ExpectContains(workflow_source,
                       "if (patch_mode == shared::SelectedPatchMode::ManualFlight)",
                       "infer_workflow.cpp should still keep the manual_flight top-level path");
        ExpectContains(workflow_source,
                       "manual_flight requires exactly one selected SAR image.",
                       "infer_workflow.cpp should still enforce the single-image manual_flight constraint");
        ExpectContains(workflow_source,
                       "SnakePatchSource patch_source(sar_norm, cfg.patch_size, cfg.stride);",
                       "infer_workflow.cpp should still keep the auto_snake top-level path");
        ExpectContainsAll(workflow_source,
                          {"publishSnapshot(control, cfg, patch_state, \"manual patch processed\"",
                           "publishSnapshot(control, cfg, patch_state, \"patch processed\""},
                          "infer_workflow.cpp should still distinguish manual and auto patch progress snapshots");
    }
}

int main()
{
    TestUiRenderHelperWasSplitIntoAnImplementationFile();
    TestStoppedAndManualTelemetrySemanticsRemainVisible();
    TestManualTelemetryStillMapsIntoObservableModeAndStatusLabels();
    TestSnakePatchSourceWasMovedIntoAPatchPlannerImplementationFile();
    TestInferWorkflowNoLongerContainsTheFullSnakePatchSourceImplementation();
    TestInferWorkflowNoLongerContainsTheLegacyImagingHelpers();
    TestInferWorkflowInternalHeaderDeclaresThePatchPlannerBoundary();
    TestOutputSinkImplementationFileExistsAndOwnsTheHdmiOutputBoundary();
    TestInferWorkflowNoLongerContainsTheFullOutputSinkImplementation();
    TestInferWorkflowKeepsTheTopLevelOrchestrationSkeleton();
    TestInferWorkflowStillSplitsManualAndAutoSnakePaths();
    std::cout << "infer_workflow_regression_test passed\n";
    return 0;
}
