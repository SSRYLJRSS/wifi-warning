// ww-launch.exe — IFEO Debugger / 启动守卫
// 系统启动受管 exe 时，先调用本程序：argv[1] = 目标 exe 全路径，其后为原始参数。
// 命中规则 → 打开警告页并不启动目标(=拦截)；否则以“绕过 IFEO 递归”的方式启动真实程序。
#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>

#include "core/config_manager.h"
#include "core/logger.h"
#include "core/network_manager.h"
#include "core/rule_engine.h"
#include "core/util.h"

#include <string>
#include <vector>

using namespace ww;

namespace {

typedef LONG (NTAPI* FnNtQueryInformationProcess)(HANDLE, ULONG, PVOID, ULONG, PULONG);
typedef LONG (NTAPI* FnNtRemoveProcessDebug)(HANDLE, HANDLE);

std::wstring g_exeDir;

std::wstring pathBaseNameWide(const std::wstring& path) {
    const wchar_t* slash = wcsrchr(path.c_str(), L'\\');
    if (!slash) slash = wcsrchr(path.c_str(), L'/');
    return slash ? std::wstring(slash + 1) : path;
}

bool isProtectedTarget(const std::wstring& target) {
    std::string base = toLowerAscii(wideToUtf8(pathBaseNameWide(target)));
    static const char* list[] = {"ww-launch.exe", "ww-admin.exe", "wifi-warning.exe", "ww-smoke.exe",
                                 "csrss.exe", "wininit.exe", "winlogon.exe", "services.exe", "lsass.exe",
                                 "svchost.exe", "smss.exe", "explorer.exe"};
    for (const char* item : list) {
        if (base == item) return true;
    }
    return false;
}

// 返回命令行中跳过第一个 token（可带引号）之后的剩余部分。
std::wstring skipFirstToken(const wchar_t* cmd) {
    if (!cmd) return L"";
    const wchar_t* p = cmd;
    while (*p == L' ' || *p == L'\t') ++p;
    if (*p == L'"') {
        ++p;
        while (*p && *p != L'"') ++p;
        if (*p == L'"') ++p;
    } else {
        while (*p && *p != L' ' && *p != L'\t') ++p;
    }
    while (*p == L' ' || *p == L'\t') ++p;
    return std::wstring(p);
}

bool httpAlive(int port) {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) { WSACleanup(); return false; }
    DWORD timeout = 150;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<u_short>(port));
    bool ok = connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != SOCKET_ERROR;
    closesocket(sock);
    WSACleanup();
    return ok;
}

bool minimalDebugLoop(DWORD pid) {
    DEBUG_EVENT event{};
    while (WaitForDebugEvent(&event, INFINITE)) {
        DWORD continueStatus = DBG_CONTINUE;
        if (event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) {
            ContinueDebugEvent(event.dwProcessId, event.dwThreadId, continueStatus);
            return true;
        }
        ContinueDebugEvent(event.dwProcessId, event.dwThreadId, continueStatus);
    }
    return false;
}

void detachDebugger(HANDLE process, DWORD pid) {
    // 优先使用内核级分离（文档推荐），其次 DebugActiveProcessStop，最后兜底事件泵。
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto query = reinterpret_cast<FnNtQueryInformationProcess>(
        ntdll ? GetProcAddress(ntdll, "NtQueryInformationProcess") : nullptr);
    auto removeDebug = reinterpret_cast<FnNtRemoveProcessDebug>(
        ntdll ? GetProcAddress(ntdll, "NtRemoveProcessDebug") : nullptr);
    if (query && removeDebug) {
        HANDLE debugObject = nullptr;
        LONG status = query(process, 30 /*ProcessDebugObjectHandle*/, &debugObject, sizeof(debugObject), nullptr);
        if (status >= 0 && debugObject && removeDebug(process, debugObject) >= 0) return;
    }
    // 分离前先消费初始调试事件，避免子进程停在初始断点或长期挂起。
    DEBUG_EVENT event{};
    if (WaitForDebugEvent(&event, 2000)) {
        ContinueDebugEvent(event.dwProcessId, event.dwThreadId, DBG_CONTINUE);
    }
    if (DebugActiveProcessStop(pid)) return;
    minimalDebugLoop(pid);
}

// 以“不触发 IFEO 递归”的方式启动真实程序：通过同卷硬链接改变镜像文件名，
// 既不会再次命中 IFEO 键，也不用调试器方式运行（避免反作弊/反调试冲突与递归死循环）。
static std::wstring dirNameOf(const std::wstring& path) {
    auto pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? std::wstring() : path.substr(0, pos);
}

static bool sameVolume(const std::wstring& a, const std::wstring& b) {
    wchar_t va[MAX_PATH] = {0}, vb[MAX_PATH] = {0};
    if (!GetVolumePathNameW(a.c_str(), va, MAX_PATH)) return false;
    if (!GetVolumePathNameW(b.c_str(), vb, MAX_PATH)) return false;
    return _wcsicmp(va, vb) == 0;
}

int launchReal(const std::wstring& target, const std::wstring& argsOnly) {
    const int kLinkFail = -123456789;
    std::wstring commandLine = L"\"" + target + L"\"";
    if (!argsOnly.empty()) commandLine += L" " + argsOnly;
    static unsigned s_seq = 0;
    auto nextName = [&](const std::wstring& dir, const wchar_t* prefix) -> std::wstring {
        wchar_t p[MAX_PATH];
        swprintf(p, MAX_PATH, L"%s\\%s-%08X.exe", dir.c_str(), prefix,
                 static_cast<unsigned>(GetTickCount64() ^ GetCurrentProcessId() ^ (++s_seq)));
        return std::wstring(p);
    };
    auto runImage = [&](const std::wstring& imagePath, const std::wstring& workDir, bool removeAfter) -> int {
        const wchar_t* wd = workDir.empty() ? nullptr : workDir.c_str();
        std::vector<wchar_t> buf(commandLine.begin(), commandLine.end());
        buf.push_back(0);
        STARTUPINFOW si{}; si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        BOOL ok = CreateProcessW(imagePath.c_str(), buf.data(), nullptr, nullptr, TRUE,
                                 CREATE_UNICODE_ENVIRONMENT, nullptr, wd, &si, &pi);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_ELEVATION_REQUIRED) {
                SHELLEXECUTEINFOW sei{}; sei.cbSize = sizeof(sei); sei.fMask = SEE_MASK_NOCLOSEPROCESS;
                sei.lpVerb = L"runas"; sei.lpFile = imagePath.c_str();
                sei.lpParameters = argsOnly.empty() ? nullptr : argsOnly.c_str();
                sei.lpDirectory = wd; sei.nShow = SW_SHOWNORMAL;
                int code = 1;
                if (ShellExecuteExW(&sei)) {
                    if (sei.hProcess) { WaitForSingleObject(sei.hProcess, INFINITE); DWORD ec = 0; GetExitCodeProcess(sei.hProcess, &ec); CloseHandle(sei.hProcess); code = (int)ec; } else code = 0;
                }
                if (removeAfter) DeleteFileW(imagePath.c_str());
                return code;
            }
            if (removeAfter) DeleteFileW(imagePath.c_str());
            return kLinkFail;
        }
        CloseHandle(pi.hThread);
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD ec = 0; GetExitCodeProcess(pi.hProcess, &ec);
        CloseHandle(pi.hProcess);
        if (removeAfter) DeleteFileW(imagePath.c_str());
        return static_cast<int>(ec);
    };
    // 1) 同目录硬链接（镜像、资源、工作目录都一致）
    {
        std::wstring dir = dirNameOf(target);
        if (!dir.empty()) {
            std::wstring link = nextName(dir, L".wwg");
            if (CreateHardLinkW(link.c_str(), target.c_str(), nullptr)) return runImage(link, dir, true);
        }
    }
    // 2) 同卷临时目录硬链接
    {
        wchar_t tmp[MAX_PATH]; DWORD n = GetTempPathW(MAX_PATH, tmp);
        if (n > 0) {
            std::wstring tmpDir(tmp);
            if (!tmpDir.empty() && tmpDir.back() != L'\\') tmpDir += L'\\';
            if (sameVolume(tmpDir, target)) {
                std::wstring link = nextName(tmpDir, L"wwg");
                if (CreateHardLinkW(link.c_str(), target.c_str(), nullptr)) return runImage(link, dirNameOf(target), true);
            }
        }
    }
    // 3) 复制到可写目录启动（源卷不支持硬链接：exFAT / 网络盘 / FAT32）
    {
        std::wstring appDir = dirNameOf(target);
        if (!appDir.empty()) {
            std::wstring dst = nextName(appDir, L".wwg");
            if (CopyFileW(target.c_str(), dst.c_str(), FALSE)) return runImage(dst, appDir, true);
        }
        wchar_t tmp[MAX_PATH]; DWORD n = GetTempPathW(MAX_PATH, tmp);
        if (n > 0) {
            std::wstring tmpDir(tmp);
            if (!tmpDir.empty() && tmpDir.back() != L'\\') tmpDir += L'\\';
            std::wstring dst = nextName(tmpDir, L"wwg");
            if (CopyFileW(target.c_str(), dst.c_str(), FALSE)) return runImage(dst, appDir, true);
        }
    }
    return -1;
}
void logBlocked(const std::string& app, const std::string& ruleId, const std::string& networkId) {
    Logger logger;
    LogRecord record;
    record.timestamp = nowIsoLocal();
    record.action = "blocked";
    record.app = pathBaseNameUtf8(app);
    record.rule_id = ruleId;
    logger.write(record);
}

std::string makeWarningUrl(int port, const std::string& appPath, const std::string& appName,
                           const std::string& argsOnly, const std::string& networkType,
                           const std::string& networkId, const std::string& ruleId) {
    return "http://127.0.0.1:" + std::to_string(port) + "/warning?app=" + urlEncode(appPath) +
           "&appName=" + urlEncode(appName) + "&appArgs=" + urlEncode(argsOnly) +
           "&networkType=" + urlEncode(networkType) + "&ssid=" + urlEncode(networkId) +
           "&networkId=" + urlEncode(networkId) + "&ruleId=" + urlEncode(ruleId);
}

void showBlockedMessage(const std::wstring& appName, const std::string& networkName) {
    std::wstring detail = L"“" + appName + L"” 在当前网络下已被 WiFi 提醒拦截。\n\n请先打开 WiFi 提醒主程序，在警告页输入密码可临时放行。";
    MessageBoxW(nullptr, detail.c_str(), L"WiFi 提醒", MB_OK | MB_ICONWARNING);
}

static std::wstring resolveAbsoluteTarget(const std::wstring& target) {
    if (target.empty()) return target;
    wchar_t found[MAX_PATH] = {0};
    if (target.find_first_of(L"\\/") == std::wstring::npos) {
        if (SearchPathW(nullptr, target.c_str(), L".exe", MAX_PATH, found, nullptr) > 0) return std::wstring(found);
    }
    wchar_t buf[MAX_PATH] = {0};
    DWORD n = GetFullPathNameW(target.c_str(), MAX_PATH, buf, nullptr);
    if (n > 0 && n < MAX_PATH) return std::wstring(buf);
    return target;
}

int runGuard(std::wstring target, const std::wstring& argsOnly) {
    target = resolveAbsoluteTarget(target);
    auto doLaunch = [&]() -> int {
        int r = launchReal(target, argsOnly);
        if (r == -1) {
            MessageBoxW(nullptr, L"无法启动：创建硬链接失败（目标磁盘与临时目录不在同一卷，或目录不可写）。请先在设置页“完全还原”后再启动。", L"WiFi 提醒", MB_OK | MB_ICONERROR);
        }
        return r;
    };
    if (target.empty()) {
        MessageBoxW(nullptr, L"缺少要启动的程序路径。", L"WiFi 提醒", MB_OK | MB_ICONERROR);
        return 2;
    }
    if (isProtectedTarget(target)) {
        return doLaunch();
    }

    ConfigManager manager;
    AppConfig config = manager.load();
    auto loadStatus = manager.lastLoadStatus();

    if (!config.settings.protection_enabled) {
        return doLaunch();
    }

    std::string targetUtf8 = wideToUtf8(target);
    auto networks = getActiveNetworks();
    auto match = findBlockingRuleForNetworks(config, networks, targetUtf8);
    if (!match) {
        // 未受管 / 当前网络不匹配 / 处于放行期 / 离线 → 放行
        return doLaunch();
    }

    std::string matchedType;
    std::string matchedId;
    for (const auto& network : networks) {
        auto single = findBlockingRuleForNetworks(config, {network}, targetUtf8);
        if (single) { matchedType = network.type; matchedId = network.id; break; }
    }
    if (matchedId.empty() && !networks.empty()) { matchedType = networks.front().type; matchedId = networks.front().id; }

    logBlocked(targetUtf8, match->rule.id, matchedId);

    std::string appName = match->app.name.empty() ? pathBaseNameUtf8(targetUtf8) : match->app.name;
    int port = config.settings.http_port > 0 ? config.settings.http_port : 18765;
    std::string url = makeWarningUrl(port, targetUtf8, appName, wideToUtf8(argsOnly), matchedType, matchedId, match->rule.id);

    if (httpAlive(port)) {
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof(sei);
        sei.lpVerb = L"open";
        std::wstring urlWide = utf8ToWide(url);
        sei.lpFile = urlWide.c_str();
        sei.nShow = SW_SHOWNORMAL;
        if (ShellExecuteExW(&sei)) return 0;  // 已拦截
    }
    // 服务未运行 / 浏览器失败：仍然拦截（不放行），并提示用户
    showBlockedMessage(pathBaseNameWide(target), matchedId);
    return 0;
}

int runDry(int argc, wchar_t** argv) {
    auto value = [&](const std::wstring& key) -> std::wstring {
        for (int i = 1; i + 1 < argc; ++i) {
            if (std::wstring(argv[i]) == key) return argv[i + 1];
        }
        return L"";
    };
    std::wstring configPath = value(L"--config");
    std::wstring appPath = value(L"--app");
    std::wstring ssid = value(L"--ssid");
    std::wstring netType = value(L"--network-type");
    std::wstring netId = value(L"--network-id");
    std::wstring appArgs = value(L"--app-args");

    auto emit = [](const std::string& text) {
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD written = 0;
        WriteFile(out, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    };

    ConfigManager manager(configPath);
    AppConfig config = manager.load();

    std::vector<NetworkIdentity> networks;
    if (netId != L"<no-adapter>" && ssid != L"<no-adapter>") {
        if (!netType.empty()) {
            networks.push_back({wideToUtf8(netType), wideToUtf8(netId), wideToUtf8(netId)});
        } else if (!ssid.empty()) {
            networks.push_back({"wifi", wideToUtf8(ssid), wideToUtf8(ssid)});
        }
    }

    std::string appUtf8 = wideToUtf8(appPath);
    if (!config.settings.protection_enabled) {
        emit("{\"decision\":\"allow\",\"reason\":\"config_or_disabled\"}\n");
        return 0;
    }
    if (networks.empty()) {
        emit("{\"decision\":\"allow\",\"reason\":\"no_wifi\"}\n");
        return 0;
    }
    auto match = findBlockingRuleForNetworks(config, networks, appUtf8);
    if (!match) {
        emit("{\"decision\":\"allow\",\"reason\":\"no_matching_rule\"}\n");
        return 0;
    }
    const std::string& type = networks.front().type;
    const std::string& id = networks.front().id;
    int port = config.settings.http_port > 0 ? config.settings.http_port : 18765;
    std::string appName = match->app.name.empty() ? pathBaseNameUtf8(appUtf8) : match->app.name;
    std::string url = makeWarningUrl(port, appUtf8, appName, wideToUtf8(appArgs), type, id, match->rule.id);
    std::string out = "{\"decision\":\"block\",\"rule_id\":\"" + match->rule.id +
                      "\",\"network_type\":\"" + type + "\",\"url\":\"" + url + "\"}\n";
    emit(out);
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    g_exeDir = executableDir();
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring target;
    std::wstring argsOnly;
    bool manual = false;

    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"--app" && i + 1 < argc) {
            target = argv[i + 1];
            manual = true;
        } else if (arg == L"--app-args" && i + 1 < argc) {
            argsOnly = argv[i + 1];
        }
    }
    if (manual) {
        // 手动模式仅用于调试；正常由 IFEO 调用。
    } else if (argc >= 2) {
        target = argv[1];
        std::wstring tail = skipFirstToken(GetCommandLineW());
        // tail 形如 "<target>" arg1 arg2，去掉首个 target token 得到纯参数
        argsOnly = skipFirstToken(tail.c_str());
    }
    bool dryRun = false;
    for (int i = 1; i < argc; ++i) {
        if (std::wstring(argv[i]) == L"--dry-run") { dryRun = true; break; }
    }
    if (dryRun) {
        int rc = runDry(argc, argv);
        LocalFree(argv);
        return rc;
    }
    LocalFree(argv);

    if (target.empty()) {
        MessageBoxW(nullptr, L"此程序由 WiFi 提醒自动调用，请勿直接运行。", L"WiFi 提醒", MB_OK | MB_ICONINFORMATION);
        return 2;
    }
    return runGuard(target, argsOnly);
}
