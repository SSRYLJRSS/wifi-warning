#include "ui/browser_launcher.h"

#include "core/util.h"

#include <windows.h>
#include <shellapi.h>

namespace ww {

bool openUrlInBrowser(const std::string& url) {
    auto result = ShellExecuteW(nullptr, L"open", utf8ToWide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<intptr_t>(result) > 32;
}

bool launchAppPath(const std::string& appPath, const std::string& arguments) {
    auto params = utf8ToWide(arguments);
    auto result = ShellExecuteW(nullptr, L"open", utf8ToWide(appPath).c_str(), params.empty() ? nullptr : params.c_str(), nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<intptr_t>(result) > 32;
}

}
