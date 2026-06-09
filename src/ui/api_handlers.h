#pragma once

#include "core/config_manager.h"
#include "core/logger.h"
#include "core/json.h"

#include <map>
#include <string>

namespace ww {

struct HttpRequest {
    std::string method;
    std::string path;
    std::string query;
    std::map<std::string, std::string> query_params;
    std::map<std::string, std::string> headers;
    std::string body;
};

struct HttpResponse {
    int status = 200;
    std::string content_type = "application/json; charset=utf-8";
    std::string body;
    std::map<std::string, std::string> headers;
};

class ApiHandlers {
public:
    ApiHandlers(ConfigManager& config, Logger& logger);

    bool handle(const HttpRequest& request, HttpResponse& response);

private:
    ConfigManager& config_;
    Logger& logger_;

    HttpResponse configGet();
    HttpResponse configPost(const std::string& body);
    HttpResponse wifiCurrent();
    HttpResponse wifiAvailable();
    HttpResponse wifiSwitch(const std::string& body);
    HttpResponse stats();
    HttpResponse bypass(const std::string& body);
    HttpResponse appBrowse();
    HttpResponse shortcutBrowse();
    HttpResponse shortcutRead(const std::string& body);
    HttpResponse appIcon(const HttpRequest& request);
    HttpResponse appStatus();
    HttpResponse appCleanup(const std::string& body);
    HttpResponse shortcutScan(const std::string& body);
    HttpResponse shortcutReplace(const std::string& body);
    HttpResponse restoreShortcuts(const std::string& body);
};

JsonValue requestBodyJson(const std::string& body);
HttpResponse jsonResponse(const JsonValue& value, int status = 200);
HttpResponse errorResponse(const std::string& message, int status = 400);

}
