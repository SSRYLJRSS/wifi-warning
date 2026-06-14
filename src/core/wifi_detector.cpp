#include "core/wifi_detector.h"

#include "core/util.h"

#include <windows.h>
#include "core/dyn_wlan.h"
#include <wlanapi.h>  // type definitions only; no function calls linked

#include <atomic>
#include <chrono>
#include <memory>

namespace ww {

// TTL cache to avoid triggering Windows location service indicator every call.
// The WLAN API (WlanOpenHandle/WlanEnumInterfaces/WlanQueryInterface) is classified
// as a location-related API by Windows because WiFi BSSID can be used for geolocation.
// Polling these APIs every few seconds causes the system to permanently show
// "Location in use: wifi-warning" in the taskbar.
// By caching results with a 30-second TTL, we reduce API calls from ~every 5s
// to ~every 30s, and in practice only when the poller requests a refresh.

static constexpr int CACHE_TTL_SECONDS = 30;

static std::atomic<std::int64_t> g_wifiCacheTimestamp{0};
static std::atomic<int> g_wifiCacheConnected{0};
static std::string g_wifiCacheSsid;
static std::string g_wifiCacheInterfaceDescription;
static std::atomic<int> g_wifiCacheAdapterAvailable{0};
static std::string g_wifiCacheError;
static std::mutex g_wifiCacheMutex;

static std::int64_t steadyNowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static std::string ssidToString(const DOT11_SSID& ssid) {
    if (ssid.uSSIDLength == 0) return "";
    return std::string(reinterpret_cast<const char*>(ssid.ucSSID), reinterpret_cast<const char*>(ssid.ucSSID) + ssid.uSSIDLength);
}

static std::wstring testSsidOverride() {
    std::wstring filePath = getEnvWide(L"WW_TEST_CURRENT_SSID_FILE");
    if (!filePath.empty()) {
        std::string ssid = readTextFileUtf8(filePath);
        while (!ssid.empty() && (ssid.back() == '\r' || ssid.back() == '\n')) {
            ssid.pop_back();
        }
        if (!ssid.empty()) return utf8ToWide(ssid);
    }
    return getEnvWide(L"WW_TEST_CURRENT_SSID");
}

// Internal implementation that bypasses cache
static WifiStatus queryWifiUncached() {
    WifiStatus status;
    std::wstring testSsid = testSsidOverride();
    if (!testSsid.empty()) {
        if (testSsid == L"<no-adapter>") {
            status.error = "No WiFi adapter";
            return status;
        }
        status.adapter_available = true;
        status.connected = testSsid != L"<disconnected>";
        status.ssid = status.connected ? wideToUtf8(testSsid) : "";
        status.interface_description = "WW_TEST adapter";
        return status;
    }

    if (!dyn_wlan::isAvailable()) {
        status.error = "wlanapi.dll not available";
        return status;
    }
    DWORD negotiated = 0;
    dyn_wlan::WlanHandle wlan;
    DWORD result = dyn_wlan::fn_WlanOpenHandle(2, nullptr, &negotiated, &wlan.handle);
    if (result != ERROR_SUCCESS) {
        status.error = "WlanOpenHandle failed";
        return status;
    }

    PWLAN_INTERFACE_INFO_LIST interfaces = nullptr;
    result = dyn_wlan::fn_WlanEnumInterfaces(wlan.handle, nullptr, &interfaces);
    if (result != ERROR_SUCCESS || !interfaces) {
        status.error = "WlanEnumInterfaces failed";
        return status;
    }
    std::unique_ptr<WLAN_INTERFACE_INFO_LIST, decltype(&dyn_wlan::freeMemory)> interfaceGuard(interfaces, dyn_wlan::freeMemory);
    status.adapter_available = interfaces->dwNumberOfItems > 0;

    for (DWORD i = 0; i < interfaces->dwNumberOfItems; ++i) {
        const auto& item = interfaces->InterfaceInfo[i];
        if (item.isState != wlan_interface_state_connected) continue;

        status.interface_description = wideToUtf8(item.strInterfaceDescription);
        DWORD dataSize = 0;
        PWLAN_CONNECTION_ATTRIBUTES attrs = nullptr;
        WLAN_OPCODE_VALUE_TYPE opcode{};
        result = dyn_wlan::fn_WlanQueryInterface(
            wlan.handle,
            &item.InterfaceGuid,
            wlan_intf_opcode_current_connection,
            nullptr,
            &dataSize,
            reinterpret_cast<PVOID*>(&attrs),
            &opcode);

        if (result == ERROR_SUCCESS && attrs) {
            std::unique_ptr<WLAN_CONNECTION_ATTRIBUTES, decltype(&dyn_wlan::freeMemory)> attrsGuard(attrs, dyn_wlan::freeMemory);
            status.connected = true;
            status.ssid = ssidToString(attrs->wlanAssociationAttributes.dot11Ssid);
            return status;
        }
    }

    return status;
}

// Write result to cache
static void updateCache(const WifiStatus& status) {
    std::lock_guard<std::mutex> lock(g_wifiCacheMutex);
    g_wifiCacheTimestamp.store(steadyNowSeconds());
    g_wifiCacheConnected.store(status.connected ? 1 : 0);
    g_wifiCacheAdapterAvailable.store(status.adapter_available ? 1 : 0);
    g_wifiCacheSsid = status.ssid;
    g_wifiCacheInterfaceDescription = status.interface_description;
    g_wifiCacheError = status.error;
}

// Read result from cache; returns true if cache is valid
static bool readFromCache(WifiStatus& status) {
    std::int64_t ts = g_wifiCacheTimestamp.load();
    if (ts == 0 || (steadyNowSeconds() - ts) > CACHE_TTL_SECONDS) return false;
    std::lock_guard<std::mutex> lock(g_wifiCacheMutex);
    status.connected = g_wifiCacheConnected.load() != 0;
    status.adapter_available = g_wifiCacheAdapterAvailable.load() != 0;
    status.ssid = g_wifiCacheSsid;
    status.interface_description = g_wifiCacheInterfaceDescription;
    status.error = g_wifiCacheError;
    return true;
}

WifiStatus getCurrentWifi() {
    // Return cached result if still fresh (within TTL)
    WifiStatus cached;
    if (readFromCache(cached)) return cached;

    // Cache miss or expired — query WLAN API
    WifiStatus status = queryWifiUncached();
    updateCache(status);
    return status;
}

// Force refresh: bypass cache and update immediately.
// Called when WLAN notification signals a connection change.
void refreshWifiCache() {
    WifiStatus status = queryWifiUncached();
    updateCache(status);
}

bool isNoWifiAdapter(const WifiStatus& status) {
    return !status.adapter_available && (status.error.empty() || status.error == "No WiFi adapter");
}

std::vector<std::string> getKnownWifiProfiles() {
    std::vector<std::string> profiles;
    DWORD negotiated = 0;
    dyn_wlan::WlanHandle wlan;
    if (!dyn_wlan::isAvailable()) return profiles;
    if (dyn_wlan::fn_WlanOpenHandle(2, nullptr, &negotiated, &wlan.handle) != ERROR_SUCCESS) return profiles;

    PWLAN_INTERFACE_INFO_LIST interfaces = nullptr;
    if (dyn_wlan::fn_WlanEnumInterfaces(wlan.handle, nullptr, &interfaces) != ERROR_SUCCESS || !interfaces) return profiles;
    std::unique_ptr<WLAN_INTERFACE_INFO_LIST, decltype(&dyn_wlan::freeMemory)> interfaceGuard(interfaces, dyn_wlan::freeMemory);

    for (DWORD i = 0; i < interfaces->dwNumberOfItems; ++i) {
        PWLAN_PROFILE_INFO_LIST list = nullptr;
        if (dyn_wlan::fn_WlanGetProfileList(wlan.handle, &interfaces->InterfaceInfo[i].InterfaceGuid, nullptr, &list) != ERROR_SUCCESS || !list) continue;
        std::unique_ptr<WLAN_PROFILE_INFO_LIST, decltype(&dyn_wlan::freeMemory)> profileGuard(list, dyn_wlan::freeMemory);
        for (DWORD j = 0; j < list->dwNumberOfItems; ++j) {
            profiles.push_back(wideToUtf8(list->ProfileInfo[j].strProfileName));
        }
    }
    return profiles;
}

}
