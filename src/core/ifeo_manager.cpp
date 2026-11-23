#include "core/ifeo_manager.h"

#include "core/json.h"
#include "core/util.h"

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace ww {

namespace {

const wchar_t* kIfeoBase = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options";
const DWORD kViews[2] = {KEY_WOW64_64KEY, KEY_WOW64_32KEY};

bool isProtectedExeLower(const std::string& lowerBase) {
    static const char* list[] = {
        "csrss.exe", "wininit.exe", "winlogon.exe", "services.exe", "lsass.exe",
        "svchost.exe", "smss.exe", "explorer.exe", "conhost.exe", "dwm.exe",
        "taskhostw.exe", "sihost.exe", "runtimebroker.exe", "backgroundtaskhost.exe",
        "dllhost.exe", "cmd.exe", "powershell.exe", "reg.exe", "msiexec.exe",
        "ww-launch.exe", "ww-admin.exe", "wifi-warning.exe", "ww-smoke.exe"};
    for (const char* item : list) {
        if (lowerBase == item) return true;
    }
    return false;
}

bool isProtectedPath(const std::string& path) {
    std::string lower = toLowerAscii(path);
    if (lower.empty()) return true;
    if (lower.find("\\windows\\system32\\") != std::string::npos) return true;
    if (lower.find("\\windows\\syswow64\\") != std::string::npos) return true;
    if (lower.find("\\windows\\winsxs\\") != std::string::npos) return true;
    return false;
}

std::wstring subkeyFor(const std::string& exeName) {
    return std::wstring(kIfeoBase) + L"\\" + utf8ToWide(exeName);
}

bool readDebuggerValue(const std::wstring& subkey, DWORD view, std::wstring* out) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey.c_str(), 0, KEY_QUERY_VALUE | view, &key) != ERROR_SUCCESS) {
        return false;
    }
    wchar_t buffer[2048];
    DWORD size = sizeof(buffer);
    DWORD type = 0;
    LONG rc = RegQueryValueExW(key, L"Debugger", nullptr, &type, reinterpret_cast<LPBYTE>(buffer), &size);
    RegCloseKey(key);
    if (rc == ERROR_SUCCESS && type == REG_SZ) {
        *out = buffer;
        return true;
    }
    return false;
}

// 去掉首尾空白与成对引号，用于宽松比较是否为本程序的启动器。
std::string normalizeDebugger(const std::wstring& value) {
    size_t start = 0, end = value.size();
    while (start < end && (value[start] == L' ' || value[start] == L'\t')) ++start;
    while (end > start && (value[end - 1] == L' ' || value[end - 1] == L'\t')) --end;
    std::wstring trimmed = value.substr(start, end - start);
    if (trimmed.size() >= 2 && trimmed.front() == L'"' && trimmed.back() == L'"') {
        trimmed = trimmed.substr(1, trimmed.size() - 2);
    }
    return toLowerAscii(wideToUtf8(trimmed));
}

bool debuggerIsOurs(const std::wstring& value, const std::string& launcherLower) {
    std::string norm = normalizeDebugger(value);
    if (norm == launcherLower) return true;
    // 升级/迁移安装目录后路径会变化：凡 basename 为 ww-launch.exe 的调试器都视为本程序（含旧版）。
    return pathBaseNameUtf8(norm) == "ww-launch.exe";
}

bool writeDebuggerValue(const std::wstring& subkey, DWORD view, const std::wstring& value, std::string* error) {
    HKEY key = nullptr;
    LONG rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, subkey.c_str(), 0, nullptr, 0, KEY_SET_VALUE | view, nullptr, &key, nullptr);
    if (rc != ERROR_SUCCESS) {
        if (error) *error = "无法创建注册表键(代码 " + std::to_string(static_cast<int>(rc)) + ")";
        return false;
    }
    LONG rc2 = RegSetValueExW(key, L"Debugger", 0, REG_SZ,
                              reinterpret_cast<const BYTE*>(value.c_str()),
                              static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    if (rc2 != ERROR_SUCCESS) {
        if (error) *error = "无法写入 Debugger(代码 " + std::to_string(static_cast<int>(rc2)) + ")";
        return false;
    }
    return true;
}

bool writeDebuggerBothViews(const std::wstring& subkey, const std::wstring& value, std::string* error) {
    for (DWORD view : kViews) {
        if (!writeDebuggerValue(subkey, view, value, error)) return false;
    }
    return true;
}

bool deleteDebuggerValueView(const std::wstring& subkey, DWORD view, std::string* error) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey.c_str(), 0, KEY_SET_VALUE | view, &key) != ERROR_SUCCESS) {
        return true;  // 键不存在即视为已清理
    }
    LONG rc = RegDeleteValueW(key, L"Debugger");
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS && rc != ERROR_FILE_NOT_FOUND) {
        if (error) *error = "无法删除 Debugger(代码 " + std::to_string(static_cast<int>(rc)) + ")";
        return false;
    }
    return true;
}

void deleteKeyIfEmptyView(const std::wstring& subkey, DWORD view) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey.c_str(), 0, KEY_QUERY_VALUE | view, &key) != ERROR_SUCCESS) return;
    DWORD subkeys = 0, values = 0;
    RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, &values, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(key);
    if (subkeys == 0 && values == 0) RegDeleteKeyExW(HKEY_LOCAL_MACHINE, subkey.c_str(), view, 0);
}

// 该视图当前值是否就是本程序的启动器。
bool viewIsOurs(const std::wstring& subkey, DWORD view, const std::string& launcherLower) {
    std::wstring current;
    if (!readDebuggerValue(subkey, view, &current)) return false;
    return debuggerIsOurs(current, launcherLower);
}

// 将一个视图还原到记录的原始值（originalUtf8 为空 = 原本没有，删除我们的值）。
// 删除前校验当前值确为本程序启动器，避免误删第三方 Debugger。
bool restoreOneView(const std::wstring& subkey, DWORD view, const std::string& originalUtf8,
                    const std::string& launcherLower, std::string* error) {
    if (originalUtf8.empty()) {
        std::wstring current;
        bool present = readDebuggerValue(subkey, view, &current);
        if (!present) return true;
        if (!debuggerIsOurs(current, launcherLower)) return true;  // 不是我们的，保持不动
        return deleteDebuggerValueView(subkey, view, error);
    }
    return writeDebuggerValue(subkey, view, utf8ToWide(originalUtf8), error);
}

struct Entry {
    std::string original64;
    std::string original32;
};

struct IfeoState {
    std::map<std::string, Entry> entries;
};

std::wstring statePath() {
    return (fs::path(executableDir()) / L"ifeo-state.json").wstring();
}

IfeoState loadState() {
    IfeoState state;
    std::string text = readTextFileUtf8(statePath());
    if (text.empty()) return state;
    auto parsed = parseJson(text);
    if (!parsed) return state;
    const JsonValue* entries = parsed->get("entries");
    if (!entries || !entries->isArray()) return state;
    for (const auto& entry : entries->asArray()) {
        const JsonValue* exe = entry.get("exe");
        if (!exe) continue;
        Entry e;
        const JsonValue* legacy = entry.get("original");
        if (legacy) {
            e.original64 = legacy->asString();
            e.original32 = legacy->asString();
        } else {
            const JsonValue* o64 = entry.get("original64");
            const JsonValue* o32 = entry.get("original32");
            e.original64 = o64 ? o64->asString() : "";
            e.original32 = o32 ? o32->asString() : "";
        }
        state.entries[exe->asString()] = e;
    }
    return state;
}

bool saveState(const IfeoState& state) {
    JsonValue::Array entries;
    for (const auto& kv : state.entries) {
        entries.push_back(JsonValue::Object{{"exe", kv.first},
                                            {"original64", kv.second.original64},
                                            {"original32", kv.second.original32}});
    }
    return writeTextFileUtf8(statePath(), stringifyJson(JsonValue::Object{{"entries", entries}}, 2));
}
std::vector<std::string> desiredExes(const AppConfig& config) {
    if (!config.settings.protection_enabled) return {};
    return managedExeNames(config);
}

}  // namespace

bool isProcessElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    bool elevated = false;
    if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size)) {
        elevated = elevation.TokenIsElevated != 0;
    }
    CloseHandle(token);
    return elevated;
}

std::wstring ifeoLauncherPath() {
    return (fs::path(executableDir()) / L"ww-launch.exe").wstring();
}

std::vector<std::string> managedExeNames(const AppConfig& config) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    auto add = [&](const std::string& path) {
        if (isProtectedPath(path)) return;
        std::string base = pathBaseNameUtf8(path);
        if (base.empty()) return;
        std::string lower = toLowerAscii(base);
        if (isProtectedExeLower(lower)) return;
        if (seen.count(lower)) return;
        seen.insert(lower);
        out.push_back(base);
    };
    for (const auto& rule : config.rules) {
        for (const auto& app : rule.blocked_apps) add(app.original_path);
    }
    return out;
}

IfeoStatus verifyIfeo(const AppConfig& config) {
    IfeoStatus status;
    status.elevated = isProcessElevated();
    std::wstring launcher = ifeoLauncherPath();
    std::string launcherLower = toLowerAscii(wideToUtf8(launcher));
    for (const auto& exe : desiredExes(config)) {
        status.desired++;
        status.managed_exes.push_back(exe);
        std::wstring subkey = subkeyFor(exe);
        if (!viewIsOurs(subkey, KEY_WOW64_64KEY, launcherLower) ||
            !viewIsOurs(subkey, KEY_WOW64_32KEY, launcherLower)) {
            status.missing++;
            status.missing_exes.push_back(exe);
        }
    }
    status.ok = status.missing == 0;
    return status;
}

IfeoApplyResult syncIfeo(const AppConfig& config) {
    IfeoApplyResult result;
    if (!isProcessElevated()) { result.error = "需要管理员权限"; return result; }
    std::wstring launcher = ifeoLauncherPath();
    if (!fileExists(launcher)) { result.error = "未找到 ww-launch.exe"; return result; }
    std::wstring launcherValue = L"\"" + launcher + L"\"";
    std::string launcherLower = toLowerAscii(wideToUtf8(launcher));
    std::vector<std::string> desired = desiredExes(config);
    std::set<std::string> desiredLower;
    for (const auto& exe : desired) desiredLower.insert(toLowerAscii(exe));

    IfeoState state = loadState();

    // 阶段一：为受管程序登记原始值（用于将来精确还原）。
    for (const auto& exe : desired) {
        if (state.entries.find(exe) != state.entries.end()) continue;
        std::wstring subkey = subkeyFor(exe);
        Entry entry;
        std::wstring current64, current32;
        bool p64 = readDebuggerValue(subkey, KEY_WOW64_64KEY, &current64);
        bool p32 = readDebuggerValue(subkey, KEY_WOW64_32KEY, &current32);
        entry.original64 = (p64 && !debuggerIsOurs(current64, launcherLower)) ? wideToUtf8(current64) : "";
        entry.original32 = (p32 && !debuggerIsOurs(current32, launcherLower)) ? wideToUtf8(current32) : "";
        state.entries[exe] = entry;
    }
    // 阶段二：先落盘状态再写注册表；落盘失败即中止，避免“已劫持却无法还原”。
    if (!saveState(state)) {
        result.error = "ifeo-state.json 写入失败，已中止以避免无法还原";
        return result;
    }

    // 阶段三：写入 IFEO。
    for (const auto& exe : desired) {
        std::wstring subkey = subkeyFor(exe);
        if (viewIsOurs(subkey, KEY_WOW64_64KEY, launcherLower) &&
            viewIsOurs(subkey, KEY_WOW64_32KEY, launcherLower)) {
            result.already++;
            result.exes.push_back(exe);
            continue;
        }
        std::string error;
        if (!writeDebuggerBothViews(subkey, launcherValue, &error)) {
            if (result.error.empty()) result.error = exe + ": " + error;
            continue;
        }
        result.applied++;
        result.exes.push_back(exe);
    }

    // 阶段四：还原不再受管的程序。
    for (auto it = state.entries.begin(); it != state.entries.end();) {
        if (desiredLower.count(toLowerAscii(it->first)) != 0) { ++it; continue; }
        std::wstring subkey = subkeyFor(it->first);
        std::string error;
        bool ok64 = restoreOneView(subkey, KEY_WOW64_64KEY, it->second.original64, launcherLower, &error);
        bool ok32 = restoreOneView(subkey, KEY_WOW64_32KEY, it->second.original32, launcherLower, &error);
        if (ok64 && ok32) {
            deleteKeyIfEmptyView(subkey, KEY_WOW64_64KEY);
            deleteKeyIfEmptyView(subkey, KEY_WOW64_32KEY);
            result.removed++;
            it = state.entries.erase(it);
        } else {
            if (result.error.empty()) result.error = it->first + ": " + error;
            ++it;
        }
    }
    if (!saveState(state) && result.error.empty()) result.error = "ifeo-state.json 更新失败";
    result.ok = result.error.empty();
    return result;
}
IfeoApplyResult restoreAllIfeo() {
    IfeoApplyResult result;
    if (!isProcessElevated()) {
        result.error = "需要管理员权限";
        return result;
    }
    std::wstring launcher = ifeoLauncherPath();
    std::string launcherLower = toLowerAscii(wideToUtf8(launcher));
    IfeoState state = loadState();
    for (auto it = state.entries.begin(); it != state.entries.end();) {
        std::wstring subkey = subkeyFor(it->first);
        std::string error;
        bool ok64 = restoreOneView(subkey, KEY_WOW64_64KEY, it->second.original64, launcherLower, &error);
        bool ok32 = restoreOneView(subkey, KEY_WOW64_32KEY, it->second.original32, launcherLower, &error);
        if (ok64 && ok32) {
            deleteKeyIfEmptyView(subkey, KEY_WOW64_64KEY);
            deleteKeyIfEmptyView(subkey, KEY_WOW64_32KEY);
            result.exes.push_back(it->first);
            result.removed++;
            it = state.entries.erase(it);
        } else {
            if (result.error.empty()) result.error = it->first + ": " + error;
            ++it;
        }
    }
    if (!saveState(state) && result.error.empty()) result.error = "ifeo-state.json 更新失败";
    result.ok = result.error.empty();
    return result;
}

bool removeIfeoForExe(const std::string& exe) {
    if (!isProcessElevated() || exe.empty()) return false;
    std::wstring launcher = ifeoLauncherPath();
    std::string launcherLower = toLowerAscii(wideToUtf8(launcher));
    IfeoState state = loadState();
    std::wstring subkey = subkeyFor(exe);
    auto it = state.entries.find(exe);
    std::string original64, original32;
    if (it != state.entries.end()) {
        original64 = it->second.original64;
        original32 = it->second.original32;
    }
    std::string error;
    bool ok64 = restoreOneView(subkey, KEY_WOW64_64KEY, original64, launcherLower, &error);
    bool ok32 = restoreOneView(subkey, KEY_WOW64_32KEY, original32, launcherLower, &error);
    if (!(ok64 && ok32)) return false;
    deleteKeyIfEmptyView(subkey, KEY_WOW64_64KEY);
    deleteKeyIfEmptyView(subkey, KEY_WOW64_32KEY);
    if (it != state.entries.end()) state.entries.erase(it);
    saveState(state);  // 尽力保存；即便失败，注册表已精确还原
    return true;
}

}  // namespace ww
