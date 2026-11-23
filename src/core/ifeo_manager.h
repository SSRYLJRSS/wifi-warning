#pragma once

#include "core/config_manager.h"

#include <string>
#include <vector>

namespace ww {

struct IfeoStatus {
    bool ok = false;
    bool elevated = false;
    int desired = 0;
    int missing = 0;
    std::string error;
    std::vector<std::string> managed_exes;
    std::vector<std::string> missing_exes;
};

struct IfeoApplyResult {
    bool ok = false;
    int applied = 0;
    int already = 0;
    int removed = 0;
    std::string error;
    std::vector<std::string> exes;
};

bool isProcessElevated();
std::wstring ifeoLauncherPath();
std::vector<std::string> managedExeNames(const AppConfig& config);

// Read-only status of whether every managed exe currently has our Debugger set.
IfeoStatus verifyIfeo(const AppConfig& config);

// Write operations require elevation (run inside ww-admin.exe).
IfeoApplyResult syncIfeo(const AppConfig& config);
IfeoApplyResult restoreAllIfeo();
bool removeIfeoForExe(const std::string& exe);

}
