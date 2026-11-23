#include "core/auto_start.h"
#include "core/config_manager.h"
#include "core/ifeo_manager.h"
#include "core/json.h"
#include "core/logger.h"
#include "core/network_manager.h"
#include "core/rule_engine.h"
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
    (void)argc;
    (void)argv;

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

    rule.blocked_apps.push_back({"Test App", fakeAppPath, fakeAppPath});
    fs::path missingApp = root / L"Program Files" / L"Missing" / L"gone.exe";
    std::string missingAppPath = ww::wideToUtf8(missingApp.wstring());
    rule.blocked_apps.push_back({"Missing App", missingAppPath, missingAppPath});
    ww::AppGroup group;
    group.id = "group_test";
    group.name = "Smoke Group";
    group.apps = rule.blocked_apps;
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

    // IFEO interception unit coverage: a config with exactly one rule + one blocked app
    // must expose that exe's basename through managedExeNames / verifyIfeo.
    {
        ww::AppConfig ifeoConfig;
        ww::Rule ifeoRule;
        ifeoRule.id = "ifeo_rule";
        ifeoRule.ssid = "Office-WiFi";
        ww::BlockedApp ifeoApp;
        ifeoApp.name = "Test App";
        ifeoApp.original_path = fakeAppPath;
        ifeoApp.icon_path = fakeAppPath;
        ifeoRule.blocked_apps.push_back(ifeoApp);
        ifeoConfig.rules.push_back(ifeoRule);

        auto managedExes = ww::managedExeNames(ifeoConfig);
        if (managedExes.size() != 1 || managedExes[0] != "app.exe") {
            return fail("managedExeNames did not return the blocked app basename");
        }
        auto ifeoStatus = ww::verifyIfeo(ifeoConfig);
        if (ifeoStatus.desired != 1) return fail("verifyIfeo desired count mismatch for single blocked app");
        bool ifeoManagedListed = false;
        for (const auto& exe : ifeoStatus.managed_exes) {
            if (exe == "app.exe") ifeoManagedListed = true;
        }
        if (!ifeoManagedListed) return fail("verifyIfeo managed_exes did not include the blocked app basename");
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
    detailRecord.action = "block_failed";
    detailRecord.app = "bad.exe";
    detailRecord.rule_id = "rule_detail";
    detailRecord.error = "permission denied";
    if (!logger.write(detailRecord)) return fail("logger detail write failed");
    auto recentRecords = logger.readRecent(30);
    bool foundDetailRecord = false;
    for (const auto& item : recentRecords) {
        if (item.action == "block_failed" && item.app == "bad.exe" && item.error == "permission denied") {
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

    SetEnvironmentVariableW(L"WW_TEST_CURRENT_SSID", L"Office-WiFi");
    SetEnvironmentVariableW(
        L"WW_TEST_AVAILABLE_WIFI_JSON",
        L"[{\"ssid\":\"Office-WiFi\",\"signal_quality\":80,\"connected\":true,\"secure\":true,\"auth\":\"WPA2-PSK\"},"
        L"{\"ssid\":\"Home-WiFi\",\"signal_quality\":91,\"connected\":false,\"secure\":true,\"auth\":\"WPA2-PSK\"}]");
    SetEnvironmentVariableW(L"WW_TEST_CONNECT_WIFI", L"success");
    SetEnvironmentVariableW(L"WW_TEST_PICK_APPS_JSON", L"[{\"path\":\"C:\\\\Picked\\\\Focus.exe\",\"name\":\"Focus App\"}]");

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
    std::string bypassBody = ww::stringifyJson(ww::JsonValue::Object{{"password", "secret"}, {"app", ""}, {"app_args", "--profile smoke --flag"}, {"rule_id", "rule_test"}});
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
    std::string interceptStatusResponse = httpGet(smokePort, "/api/intercept/status");
    std::string plainConfigBody = ww::stringifyJson(ww::JsonValue::Object{{"config", ww::configToJson(manager.load())}});
    std::string plainConfigResponse = httpPostJson(smokePort, "/api/config", plainConfigBody);
    std::string appStatusResponse = httpGet(smokePort, "/api/apps/status");
    std::string appCleanupBody = ww::stringifyJson(ww::JsonValue::Object{{"rule_id", "rule_test"}, {"app_path", missingAppPath}});
    std::string appCleanupResponse = httpPostJson(smokePort, "/api/apps/cleanup", appCleanupBody);
    auto afterAppCleanup = manager.load();
    bool missingAppStillConfigured = false;
    for (const auto& app : afterAppCleanup.rules[0].blocked_apps) {
        if (ww::toLowerAscii(app.original_path) == ww::toLowerAscii(missingAppPath)) missingAppStillConfigured = true;
    }
    if (missingAppStillConfigured) {
        server.stop();
        return fail("app cleanup API did not remove missing app");
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
    server.stop();
    if (configResponse.find("200 OK") == std::string::npos || configResponse.find("\"ok\": true") == std::string::npos) {
        return fail("http /api/config smoke failed");
    }
    if (splitHeaderResponse.find("200 OK") == std::string::npos || splitHeaderResponse.find("\"ok\": true") == std::string::npos) {
        return fail("http split header request failed");
    }
    if (largeHeaderResponse.find("400 Bad Request") == std::string::npos) {
        return fail("http large header limit failed");
    }
    if (settingsResponse.find("200 OK") == std::string::npos || settingsResponse.find("WiFi") == std::string::npos || settingsResponse.find("settings.js") == std::string::npos) {
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
    if (statsResponse.find("200 OK") == std::string::npos || statsResponse.find("\"blocked_week\"") == std::string::npos) {
        return fail("http /api/stats smoke failed");
    }
    if (appBrowseResponse.find("200 OK") == std::string::npos || appBrowseResponse.find("Focus.exe") == std::string::npos || appBrowseResponse.find("Focus App") == std::string::npos) {
        return fail("http /api/apps/browse smoke failed");
    }
    if (interceptStatusResponse.find("200 OK") == std::string::npos || interceptStatusResponse.find("\"mode\": \"ifeo\"") == std::string::npos || interceptStatusResponse.find("\"desired\"") == std::string::npos || interceptStatusResponse.find("\"missing\"") == std::string::npos || interceptStatusResponse.find("\"active\"") == std::string::npos) {
        return fail("http /api/intercept/status smoke failed");
    }
    if (plainConfigBody.find("shortcut") != std::string::npos) {
        return fail("config payload unexpectedly contains shortcut fields");
    }
    if (plainConfigResponse.find("200 OK") == std::string::npos || plainConfigResponse.find("\"ok\": true") == std::string::npos) {
        return fail("http /api/config save without shortcut fields failed");
    }
    if (appStatusResponse.find("200 OK") == std::string::npos || appStatusResponse.find("Missing App") == std::string::npos || appStatusResponse.find("\"missing\": true") == std::string::npos || appStatusResponse.find("\"exists\": true") == std::string::npos) {
        return fail("http /api/apps/status smoke failed");
    }
    if (appCleanupResponse.find("200 OK") == std::string::npos || appCleanupResponse.find("\"removed\": true") == std::string::npos || appCleanupResponse.find("Missing App") != std::string::npos) {
        return fail("http /api/apps/cleanup smoke failed");
    }
    if (disableResponse.find("200 OK") == std::string::npos) {
        return fail("http config disable response failed");
    }
    auto afterDisable = manager.load();
    if (afterDisable.settings.protection_enabled) return fail("api config disable did not persist protection off");

    afterDisable.settings.protection_enabled = true;
    if (!manager.save(afterDisable)) return fail("save launcher block fixture failed");

    std::string launcher = launcherPath();
    std::string configPathUtf8 = ww::wideToUtf8(configPath.wstring());

    LauncherRunResult blockDecision = runLauncherDry(launcher, configPathUtf8, fakeAppPath, "Office-WiFi", "--profile smoke --flag");
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

    LauncherRunResult wiredBlockDecision = runLauncherDryForNetwork(launcher, configPathUtf8, fakeAppPath, "wired", "Ethernet 1", "rule_wired", "--profile smoke --flag");
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

    SetEnvironmentVariableW(L"WW_TEST_CURRENT_NETWORK_TYPE", L"");
    SetEnvironmentVariableW(L"WW_TEST_CURRENT_NETWORK_ID", L"<no-adapter>");
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


    std::cout << "PASS: smoke checks completed; launcher_block_ms=" << blockDecision.elapsed_ms << "\n";
    return 0;
}
