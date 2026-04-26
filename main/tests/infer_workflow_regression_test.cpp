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
}

int main()
{
    TestUiRenderHelperWasSplitIntoAnImplementationFile();
    TestStoppedAndManualTelemetrySemanticsRemainVisible();
    TestManualTelemetryStillMapsIntoObservableModeAndStatusLabels();
    std::cout << "infer_workflow_regression_test passed\n";
    return 0;
}
