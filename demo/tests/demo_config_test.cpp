#include "demo_config.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
    void Fail(const std::string &message)
    {
        std::cerr << "demo_config_test failed: " << message << '\n';
        std::exit(1);
    }

    void Expect(bool ok, const std::string &message)
    {
        if (!ok)
        {
            Fail(message);
        }
    }

    std::filesystem::path MakeTempFile(const std::string &name, const std::string &content)
    {
        const auto root = std::filesystem::temp_directory_path() / "demo_config_test";
        std::filesystem::create_directories(root);
        const auto path = root / name;
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << content;
        return path;
    }

    void TestLoadsMinimalYaml()
    {
        const auto path = MakeTempFile(
            "ok.yaml",
            "demo:\n"
            "  input_dir: ./input\n"
            "  output_dir: ./output\n"
            "  json_path: ./model.json\n"
            "  raw_path: ./model.raw\n");
        const demo::DemoConfig cfg = demo::LoadDemoConfig(path);
        Expect(cfg.input_dir == "./input", "input_dir mismatch");
        Expect(cfg.output_dir == "./output", "output_dir mismatch");
        Expect(cfg.json_path == "./model.json", "json_path mismatch");
        Expect(cfg.raw_path == "./model.raw", "raw_path mismatch");
    }

    void TestMissingFieldThrows()
    {
        const auto path = MakeTempFile(
            "missing.yaml",
            "demo:\n"
            "  input_dir: ./input\n"
            "  output_dir: ./output\n"
            "  json_path: ./model.json\n");
        try
        {
            (void)demo::LoadDemoConfig(path);
            Fail("expected missing raw_path to throw");
        }
        catch (const std::runtime_error &e)
        {
            Expect(std::string(e.what()).find("raw_path") != std::string::npos,
                   "exception should mention raw_path");
        }
    }

    void TestLoadsUtf8BomYaml()
    {
        const std::string bom_yaml =
            std::string("\xEF\xBB\xBF") +
            "demo:\n"
            "  input_dir: ./input\n"
            "  output_dir: ./output\n"
            "  json_path: ./model.json\n"
            "  raw_path: ./model.raw\n";
        const auto path = MakeTempFile("bom.yaml", bom_yaml);
        const demo::DemoConfig cfg = demo::LoadDemoConfig(path);
        Expect(cfg.input_dir == "./input", "BOM yaml input_dir mismatch");
        Expect(cfg.output_dir == "./output", "BOM yaml output_dir mismatch");
        Expect(cfg.json_path == "./model.json", "BOM yaml json_path mismatch");
        Expect(cfg.raw_path == "./model.raw", "BOM yaml raw_path mismatch");
    }
}

int main()
{
    TestLoadsMinimalYaml();
    TestMissingFieldThrows();
    TestLoadsUtf8BomYaml();
    std::cout << "demo_config_test passed\n";
    return 0;
}
