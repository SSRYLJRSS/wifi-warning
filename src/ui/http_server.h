#pragma once

#include "ui/api_handlers.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace ww {

class HttpServer {
public:
    HttpServer(int port, ConfigManager& config, Logger& logger);
    ~HttpServer();

    bool start();
    void stop();
    int port() const;

private:
    int port_;
    ConfigManager& config_;
    Logger& logger_;
    std::unique_ptr<ApiHandlers> api_;
    std::atomic<bool> running_{false};
    std::atomic<int> active_clients_{0};
    std::thread worker_;
    std::mutex clients_mutex_;
    std::condition_variable clients_done_;
    uintptr_t listen_socket_ = 0;

    void run();
    void acceptLoop();
    void handleClient(uintptr_t clientSocket);
    HttpResponse route(const HttpRequest& request);
};

}
