#include "workflow/web/web_console.hpp"

#include "workflow/infer/infer_config.hpp"
#include "workflow/shared/config_utils.hpp"
#include "workflow/web/web_console_config.hpp"
#include "workflow/web/web_console_controller.hpp"
#include "workflow/web/web_console_server.hpp"
#include "workflow/rd/rd_config.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <exception>
#include <iostream>
#include <memory>
#include <thread>
#include <unordered_map>

namespace workflow::web
{
    namespace
    {
        // Web Console 主线程轮询的全局停止标记。
        std::atomic<bool> g_web_console_stop{false};

        // Ctrl+C / SIGTERM 信号处理器：这里只做轻量标记，不在信号处理器里做复杂逻辑。
        void handleInterrupt(int)
        {
            g_web_console_stop = true;
        }

        // 把结构化的 flight settings 转成 controller.applySettings 使用的扁平字段表。
        std::unordered_map<std::string, std::string> MakeFlightSettingsFields(const FlightSettings &settings)
        {
            return {
                {"flight.manual_step_px", std::to_string(settings.manual_step_px)},
                {"flight.boost_step_px", std::to_string(settings.boost_step_px)},
                {"flight.trigger_distance_px", std::to_string(settings.trigger_distance_px)},
                {"flight.cache_grid_px", std::to_string(settings.cache_grid_px)},
                {"flight.path_overlay", settings.path_overlay ? "true" : "false"},
                {"flight.control_bindings", settings.control_bindings},
            };
        }

        // 把 Web 配置里保存的默认 flight settings 先应用到内存态 controller。
        void ApplyInitialFlightSettings(WebConsoleController &controller, const FlightSettings &settings)
        {
            const auto response = controller.applySettings(MakeFlightSettingsFields(settings));
            if (response.find("\"ok\":false") != std::string::npos)
            {
                throw std::runtime_error("Failed to apply persisted flight settings.");
            }
        }

        // 在 Web Console 退出时，把 infer / rd / web 三份运行态配置持久化到 runtime YAML。
        void PersistControllerConfigs(const std::filesystem::path &web_config_path,
                                      const WebConsoleConfig &base_web_cfg,
                                      const WebConsoleController &controller)
        {
            infer::SaveConfig(shared::RuntimeConfigPath(base_web_cfg.infer_config_path), controller.inferConfig());
            rd::SaveConfig(shared::RuntimeConfigPath(base_web_cfg.rd_config_path), controller.rdConfig());

            WebConsoleConfig persisted_web_cfg = base_web_cfg;
            persisted_web_cfg.flight_settings = controller.flightSettings();
            SaveConfig(shared::RuntimeConfigPath(web_config_path), persisted_web_cfg);
        }
    }

    // Web Console 顶层入口。
    // 负责配置加载、controller/server 构造、web 线程托管，以及退出时的统一收尾。
    int Run(const std::filesystem::path &config_path)
    {
        std::thread web_thread;
        std::unique_ptr<WebConsoleController> controller;
        std::unique_ptr<WebConsoleServer> server;
        std::exception_ptr server_error;
        WebConsoleConfig web_cfg;
        bool web_cfg_loaded = false;
        bool configs_persisted = false;
        try
        {
            g_web_console_stop = false;
            std::signal(SIGINT, handleInterrupt);
            std::signal(SIGTERM, handleInterrupt);

            web_cfg = LoadConfig(config_path);
            web_cfg_loaded = true;
            const infer::AppConfig infer_cfg = infer::LoadConfig(web_cfg.infer_config_path);
            const rd::AppConfig rd_cfg = rd::LoadConfig(web_cfg.rd_config_path);

            controller = std::make_unique<WebConsoleController>(infer_cfg, rd_cfg);
            ApplyInitialFlightSettings(*controller, web_cfg.flight_settings);
            server = std::make_unique<WebConsoleServer>(web_cfg, *controller);

            std::cout << "Web Console listening on http://" << web_cfg.bind_address << ":" << web_cfg.port << std::endl;
            std::cout << "Board IP: http://" << web_cfg.board_ip << ":" << web_cfg.port << std::endl;
            std::cout << "Press Ctrl+C to stop the embedded web service." << std::endl;

            web_thread = std::thread([&]() {
                try
                {
                    server->Run();
                }
                catch (...)
                {
                    server_error = std::current_exception();
                }
                g_web_console_stop = true;
            });

            while (!g_web_console_stop.load())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }

            server->Stop();
            controller->RequestWorkerStop();

            if (web_thread.joinable())
            {
                web_thread.join();
            }
            controller->JoinWorker();
            PersistControllerConfigs(config_path, web_cfg, *controller);
            configs_persisted = true;

            if (server_error)
            {
                std::rethrow_exception(server_error);
            }
            return 0;
        }
        catch (const std::exception &e)
        {
            g_web_console_stop = true;
            if (server)
            {
                server->Stop();
            }
            if (controller)
            {
                controller->RequestWorkerStop();
            }
            if (web_thread.joinable())
            {
                web_thread.join();
            }
            if (controller)
            {
                controller->JoinWorker();
            }
            if (controller && web_cfg_loaded && !configs_persisted)
            {
                try
                {
                    PersistControllerConfigs(config_path, web_cfg, *controller);
                }
                catch (const std::exception &persist_error)
                {
                    std::cerr << "web_console persist failed: " << persist_error.what() << std::endl;
                }
            }
            std::cerr << "web_console failed: " << e.what() << std::endl;
            return 2;
        }
    }
}
