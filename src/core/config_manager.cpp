#include "core/config_manager.h"

#include "core/util.h"

#include <filesystem>

namespace fs = std::filesystem;

namespace ww {

static const JsonValue* field(const JsonValue& value, const std::string& key) {
    return value.get(key);
}

static std::string stringField(const JsonValue& value, const std::string& key, const std::string& fallback = "") {
    const auto* item = field(value, key);
    return item ? item->asString(fallback) : fallback;
}

static bool boolField(const JsonValue& value, const std::string& key, bool fallback = false) {
    const auto* item = field(value, key);
    return item ? item->asBool(fallback) : fallback;
}

static int intField(const JsonValue& value, const std::string& key, int fallback = 0) {
    const auto* item = field(value, key);
    return item ? static_cast<int>(item->asNumber(fallback)) : fallback;
}

static JsonValue appToJson(const BlockedApp& app) {
    JsonValue::Array shortcutPaths;
    for (const auto& path : app.shortcut_paths) shortcutPaths.push_back(path);

    JsonValue::Array replacements;
    for (const auto& shortcut : app.replaced_shortcuts) {
        replacements.push_back(JsonValue::Object{
            {"original_lnk", shortcut.original_lnk},
            {"backup_lnk", shortcut.backup_lnk}
        });
    }
    return JsonValue::Object{
        {"name", app.name},
        {"original_path", app.original_path},
        {"icon_path", app.icon_path},
        {"shortcut_paths", shortcutPaths},
        {"replaced_shortcuts", replacements}
    };
}

static BlockedApp appFromJson(const JsonValue& appValue) {
    BlockedApp app;
    app.name = stringField(appValue, "name");
    app.original_path = stringField(appValue, "original_path");
    app.icon_path = stringField(appValue, "icon_path");
    if (const auto* paths = field(appValue, "shortcut_paths")) {
        for (const auto& pathValue : paths->asArray()) {
            std::string path = pathValue.asString();
            if (!path.empty()) app.shortcut_paths.push_back(path);
        }
    }
    if (const auto* shortcuts = field(appValue, "replaced_shortcuts")) {
        for (const auto& shortcutValue : shortcuts->asArray()) {
            app.replaced_shortcuts.push_back({
                stringField(shortcutValue, "original_lnk"),
                stringField(shortcutValue, "backup_lnk")
            });
        }
    }
    return app;
}

JsonValue configToJson(const AppConfig& config) {
    JsonValue::Object settings;
    settings["auto_start"] = config.settings.auto_start;
    settings["protection_enabled"] = config.settings.protection_enabled;
    settings["dark_mode"] = config.settings.dark_mode;
    settings["bypass_password"] = config.settings.bypass_password;
    settings["language"] = config.settings.language;
    settings["http_port"] = config.settings.http_port;
    settings["bypass_timeout_minutes"] = config.settings.bypass_timeout_minutes;
    settings["bypass_until_epoch"] = static_cast<double>(config.settings.bypass_until_epoch);

    auto appsToJson = [](const std::vector<BlockedApp>& blockedApps) {
        JsonValue::Array apps;
        for (const auto& app : blockedApps) apps.push_back(appToJson(app));
        return apps;
    };

    JsonValue::Array appGroups;
    for (const auto& group : config.app_groups) {
        appGroups.push_back(JsonValue::Object{
            {"id", group.id},
            {"name", group.name},
            {"apps", appsToJson(group.apps)}
        });
    }

    JsonValue::Array rules;
    for (const auto& rule : config.rules) {
        rules.push_back(JsonValue::Object{
            {"id", rule.id},
            {"ssid", rule.ssid},
            {"network_type", rule.network_type.empty() ? "wifi" : rule.network_type},
            {"network_id", rule.network_id.empty() ? rule.ssid : rule.network_id},
            {"network_name", rule.network_name.empty() ? (rule.network_id.empty() ? rule.ssid : rule.network_id) : rule.network_name},
            {"app_group_id", rule.app_group_id},
            {"blocked_apps", appsToJson(rule.blocked_apps)},
            {"safe_wifi_ssid", rule.safe_wifi_ssid},
            {"safe_wifi_password", rule.safe_wifi_password},
            {"description", rule.description}
        });
    }

    return JsonValue::Object{
        {"version", config.version},
        {"settings", settings},
        {"app_groups", appGroups},
        {"rules", rules}
    };
}

AppConfig configFromJson(const JsonValue& value) {
    AppConfig config = ConfigManager::defaults();
    config.version = stringField(value, "version", config.version);

    if (const auto* settings = field(value, "settings")) {
        config.settings.auto_start = boolField(*settings, "auto_start", config.settings.auto_start);
        config.settings.protection_enabled = boolField(*settings, "protection_enabled", config.settings.protection_enabled);
        config.settings.dark_mode = boolField(*settings, "dark_mode", config.settings.dark_mode);
        config.settings.bypass_password = stringField(*settings, "bypass_password", config.settings.bypass_password);
        config.settings.language = stringField(*settings, "language", config.settings.language);
        config.settings.http_port = intField(*settings, "http_port", config.settings.http_port);
        config.settings.bypass_timeout_minutes = intField(*settings, "bypass_timeout_minutes", config.settings.bypass_timeout_minutes);
        if (const auto* bypassEpoch = field(*settings, "bypass_until_epoch")) {
            config.settings.bypass_until_epoch = static_cast<std::int64_t>(bypassEpoch->asNumber(0));
        }
    }

    config.app_groups.clear();
    if (const auto* appGroups = field(value, "app_groups")) {
        for (const auto& item : appGroups->asArray()) {
            AppGroup group;
            group.id = stringField(item, "id");
            group.name = stringField(item, "name");
            if (const auto* apps = field(item, "apps")) {
                for (const auto& appValue : apps->asArray()) {
                    BlockedApp app = appFromJson(appValue);
                    if (!app.original_path.empty()) group.apps.push_back(app);
                }
            }
            if (!group.id.empty() && !group.name.empty()) config.app_groups.push_back(group);
        }
    }

    config.rules.clear();
    if (const auto* rules = field(value, "rules")) {
        for (const auto& item : rules->asArray()) {
            Rule rule;
            rule.id = stringField(item, "id");
            rule.ssid = stringField(item, "ssid");
            rule.network_type = stringField(item, "network_type", "wifi");
            rule.network_id = stringField(item, "network_id", rule.ssid);
            rule.network_name = stringField(item, "network_name", rule.network_id.empty() ? rule.ssid : rule.network_id);
            if (rule.network_type.empty()) rule.network_type = "wifi";
            if (rule.network_id.empty() && !rule.ssid.empty()) rule.network_id = rule.ssid;
            if (rule.ssid.empty() && rule.network_type == "wifi") rule.ssid = rule.network_id;
            rule.app_group_id = stringField(item, "app_group_id");
            rule.safe_wifi_ssid = stringField(item, "safe_wifi_ssid");
            rule.safe_wifi_password = stringField(item, "safe_wifi_password");
            rule.description = stringField(item, "description");
            if (const auto* apps = field(item, "blocked_apps")) {
                for (const auto& appValue : apps->asArray()) {
                    BlockedApp app = appFromJson(appValue);
                    if (!app.original_path.empty()) rule.blocked_apps.push_back(app);
                }
            }
            if (!rule.id.empty() && !rule.network_id.empty()) config.rules.push_back(rule);
        }
    }
    return config;
}

ConfigManager::ConfigManager(std::wstring path) : path_(std::move(path)) {
    if (path_.empty()) path_ = appConfigPath();
}

AppConfig ConfigManager::defaults() {
    AppConfig config;
    config.version = "1.9.1";
    config.settings.auto_start = true;
    config.settings.protection_enabled = true;
    config.settings.dark_mode = true;
    config.settings.bypass_password = "";
    config.settings.language = "zh-CN";
    config.settings.http_port = 18765;
    return config;
}

AppConfig ConfigManager::load() {
    std::lock_guard lock(mutex_);
    last_status_ = {};
    ensureDirectory(fs::path(path_).parent_path().wstring());
    if (!fileExists(path_)) {
        cached_ = defaults();
        writeTextFileUtf8(path_, stringifyJson(configToJson(cached_)));
        loaded_ = true;
        return cached_;
    }

    std::string raw = readTextFileUtf8(path_);
    std::string error;
    auto parsed = parseJson(raw, &error);
    if (!parsed) {
        fs::path original(path_);
        fs::path backup = original;
        backup += L".corrupt-" + utf8ToWide(dateStampLocal());
        std::error_code ec;
        fs::copy_file(original, backup, fs::copy_options::overwrite_existing, ec);
        cached_ = defaults();
        writeTextFileUtf8(path_, stringifyJson(configToJson(cached_)));
        last_status_.repaired = true;
        last_status_.backup_path = backup.wstring();
        last_status_.error = error;
    } else {
        cached_ = configFromJson(*parsed);
    }
    loaded_ = true;
    return cached_;
}

bool ConfigManager::save(const AppConfig& config) {
    std::lock_guard lock(mutex_);
    ensureDirectory(fs::path(path_).parent_path().wstring());
    bool ok = writeTextFileUtf8(path_, stringifyJson(configToJson(config)));
    if (ok) {
        cached_ = config;
        loaded_ = true;
    }
    return ok;
}

AppConfig ConfigManager::get() {
    std::unique_lock lock(mutex_);
    if (loaded_) return cached_;
    lock.unlock();
    return load();
}

bool ConfigManager::update(const AppConfig& config) {
    return save(config);
}

std::wstring ConfigManager::path() const {
    return path_;
}

ConfigLoadStatus ConfigManager::lastLoadStatus() const {
    std::lock_guard lock(mutex_);
    return last_status_;
}

}
