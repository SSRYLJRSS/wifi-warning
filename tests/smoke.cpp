#include "core/auto_start.h"
#include "core/config_manager.h"
#include "core/json.h"
#include "core/logger.h"
#include "core/network_manager.h"
#include "core/rule_engine.h"
#include "core/shortcut_manager.h"
#include "core/util.h"
#include "core/wifi_detector.h"
#include "ui/http_server.h"

#include <winsock2.h>
#include <windows.h>

#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static std::string httpGet(int port, const std::string& path) {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return "";
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        WSACleanup();
        return "";
    }
    DWORD timeout = 2000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<u_short>(port));
    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(sock);
        WSACleanup();
        return "";
    }
    std::string request = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    send(sock, request.data(), static_cast<int>(request.size()), 0);
    std::string response;
    char buffer[2048];
    int received = 0;
    while ((received = recv(sock, buffer, sizeof(buffer), 0)) > 0) {
        response.append(buffer, buffer + received);
    }
    closesocket(sock);
    WSACleanup();
    return response;
}

static std::string httpPostJson(int port, const std::string& path, const std::string& body) {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return "";
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        WSACleanup();
        return "";
    }
    DWORD timeout = 2000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<u_short>(port));
    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(sock);
        WSACleanup();
        return "";
    }
    std::ostringstream request;
    request << "POST " << path << " HTTP/1.1\r\n"
            << "Host: localhost\r\n"
            << "Content-Type: application/json\r\n"
            << "Content-Length: " << body.size() << "\r\n"
            << "Connection: close\r\n\r\n"
            << body;
    std::string bytes = request.str();
    send(sock, bytes.data(), static_cast<int>(bytes.size()), 0);
    std::string response;
    char buffer[2048];
    int received = 0;
    while ((received = recv(sock, buffer, sizeof(buffer), 0)) > 0) {
        response.append(buffer, buffer + received);
    }
    closesocket(sock);
    WSACleanup();
    return response;
}

static std::string httpSplitHeaderGet(int port, const std::string& path) {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return "";
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        WSACleanup();
        return "";
    }
    DWORD timeout = 2000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<u_short>(port));
    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(sock);
        WSACleanup();
        return "";
    }
    std::string first = "GET " + path + " HTTP/1.1\r\nHo";
    std::string second = "st: localhost\r\nConnection: close\r\n\r\n";
    send(sock, first.data(), static_cast<int>(first.size()), 0);
    Sleep(20);
    send(sock, second.data(), static_cast<int>(second.size()), 0);
    std::string response;
    char buffer[2048];
    int received = 0;
    while ((received = recv(sock, buffer, sizeof(buffer), 0)) > 0) {
        response.append(buffer, buffer + received);
    }
    closesocket(sock);
    WSACleanup();
    return response;
}

static std::string launcherPath() {
    wchar_t buffer[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    fs::path current(buffer);
    fs::path launcher = current.parent_path() / L"ww-launch.exe";
    return ww::wideToUtf8(launcher.wstring());
}

static std::wstring quoteProcessArg(const std::wstring& value) {
    std::wstring out = L"\"";
    for (wchar_t ch : value) {
        if (ch == L'"') out += L'\\';
        out += ch;
    }
    out += L"\"";
    return out;
}

struct LauncherRunResult {
    std::string output;
    DWORD elapsed_ms = 0;
};

static LauncherRunResult runLauncherDry(const std::string& launcher, const std::string& configPath, const std::string& appPath, const std::string& ssid, const std::string& appArgs = "") {
    LauncherRunResult result;
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &security, 0)) return result;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    std::wstring command = quoteProcessArg(ww::utf8ToWide(launcher))
        + L" --dry-run --config " + quoteProcessArg(ww::utf8ToWide(configPath))
        + L" --app " + quoteProcessArg(ww::utf8ToWide(appPath))
        + L" --rule rule_test --ssid " + quoteProcessArg(ww::utf8ToWide(ssid));
    if (!appArgs.empty()) command += L" --app-args " + quoteProcessArg(ww::utf8ToWide(appArgs));

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};

    BOOL ok = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    CloseHandle(writePipe);
    if (!ok) {
        CloseHandle(readPipe);
        return result;
    }

    char buffer[512];
    DWORD read = 0;
    DWORD started = GetTickCount();
    while (ReadFile(readPipe, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
        result.output.append(buffer, buffer + read);
    }
    WaitForSingleObject(process.hProcess, 5000);
    result.elapsed_ms = GetTickCount() - started;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(readPipe);
    return result;
}

static LauncherRunResult runLauncherDryForNetwork(
    const std::string& launcher,
    const std::string& configPath,
    const std::string& appPath,
    const std::string& networkType,
    const std::string& networkId,
    const std::string& ruleId,
    const std::string& appArgs = "") {
    LauncherRunResult result;
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &security, 0)) return result;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    std::wstring command = quoteProcessArg(ww::utf8ToWide(launcher))
        + L" --dry-run --config " + quoteProcessArg(ww::utf8ToWide(configPath))
        + L" --app " + quoteProcessArg(ww::utf8ToWide(appPath))
        + L" --rule " + quoteProcessArg(ww::utf8ToWide(ruleId))
        + L" --network-type " + quoteProcessArg(ww::utf8ToWide(networkType))
        + L" --network-id " + quoteProcessArg(ww::utf8ToWide(networkId));
    if (!appArgs.empty()) command += L" --app-args " + quoteProcessArg(ww::utf8ToWide(appArgs));

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};

    BOOL ok = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    CloseHandle(writePipe);
    if (!ok) {
        CloseHandle(readPipe);
        return result;
    }

    char buffer[512];
    DWORD read = 0;
    DWORD started = GetTickCount();
    while (ReadFile(readPipe, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
        result.output.append(buffer, buffer + read);
    }
    WaitForSingleObject(process.hProcess, 5000);
    result.elapsed_ms = GetTickCount() - started;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(readPipe);
    return result;
}

static DWORD runLauncherWithEnv(const std::wstring& command, const std::vector<std::pair<std::wstring, std::wstring>>& env) {
    std::vector<std::pair<std::wstring, std::wstring>> oldValues;
    for (const auto& [key, value] : env) {
        DWORD needed = GetEnvironmentVariableW(key.c_str(), nullptr, 0);
        std::wstring previous;
        if (needed > 0) {
            previous.resize(needed);
            DWORD written = GetEnvironmentVariableW(key.c_str(), previous.data(), needed);
            previous.resize(written);
        }
        oldValues.push_back({key, previous});
        SetEnvironmentVariableW(key.c_str(), value.c_str());
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::wstring mutableCommand = command;
    BOOL ok = CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    DWORD exitCode = 0xFFFFFFFF;
    if (ok) {
        WaitForSingleObject(process.hProcess, 5000);
        GetExitCodeProcess(process.hProcess, &exitCode);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }

    for (const auto& [key, previous] : oldValues) {
        SetEnvironmentVariableW(key.c_str(), previous.empty() ? nullptr : previous.c_str());
    }
    return exitCode;
}

static DWORD runServiceCommand(const std::vector<std::wstring>& args) {
    fs::path service = fs::path(ww::utf8ToWide(launcherPath())).parent_path() / L"wifi-warning.exe";
    std::wstring command = quoteProcessArg(service.wstring());
    for (const auto& arg : args) command += L" " + quoteProcessArg(arg);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    BOOL ok = CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    if (!ok) return 0xFFFFFFFF;
    WaitForSingleObject(process.hProcess, 5000);
    DWORD exitCode = 0;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return exitCode;
}

static int fail(const std::string& message) {
    std::cerr << "FAIL: " << message << "\n";
    return 1;
}

static fs::path smokeRoot() {
    std::wstring overrideRoot = ww::getEnvWide(L"WW_SMOKE_ROOT");
    if (!overrideRoot.empty()) {
        std::error_code ec;
        fs::create_directories(overrideRoot, ec);
        return fs::path(overrideRoot);
    }
    fs::path root = fs::current_path() / L"build" / L"smoke";
    std::error_code ec;
    fs::create_directories(root, ec);
    return root;
}

int wmain(int argc, wchar_t** argv) {
    if (argc >= 3 && wcscmp(argv[1], L"--scan") == 0) {
        std::string target = ww::wideToUtf8(argv[2]);
        ww::JsonValue::Array rootsJson;
        for (const auto& root : ww::shortcutScanRoots()) {
            rootsJson.push_back(root);
        }
        auto shortcuts = ww::findShortcutsForApp(target);
        ww::JsonValue::Array shortcutsJson;
        for (const auto& shortcut : shortcuts) {
            shortcutsJson.push_back(ww::JsonValue::Object{
                {"path", shortcut.path},
                {"target_path", shortcut.target_path},
                {"arguments", shortcut.arguments},
                {"working_dir", shortcut.working_dir},
                {"icon_path", shortcut.icon_path},
                {"icon_index", shortcut.icon_index},
                {"description", shortcut.description}
            });
        }
        std::cout << ww::stringifyJson(ww::JsonValue::Object{
            {"target", target},
            {"roots", rootsJson},
            {"count", static_cast<int>(shortcuts.size())},
            {"shortcuts", shortcutsJson}
        }) << std::endl;
        return shortcuts.empty() ? 1 : 0;
    }

    fs::path root = smokeRoot();
    fs::path configPath = root / L"config.json";
    fs::path logDir = root / L"logs";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    ww::ConfigManager manager(configPath.wstring());
    auto config = manager.load();
    if (config.settings.http_port != 18765) return fail("default port mismatch");

    ww::Rule rule;
    rule.id = "rule_test";
    rule.ssid = "Office-WiFi";
    rule.safe_wifi_ssid = "Home-WiFi";
    rule.safe_wifi_password = "safe-secret";
    rule.description = "smoke";
    fs::path fakeApp = root / L"Program Files" / L"Test" / L"app.exe";
    fs::create_directories(fakeApp.parent_path(), ec);
    ww::writeTextFileUtf8(fakeApp.wstring(), "fake app");
    std::string fakeAppPath = ww::wideToUtf8(fakeApp.wstring());

    rule.blocked_apps.push_back({
        "Test App",
        fakeAppPath,
        fakeAppPath,
        {},
        {}
    });
    fs::path missingApp = root / L"Program Files" / L"Missing" / L"gone.exe";
    std::string missingAppPath = ww::wideToUtf8(missingApp.wstring());
    rule.blocked_apps.push_back({
        "Missing App",
        missingAppPath,
        missingAppPath,
        {},
        {}
    });
    ww::AppGroup group;
    group.id = "group_test";
    group.name = "Smoke Group";
    group.apps = rule.blocked_apps;
    for (auto& app : group.apps) app.replaced_shortcuts.clear();
    rule.app_group_id = group.id;

    ww::Rule wiredRule = rule;
    wiredRule.id = "rule_wired";
    wiredRule.ssid = "";
    wiredRule.network_type = "wired";
    wiredRule.network_id = "Ethernet 1";
    wiredRule.network_name = "Ethernet 1";
    wiredRule.safe_wifi_ssid = "";
    wiredRule.safe_wifi_password = "";
    wiredRule.description = "wired smoke";
    wiredRule.blocked_apps = {rule.blocked_apps[0]};

    config.app_groups.push_back(group);
    config.rules.push_back(rule);
    config.rules.push_back(wiredRule);
    config.settings.bypass_password = ww::sha256Hex("secret");
    if (!manager.save(config)) return fail("config save failed");

    auto loaded = manager.load();
    auto match = ww::findBlockingRule(loaded, "Office-WiFi", fakeAppPath);
    if (!match || match->rule.id != "rule_test") return fail("rule engine did not match");
    if (ww::findBlockingRule(loaded, "Home-WiFi", fakeAppPath)) return fail("rule engine matched safe ssid");
    auto wiredMatch = ww::findBlockingRuleForNetwork(loaded, ww::NetworkIdentity{"wired", "Ethernet 1", "Ethernet 1"}, fakeAppPath);
    if (!wiredMatch || wiredMatch->rule.id != "rule_wired") return fail("rule engine did not match wired network");
    if (ww::findBlockingRuleForNetwork(loaded, ww::NetworkIdentity{"wired", "Ethernet 2", "Ethernet 2"}, fakeAppPath)) {
        return fail("rule engine matched wrong wired network");
    }

    std::wstring autoStartTestKey = L"Software\\WiFiWarningSmoke\\" + std::to_wstring(GetCurrentProcessId());
    SetEnvironmentVariableW(L"WW_TEST_AUTOSTART_RUN_KEY", autoStartTestKey.c_str());
    HKEY autoStartKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, autoStartTestKey.c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &autoStartKey, nullptr) != ERROR_SUCCESS) {
        SetEnvironmentVariableW(L"WW_TEST_AUTOSTART_RUN_KEY", nullptr);
        return fail("autostart smoke registry key create failed");
    }
    RegCloseKey(autoStartKey);
    std::string autoStartError;
    if (!ww::setAutoStart(true, fakeApp.wstring(), &autoStartError)) {
        RegDeleteTreeW(HKEY_CURRENT_USER, autoStartTestKey.c_str());
        SetEnvironmentVariableW(L"WW_TEST_AUTOSTART_RUN_KEY", nullptr);
        return fail("autostart enable failed: " + autoStartError);
    }
    if (!ww::isAutoStartEnabled()) {
        RegDeleteTreeW(HKEY_CURRENT_USER, autoStartTestKey.c_str());
        SetEnvironmentVariableW(L"WW_TEST_AUTOSTART_RUN_KEY", nullptr);
        return fail("autostart enabled state was not detected");
    }
    if (RegOpenKeyExW(HKEY_CURRENT_USER, autoStartTestKey.c_str(), 0, KEY_QUERY_VALUE, &autoStartKey) != ERROR_SUCCESS) {
        RegDeleteTreeW(HKEY_CURRENT_USER, autoStartTestKey.c_str());
        SetEnvironmentVariableW(L"WW_TEST_AUTOSTART_RUN_KEY", nullptr);
        return fail("autostart smoke registry key open failed");
    }
    std::wstring autoStartValue(1024, L'\0');
    DWORD autoStartType = 0;
    DWORD autoStartBytes = static_cast<DWORD>(autoStartValue.size() * sizeof(wchar_t));
    LONG autoStartQuery = RegQueryValueExW(autoStartKey, L"WiFiWarning", nullptr, &autoStartType, reinterpret_cast<BYTE*>(autoStartValue.data()), &autoStartBytes);
    RegCloseKey(autoStartKey);
    autoStartValue.resize(autoStartBytes > 0 ? (autoStartBytes / sizeof(wchar_t)) : 0);
    while (!autoStartValue.empty() && autoStartValue.back() == L'\0') autoStartValue.pop_back();
    if (autoStartQuery != ERROR_SUCCESS || autoStartType != REG_SZ || autoStartValue.find(L"--minimized") == std::wstring::npos || autoStartValue.find(fakeApp.wstring()) == std::wstring::npos) {
        RegDeleteTreeW(HKEY_CURRENT_USER, autoStartTestKey.c_str());
        SetEnvironmentVariableW(L"WW_TEST_AUTOSTART_RUN_KEY", nullptr);
        return fail("autostart registry value mismatch");
    }
    if (!ww::setAutoStart(false, fakeApp.wstring(), &autoStartError) || ww::isAutoStartEnabled()) {
        RegDeleteTreeW(HKEY_CURRENT_USER, autoStartTestKey.c_str());
        SetEnvironmentVariableW(L"WW_TEST_AUTOSTART_RUN_KEY", nullptr);
        return fail("autostart disable failed");
    }
    RegDeleteTreeW(HKEY_CURRENT_USER, autoStartTestKey.c_str());
    SetEnvironmentVariableW(L"WW_TEST_AUTOSTART_RUN_KEY", nullptr);

    std::string error;
    auto parsed = ww::parseJson(ww::stringifyJson(ww::configToJson(loaded)), &error);
    if (!parsed) return fail("config json roundtrip failed: " + error);
    auto roundtrip = ww::configFromJson(*parsed);
    if (roundtrip.app_groups.size() != 1 || roundtrip.app_groups[0].apps.size() != 2 || roundtrip.rules[0].app_group_id != "group_test") {
        return fail("config json roundtrip lost app groups");
    }
    bool foundWiredRoundtrip = false;
    for (const auto& item : roundtrip.rules) {
        if (item.id == "rule_wired" && item.network_type == "wired" && item.network_id == "Ethernet 1") {
            foundWiredRoundtrip = true;
            break;
        }
    }
    if (!foundWiredRoundtrip) return fail("config json roundtrip lost wired rule fields");

    ww::Logger logger(logDir.wstring());
    ww::LogRecord record;
    record.timestamp = ww::nowIsoLocal();
    record.ssid = "Office-WiFi";
    record.app = "app.exe";
    record.action = "blocked";
    record.rule_id = "rule_test";
    if (!logger.write(record)) return fail("logger write failed");
    ww::LogRecord detailRecord;
    detailRecord.timestamp = ww::nowIsoLocal();
    detailRecord.action = "shortcut_replace_failed";
    detailRecord.shortcut = "bad.lnk";
    detailRecord.backup = "bad.lnk.backup";
    detailRecord.error = "permission denied";
    if (!logger.write(detailRecord)) return fail("logger detail write failed");
    auto recentRecords = logger.readRecent(30);
    bool foundDetailRecord = false;
    for (const auto& item : recentRecords) {
        if (item.action == "shortcut_replace_failed" && item.shortcut == "bad.lnk" && item.error == "permission denied") {
            foundDetailRecord = true;
            break;
        }
    }
    if (!foundDetailRecord) return fail("logger detail record was not readable");
    auto stats = logger.statsJson(30);
    const auto* blockedWeek = stats.get("blocked_week");
    if (!blockedWeek || blockedWeek->asNumber() < 1) return fail("logger stats missing blocked event");

    fs::path corruptConfigPath = root / L"corrupt-config.json";
    ww::writeTextFileUtf8(corruptConfigPath.wstring(), "{not-json");
    ww::ConfigManager corruptManager(corruptConfigPath.wstring());
    auto repairedConfig = corruptManager.load();
    auto repairStatus = corruptManager.lastLoadStatus();
    if (!repairStatus.repaired || repairStatus.backup_path.empty() || repairStatus.error.empty()) return fail("corrupt config repair status missing");
    if (!fs::exists(repairStatus.backup_path, ec)) return fail("corrupt config backup file missing");
    if (repairedConfig.settings.http_port != 18765) return fail("corrupt config did not reset defaults");

    fs::path shortcutRoots = root / L"ShortcutRoots";
    fs::path desktopRoot = shortcutRoots / L"Desktop";
    fs::path startMenuRoot = shortcutRoots / L"Start Menu";
    fs::path taskbarRoot = shortcutRoots / L"TaskBar";
    fs::create_directories(desktopRoot, ec);
    fs::create_directories(startMenuRoot / L"Programs", ec);
    fs::create_directories(taskbarRoot, ec);

    fs::path originalShortcut = root / L"real-shortcut.lnk";
    fs::path fakeLauncher = root / L"ww-launch.exe";
    std::string originalAppArgs = "--profile smoke --flag";
    ww::writeTextFileUtf8(fakeLauncher.wstring(), "fake launcher");
    ww::ShortcutCandidate sourceShortcut;
    sourceShortcut.path = ww::wideToUtf8(originalShortcut.wstring());
    sourceShortcut.target_path = fakeAppPath;
    sourceShortcut.working_dir = ww::wideToUtf8(fakeApp.parent_path().wstring());
    sourceShortcut.icon_path = fakeAppPath;
    sourceShortcut.icon_index = 7;
    sourceShortcut.description = "Smoke shortcut";

    std::string shortcutError;
    if (!ww::createShortcutFile(sourceShortcut.path, fakeAppPath, originalAppArgs, sourceShortcut, &shortcutError)) {
        return fail("create real shortcut failed: " + shortcutError);
    }
    fs::path restoreBackup = root / L"restore-backup.lnk";
    fs::path restoreOriginal = root / L"restore-original.lnk";
    sourceShortcut.path = ww::wideToUtf8(restoreBackup.wstring());
    if (!ww::createShortcutFile(sourceShortcut.path, fakeAppPath, originalAppArgs, sourceShortcut, &shortcutError)) {
        return fail("create restore backup shortcut failed: " + shortcutError);
    }
    sourceShortcut.path = ww::wideToUtf8(restoreOriginal.wstring());
    if (!ww::createShortcutFile(sourceShortcut.path, ww::wideToUtf8(fakeLauncher.wstring()), "", sourceShortcut, &shortcutError)) {
        return fail("create restore original shortcut failed: " + shortcutError);
    }
    auto restore = ww::restoreShortcut({ww::wideToUtf8(restoreOriginal.wstring()), ww::wideToUtf8(restoreBackup.wstring())});
    if (!restore.ok) return fail("shortcut restore failed: " + restore.error);
    ww::ShortcutCandidate restoreCheck;
    if (!ww::readShortcutFile(ww::wideToUtf8(restoreOriginal.wstring()), restoreCheck)) return fail("read restored shortcut fixture failed");
    if (ww::toLowerAscii(restoreCheck.target_path) != ww::toLowerAscii(fakeAppPath)) return fail("shortcut restore target mismatch");
    ww::AppConfig restoreRecordConfig = ww::ConfigManager::defaults();
    restoreRecordConfig.rules.push_back(rule);
    restoreRecordConfig.rules[0].blocked_apps[0].replaced_shortcuts = {
        {"C:\\Smoke\\ok.lnk", "C:\\Smoke\\ok-backup.lnk"},
        {"C:\\Smoke\\failed.lnk", "C:\\Smoke\\failed-backup.lnk"}
    };
    restoreRecordConfig.rules.push_back(rule);
    restoreRecordConfig.rules[1].id = "rule_second";
    restoreRecordConfig.rules[1].blocked_apps[0].replaced_shortcuts = {
        {"C:\\Smoke\\ok.lnk", "C:\\Smoke\\ok-backup.lnk"}
    };
    ww::clearRestoredShortcutRecords(restoreRecordConfig, {
        {true, "C:\\Smoke\\ok.lnk", "C:\\Smoke\\ok-backup.lnk", ""},
        {false, "C:\\Smoke\\failed.lnk", "C:\\Smoke\\failed-backup.lnk", "restore failed"}
    });
    if (restoreRecordConfig.rules[0].blocked_apps[0].replaced_shortcuts.size() != 1 ||
        restoreRecordConfig.rules[0].blocked_apps[0].replaced_shortcuts[0].original_lnk != "C:\\Smoke\\failed.lnk" ||
        !restoreRecordConfig.rules[1].blocked_apps[0].replaced_shortcuts.empty()) {
        return fail("successful restore records were not cleared while failures were kept");
    }

    sourceShortcut.path = ww::wideToUtf8(originalShortcut.wstring());
    ww::ShortcutCandidate readOriginal;
    if (!ww::readShortcutFile(sourceShortcut.path, readOriginal)) return fail("read original shortcut failed");
    if (readOriginal.target_path.empty()) return fail("original shortcut target missing");
    if (readOriginal.arguments != originalAppArgs) return fail("original shortcut arguments mismatch");
    if (readOriginal.icon_index != sourceShortcut.icon_index) return fail("original shortcut icon index mismatch");

    auto replaceResult = ww::replaceShortcut(readOriginal, ww::wideToUtf8(fakeLauncher.wstring()), fakeAppPath, "rule_test");
    if (!replaceResult.ok) return fail("replace real shortcut failed: " + replaceResult.error);
    if (replaceResult.backup.empty() || replaceResult.backup.find(".lnk.backup") != std::string::npos) {
        return fail("replace real shortcut used visible sidecar backup");
    }
    fs::path visibleBackup = originalShortcut;
    visibleBackup += L".backup";
    if (fs::exists(visibleBackup, ec)) return fail("replace real shortcut left visible backup beside shortcut");
    ww::ShortcutCandidate readReplacement;
    if (!ww::readShortcutFile(sourceShortcut.path, readReplacement)) return fail("read replacement shortcut failed");
    if (ww::toLowerAscii(readReplacement.target_path) != ww::toLowerAscii(ww::wideToUtf8(fakeLauncher.wstring()))) {
        return fail("replacement shortcut target mismatch");
    }
    if (readReplacement.arguments.find("--app-args") == std::string::npos || readReplacement.arguments.find(originalAppArgs) == std::string::npos) {
        return fail("replacement shortcut arguments did not preserve original app args");
    }
    if (readReplacement.icon_index != sourceShortcut.icon_index) return fail("replacement shortcut icon index mismatch");

    std::vector<fs::path> scannedShortcutPaths = {
        desktopRoot / L"Test App.lnk",
        startMenuRoot / L"Programs" / L"Test App Menu.lnk",
        taskbarRoot / L"Test App Pinned.LNK"
    };
    for (const auto& shortcutPath : scannedShortcutPaths) {
        sourceShortcut.path = ww::wideToUtf8(shortcutPath.wstring());
        if (!ww::createShortcutFile(sourceShortcut.path, fakeAppPath, originalAppArgs, sourceShortcut, &shortcutError)) {
            return fail("create scanned shortcut failed: " + shortcutError);
        }
    }
    std::string selectedShortcutPath = ww::wideToUtf8(scannedShortcutPaths[0].wstring());
    config = manager.load();
    config.rules[0].blocked_apps[0].shortcut_paths = {selectedShortcutPath};
    config.app_groups[0].apps[0].shortcut_paths = {selectedShortcutPath};
    if (!manager.save(config)) return fail("save selected shortcut fixture failed");

    fs::path otherApp = root / L"Program Files" / L"Other" / L"other.exe";
    fs::create_directories(otherApp.parent_path(), ec);
    ww::writeTextFileUtf8(otherApp.wstring(), "other app");
    sourceShortcut.path = ww::wideToUtf8((desktopRoot / L"Other App.lnk").wstring());
    sourceShortcut.target_path = ww::wideToUtf8(otherApp.wstring());
    sourceShortcut.working_dir = ww::wideToUtf8(otherApp.parent_path().wstring());
    sourceShortcut.icon_path = sourceShortcut.target_path;
    sourceShortcut.icon_index = 0;
    if (!ww::createShortcutFile(sourceShortcut.path, sourceShortcut.target_path, "", sourceShortcut, &shortcutError)) {
        return fail("create unrelated shortcut failed: " + shortcutError);
    }

    std::wstring rootOverride = desktopRoot.wstring() + L";" + startMenuRoot.wstring() + L";" + taskbarRoot.wstring();
    SetEnvironmentVariableW(L"WW_SHORTCUT_ROOTS", rootOverride.c_str());
    SetEnvironmentVariableW(L"WW_TEST_CURRENT_SSID", L"Office-WiFi");
    SetEnvironmentVariableW(
        L"WW_TEST_AVAILABLE_WIFI_JSON",
        L"[{\"ssid\":\"Office-WiFi\",\"signal_quality\":80,\"connected\":true,\"secure\":true,\"auth\":\"WPA2-PSK\"},"
        L"{\"ssid\":\"Home-WiFi\",\"signal_quality\":91,\"connected\":false,\"secure\":true,\"auth\":\"WPA2-PSK\"}]");
    SetEnvironmentVariableW(L"WW_TEST_CONNECT_WIFI", L"success");
    SetEnvironmentVariableW(L"WW_TEST_PICK_APPS_JSON", L"[{\"path\":\"C:\\\\Picked\\\\Focus.exe\",\"name\":\"Focus App\"}]");
    std::string pickedShortcutJsonUtf8 = "[\"" + ww::jsonEscape(selectedShortcutPath) + "\"]";
    std::wstring pickedShortcutJson = ww::utf8ToWide(pickedShortcutJsonUtf8);
    SetEnvironmentVariableW(L"WW_TEST_PICK_SHORTCUTS_JSON", pickedShortcutJson.c_str());
    auto discoveredShortcuts = ww::findShortcutsForApp(fakeAppPath);
    if (discoveredShortcuts.size() != scannedShortcutPaths.size()) {
        return fail("shortcut scanner found " + std::to_string(discoveredShortcuts.size()) + " target shortcuts");
    }

    fs::path testSsidFile = root / L"current-ssid.txt";
    ww::writeTextFileUtf8(testSsidFile.wstring(), "Home-WiFi");
    SetEnvironmentVariableW(L"WW_TEST_CURRENT_SSID_FILE", testSsidFile.wstring().c_str());
    auto fileWifi = ww::getCurrentWifi();
    if (!fileWifi.connected || fileWifi.ssid != "Home-WiFi") return fail("wifi detector file override failed");
    ww::writeTextFileUtf8(testSsidFile.wstring(), "Office-WiFi");

    SetEnvironmentVariableW(L"WW_TEST_CURRENT_NETWORK_TYPE", L"wired");
    SetEnvironmentVariableW(L"WW_TEST_CURRENT_NETWORK_ID", L"Ethernet 1");
    SetEnvironmentVariableW(
        L"WW_TEST_WIRED_ADAPTERS_JSON",
        L"[{\"id\":\"Ethernet 1\",\"name\":\"Ethernet 1\",\"connected\":true,\"enabled\":true,\"status\":\"up\"},"
        L"{\"id\":\"Ethernet Backup\",\"name\":\"Ethernet Backup\",\"connected\":false,\"enabled\":true,\"status\":\"down\"}]");
    SetEnvironmentVariableW(L"WW_TEST_WIRED_ACTION", L"success");
    auto currentNetwork = ww::getCurrentNetwork();
    if (!currentNetwork.connected || currentNetwork.type != "wired" || currentNetwork.id != "Ethernet 1") {
        return fail("network manager wired override failed");
    }
    auto wiredAdapters = ww::listWiredAdapters();
    if (wiredAdapters.size() != 2 || wiredAdapters[0].id != "Ethernet 1" || !wiredAdapters[0].connected) {
        return fail("network manager wired adapter override failed");
    }
    auto wiredAction = ww::setWiredAdapterEnabled("Ethernet 1", false);
    if (!wiredAction.ok || !wiredAction.requested || wiredAction.status != "disable_requested") {
        return fail("network manager wired action override failed");
    }

    ww::AppConfig restoreViaApiConfig = manager.load();
    restoreViaApiConfig.rules[0].blocked_apps[0].replaced_shortcuts = {{replaceResult.shortcut, replaceResult.backup}};
    restoreViaApiConfig.settings.protection_enabled = true;
    if (!manager.save(restoreViaApiConfig)) return fail("save api restore fixture failed");

    constexpr int smokePort = 18766;
    auto httpConfig = manager.load();
    httpConfig.settings.http_port = smokePort;
    if (!manager.save(httpConfig)) return fail("config save for http smoke failed");
    ww::HttpServer server(smokePort, manager, logger);
    if (!server.start()) return fail("http server did not start");
    Sleep(160);
    std::string configResponse = httpGet(smokePort, "/api/config");
    std::string splitHeaderResponse = httpSplitHeaderGet(smokePort, "/api/config");
    std::string largeHeaderResponse = httpGet(smokePort, "/" + std::string(20000, 'a'));
    std::string settingsResponse = httpGet(smokePort, "/settings");
    std::string faviconResponse = httpGet(smokePort, "/favicon.ico");
    std::string wifiCurrentResponse = httpGet(smokePort, "/api/wifi/current");
    std::string networkCurrentResponse = httpGet(smokePort, "/api/network/current");
    std::string wiredAdaptersResponse = httpGet(smokePort, "/api/network/wired");
    std::string wiredToggleBody = ww::stringifyJson(ww::JsonValue::Object{{"id", "Ethernet 1"}, {"enabled", false}});
    std::string wiredToggleResponse = httpPostJson(smokePort, "/api/network/wired/toggle", wiredToggleBody);
    std::string wifiAvailableResponse = httpGet(smokePort, "/api/wifi/available");
    std::string wifiSwitchBody = ww::stringifyJson(ww::JsonValue::Object{{"ssid", "Home-WiFi"}});
    std::string wifiSwitchResponse = httpPostJson(smokePort, "/api/wifi/switch", wifiSwitchBody);
    SetEnvironmentVariableW(L"WW_TEST_CONNECT_WIFI", L"dialog");
    std::string wifiDialogBody = ww::stringifyJson(ww::JsonValue::Object{{"ssid", "Cafe-WiFi"}});
    std::string wifiDialogResponse = httpPostJson(smokePort, "/api/wifi/switch", wifiDialogBody);
    SetEnvironmentVariableW(L"WW_TEST_CONNECT_WIFI", L"request");
    std::string wifiRequestBody = ww::stringifyJson(ww::JsonValue::Object{{"ssid", "Pending-WiFi"}});
    std::string wifiRequestResponse = httpPostJson(smokePort, "/api/wifi/switch", wifiRequestBody);
    SetEnvironmentVariableW(L"WW_TEST_CONNECT_WIFI", L"success");
    SetEnvironmentVariableW(L"WW_TEST_CONNECT_WIFI", L"password:Home-WiFi:safe-secret");
    std::string wifiPasswordBody = ww::stringifyJson(ww::JsonValue::Object{{"ssid", "Home-WiFi"}, {"password", "safe-secret"}});
    std::string wifiPasswordResponse = httpPostJson(smokePort, "/api/wifi/switch", wifiPasswordBody);
    SetEnvironmentVariableW(L"WW_TEST_CONNECT_WIFI", L"password:Home-WiFi:safe-secret;Home-WiFi");
    std::string wifiMultiModeBody = ww::stringifyJson(ww::JsonValue::Object{{"ssid", "Home-WiFi"}});
    std::string wifiMultiModeResponse = httpPostJson(smokePort, "/api/wifi/switch", wifiMultiModeBody);
    SetEnvironmentVariableW(L"WW_TEST_CONNECT_WIFI", L"success");
    std::string bypassWrongBody = ww::stringifyJson(ww::JsonValue::Object{{"password", "wrong"}, {"app", ""}, {"rule_id", "rule_test"}});
    std::string bypassWrongResponse = httpPostJson(smokePort, "/api/bypass", bypassWrongBody);
    std::string bypassBody = ww::stringifyJson(ww::JsonValue::Object{{"password", "secret"}, {"app", ""}, {"app_args", originalAppArgs}, {"rule_id", "rule_test"}});
    std::string bypassResponse = httpPostJson(smokePort, "/api/bypass", bypassBody);
    auto afterBypass = manager.load();
    auto afterBypassJson = ww::configToJson(afterBypass);
    const auto* afterBypassSettings = afterBypassJson.get("settings");
    // v1.7: bypass API now sets bypass_until_epoch for timed bypass window
    const auto* bypassEpoch = afterBypassSettings ? afterBypassSettings->get("bypass_until_epoch") : nullptr;
    if (!bypassEpoch || bypassEpoch->asNumber() <= 0) {
        server.stop();
        return fail("http /api/bypass did not set bypass_until_epoch");
    }
    // Reset bypass_until_epoch so subsequent launcher tests are not affected
    {
        auto resetConfig = manager.load();
        resetConfig.settings.bypass_until_epoch = 0;
        if (!manager.save(resetConfig)) {
            server.stop();
            return fail("failed to reset bypass_until_epoch after bypass test");
        }
    }
    std::string warningResponse = httpGet(smokePort, "/warning?appName=Test%20App&app=" + ww::urlEncode(fakeAppPath) + "&ssid=Office-WiFi&ruleId=rule_test");
    std::string warningJsResponse = httpGet(smokePort, "/js/warning.js");
    std::string appIconResponse = httpGet(smokePort, "/api/apps/icon?path=" + ww::urlEncode(launcherPath()));
    std::string pickerResponse = httpGet(smokePort, "/wifi-picker");
    std::string statsResponse = httpGet(smokePort, "/api/stats");
    std::string appBrowseResponse = httpPostJson(smokePort, "/api/apps/browse", "{}");
    std::string shortcutBrowseResponse = httpPostJson(smokePort, "/api/shortcuts/browse", "{}");
    std::string shortcutReadBody = ww::stringifyJson(ww::JsonValue::Object{{"shortcut_path", selectedShortcutPath}});
    std::string shortcutReadResponse = httpPostJson(smokePort, "/api/shortcuts/read", shortcutReadBody);
    std::string appStatusResponse = httpGet(smokePort, "/api/apps/status");
    std::string appCleanupBody = ww::stringifyJson(ww::JsonValue::Object{{"rule_id", "rule_test"}, {"app_path", missingAppPath}});
    std::string appCleanupResponse = httpPostJson(smokePort, "/api/apps/cleanup", appCleanupBody);
    std::string scanBody = ww::stringifyJson(ww::JsonValue::Object{{"app_path", fakeAppPath}});
    std::string scanResponse = httpPostJson(smokePort, "/api/shortcuts/scan", scanBody);
    std::string replaceBody = ww::stringifyJson(ww::JsonValue::Object{{"app_path", fakeAppPath}, {"rule_id", "rule_test"}});
    SetEnvironmentVariableW(L"WW_TEST_SHORTCUT_REPLACE_FAIL", L"1");
    std::string replaceFailResponse = httpPostJson(smokePort, "/api/shortcuts/replace", replaceBody);
    SetEnvironmentVariableW(L"WW_TEST_SHORTCUT_REPLACE_FAIL", nullptr);
    std::string replaceFailStatsResponse = httpGet(smokePort, "/api/stats");
    std::string replaceResponse = httpPostJson(smokePort, "/api/shortcuts/replace", replaceBody);
    auto afterShortcutReplace = manager.load();
    bool missingAppStillConfigured = false;
    for (const auto& app : afterShortcutReplace.rules[0].blocked_apps) {
        if (ww::toLowerAscii(app.original_path) == ww::toLowerAscii(missingAppPath)) missingAppStillConfigured = true;
    }
    if (missingAppStillConfigured) {
        server.stop();
        return fail("app cleanup API did not remove missing app");
    }
    if (afterShortcutReplace.rules[0].blocked_apps[0].replaced_shortcuts.size() != 2) {
        server.stop();
        return fail("shortcut replace API did not persist selected replacement records");
    }
    autoStartTestKey = L"Software\\WiFiWarningSmoke\\Api\\" + std::to_wstring(GetCurrentProcessId());
    SetEnvironmentVariableW(L"WW_TEST_AUTOSTART_RUN_KEY", autoStartTestKey.c_str());
    auto autoStartApiConfig = manager.load();
    autoStartApiConfig.settings.auto_start = true;
    std::string autoStartApiBody = ww::stringifyJson(ww::JsonValue::Object{{"config", ww::configToJson(autoStartApiConfig)}});
    std::string autoStartApiResponse = httpPostJson(smokePort, "/api/config", autoStartApiBody);
    std::string autoStartApiGetResponse = httpGet(smokePort, "/api/config");
    if (autoStartApiResponse.find("200 OK") == std::string::npos || autoStartApiResponse.find("\"auto_start_synced\": true") == std::string::npos || autoStartApiResponse.find("\"auto_start_registered\": true") == std::string::npos) {
        server.stop();
        RegDeleteTreeW(HKEY_CURRENT_USER, autoStartTestKey.c_str());
        SetEnvironmentVariableW(L"WW_TEST_AUTOSTART_RUN_KEY", nullptr);
        return fail("http /api/config autostart enable sync failed");
    }
    if (autoStartApiGetResponse.find("\"auto_start_registered\": true") == std::string::npos) {
        server.stop();
        RegDeleteTreeW(HKEY_CURRENT_USER, autoStartTestKey.c_str());
        SetEnvironmentVariableW(L"WW_TEST_AUTOSTART_RUN_KEY", nullptr);
        return fail("http /api/config autostart registered state missing");
    }
    autoStartApiConfig = manager.load();
    autoStartApiConfig.settings.auto_start = false;
    autoStartApiBody = ww::stringifyJson(ww::JsonValue::Object{{"config", ww::configToJson(autoStartApiConfig)}});
    autoStartApiResponse = httpPostJson(smokePort, "/api/config", autoStartApiBody);
    if (autoStartApiResponse.find("200 OK") == std::string::npos || autoStartApiResponse.find("\"auto_start_synced\": true") == std::string::npos || autoStartApiResponse.find("\"auto_start_registered\": false") == std::string::npos) {
        server.stop();
        RegDeleteTreeW(HKEY_CURRENT_USER, autoStartTestKey.c_str());
        SetEnvironmentVariableW(L"WW_TEST_AUTOSTART_RUN_KEY", nullptr);
        return fail("http /api/config autostart disable sync failed");
    }
    RegDeleteTreeW(HKEY_CURRENT_USER, autoStartTestKey.c_str());
    SetEnvironmentVariableW(L"WW_TEST_AUTOSTART_RUN_KEY", nullptr);
    auto disableConfig = manager.load();
    disableConfig.settings.protection_enabled = false;
    std::string disableBody = ww::stringifyJson(ww::JsonValue::Object{{"config", ww::configToJson(disableConfig)}});
    std::string disableResponse = httpPostJson(smokePort, "/api/config", disableBody);
    auto fallbackConfig = manager.load();
    fallbackConfig.settings.protection_enabled = true;
    if (!manager.save(fallbackConfig)) {
        server.stop();
        return fail("save launcher browser fallback fixture failed");
    }
    fs::path fallbackMarker = root / L"browser-fallback.txt";
    std::wstring fallbackCommand = quoteProcessArg(ww::utf8ToWide(launcherPath()))
        + L" --no-launch --config " + quoteProcessArg(configPath.wstring())
        + L" --app " + quoteProcessArg(ww::utf8ToWide(fakeAppPath))
        + L" --rule rule_test --ssid Office-WiFi";
    DWORD fallbackExit = runLauncherWithEnv(fallbackCommand, {
        {L"WW_TEST_SHELLOPEN_FAIL", L"1"},
        {L"WW_TEST_NO_MESSAGEBOX", L"1"},
        {L"WW_TEST_FALLBACK_FILE", fallbackMarker.wstring()}
    });
    std::string fallbackReason = ww::readTextFileUtf8(fallbackMarker.wstring());
    server.stop();
    auto serviceFallbackConfig = manager.load();
    serviceFallbackConfig.settings.protection_enabled = true;
    if (!manager.save(serviceFallbackConfig)) return fail("save launcher service fallback fixture failed");
    fs::path serviceFallbackMarker = root / L"service-fallback.txt";
    DWORD serviceFallbackExit = runLauncherWithEnv(fallbackCommand, {
        {L"WW_TEST_NO_MESSAGEBOX", L"1"},
        {L"WW_TEST_FALLBACK_FILE", serviceFallbackMarker.wstring()}
    });
    std::string serviceFallbackReason = ww::readTextFileUtf8(serviceFallbackMarker.wstring());
    auto fallbackDoneConfig = manager.load();
    fallbackDoneConfig.settings.protection_enabled = false;
    if (!manager.save(fallbackDoneConfig)) {
        return fail("restore disabled config after fallback fixture failed");
    }
    if (configResponse.find("200 OK") == std::string::npos || configResponse.find("\"ok\": true") == std::string::npos) {
        return fail("http /api/config smoke failed");
    }
    if (splitHeaderResponse.find("200 OK") == std::string::npos || splitHeaderResponse.find("\"ok\": true") == std::string::npos) {
        return fail("http split header request failed");
    }
    if (largeHeaderResponse.find("400 Bad Request") == std::string::npos) {
        return fail("http large header limit failed");
    }
    if (settingsResponse.find("200 OK") == std::string::npos || settingsResponse.find("WiFi") == std::string::npos || settingsResponse.find("settings.js") == std::string::npos || settingsResponse.find("browseGroupShortcuts") == std::string::npos) {
        return fail("http /settings smoke failed");
    }
    if (faviconResponse.find("200 OK") == std::string::npos || faviconResponse.find("Content-Type: image/x-icon") == std::string::npos) {
        return fail("http favicon smoke failed");
    }
    if (wifiCurrentResponse.find("200 OK") == std::string::npos || wifiCurrentResponse.find("\"adapter_available\"") == std::string::npos) {
        return fail("http /api/wifi/current smoke failed");
    }
    if (wifiCurrentResponse.find("\"ssid\": \"Office-WiFi\"") == std::string::npos) {
        return fail("http /api/wifi/current test ssid failed");
    }
    if (networkCurrentResponse.find("200 OK") == std::string::npos || networkCurrentResponse.find("\"type\": \"wired\"") == std::string::npos || networkCurrentResponse.find("Ethernet 1") == std::string::npos) {
        return fail("http /api/network/current wired smoke failed");
    }
    if (wiredAdaptersResponse.find("200 OK") == std::string::npos || wiredAdaptersResponse.find("\"adapters\"") == std::string::npos || wiredAdaptersResponse.find("Ethernet Backup") == std::string::npos) {
        return fail("http /api/network/wired smoke failed");
    }
    if (wiredToggleResponse.find("200 OK") == std::string::npos || wiredToggleResponse.find("\"ok\": true") == std::string::npos || wiredToggleResponse.find("disable_requested") == std::string::npos) {
        return fail("http /api/network/wired/toggle smoke failed");
    }
    if (wifiAvailableResponse.find("200 OK") == std::string::npos || wifiAvailableResponse.find("\"networks\"") == std::string::npos || wifiAvailableResponse.find("Home-WiFi") == std::string::npos) {
        return fail("http /api/wifi/available smoke failed");
    }
    if (wifiSwitchResponse.find("200 OK") == std::string::npos || wifiSwitchResponse.find("\"ok\": true") == std::string::npos || wifiSwitchResponse.find("Home-WiFi") == std::string::npos) {
        return fail("http /api/wifi/switch smoke failed");
    }
    if (wifiPasswordResponse.find("200 OK") == std::string::npos || wifiPasswordResponse.find("\"password_supplied\": true") == std::string::npos || wifiPasswordResponse.find("connected_with_password") == std::string::npos) {
        return fail("http /api/wifi/switch password smoke failed");
    }
    if (wifiMultiModeResponse.find("200 OK") == std::string::npos || wifiMultiModeResponse.find("\"password_supplied\": false") == std::string::npos || wifiMultiModeResponse.find("\"status\": \"connected\"") == std::string::npos) {
        return fail("http /api/wifi/switch multi-mode smoke failed");
    }
    if (wifiDialogResponse.find("200 OK") == std::string::npos || wifiDialogResponse.find("\"native_dialog_opened\": true") == std::string::npos || wifiDialogResponse.find("Cafe-WiFi") == std::string::npos) {
        return fail("http /api/wifi/switch native dialog smoke failed");
    }
    if (wifiRequestResponse.find("200 OK") == std::string::npos || wifiRequestResponse.find("\"connect_requested\": true") == std::string::npos || wifiRequestResponse.find("\"status\": \"connect_requested\"") == std::string::npos) {
        return fail("http /api/wifi/switch connect requested smoke failed");
    }
    if (bypassWrongResponse.find("403") == std::string::npos || bypassWrongResponse.find("\"ok\": false") == std::string::npos) {
        return fail("http /api/bypass wrong password smoke failed");
    }
    if (bypassResponse.find("200 OK") == std::string::npos || bypassResponse.find("\"ok\": true") == std::string::npos) {
        return fail("http /api/bypass password smoke failed");
    }
    if (warningResponse.find("200 OK") == std::string::npos || warningResponse.find("WiFi") == std::string::npos) {
        return fail("http /warning smoke failed");
    }
    if (warningResponse.find("appIcon") == std::string::npos || warningResponse.find("fallbackIcon") == std::string::npos) {
        return fail("http /warning app icon markup missing");
    }
    if (warningJsResponse.find("200 OK") == std::string::npos || warningJsResponse.find("safe_wifi_password") == std::string::npos || warningJsResponse.find("Api.switchWifi(safe") == std::string::npos || warningJsResponse.find("Api.toggleWired") == std::string::npos) {
        return fail("http /warning safe wifi password wiring missing");
    }
    if (warningJsResponse.find("appIconUrl") == std::string::npos || warningJsResponse.find("loadAppIcon") == std::string::npos) {
        return fail("http /warning app icon script wiring missing");
    }
    if (appIconResponse.find("200 OK") == std::string::npos || appIconResponse.find("Content-Type: image/x-icon") == std::string::npos) {
        return fail("http /api/apps/icon smoke failed");
    }
    if (appIconResponse.find(std::string("\0\0\1\0", 4)) == std::string::npos) {
        return fail("http /api/apps/icon did not return ico bytes");
    }
    if (pickerResponse.find("200 OK") == std::string::npos || pickerResponse.find("选择网络") == std::string::npos) {
        return fail("http /wifi-picker smoke failed");
    }
    if (scanResponse.find("200 OK") == std::string::npos || scanResponse.find("\"shortcuts\"") == std::string::npos || scanResponse.find("Other App") != std::string::npos) {
        return fail("http /api/shortcuts/scan smoke failed");
    }
    if (statsResponse.find("200 OK") == std::string::npos || statsResponse.find("\"blocked_week\"") == std::string::npos) {
        return fail("http /api/stats smoke failed");
    }
    if (appBrowseResponse.find("200 OK") == std::string::npos || appBrowseResponse.find("Focus.exe") == std::string::npos || appBrowseResponse.find("Focus App") == std::string::npos) {
        return fail("http /api/apps/browse smoke failed");
    }
    if (shortcutBrowseResponse.find("200 OK") == std::string::npos || shortcutBrowseResponse.find("Test App.lnk") == std::string::npos || shortcutBrowseResponse.find("app.exe") == std::string::npos) {
        return fail("http /api/shortcuts/browse smoke failed");
    }
    if (shortcutReadResponse.find("200 OK") == std::string::npos || shortcutReadResponse.find("Test App.lnk") == std::string::npos || shortcutReadResponse.find("app.exe") == std::string::npos) {
        return fail("http /api/shortcuts/read smoke failed");
    }
    if (appStatusResponse.find("200 OK") == std::string::npos || appStatusResponse.find("Missing App") == std::string::npos || appStatusResponse.find("\"missing\": true") == std::string::npos || appStatusResponse.find("\"exists\": true") == std::string::npos) {
        return fail("http /api/apps/status smoke failed");
    }
    if (appCleanupResponse.find("200 OK") == std::string::npos || appCleanupResponse.find("\"removed\": true") == std::string::npos || appCleanupResponse.find("Missing App") != std::string::npos) {
        return fail("http /api/apps/cleanup smoke failed");
    }
    if (scanResponse.find("\"icon_index\": 7") == std::string::npos) {
        return fail("http /api/shortcuts/scan icon index missing");
    }
    if (scanResponse.find("\"arguments\": \"--profile smoke --flag\"") == std::string::npos) {
        return fail("http /api/shortcuts/scan arguments missing");
    }
    if (replaceResponse.find("200 OK") == std::string::npos || replaceResponse.find("\"results\"") == std::string::npos || replaceResponse.find("\"ok\": true") == std::string::npos) {
        return fail("http /api/shortcuts/replace smoke failed");
    }
    if (replaceFailResponse.find("200 OK") == std::string::npos || replaceFailResponse.find("\"ok\": false") == std::string::npos || replaceFailResponse.find("test shortcut replace failure") == std::string::npos) {
        return fail("http /api/shortcuts/replace failure response missing");
    }
    if (replaceFailStatsResponse.find("shortcut_replace_failed") == std::string::npos || replaceFailStatsResponse.find("test shortcut replace failure") == std::string::npos) {
        return fail("shortcut replace failure was not logged");
    }
    if (disableResponse.find("200 OK") == std::string::npos || disableResponse.find("\"restore_results\"") == std::string::npos) {
        return fail("http config disable restore response failed");
    }
    if (fallbackExit != 0 || fallbackReason != "browser_open_failed") {
        return fail("launcher browser fallback failed: exit=" + std::to_string(fallbackExit) + " marker=" + fallbackReason);
    }
    if (serviceFallbackExit != 0 || serviceFallbackReason != "service_unavailable") {
        return fail("launcher service fallback failed: exit=" + std::to_string(serviceFallbackExit) + " marker=" + serviceFallbackReason);
    }
    ww::ShortcutCandidate readRestored;
    if (!ww::readShortcutFile(ww::wideToUtf8(originalShortcut.wstring()), readRestored)) return fail("read api-restored shortcut failed");
    if (ww::toLowerAscii(readRestored.target_path) != ww::toLowerAscii(fakeAppPath)) return fail("api-restored shortcut target mismatch");
    if (readRestored.arguments != originalAppArgs) return fail("api-restored shortcut arguments mismatch");
    if (readRestored.icon_index != 7) return fail("api-restored shortcut icon index mismatch");
    auto afterDisable = manager.load();
    if (afterDisable.settings.protection_enabled) return fail("api config disable did not persist protection off");
    if (!afterDisable.rules[0].blocked_apps[0].replaced_shortcuts.empty()) return fail("api config disable did not clear shortcut records");
    for (const auto& shortcutPath : scannedShortcutPaths) {
        ww::ShortcutCandidate restoredScannedShortcut;
        if (!ww::readShortcutFile(ww::wideToUtf8(shortcutPath.wstring()), restoredScannedShortcut)) return fail("read restored scanned shortcut failed");
        if (ww::toLowerAscii(restoredScannedShortcut.target_path) != ww::toLowerAscii(fakeAppPath)) {
            return fail("restored scanned shortcut target mismatch");
        }
        if (restoredScannedShortcut.arguments != originalAppArgs) {
            return fail("restored scanned shortcut arguments mismatch");
        }
    }

    afterDisable.settings.protection_enabled = true;
    if (!manager.save(afterDisable)) return fail("save launcher block fixture failed");

    std::string launcher = launcherPath();
    std::string configPathUtf8 = ww::wideToUtf8(configPath.wstring());

    LauncherRunResult blockDecision = runLauncherDry(launcher, configPathUtf8, fakeAppPath, "Office-WiFi", originalAppArgs);
    if (blockDecision.output.find("\"decision\":\"block\"") == std::string::npos || blockDecision.output.find("\"rule_id\":\"rule_test\"") == std::string::npos) {
        return fail("launcher dry-run block decision failed: " + blockDecision.output);
    }
    if (blockDecision.output.find("appArgs=--profile+smoke+--flag") == std::string::npos) {
        return fail("launcher dry-run warning URL did not include app args: " + blockDecision.output);
    }
    if (blockDecision.elapsed_ms > 200) {
        return fail("launcher dry-run exceeded 200ms: " + std::to_string(blockDecision.elapsed_ms));
    }

    LauncherRunResult allowDecision = runLauncherDry(launcher, configPathUtf8, fakeAppPath, "Home-WiFi");
    if (allowDecision.output.find("\"decision\":\"allow\"") == std::string::npos || allowDecision.output.find("no_matching_rule") == std::string::npos) {
        return fail("launcher dry-run allow decision failed: " + allowDecision.output);
    }

    LauncherRunResult wiredBlockDecision = runLauncherDryForNetwork(launcher, configPathUtf8, fakeAppPath, "wired", "Ethernet 1", "rule_wired", originalAppArgs);
    if (wiredBlockDecision.output.find("\"decision\":\"block\"") == std::string::npos
        || wiredBlockDecision.output.find("\"rule_id\":\"rule_wired\"") == std::string::npos
        || wiredBlockDecision.output.find("\"network_type\":\"wired\"") == std::string::npos
        || wiredBlockDecision.output.find("networkId=Ethernet+1") == std::string::npos) {
        return fail("launcher wired dry-run block decision failed: " + wiredBlockDecision.output);
    }

    LauncherRunResult wiredAllowDecision = runLauncherDryForNetwork(launcher, configPathUtf8, fakeAppPath, "wired", "Ethernet 2", "rule_wired");
    if (wiredAllowDecision.output.find("\"decision\":\"allow\"") == std::string::npos || wiredAllowDecision.output.find("no_matching_rule") == std::string::npos) {
        return fail("launcher wired dry-run allow decision failed: " + wiredAllowDecision.output);
    }

    SetEnvironmentVariableW(L"WW_TEST_CURRENT_NETWORK_TYPE", nullptr);
    SetEnvironmentVariableW(L"WW_TEST_CURRENT_NETWORK_ID", nullptr);
    SetEnvironmentVariableW(L"WW_TEST_WIRED_ADAPTERS_JSON", nullptr);
    SetEnvironmentVariableW(L"WW_TEST_WIRED_ACTION", nullptr);
    SetEnvironmentVariableW(L"WW_TEST_CURRENT_SSID", L"<no-adapter>");
    LauncherRunResult noAdapterDecision = runLauncherDry(launcher, configPathUtf8, fakeAppPath, "");
    SetEnvironmentVariableW(L"WW_TEST_CURRENT_SSID", L"Office-WiFi");
    if (noAdapterDecision.output.find("\"decision\":\"allow\"") == std::string::npos || noAdapterDecision.output.find("no_wifi") == std::string::npos) {
        return fail("launcher no-adapter decision failed: " + noAdapterDecision.output);
    }

    auto disabledConfig = manager.load();
    disabledConfig.settings.protection_enabled = false;
    if (!manager.save(disabledConfig)) return fail("save disabled launcher config failed");
    LauncherRunResult disabledDecision = runLauncherDry(launcher, configPathUtf8, fakeAppPath, "Office-WiFi");
    if (disabledDecision.output.find("\"decision\":\"allow\"") == std::string::npos || disabledDecision.output.find("config_or_disabled") == std::string::npos) {
        return fail("launcher disabled decision failed: " + disabledDecision.output);
    }

    disabledConfig.settings.protection_enabled = true;
    if (!manager.save(disabledConfig)) return fail("save launcher log fixture failed");
    SetEnvironmentVariableW(L"APPDATA", root.wstring().c_str());
    std::wstring logCommand = quoteProcessArg(ww::utf8ToWide(launcher))
        + L" --no-launch --config " + quoteProcessArg(ww::utf8ToWide(configPathUtf8))
        + L" --app " + quoteProcessArg(ww::utf8ToWide(fakeAppPath))
        + L" --rule rule_test --ssid Office-WiFi";
    STARTUPINFOW logStartup{};
    logStartup.cb = sizeof(logStartup);
    PROCESS_INFORMATION logProcess{};
    BOOL logProcessStarted = CreateProcessW(nullptr, logCommand.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &logStartup, &logProcess);
    if (!logProcessStarted) return fail("launcher log process did not start");
    WaitForSingleObject(logProcess.hProcess, 5000);
    CloseHandle(logProcess.hThread);
    CloseHandle(logProcess.hProcess);
    fs::path launcherLogDir = root / L"WiFiWarning" / L"logs";
    bool foundBlockedLog = false;
    if (fs::exists(launcherLogDir, ec)) {
        for (const auto& entry : fs::directory_iterator(launcherLogDir, ec)) {
            if (!entry.is_regular_file()) continue;
            std::string content = ww::readTextFileUtf8(entry.path().wstring());
            if (content.find("\"action\":\"blocked\"") != std::string::npos && content.find("\"rule_id\":\"rule_test\"") != std::string::npos) {
                foundBlockedLog = true;
                break;
            }
        }
    }
    if (!foundBlockedLog) return fail("launcher blocked log was not written");

    auto uninstallConfig = manager.load();
    uninstallConfig.settings.protection_enabled = true;
    uninstallConfig.settings.auto_start = true;
    uninstallConfig.rules[0].blocked_apps[0].replaced_shortcuts = {};
    ww::ConfigManager uninstallManager((root / L"WiFiWarning" / L"config.json").wstring());
    if (!uninstallManager.save(uninstallConfig)) return fail("save uninstall fixture failed");
    SetEnvironmentVariableW(L"APPDATA", root.wstring().c_str());
    DWORD uninstallExit = runServiceCommand({L"--uninstall-restore", L"--no-autostart-sync"});
    if (uninstallExit != 0) return fail("uninstall restore command failed: " + std::to_string(uninstallExit));
    auto afterUninstall = uninstallManager.load();
    if (afterUninstall.settings.protection_enabled) return fail("uninstall restore did not disable protection");
    if (afterUninstall.settings.auto_start) return fail("uninstall restore did not disable autostart in config");

    std::cout << "PASS: smoke checks completed; launcher_block_ms=" << blockDecision.elapsed_ms << "\n";
    return 0;
}
