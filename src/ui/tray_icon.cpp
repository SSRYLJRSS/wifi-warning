#include "ui/tray_icon.h"

#include "core/ifeo_manager.h"
#include "core/util.h"
#include "core/wifi_detector.h"
#include "core/network_manager.h"
#include "core/dyn_wlan.h"
#include <wlanapi.h>
#include "resources/resource.h"
#include "ui/browser_launcher.h"

#include <windows.h>
#include <shellapi.h>

#include <thread>
#include <vector>
#include <atomic>
#include <mutex>
#include <functional>
#include <chrono>

namespace ww {

// Global event for WLAN notification signal — replaces 5-second blind polling.
// When WiFi state changes, the WLAN callback sets this event, waking the poller.
// A periodic timer (60s) also signals as a safety net for edge cases.
static HANDLE g_wifiChangeEvent = nullptr;

// WLAN notification callback — invoked by Windows when WiFi connection state changes.
// This is the proper event-driven approach: no need to poll every 5 seconds.
static VOID CALLBACK wlanNotificationCallback(
    PWLAN_NOTIFICATION_DATA pData,
    PVOID pContext) {
    if (!pData) return;
    // We only care about ACM (Auto Configuration Module) notifications
    // which signal connection/disconnection events
    if (pData->NotificationSource == WLAN_NOTIFICATION_SOURCE_ACM) {
        // Signal the poller to refresh
        if (g_wifiChangeEvent) {
            SetEvent(g_wifiChangeEvent);
        }
    }
}

// Register WLAN notification callback on the WLAN handle.
// WlanRegisterNotification registers globally for ALL interfaces on the handle,
// not per-interface. Returns true if registration succeeded.
[[maybe_unused]] static bool registerWlanNotification(HANDLE wlanHandle) {
    if (!dyn_wlan::fn_WlanRegisterNotification) return false;
    DWORD result = dyn_wlan::fn_WlanRegisterNotification(
        wlanHandle,
        WLAN_NOTIFICATION_SOURCE_ACM,  // Only connection events, not scan results
        TRUE,                            // bIgnoreDuplicate = true (dedup rapid events)
        reinterpret_cast<PVOID>(wlanNotificationCallback),
        nullptr,                         // pCallbackContext
        nullptr,                         // pCallbackPrevContext
        nullptr                          // pdwNegotiatedVersion (optional)
    );
    return result == ERROR_SUCCESS;
}

static constexpr UINT WM_TRAY = WM_APP + 1;
static constexpr UINT ID_SETTINGS = 1001;
static constexpr UINT ID_STATS = 1002;
static constexpr UINT ID_TOGGLE = 1003;
static constexpr UINT ID_EXIT = 1004;
static constexpr UINT ID_CURRENT = 1005;
static constexpr UINT ID_WIRED_BASE = 2001;
static constexpr UINT ID_WIRED_MAX = 2100;
static constexpr UINT ID_WIFI_TOGGLE = 1006;
static constexpr UINT ID_RESTORE_LAST = 1007;
static constexpr UINT ID_SELF_TEST_TIMER = 3001;

struct TrayIconImpl {
    ConfigManager& config;
    Logger& logger;
    int port;
    std::mutex state_mutex;
    std::atomic<int> pending_actions{0};
    HWND hwnd = nullptr;
    NOTIFYICONDATAW nid{};
    HICON icon = nullptr;
    std::string ssid;
    bool adapter_available = true;
    bool running = true;
    bool icon_added = false;
    bool no_adapter_notice_shown = false;
    std::vector<WiredAdapter> cached_wired_adapters;
    std::vector<WiredAdapter> cached_wifi_adapters;
    std::thread poller;
    struct Notice {
        std::wstring title;
        std::wstring message;
        DWORD flags = NIIF_INFO;
    };
    std::vector<Notice> startup_notices;

    TrayIconImpl(ConfigManager& config, Logger& logger, int port) : config(config), logger(logger), port(port) {}
};

static TrayIconImpl* getImpl(HWND hwnd) {
    return reinterpret_cast<TrayIconImpl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

static void openLocal(int port, const std::string& path) {
    openUrlInBrowser("http://localhost:" + std::to_string(port) + path);
}

static int restoreIfeoInterception(TrayIconImpl* impl) {
    auto config = impl->config.load();
    std::wstring admin = std::wstring(executableDir()) + L"\\ww-admin.exe";
    int code = -1;
    if (fileExists(admin)) {
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
    LogRecord record;
    record.timestamp = nowIsoLocal();
    record.action = code == 0 ? "ifeo_restored" : "ifeo_restore_failed";
    impl->logger.write(record);
    if (code != 0) return 1;
    config.settings.protection_enabled = false;
    impl->config.save(config);
    return 0;
}

static int syncIfeoInterception(TrayIconImpl* impl) {
    std::wstring admin = std::wstring(executableDir()) + L"\\ww-admin.exe";
    int code = -1;
    if (fileExists(admin)) {
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb = L"runas";
        sei.lpFile = admin.c_str();
        sei.lpParameters = L"--ifeo-sync";
        sei.nShow = SW_HIDE;
        if (ShellExecuteExW(&sei) && sei.hProcess) {
            WaitForSingleObject(sei.hProcess, 120000);
            DWORD exitCode = 0;
            GetExitCodeProcess(sei.hProcess, &exitCode);
            CloseHandle(sei.hProcess);
            code = static_cast<int>(exitCode);
        }
    }
    LogRecord record;
    record.timestamp = nowIsoLocal();
    record.action = code == 0 ? "ifeo_synced" : "ifeo_sync_failed";
    impl->logger.write(record);
    return code == 0 ? 0 : 1;
}

static void showBalloon(TrayIconImpl* impl, const std::wstring& title, const std::wstring& message, DWORD flags = NIIF_INFO) {
    if (!impl->icon_added) return;
    NOTIFYICONDATAW notice = impl->nid;
    notice.uFlags = NIF_INFO;
    notice.dwInfoFlags = flags;
    wcsncpy_s(notice.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(notice.szInfo, message.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &notice);
}

// Run a menu action off the UI thread while guarding impl lifetime (no dangling on exit).
static void runAsyncAction(TrayIconImpl* impl, std::function<void()> fn) {
    impl->pending_actions.fetch_add(1);
    std::thread([impl, fn] {
        fn();
        impl->pending_actions.fetch_sub(1);
    }).detach();
}

static void disableProtectionForNoAdapter(TrayIconImpl* impl, const WifiStatus& wifi) {
    if (!isNoWifiAdapter(wifi)) return;
    // 不再因“没有 WiFi 网卡”自动关闭防护：以太网机器会因此被误关而导致漏拦。
    if (impl->no_adapter_notice_shown) return;
    impl->no_adapter_notice_shown = true;
    LogRecord record;
    record.timestamp = nowIsoLocal();
    record.action = "no_adapter_detected";
    record.error = wifi.error;
    impl->logger.write(record);
    showBalloon(impl, L"未检测到 WiFi 适配器", L"有线网络规则仍会正常生效，防护保持开启。", NIIF_INFO);
}
static void showMenu(HWND hwnd, TrayIconImpl* impl) {
    HMENU menu = CreatePopupMenu();
    std::vector<WiredAdapter> wifiAdaptersCopy, wiredAdaptersCopy;
    { std::lock_guard<std::mutex> lk(impl->state_mutex); wifiAdaptersCopy = impl->cached_wifi_adapters; wiredAdaptersCopy = impl->cached_wired_adapters; }
    std::wstring wifi = L"当前网络: ";
    wifi += impl->ssid.empty() ? L"未连接" : utf8ToWide(impl->ssid);
    if (!impl->adapter_available) wifi = L"\u5f53\u524d\u7f51\u7edc: \u672a\u68c0\u6d4b\u5230 WiFi \u9002\u914d\u5668";
    AppendMenuW(menu, MF_STRING | MF_DISABLED, ID_CURRENT, wifi.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    if (!wifiAdaptersCopy.empty()) {
        const auto& wa = wifiAdaptersCopy[0];
        AppendMenuW(menu, MF_STRING, ID_WIFI_TOGGLE, wa.enabled ? L"禁用 WiFi" : L"启用 WiFi");
    } else {
        auto cfg = impl->config.get();
        if (cfg.network_state.last_disabled_type == "wifi" && !cfg.network_state.last_disabled_name.empty())
            AppendMenuW(menu, MF_STRING, ID_RESTORE_LAST, L"启用 WiFi");
    }

    // Wired adapter toggle entries (use cached list to avoid blocking UI thread with netsh)
    const auto& wiredAdapters = wiredAdaptersCopy;
    if (!wiredAdapters.empty()) {
        for (int i = 0; i < static_cast<int>(wiredAdapters.size()); ++i) {
            if (static_cast<unsigned>(i) >= ID_WIRED_MAX - ID_WIRED_BASE) break;
            const auto& adapter = wiredAdapters[i];
            // Only show connected adapters and disabled (previously-toggled) adapters.
            // Skip enabled-but-disconnected adapters that the user has never toggled.
            if (adapter.enabled && !adapter.connected) continue;
            std::wstring label = utf8ToWide(adapter.name.empty() ? adapter.id : adapter.name);
            if (adapter.connected) {
                label += L" (已连接 - 点击禁用)";
            } else {
                label += L" (已禁用 - 点击启用)";
            }
            // Use original index i as menu ID so WM_COMMAND can map directly
            AppendMenuW(menu, MF_STRING, ID_WIRED_BASE + i, label.c_str());
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }

    auto config = impl->config.load();
    if (!config.network_state.last_disabled_name.empty()) {
        std::wstring restoreLabel = L"恢复上次禁用: " + utf8ToWide(config.network_state.last_disabled_name);
        AppendMenuW(menu, MF_STRING, ID_RESTORE_LAST, restoreLabel.c_str());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(menu, MF_STRING, ID_TOGGLE, config.settings.protection_enabled ? L"防护: 开启" : L"防护: 关闭");
    AppendMenuW(menu, MF_STRING, ID_SETTINGS, L"设置");
    AppendMenuW(menu, MF_STRING, ID_STATS, L"统计");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_EXIT, L"退出");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* impl = getImpl(hwnd);
    if (msg == WM_COMMAND && impl) {
        switch (LOWORD(wp)) {
            case ID_SETTINGS:
                openLocal(impl->port, "/");
                return 0;
            case ID_STATS:
                openLocal(impl->port, "/?view=activity");
                return 0;
            case ID_WIFI_TOGGLE: {
                WiredAdapter adapter;
                bool haveAdapter = false;
                { std::lock_guard<std::mutex> lk(impl->state_mutex); if (!impl->cached_wifi_adapters.empty()) { adapter = impl->cached_wifi_adapters[0]; haveAdapter = true; } }
                if (haveAdapter) {
                    bool newState = !adapter.enabled;
                    runAsyncAction(impl, [impl, adapter, newState] {
                        auto result = setAdapterEnabled("wifi", adapter.id, newState);
                        if (result.ok && !newState) {
                            impl->config.mutate([&](AppConfig& c) {
                                c.network_state.last_disabled_type = "wifi";
                                c.network_state.last_disabled_name = adapter.name.empty() ? adapter.id : adapter.name;
                                c.network_state.last_disabled_epoch = nowUnixSeconds();
                            });
                        } else if (result.ok && newState) {
                            impl->config.mutate([&](AppConfig& c) {
                                if (c.network_state.last_disabled_name == adapter.id || c.network_state.last_disabled_name == adapter.name) {
                                    c.network_state.last_disabled_type.clear();
                                    c.network_state.last_disabled_name.clear();
                                    c.network_state.last_disabled_epoch = 0;
                                }
                            });
                        }
                        showBalloon(impl, L"WiFi", result.ok ? (newState ? L"已启用 WiFi" : L"已禁用 WiFi") : L"操作失败（可能需要管理员权限）");
                        { std::lock_guard<std::mutex> lk(impl->state_mutex); impl->cached_wifi_adapters = listWifiAdapters(); impl->cached_wired_adapters = listWiredAdapters(); }
                    });
                }
                return 0;
            }
            case ID_RESTORE_LAST: {
                std::string type, name;
                { auto snap = impl->config.get(); type = snap.network_state.last_disabled_type; name = snap.network_state.last_disabled_name; }
                if (name.empty()) return 0;
                runAsyncAction(impl, [impl, type, name] {
                    auto result = setAdapterEnabled(type, name, true);
                    if (result.ok) {
                        impl->config.mutate([&](AppConfig& c) {
                            c.network_state.last_disabled_type.clear();
                            c.network_state.last_disabled_name.clear();
                            c.network_state.last_disabled_epoch = 0;
                        });
                        showBalloon(impl, L"恢复连接", L"已恢复上次禁用的连接");
                    } else {
                        showBalloon(impl, L"恢复连接", L"恢复失败（可能需要管理员权限）");
                    }
                    { std::lock_guard<std::mutex> lk(impl->state_mutex); impl->cached_wifi_adapters = listWifiAdapters(); impl->cached_wired_adapters = listWiredAdapters(); }
                });
                return 0;
            }
            case ID_TOGGLE: {
                auto config = impl->config.load();
                if (config.settings.protection_enabled) {
                    int failures = restoreIfeoInterception(impl);
                    if (failures > 0) {
                        MessageBoxW(hwnd, L"还原失败，防护仍保持开启。请重试或使用设置页“完全还原”。", L"WiFi 提醒", MB_OK | MB_ICONWARNING);
                    }
                } else {
                    int code = syncIfeoInterception(impl);
                    if (code == 0) {
                        config.settings.protection_enabled = true;
                        impl->config.save(config);
                        showBalloon(impl, L"防护已开启", L"已写入系统拦截。");
                    } else {
                        MessageBoxW(hwnd, L"开启防护失败：需要管理员授权或写入失败，请稍后重试。", L"WiFi 提醒", MB_OK | MB_ICONWARNING);
                    }
                }
                return 0;
            }
            default:
                if (LOWORD(wp) >= ID_WIRED_BASE && LOWORD(wp) < ID_WIRED_MAX) {
                    int idx = LOWORD(wp) - ID_WIRED_BASE;
                    // Use cached adapters (same data as the menu was built from)
                    const auto& adapters = impl->cached_wired_adapters;
                    if (idx < static_cast<int>(adapters.size())) {
                        const auto& adapter = adapters[idx];
                        bool newState = !adapter.enabled;
                        std::wstring adapterNameW = utf8ToWide(adapter.name.empty() ? adapter.id : adapter.name);
                        // Run netsh in a background thread to avoid blocking the UI
                        std::thread([adapterId = adapter.id, newState, adapterNameW, impl] {
                            auto result = setWiredAdapterEnabled(adapterId, newState);
                            if (result.ok) {
                                std::wstring msg = newState
                                    ? L"正在启用: " + adapterNameW
                                    : L"正在禁用: " + adapterNameW;
                                showBalloon(impl, L"有线网卡", msg.c_str());
                            }
                            // Refresh adapter list after toggle
                            impl->cached_wired_adapters = listWiredAdapters();
                        }).detach();
                    }
                    return 0;
                }
                break;
            case ID_EXIT: {
                auto config = impl->config.load();
                bool shouldExit = true;
                if (config.settings.protection_enabled) {
                    int choice = MessageBoxW(
                        hwnd,
                        L"防护仍在开启。退出前是否还原所有 IFEO 拦截并关闭防护？\n\n选择“是”还原并退出；选择“否”直接退出；选择“取消”返回。",
                        L"WiFi 提醒",
                        MB_YESNOCANCEL | MB_ICONQUESTION);
                    if (choice == IDCANCEL) {
                        shouldExit = false;
                    } else if (choice == IDYES) {
                        int failures = restoreIfeoInterception(impl);
                        if (failures > 0) {
                            MessageBoxW(hwnd, L"有部分 IFEO 拦截还原失败，应用仍将退出。", L"WiFi 提醒", MB_OK | MB_ICONWARNING);
                        }
                    }
                } else {
                    shouldExit = MessageBoxW(hwnd, L"确定退出 WiFi 提醒？", L"WiFi 提醒", MB_OKCANCEL | MB_ICONQUESTION) == IDOK;
                }

                if (shouldExit) {
                    impl->running = false;
                    DestroyWindow(hwnd);
                }
                return 0;
            }
        }
    }
    if (msg == WM_TRAY && impl) {
        if (lp == WM_RBUTTONUP) showMenu(hwnd, impl);
        if (lp == WM_LBUTTONDBLCLK) openLocal(impl->port, "/");
        return 0;
    }
    if (msg == WM_TIMER && wp == ID_SELF_TEST_TIMER) {
        KillTimer(hwnd, ID_SELF_TEST_TIMER);
        if (impl) impl->running = false;
        DestroyWindow(hwnd);
        return 0;
    }
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

TrayIcon::TrayIcon(ConfigManager& config, Logger& logger, int port) : impl_(std::make_unique<TrayIconImpl>(config, logger, port)) {}

TrayIcon::~TrayIcon() {
    impl_->running = false;
    // Wake poller from WaitForSingleObject so it exits promptly (< 1s)
    if (g_wifiChangeEvent) SetEvent(g_wifiChangeEvent);
    if (impl_->poller.joinable()) impl_->poller.join();
    for (int i = 0; i < 600 && impl_->pending_actions.load() > 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (impl_->icon_added) Shell_NotifyIconW(NIM_DELETE, &impl_->nid);
    if (impl_->icon) DestroyIcon(impl_->icon);
}

void TrayIcon::updateWifiText(const WifiStatus& wifi) {
    const std::string& ssid = wifi.ssid;
    impl_->adapter_available = wifi.adapter_available;
    impl_->ssid = ssid;
    std::wstring tip = L"WiFi 提醒 - ";
    tip += ssid.empty() ? L"未连接 WiFi" : utf8ToWide(ssid);
    if (!wifi.adapter_available) tip = L"WiFi \u63d0\u9192 - \u672a\u68c0\u6d4b\u5230 WiFi \u9002\u914d\u5668";
    wcsncpy_s(impl_->nid.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &impl_->nid);
    disableProtectionForNoAdapter(impl_.get(), wifi);
}

void TrayIcon::addStartupNotice(const std::wstring& title, const std::wstring& message, bool warning) {
    TrayIconImpl::Notice notice;
    notice.title = title;
    notice.message = message;
    notice.flags = warning ? NIIF_WARNING : NIIF_INFO;
    impl_->startup_notices.push_back(std::move(notice));
}

int TrayIcon::run(int selfTestMs, const std::wstring& readyFile, int selfTestCommand) {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    const wchar_t* className = L"WiFiWarningTrayWindow";
    WNDCLASSW wc{};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = className;
    RegisterClassW(&wc);

    impl_->hwnd = CreateWindowExW(0, className, L"WiFi 提醒", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
    SetWindowLongPtrW(impl_->hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(impl_.get()));

    impl_->nid.cbSize = sizeof(NOTIFYICONDATAW);
    impl_->nid.hWnd = impl_->hwnd;
    impl_->nid.uID = 1;
    impl_->nid.uFlags = NIF_MESSAGE | NIF_TIP | NIF_ICON;
    impl_->nid.uCallbackMessage = WM_TRAY;
    impl_->icon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
    impl_->nid.hIcon = impl_->icon ? impl_->icon : LoadIconW(nullptr, IDI_WARNING);
    wcsncpy_s(impl_->nid.szTip, L"WiFi 提醒", _TRUNCATE);
    impl_->icon_added = Shell_NotifyIconW(NIM_ADD, &impl_->nid) == TRUE;
    impl_->cached_wired_adapters = listWiredAdapters();
    updateWifiText(getCurrentWifi());
    for (const auto& notice : impl_->startup_notices) {
        showBalloon(impl_.get(), notice.title, notice.message, notice.flags);
    }
    if (!readyFile.empty()) {
        std::string ready = "tray:";
        ready += impl_->hwnd ? "window" : "no-window";
        ready += impl_->icon_added ? ":icon" : ":no-icon";
        ready += impl_->adapter_available ? ":adapter" : ":no-adapter";
        writeTextFileUtf8(readyFile, ready);
    }
    if (selfTestMs > 0) {
        SetTimer(impl_->hwnd, ID_SELF_TEST_TIMER, static_cast<UINT>(selfTestMs), nullptr);
    }
    if (selfTestCommand > 0) {
        PostMessageW(impl_->hwnd, WM_COMMAND, static_cast<WPARAM>(selfTestCommand), 0);
    }

    // Event-driven poller: waits for WLAN state change signal instead of
    // polling every 5 seconds. This eliminates the persistent "Location in use"
    // indicator caused by frequent WlanOpenHandle/WlanEnumInterfaces calls.
    // Falls back to 60-second periodic refresh as safety net.
    g_wifiChangeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    impl_->poller = std::thread([this] {
        // Initial query with cache priming
        auto wifi = getCurrentWifi();
        updateWifiText(wifi);
        { std::lock_guard<std::mutex> lk(impl_->state_mutex); impl_->cached_wired_adapters = listWiredAdapters(); impl_->cached_wifi_adapters = listWifiAdapters(); }
        while (impl_->running) {
            // Wait for WLAN change event, with 60-second timeout as safety net
            DWORD waitResult = WaitForSingleObject(g_wifiChangeEvent, 60000);

            if (!impl_->running) break;

            if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_TIMEOUT) {
                // Reset the event (manual-reset)
                ResetEvent(g_wifiChangeEvent);

                // Refresh WiFi cache (may be cached internally for 30s TTL,
                // but we signal it to refresh now since a real event occurred)
                refreshWifiCache();
                auto wifi = getCurrentWifi();
                updateWifiText(wifi);
                { std::lock_guard<std::mutex> lk(impl_->state_mutex); impl_->cached_wired_adapters = listWiredAdapters(); impl_->cached_wifi_adapters = listWifiAdapters(); }
                trimCurrentProcessWorkingSet();
            }
        }

        // Cleanup: event handle
        if (g_wifiChangeEvent) {
            CloseHandle(g_wifiChangeEvent);
            g_wifiChangeEvent = nullptr;
        }
    });

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    impl_->running = false;
    return 0;
}

}
