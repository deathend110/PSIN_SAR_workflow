#include <algorithm>
#include <cassert>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace
{
    struct ScopedStreamRedirect
    {
        ScopedStreamRedirect(std::istream &input,
                             std::streambuf *new_input_buffer,
                             std::ostream &output,
                             std::streambuf *new_output_buffer)
            : input_(input), output_(output), old_input_(input.rdbuf(new_input_buffer)), old_output_(output.rdbuf(new_output_buffer))
        {
        }

        ~ScopedStreamRedirect()
        {
            input_.rdbuf(old_input_);
            output_.rdbuf(old_output_);
        }

    private:
        std::istream &input_;
        std::ostream &output_;
        std::streambuf *old_input_;
        std::streambuf *old_output_;
    };

    std::vector<std::string> g_call_log;
    int g_rd_return_code = 0;
    int g_infer_return_code = 0;
    int g_web_return_code = 0;

    std::size_t CountOccurrences(const std::string &text, const std::string &needle)
    {
        if (needle.empty())
        {
            return 0;
        }

        std::size_t count = 0;
        std::size_t pos = 0;
        while ((pos = text.find(needle, pos)) != std::string::npos)
        {
            ++count;
            pos += needle.size();
        }
        return count;
    }
}

namespace workflow::rd
{
    int Run(const std::filesystem::path &)
    {
        g_call_log.push_back("rd");
        return g_rd_return_code;
    }
}

namespace workflow::infer
{
    int Run(const std::filesystem::path &)
    {
        g_call_log.push_back("infer");
        return g_infer_return_code;
    }
}

namespace workflow::web
{
    int Run(const std::filesystem::path &)
    {
        g_call_log.push_back("web");
        return g_web_return_code;
    }
}

#define main psin_workflow_main_for_test
#include "../src/main.cpp"
#undef main

namespace
{
    void ResetStubState()
    {
        g_call_log.clear();
        g_rd_return_code = 0;
        g_infer_return_code = 0;
        g_web_return_code = 0;
    }

    void TestWebConsoleReturnReentersMenu()
    {
        ResetStubState();
        g_web_return_code = 0;

        std::istringstream input("3\n0\n");
        std::ostringstream output;
        {
            ScopedStreamRedirect redirect(std::cin, input.rdbuf(), std::cout, output.rdbuf());
            const int result = psin_workflow_main_for_test();
            assert(result == 0);
        }

        assert((g_call_log == std::vector<std::string>{"web"}));
        assert(CountOccurrences(output.str(), "=== PSIN SAR Workflow ===") == 2);
    }

    void TestRdStillExitsImmediately()
    {
        ResetStubState();
        g_rd_return_code = 7;

        std::istringstream input("1\n");
        std::ostringstream output;
        {
            ScopedStreamRedirect redirect(std::cin, input.rdbuf(), std::cout, output.rdbuf());
            const int result = psin_workflow_main_for_test();
            assert(result == 7);
        }

        assert((g_call_log == std::vector<std::string>{"rd"}));
        assert(CountOccurrences(output.str(), "=== PSIN SAR Workflow ===") == 1);
    }
}

int main()
{
    TestWebConsoleReturnReentersMenu();
    TestRdStillExitsImmediately();
    std::cout << "main_menu_regression_test passed\n";
    return 0;
}
