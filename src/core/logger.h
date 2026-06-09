#pragma once

#include "core/json.h"

#include <mutex>
#include <string>
#include <vector>

namespace ww {

struct LogRecord {
    std::string timestamp;
    std::string ssid;
    std::string app;
    std::string action;
    std::string rule_id;
    std::string bypass_type;
    std::string wifi_action;
    std::string target_ssid;
    std::string shortcut;
    std::string backup;
    std::string error;
};

class Logger {
public:
    explicit Logger(std::wstring logDir = L"");
    void rotate(int daysToKeep = 30);
    bool write(const LogRecord& record);
    std::vector<LogRecord> readRecent(int days = 30) const;
    JsonValue statsJson(int days = 30) const;

private:
    void rotateUnlocked(int daysToKeep);

    std::wstring logDir_;
    mutable std::mutex mutex_;
    std::string last_rotation_date_;
};

}
