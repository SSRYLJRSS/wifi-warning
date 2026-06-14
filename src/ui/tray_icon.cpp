#include "ui/tray_icon.h"

#include "core/shortcut_manager.h"
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
static bool registerWlanNotification(HANDLE wlanHandle) {
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
static constexpr UINT ID_SELF_TEST_TIMER = 3001;

struct TrayIconImpl {
    ConfigManager& config;
    Logger& logger;
    int port;
    HWND hwnd = nullptr;
    NOTIFYICONDATAW nid{};
    HICON icon = nullptr;
    std::string ssid;
    bool adapter_available = true;
    bool running = true;
    bool icon_added = false;
    bool no_adapter_notice_shown = false;
    std::vector<WiredAdapter> cached_wired_adapters;
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

static void logShortcutResult(TrayIconImpl* impl, const ShortcutOperationResult& result) {
    LogRecord record;
    record.timestamp = nowIsoLocal();
    record.action = result.ok ? "shortcut_restored" : "shortcut_restore_failed";
    record.shortcut = result.shortcut;
    record.backup = result.backup;
    record.error = result.error;
    impl->logger.write(record);
}

static int restoreAllTrackedShortcuts(TrayIconImpl* impl) {
    auto config = impl->config.load();
    auto results = restoreAllShortcuts(config);
    int failures = 0;
    for (const auto& result : results) {
        logShortcutResult(impl, result);
        if (!result.ok) ++failures;
    }

    clearRestoredShortcutRecords(config, results);
    config.settings.protection_enabled = false;
    impl->config.save(config);
    return failures;
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

static void disableProtectionForNoAdapter(TrayIconImpl* impl, const WifiStatus& wifi) {
    if (!isNoWifiAdapter(wifi)) return;
    auto config = impl->config.load();
    if (!config.settings.protection_enabled) return;

    restoreAllTrackedShortcuts(impl);

    LogRecord record;
    record.timestamp = nowIsoLocal();
    record.action = "protection_disabled_no_adapter";
    record.error = wifi.error;
    impl->logger.write(record);

    if (!impl->no_adapter_notice_shown) {
        impl->no_adapter_notice_shown = true;
        showBalloon(
            impl,
            L"\u672a\u68c0\u6d4b\u5230 WiFi \u9002\u914d\u5668",
            L"\u5df2\u5173\u95ed\u9632\u62a4\u5e76\u5c1d\u8bd5\u8fd8\u539f\u5feb\u6377\u65b9\u5f0f\u3002",
            NIIF_WARNING);
    }
}

static void showMenu(HWND hwnd, TrayIconImpl* impl) {
    HMENU menu = CreatePopupMenu();
    std::wstring wifi = L"当前网络: ";
    wifi += impl->ssid.empty() ? L"未连接" : utf8ToWide(impl->ssid);
    if (!impl->adapter_available) wifi = L"\u5f53\u524d\u7f51\u7edc: \u672a\u68c0\u6d4b\u5230 WiFi \u9002\u914d\u5668";
    AppendMenuW(menu, MF_STRING | MF_DISABLED, ID_CURRENT, wifi.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // Wired adapter toggle entries (use cached list to avoid blocking UI thread with netsh)
    const auto& wiredAdapters = impl->cached_wired_adapters;
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
                openLocal(impl->port, "/settings");
                return 0;
            case ID_STATS:
                openLocal(impl->port, "/settings#stats");
                return 0;
            case ID_TOGGLE: {
                auto config = impl->config.load();
                if (config.settings.protection_enabled) {
                    int failures = restoreAllTrackedShortcuts(impl);
                    if (failures > 0) {
                        MessageBoxW(hwnd, L"防护已关闭，但有部分快捷方式还原失败。请在设置页查看详情。", L"WiFi 提醒", MB_OK | MB_ICONWARNING);
                    }
                } else {
                    config.settings.protection_enabled = true;
                    impl->config.save(config);
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
                        L"防护仍在开启。退出前是否还原所有已替换的快捷方式并关闭防护？\n\n选择“是”还原并退出；选择“否”直接退出；选择“取消”返回。",
                        L"WiFi 提醒",
                        MB_YESNOCANCEL | MB_ICONQUESTION);
                    if (choice == IDCANCEL) {
                        shouldExit = false;
                    } else if (choice == IDYES) {
                        int failures = restoreAllTrackedShortcuts(impl);
                        if (failures > 0) {
                            MessageBoxW(hwnd, L"有部分快捷方式还原失败，应用仍将退出。", L"WiFi 提醒", MB_OK | MB_ICONWARNING);
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
        if (lp == WM_LBUTTONDBLCLK) openLocal(impl->port, "/settings");
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
        impl_->cached_wired_adapters = listWiredAdapters();

        // Try to register WLAN notification callback for event-driven refresh.
        // The WLAN handle must remain open for the lifetime of the callback,
        // so we allocate it on the heap and let the poller thread clean it up on exit.
        // WlanRegisterNotification is handle-global (all interfaces), so register once.
        auto* wlanHolder = new dyn_wlan::WlanHandle();
        if (dyn_wlan::isAvailable()) {
            DWORD negotiated = 0;
            if (dyn_wlan::fn_WlanOpenHandle(2, nullptr, &negotiated, &wlanHolder->handle) == ERROR_SUCCESS) {
                registerWlanNotification(wlanHolder->handle);
            }
        }

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
                impl_->cached_wired_adapters = listWiredAdapters();
                trimCurrentProcessWorkingSet();
            }
        }

        // Cleanup: close WLAN handle (unregisters callback implicitly)
        delete wlanHolder;
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
