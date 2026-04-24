#include "workflow/web/web_console.hpp"

#include "workflow/infer/infer_config.hpp"
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

namespace workflow::web
{
    namespace
    {
        std::atomic<bool> g_web_console_stop{false};

        void handleInterrupt(int)
        {
            g_web_console_stop = true;
        }
    }

    int Run(const std::filesystem::path &config_path)
    {
        std::thread web_thread;
        std::unique_ptr<WebConsoleController> controller;
        std::unique_ptr<WebConsoleServer> server;
        std::exception_ptr server_error;
        try
        {
            g_web_console_stop = false;
            std::signal(SIGINT, handleInterrupt);
            std::signal(SIGTERM, handleInterrupt);

            const WebConsoleConfig web_cfg = LoadConfig(config_path);
            const infer::AppConfig infer_cfg = infer::LoadConfig(web_cfg.infer_config_path);
            const rd::AppConfig rd_cfg = rd::LoadConfig(web_cfg.rd_config_path);

            controller = std::make_unique<WebConsoleController>(infer_cfg, rd_cfg);
            server = std::make_unique<WebConsoleServer>(web_cfg, *controller);

            std::cout << "Web Console listening on http://" << web_cfg.bind_address << ":" << web_cfg.port << std::endl;
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
            std::cerr << "web_console failed: " << e.what() << std::endl;
            return 2;
        }
    }
}
