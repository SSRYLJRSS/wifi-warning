#include "core/logger.h"

#include "core/util.h"

#include <filesystem>
#include <fstream>
#include <map>

namespace fs = std::filesystem;

namespace ww {

static JsonValue recordToJson(const LogRecord& record) {
    JsonValue::Object object;
    object["timestamp"] = record.timestamp.empty() ? nowIsoLocal() : record.timestamp;
    if (!record.ssid.empty()) object["ssid"] = record.ssid;
    if (!record.app.empty()) object["app"] = record.app;
    if (!record.action.empty()) object["action"] = record.action;
    if (!record.rule_id.empty()) object["rule_id"] = record.rule_id;
    if (!record.bypass_type.empty()) object["bypass_type"] = record.bypass_type;
    if (!record.wifi_action.empty()) object["wifi_action"] = record.wifi_action;
    if (!record.target_ssid.empty()) object["target_ssid"] = record.target_ssid;
    if (!record.shortcut.empty()) object["shortcut"] = record.shortcut;
    if (!record.backup.empty()) object["backup"] = record.backup;
    if (!record.error.empty()) object["error"] = record.error;
    return object;
}

static LogRecord recordFromJson(const JsonValue& value) {
    LogRecord record;
    if (const auto* item = value.get("timestamp")) record.timestamp = item->asString();
    if (const auto* item = value.get("ssid")) record.ssid = item->asString();
    if (const auto* item = value.get("app")) record.app = item->asString();
    if (const auto* item = value.get("action")) record.action = item->asString();
    if (const auto* item = value.get("rule_id")) record.rule_id = item->asString();
    if (const auto* item = value.get("bypass_type")) record.bypass_type = item->asString();
    if (const auto* item = value.get("wifi_action")) record.wifi_action = item->asString();
    if (const auto* item = value.get("target_ssid")) record.target_ssid = item->asString();
    if (const auto* item = value.get("shortcut")) record.shortcut = item->asString();
    if (const auto* item = value.get("backup")) record.backup = item->asString();
    if (const auto* item = value.get("error")) record.error = item->asString();
    return record;
}

Logger::Logger(std::wstring logDir) : logDir_(std::move(logDir)) {
    if (logDir_.empty()) logDir_ = logsDir();
    ensureDirectory(logDir_);
}

void Logger::rotate(int daysToKeep) {
    std::lock_guard lock(mutex_);
    rotateUnlocked(daysToKeep);
    last_rotation_date_ = dateStampLocal();
}

void Logger::rotateUnlocked(int daysToKeep) {
    ensureDirectory(logDir_);
    const auto cutoff = fs::file_time_type::clock::now() - std::chrono::hours(daysToKeep * 24);
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(logDir_, ec)) {
        if (ec) break;
        if (!entry.is_regular_file() || entry.path().extension() != L".jsonl") continue;
        if (entry.last_write_time(ec) < cutoff) fs::remove(entry.path(), ec);
    }
}

bool Logger::write(const LogRecord& record) {
    std::lock_guard lock(mutex_);
    std::string today = dateStampLocal();
    if (last_rotation_date_ != today) {
        rotateUnlocked(30);
        last_rotation_date_ = today;
    }
    fs::path path(logDir_);
    path /= utf8ToWide(today + ".jsonl");
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out) return false;
    out << stringifyJson(recordToJson(record), 0);
    return out.good();
}

std::vector<LogRecord> Logger::readRecent(int days) const {
    std::lock_guard lock(mutex_);
    std::vector<LogRecord> records;
    std::error_code ec;
    const auto cutoff = fs::file_time_type::clock::now() - std::chrono::hours(days * 24);
    for (const auto& entry : fs::directory_iterator(logDir_, ec)) {
        if (ec) break;
        if (!entry.is_regular_file() || entry.path().extension() != L".jsonl") continue;
        if (entry.last_write_time(ec) < cutoff) continue;
        std::ifstream in(entry.path(), std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            std::string error;
            auto parsed = parseJson(line, &error);
            if (parsed) records.push_back(recordFromJson(*parsed));
        }
    }
    return records;
}

JsonValue Logger::statsJson(int days) const {
    auto records = readRecent(days);
    int blockedToday = 0;
    int blockedWeek = 0;
    std::map<std::string, int> appCounts;
    JsonValue::Array rows;
    std::string today = dateStampLocal();

    const size_t maxRows = 500;
    for (const auto& record : records) {
        if (record.action == "blocked") {
            ++blockedWeek;
            if (record.timestamp.rfind(today, 0) == 0) ++blockedToday;
            if (!record.app.empty()) ++appCounts[record.app];
        }
        if (rows.size() < maxRows) rows.push_back(recordToJson(record));
    }

    std::string topApp;
    int topCount = 0;
    for (const auto& [app, count] : appCounts) {
        if (count > topCount) {
            topApp = app;
            topCount = count;
        }
    }

    return JsonValue::Object{
        {"blocked_today", blockedToday},
        {"blocked_week", blockedWeek},
        {"top_blocked_app", topApp},
        {"top_blocked_count", topCount},
        {"records", rows}
    };
}

}
