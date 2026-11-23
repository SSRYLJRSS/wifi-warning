#include "core/auto_start.h"
#include "core/config_manager.h"
#include "core/logger.h"
#include "core/network_manager.h"
#include "core/ifeo_manager.h"
#include "core/util.h"
#include "core/wifi_detector.h"
#include "ui/browser_launcher.h"
#include "ui/http_server.h"
#include "ui/tray_icon.h"

#include <windows.h>
#include <shellapi.h>

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static std::vector<std::wstring> commandLineArgs() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    LocalFree(argv);
    return args;
}

static bool hasArg(const std::vector<std::wstring>& args, const wchar_t* needle) {
    for (const auto& arg : args) {
        if (arg == needle) return true;
    }
    return false;
}

static std::wstring argValue(const std::vector<std::wstring>& args, const wchar_t* name) {
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == name) return args[i + 1];
    }
    return L"";
}

static int intArg(const std::vector<std::wstring>& args, const wchar_t* name, int fallback) {
    auto value = argValue(args, name);
    if (value.empty()) return fallback;
    try {
        return std::stoi(value);
    } catch (...) {
        return fallback;
    }
}

static bool isConsoleAttached() {
    return GetConsoleWindow() != nullptr || AttachConsole(ATTACH_PARENT_PROCESS);
}

static void consoleLine(const std::wstring& text) {
    if (!isConsoleAttached()) return;
    DWORD written = 0;
    std::wstring line = text + L"\n";
    WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), line.c_str(), static_cast<DWORD>(line.size()), &written, nullptr);
}

static int restoreIfeoAndSave(ww::ConfigManager& configManager, ww::Logger& logger, bool uninstallMode = false, bool syncAutoStart = true) {
    auto config = configManager.load();

    int code = -1;
    std::wstring admin = std::wstring(ww::executableDir()) + L"\\ww-admin.exe";
    if (ww::fileExists(admin)) {
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb = L"runas";
        sei.lpFile = admin.c_str();
        sei.lpParameters = L"--ifeo-restore";
        sei.nShow = SW_HIDE;
        if (ShellExecuteExW(&sei) && sei.hProcess) {
            WaitForSingleObject(sei.hProcess, 120000);
            DWORD exitCode = 0;
            GetExitCodeProcess(sei.hProcess, &exitCode);
            CloseHandle(sei.hProcess);
            code = static_cast<int>(exitCode);
        }
    }
    ww::LogRecord record;
    record.timestamp = ww::nowIsoLocal();
    record.action = code == 0 ? "ifeo_restored" : "ifeo_restore_failed";
    if (code != 0) record.error = "退出码 " + std::to_string(code);
    logger.write(record);
    if (code == 0) {
        config.settings.protection_enabled = false;
        if (uninstallMode) {
            config.settings.auto_start = false;
            if (syncAutoStart) {
                std::string ignored;
                ww::setAutoStart(false, ww::executablePath(), &ignored);
            }
        }
        configManager.save(config);
        consoleLine(L"已关闭防护并还原 IFEO 拦截。");
    } else {
        consoleLine(L"还原 IFEO 拦截失败或已取消（退出码: " + std::to_wstring(code) + L"），防护保持开启。");
    }
    return code == 0 ? 0 : 2;
}

static void logConfigRepair(ww::Logger& logger, const ww::ConfigLoadStatus& status) {
    if (!status.repaired) return;
    ww::LogRecord record;
    record.timestamp = ww::nowIsoLocal();
    record.action = "config_repaired";
    record.backup = ww::wideToUtf8(status.backup_path);
    record.error = status.error;
    logger.write(record);
}

static void addStartupNotices(ww::TrayIcon& tray, const ww::ConfigLoadStatus& configStatus, bool disabledForNoAdapter) {
    if (configStatus.repaired) {
        tray.addStartupNotice(
            L"\u914d\u7f6e\u5df2\u91cd\u7f6e",
            L"\u914d\u7f6e\u6587\u4ef6\u635f\u574f\uff0c\u5df2\u5907\u4efd\u5e76\u6062\u590d\u9ed8\u8ba4\u8bbe\u7f6e\u3002");
    }
    if (disabledForNoAdapter) {
        tray.addStartupNotice(
            L"\u672a\u68c0\u6d4b\u5230\u7f51\u7edc\u9002\u914d\u5668",
            L"\u5df2\u5173\u95ed\u9632\u62a4\u3002");
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    auto args = commandLineArgs();
    // v1.9: config.json and logs are now stored alongside the executable.
    // No need to pre-create directories — ConfigManager and Logger do it internally.

    ww::ConfigManager configManager;
    auto config = configManager.load();
    auto configLoadStatus = configManager.lastLoadStatus();
    ww::Logger logger;
    logger.rotate();
    logConfigRepair(logger, configLoadStatus);

    if (hasArg(args, L"--restore-shortcuts") || hasArg(args, L"--uninstall-restore")) {
        return restoreIfeoAndSave(configManager, logger, hasArg(args, L"--uninstall-restore"), !hasArg(args, L"--no-autostart-sync"));
    }

    if (hasArg(args, L"--status")) {
        consoleLine(L"配置: " + configManager.path());
        consoleLine(L"HTTP 端口: " + std::to_wstring(config.settings.http_port));
        consoleLine(config.settings.protection_enabled ? L"防护: 开启" : L"防护: 关闭");
        return 0;
    }

    fs::path launcher = fs::path(ww::executableDir()) / L"ww-launch.exe";
    if (!ww::fileExists(launcher.wstring())) {
        MessageBoxW(nullptr, L"启动器 ww-launch.exe 缺失，拦截无法生效。请重新安装。", L"WiFi 提醒", MB_OK | MB_ICONWARNING);
        restoreIfeoAndSave(configManager, logger);
        config = configManager.load();
    }

    std::string ignored;
    if (!hasArg(args, L"--no-autostart-sync")) {
        ww::setAutoStart(config.settings.auto_start, ww::executablePath(), &ignored);
    }

    bool disabledForNoAdapter = false;  // 不再因无 WiFi 适配器自动关闭防护（以太网机器会漏拦）

    ww::HttpServer server(config.settings.http_port, configManager, logger);
    if (!server.start()) {
        MessageBoxW(nullptr, L"无法启动本地设置服务，请检查端口是否被占用。", L"WiFi 提醒", MB_OK | MB_ICONERROR);
        return 1;
    }

    if (hasArg(args, L"--self-test-server")) {
        int durationMs = intArg(args, L"--self-test-server", 5000);
        consoleLine(L"self-test-server running on port " + std::to_wstring(config.settings.http_port));
        std::wstring readyPath = argValue(args, L"--ready-file");
        if (!readyPath.empty()) {
            ww::writeTextFileUtf8(readyPath, "ready:" + std::to_string(config.settings.http_port));
        }
            Sleep(static_cast<DWORD>(durationMs));
        server.stop();
        return 0;
    }

    if (hasArg(args, L"--self-test-tray")) {
        int durationMs = intArg(args, L"--self-test-tray", 2500);
        std::wstring readyPath = argValue(args, L"--ready-file");
        std::wstring command = argValue(args, L"--self-test-tray-command");
        int commandId = command == L"toggle" ? 1003 : 0;
            ww::TrayIcon tray(configManager, logger, config.settings.http_port);
        addStartupNotices(tray, configLoadStatus, disabledForNoAdapter);
        int code = tray.run(durationMs, readyPath, commandId);
        server.stop();
        return code;
    }

    if (!hasArg(args, L"--minimized")) {
        ww::openUrlInBrowser("http://localhost:" + std::to_string(config.settings.http_port) + "/");
    }

    ww::TrayIcon tray(configManager, logger, config.settings.http_port);
    addStartupNotices(tray, configLoadStatus, disabledForNoAdapter);
    int code = tray.run();
    server.stop();
    return code;
}
