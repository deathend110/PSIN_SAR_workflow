#include "workflow/infer/infer_workflow.hpp"
#include "workflow/rd/rd_workflow.hpp"
#include "workflow/shared/app_mode.hpp"
#include "workflow/shared/config_utils.hpp"

#include <iostream>
#include <string>

namespace
{
    constexpr const char *kRdConfigPath = "configs/rd_imaging.yaml";
    constexpr const char *kInferConfigPath = "configs/infer_workflow.yaml";

    workflow::AppMode PromptForMode()
    {
        while (true)
        {
            std::cout << "\n=== PSIN SAR Workflow ===\n";
            std::cout << "1. RD only        (" << kRdConfigPath << ")\n";
            std::cout << "2. Inference only (" << kInferConfigPath << ")\n";
            std::cout << "0. Exit\n";
            std::cout << "Select mode: ";

            std::string input;
            if (!std::getline(std::cin, input))
            {
                return workflow::AppMode::Exit;
            }

            input = workflow::shared::Trim(input);
            if (input == "1")
            {
                return workflow::AppMode::RdOnly;
            }
            if (input == "2")
            {
                return workflow::AppMode::InferOnly;
            }
            if (input == "0")
            {
                return workflow::AppMode::Exit;
            }

            std::cout << "Invalid input. Please select 1, 2, or 0.\n";
        }
    }
}

int main()
{
    switch (PromptForMode())
    {
    case workflow::AppMode::RdOnly:
        return workflow::rd::Run(kRdConfigPath);
    case workflow::AppMode::InferOnly:
        return workflow::infer::Run(kInferConfigPath);
    case workflow::AppMode::Exit:
    default:
        std::cout << "Exit without running any workflow.\n";
        return 0;
    }
}
