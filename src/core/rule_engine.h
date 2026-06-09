#pragma once

#include "core/config_manager.h"

#include <optional>
#include <string>

namespace ww {

struct RuleMatch {
    Rule rule;
    BlockedApp app;
};

std::optional<RuleMatch> findBlockingRule(const AppConfig& config, const std::string& ssid, const std::string& appPath);
bool hasActiveBypass(const AppConfig& config);

}
