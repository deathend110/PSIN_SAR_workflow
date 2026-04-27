#include "demo_config.hpp"
#include "image_collector.hpp"
#include "model_runner.hpp"
#include "result_writer.hpp"

#include <exception>
#include <iostream>
#include <memory>

int main(int argc, char **argv)
{
    try
    {
        if (argc != 2)
        {
            std::cerr << "usage: demo_simple_infer <config.yaml>\n";
            return 1;
        }

        std::cerr << "[demo] load config\n";
        const demo::DemoConfig cfg = demo::LoadDemoConfig(argv[1]);
        std::cerr << "[demo] config loaded"
                  << " input_dir=" << cfg.input_dir.string()
                  << " output_dir=" << cfg.output_dir.string()
                  << " json_path=" << cfg.json_path.string()
                  << " raw_path=" << cfg.raw_path.string() << '\n';

        std::cerr << "[demo] collect images\n";
        const auto paths = demo::CollectImages(cfg.input_dir);
        std::cerr << "[demo] collected supported paths=" << paths.size() << '\n';

        std::size_t valid_count = 0;
        std::size_t skipped_count = 0;
        std::unique_ptr<demo::ModelRunner> runner;
        for (const auto &path : paths)
        {
            const demo::ImageLoadResult loaded = demo::LoadOneImage(path);
            if (!loaded.valid)
            {
                ++skipped_count;
                std::cout << "skipped: " << loaded.path.filename().string() << " reason=" << loaded.reason << '\n';
                continue;
            }

            ++valid_count;
            if (runner == nullptr)
            {
                std::cerr << "[demo] init model runner\n";
                runner = std::make_unique<demo::ModelRunner>(cfg.json_path, cfg.raw_path);
            }
            std::cerr << "[demo] infer " << loaded.path.filename().string() << '\n';
            const demo::InferenceResult result = runner->Run(loaded.image);
            demo::WriteOutputs(cfg.output_dir, loaded.path.stem().string(), result.restore, result.mask_class);
            std::cout << "processed: " << loaded.path.filename().string() << '\n';
        }
        std::cout << "done valid=" << valid_count << " skipped=" << skipped_count << '\n';
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "demo_simple_infer failed: " << e.what() << '\n';
        return 1;
    }
}
