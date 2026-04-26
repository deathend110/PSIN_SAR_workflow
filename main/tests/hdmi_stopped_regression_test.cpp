#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace
{
    void Fail(const std::string &message)
    {
        std::cerr << "hdmi_stopped_regression_test failed: " << message << '\n';
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

    std::string SliceFunctionBody(const std::string &source, const std::string &function_signature)
    {
        const std::size_t signature_pos = source.find(function_signature);
        Expect(signature_pos != std::string::npos, "missing function signature: " + function_signature);

        const std::size_t body_start = source.find('{', signature_pos);
        Expect(body_start != std::string::npos, "missing function body for: " + function_signature);

        int brace_depth = 0;
        for (std::size_t i = body_start; i < source.size(); ++i)
        {
            if (source[i] == '{')
            {
                ++brace_depth;
            }
            else if (source[i] == '}')
            {
                --brace_depth;
                if (brace_depth == 0)
                {
                    return source.substr(body_start, i - body_start + 1);
                }
            }
        }

        Fail("unclosed function body for: " + function_signature);
        return {};
    }

    void TestHdmiStopPathEmitsStoppedTerminalFrame()
    {
        const auto source_path = FindRepoFile("main/src/infer/output_sink.cpp");
        const std::string source = ReadFileText(source_path);
        const std::string run_body = SliceFunctionBody(source, "void HdmiRenderWorker::run()");

        Expect(run_body.find("STOPPED") != std::string::npos,
               "HdmiRenderWorker::run() still lacks a STOPPED terminal-frame path");
    }

    void TestStatusBadgeIsNotHardcodedGreenRunning()
    {
        const auto source_path = FindRepoFile("main/src/infer/ui_render.cpp");
        const std::string source = ReadFileText(source_path);

        Expect(source.find("drawBadge(badge_x, ui_context.status_label, running_text_color, true);") == std::string::npos,
               "composeIndustrialUiFrame() still hardcodes the status badge as green RUNNING");
    }
}

int main()
{
    TestHdmiStopPathEmitsStoppedTerminalFrame();
    TestStatusBadgeIsNotHardcodedGreenRunning();
    std::cout << "hdmi_stopped_regression_test passed\n";
    return 0;
}
