#include "core/auto_start.h"
#include "core/config_manager.h"
#include "core/logger.h"
#include "core/network_manager.h"
#include "core/shortcut_manager.h"
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

static int restoreAllAndSave(ww::ConfigManager& configManager, ww::Logger& logger, bool uninstallMode = false, bool syncAutoStart = true) {
    auto config = configManager.load();
    auto results = ww::restoreAllShortcuts(config);
    int failures = 0;
    for (const auto& result : results) {
        ww::LogRecord record;
        record.timestamp = ww::nowIsoLocal();
        record.action = result.ok ? "shortcut_restored" : "shortcut_restore_failed";
        record.shortcut = result.shortcut;
        record.backup = result.backup;
        record.error = result.error;
        logger.write(record);
        if (!result.ok) ++failures;
    }
    ww::clearRestoredShortcutRecords(config, results);
    config.settings.protection_enabled = false;
    if (uninstallMode) {
        config.settings.auto_start = false;
        if (syncAutoStart) {
            std::string ignored;
            ww::setAutoStart(false, ww::executablePath(), &ignored);
        }
    }
    configManager.save(config);
    consoleLine(L"已尝试还原所有快捷方式，失败数: " + std::to_wstring(failures));
    return failures == 0 ? 0 : 2;
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
            L"\u5df2\u5173\u95ed\u9632\u62a4\u5e76\u5c1d\u8bd5\u8fd8\u539f\u5feb\u6377\u65b9\u5f0f\u3002");
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    auto args = commandLineArgs();
    ww::ensureDirectory(ww::appDataRoot());
    ww::ensureDirectory(ww::logsDir());

    ww::ConfigManager configManager;
    auto config = configManager.load();
    auto configLoadStatus = configManager.lastLoadStatus();
    ww::Logger logger;
    logger.rotate();
    logConfigRepair(logger, configLoadStatus);

    if (ww::reconcileShortcutRecords(config) > 0) {
        configManager.save(config);
    }

    if (hasArg(args, L"--restore-shortcuts") || hasArg(args, L"--uninstall-restore")) {
        return restoreAllAndSave(configManager, logger, hasArg(args, L"--uninstall-restore"), !hasArg(args, L"--no-autostart-sync"));
    }

    if (hasArg(args, L"--status")) {
        consoleLine(L"配置: " + configManager.path());
        consoleLine(L"HTTP 端口: " + std::to_wstring(config.settings.http_port));
        consoleLine(config.settings.protection_enabled ? L"防护: 开启" : L"防护: 关闭");
        return 0;
    }

    fs::path launcher = fs::path(ww::executableDir()) / L"ww-launch.exe";
    if (!ww::fileExists(launcher.wstring())) {
        restoreAllAndSave(configManager, logger);
        config = configManager.load();
        MessageBoxW(nullptr, L"启动器缺失，已尝试还原所有被替换的快捷方式。", L"WiFi 提醒", MB_OK | MB_ICONWARNING);
    }

    std::string ignored;
    if (!hasArg(args, L"--no-autostart-sync")) {
        ww::setAutoStart(config.settings.auto_start, ww::executablePath(), &ignored);
    }

    bool disabledForNoAdapter = false;
    auto startupNetwork = ww::getCurrentNetwork();
    if (ww::isNoNetworkAdapter(startupNetwork) && config.settings.protection_enabled) {
        restoreAllAndSave(configManager, logger);
        config = configManager.load();
        ww::LogRecord record;
        record.timestamp = ww::nowIsoLocal();
        record.action = "protection_disabled_no_adapter";
        record.error = startupNetwork.error;
        logger.write(record);
        disabledForNoAdapter = true;
    }

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
        ww::trimCurrentProcessWorkingSet();
        Sleep(static_cast<DWORD>(durationMs));
        server.stop();
        return 0;
    }

    if (hasArg(args, L"--self-test-tray")) {
        int durationMs = intArg(args, L"--self-test-tray", 2500);
        std::wstring readyPath = argValue(args, L"--ready-file");
        std::wstring command = argValue(args, L"--self-test-tray-command");
        int commandId = command == L"toggle" ? 1003 : 0;
        ww::trimCurrentProcessWorkingSet();
        ww::TrayIcon tray(configManager, logger, config.settings.http_port);
        addStartupNotices(tray, configLoadStatus, disabledForNoAdapter);
        int code = tray.run(durationMs, readyPath, commandId);
        server.stop();
        return code;
    }

    if (!hasArg(args, L"--minimized")) {
        ww::openUrlInBrowser("http://localhost:" + std::to_string(config.settings.http_port) + "/settings");
    }

    ww::trimCurrentProcessWorkingSet();
    ww::TrayIcon tray(configManager, logger, config.settings.http_port);
    addStartupNotices(tray, configLoadStatus, disabledForNoAdapter);
    int code = tray.run();
    server.stop();
    return code;
}
