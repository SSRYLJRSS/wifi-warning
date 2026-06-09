#include "core/rule_engine.h"

#include "core/util.h"

#include <filesystem>

namespace fs = std::filesystem;

namespace ww {

static std::string normalizePath(const std::string& path) {
    std::error_code ec;
    fs::path p(utf8ToWide(path));
    auto absolute = fs::weakly_canonical(p, ec);
    if (!ec) return toLowerAscii(wideToUtf8(absolute.wstring()));
    std::wstring wide = p.lexically_normal().wstring();
    return toLowerAscii(wideToUtf8(wide));
}

bool hasActiveBypass(const AppConfig& config) {
    (void)config;
    return false;
}

std::optional<RuleMatch> findBlockingRule(const AppConfig& config, const std::string& ssid, const std::string& appPath) {
    if (!config.settings.protection_enabled || ssid.empty() || appPath.empty() || hasActiveBypass(config)) {
        return std::nullopt;
    }

    std::string normalizedApp = normalizePath(appPath);
    for (const auto& rule : config.rules) {
        if (rule.ssid != ssid) continue;
        for (const auto& app : rule.blocked_apps) {
            if (normalizePath(app.original_path) == normalizedApp) {
                return RuleMatch{rule, app};
            }
        }
    }
    return std::nullopt;
}

}
