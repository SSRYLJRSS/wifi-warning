#include "core/auto_start.h"

#include "core/util.h"

#include <windows.h>

namespace ww {

static constexpr const wchar_t* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static constexpr const wchar_t* VALUE_NAME = L"WiFiWarning";

static std::wstring runKeyPath() {
    std::wstring overrideKey = getEnvWide(L"WW_TEST_AUTOSTART_RUN_KEY");
    return overrideKey.empty() ? std::wstring(RUN_KEY) : overrideKey;
}

bool setAutoStart(bool enabled, const std::wstring& exePath, std::string* error) {
    HKEY key = nullptr;
    std::wstring keyPath = runKeyPath();
    LONG result = RegCreateKeyExW(HKEY_CURRENT_USER, keyPath.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (result != ERROR_SUCCESS) {
        if (error) *error = "无法打开启动项注册表";
        return false;
    }

    if (enabled) {
        std::wstring value = quoteArg(exePath) + L" --minimized";
        result = RegSetValueExW(key, VALUE_NAME, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    } else {
        result = RegDeleteValueW(key, VALUE_NAME);
        if (result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS;
    }
    RegCloseKey(key);

    if (result != ERROR_SUCCESS && error) *error = "启动项写入失败";
    return result == ERROR_SUCCESS;
}

bool isAutoStartEnabled() {
    HKEY key = nullptr;
    std::wstring keyPath = runKeyPath();
    if (RegOpenKeyExW(HKEY_CURRENT_USER, keyPath.c_str(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
    DWORD type = 0;
    DWORD bytes = 0;
    LONG result = RegQueryValueExW(key, VALUE_NAME, nullptr, &type, nullptr, &bytes);
    RegCloseKey(key);
    return result == ERROR_SUCCESS && type == REG_SZ && bytes > 0;
}

}
