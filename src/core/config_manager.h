#pragma once

#include "core/json.h"

#include <mutex>
#include <string>
#include <vector>

namespace ww {

struct ShortcutReplacement {
    std::string original_lnk;
    std::string backup_lnk;
};

struct BlockedApp {
    std::string name;
    std::string original_path;
    std::string icon_path;
    std::vector<std::string> shortcut_paths;
    std::vector<ShortcutReplacement> replaced_shortcuts;
};

struct Rule {
    std::string id;
    std::string ssid;
    std::string network_type = "wifi";
    std::string network_id;
    std::string network_name;
    std::string app_group_id;
    std::vector<BlockedApp> blocked_apps;
    std::string safe_wifi_ssid;
    std::string safe_wifi_password;
    std::string description;
};

struct AppGroup {
    std::string id;
    std::string name;
    std::vector<BlockedApp> apps;
};

struct Settings {
    bool auto_start = true;
    bool protection_enabled = true;
    bool dark_mode = true;
    std::string bypass_password;
    std::string language = "zh-CN";
    int http_port = 18765;
    int bypass_timeout_minutes = 0;
    std::int64_t bypass_until_epoch = 0;
};

struct AppConfig {
    std::string version = "1.9.0";
    Settings settings;
    std::vector<AppGroup> app_groups;
    std::vector<Rule> rules;
};

struct ConfigLoadStatus {
    bool repaired = false;
    std::wstring backup_path;
    std::string error;
};

JsonValue configToJson(const AppConfig& config);
AppConfig configFromJson(const JsonValue& value);

class ConfigManager {
public:
    explicit ConfigManager(std::wstring path = L"");

    AppConfig load();
    bool save(const AppConfig& config);
    AppConfig get();
    bool update(const AppConfig& config);
    std::wstring path() const;
    ConfigLoadStatus lastLoadStatus() const;

    static AppConfig defaults();

private:
    std::wstring path_;
    mutable std::mutex mutex_;
    AppConfig cached_;
    ConfigLoadStatus last_status_;
    bool loaded_ = false;
};

}
