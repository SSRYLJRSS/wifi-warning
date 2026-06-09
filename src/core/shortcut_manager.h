#pragma once

#include "core/config_manager.h"

#include <string>
#include <vector>

namespace ww {

struct ShortcutCandidate {
    std::string path;
    std::string target_path;
    std::string arguments;
    std::string working_dir;
    std::string icon_path;
    int icon_index = 0;
    std::string description;
};

struct ShortcutOperationResult {
    bool ok = false;
    std::string shortcut;
    std::string backup;
    std::string error;
};

std::vector<ShortcutCandidate> findShortcutsForApp(const std::string& targetExe);
std::vector<std::string> shortcutScanRoots();
bool createShortcutFile(const std::string& shortcutPath, const std::string& targetPath, const std::string& arguments, const ShortcutCandidate& source, std::string* error = nullptr);
bool readShortcutFile(const std::string& shortcutPath, ShortcutCandidate& out);
ShortcutOperationResult replaceShortcutByPath(const std::string& shortcutPath, const std::string& launcherPath, const std::string& ruleId, ShortcutCandidate* sourceOut = nullptr);
ShortcutOperationResult replaceShortcut(const ShortcutCandidate& shortcut, const std::string& launcherPath, const std::string& appPath, const std::string& ruleId);
ShortcutOperationResult restoreShortcut(const ShortcutReplacement& replacement);
std::vector<ShortcutOperationResult> restoreShortcutsForRule(const AppConfig& config, const std::string& ruleId);
std::vector<ShortcutOperationResult> restoreAllShortcuts(const AppConfig& config);
void clearRestoredShortcutRecords(AppConfig& config, const std::vector<ShortcutOperationResult>& results);
int reconcileShortcutRecords(AppConfig& config);

}
