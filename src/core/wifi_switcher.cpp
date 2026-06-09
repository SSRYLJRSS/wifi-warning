#include "core/wifi_switcher.h"

#include "core/util.h"
#include "core/wifi_detector.h"

#include <windows.h>
#include <shellapi.h>
#include <wlanapi.h>

#include <chrono>
#include <sstream>
#include <memory>
#include <thread>
#include <vector>

namespace ww {

static std::string ssidToString(const DOT11_SSID& ssid) {
    if (ssid.uSSIDLength == 0) return "";
    return std::string(reinterpret_cast<const char*>(ssid.ucSSID), reinterpret_cast<const char*>(ssid.ucSSID) + ssid.uSSIDLength);
}

static std::string authToString(DOT11_AUTH_ALGORITHM auth) {
    switch (auth) {
        case DOT11_AUTH_ALGO_80211_OPEN: return "开放";
        case DOT11_AUTH_ALGO_RSNA: return "WPA2";
        case DOT11_AUTH_ALGO_RSNA_PSK: return "WPA2-PSK";
        case DOT11_AUTH_ALGO_WPA: return "WPA";
        case DOT11_AUTH_ALGO_WPA_PSK: return "WPA-PSK";
        default: return "安全网络";
    }
}

struct WlanHandle {
    HANDLE handle = nullptr;
    ~WlanHandle() {
        if (handle) WlanCloseHandle(handle, nullptr);
    }
};

std::vector<AvailableWifiNetwork> listAvailableWifiNetworks() {
    std::vector<AvailableWifiNetwork> networks;
    std::wstring testJsonWide = getEnvWide(L"WW_TEST_AVAILABLE_WIFI_JSON");
    if (!testJsonWide.empty()) {
        std::string error;
        auto parsed = parseJson(wideToUtf8(testJsonWide), &error);
        if (!parsed) return networks;
        for (const auto& item : parsed->asArray()) {
            AvailableWifiNetwork network;
            if (const auto* value = item.get("ssid")) network.ssid = value->asString();
            if (const auto* value = item.get("signal_quality")) network.signal_quality = static_cast<int>(value->asNumber());
            if (const auto* value = item.get("connected")) network.connected = value->asBool();
            if (const auto* value = item.get("secure")) network.secure = value->asBool(true);
            if (const auto* value = item.get("auth")) network.auth = value->asString(network.secure ? "安全网络" : "开放");
            if (!network.ssid.empty()) networks.push_back(network);
        }
        return networks;
    }

    auto current = getCurrentWifi();
    DWORD negotiated = 0;
    WlanHandle wlan;
    if (WlanOpenHandle(2, nullptr, &negotiated, &wlan.handle) != ERROR_SUCCESS) return networks;

    PWLAN_INTERFACE_INFO_LIST interfaces = nullptr;
    if (WlanEnumInterfaces(wlan.handle, nullptr, &interfaces) != ERROR_SUCCESS || !interfaces) return networks;
    std::unique_ptr<WLAN_INTERFACE_INFO_LIST, decltype(&WlanFreeMemory)> interfaceGuard(interfaces, WlanFreeMemory);

    for (DWORD i = 0; i < interfaces->dwNumberOfItems; ++i) {
        PWLAN_AVAILABLE_NETWORK_LIST list = nullptr;
        if (WlanGetAvailableNetworkList(wlan.handle, &interfaces->InterfaceInfo[i].InterfaceGuid, 0, nullptr, &list) != ERROR_SUCCESS || !list) continue;
        std::unique_ptr<WLAN_AVAILABLE_NETWORK_LIST, decltype(&WlanFreeMemory)> listGuard(list, WlanFreeMemory);
        for (DWORD j = 0; j < list->dwNumberOfItems; ++j) {
            const auto& item = list->Network[j];
            std::string ssid = ssidToString(item.dot11Ssid);
            if (ssid.empty()) continue;
            AvailableWifiNetwork network;
            network.ssid = ssid;
            network.signal_quality = static_cast<int>(item.wlanSignalQuality);
            network.connected = current.connected && current.ssid == ssid;
            network.secure = item.bSecurityEnabled;
            network.auth = network.secure ? authToString(item.dot11DefaultAuthAlgorithm) : "开放";
            networks.push_back(network);
        }
    }
    return networks;
}

static bool openNativeWifiFlyout() {
    auto result = ShellExecuteW(nullptr, L"open", L"ms-availablenetworks:", nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<intptr_t>(result) > 32;
}

static std::string xmlEscape(const std::string& value) {
    std::ostringstream out;
    for (char ch : value) {
        switch (ch) {
            case '&': out << "&amp;"; break;
            case '<': out << "&lt;"; break;
            case '>': out << "&gt;"; break;
            case '"': out << "&quot;"; break;
            case '\'': out << "&apos;"; break;
            default: out << ch; break;
        }
    }
    return out.str();
}

static std::wstring wifiProfileXml(const std::string& ssid, const std::string& password) {
    std::string name = xmlEscape(ssid);
    std::string key = xmlEscape(password);
    std::ostringstream xml;
    xml << "<?xml version=\"1.0\"?>"
        << "<WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\">"
        << "<name>" << name << "</name>"
        << "<SSIDConfig><SSID><name>" << name << "</name></SSID></SSIDConfig>"
        << "<connectionType>ESS</connectionType>"
        << "<connectionMode>auto</connectionMode>"
        << "<MSM><security>"
        << "<authEncryption><authentication>WPA2PSK</authentication><encryption>AES</encryption><useOneX>false</useOneX></authEncryption>"
        << "<sharedKey><keyType>passPhrase</keyType><protected>false</protected><keyMaterial>" << key << "</keyMaterial></sharedKey>"
        << "</security></MSM>"
        << "</WLANProfile>";
    return utf8ToWide(xml.str());
}

static std::vector<std::string> splitTestConnectModes(const std::string& value) {
    std::vector<std::string> modes;
    size_t start = 0;
    while (start <= value.size()) {
        size_t end = value.find(';', start);
        std::string mode = value.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!mode.empty()) modes.push_back(mode);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return modes.empty() ? std::vector<std::string>{value} : modes;
}

static bool waitForConnectedSsid(const std::string& ssid, DWORD timeoutMs = 12000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    do {
        auto current = getCurrentWifi();
        if (current.connected && current.ssid == ssid) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

static DWORD preferredInterfaceIndex(PWLAN_INTERFACE_INFO_LIST interfaces) {
    if (!interfaces || interfaces->dwNumberOfItems == 0) return 0;
    for (DWORD i = 0; i < interfaces->dwNumberOfItems; ++i) {
        if (interfaces->InterfaceInfo[i].isState == wlan_interface_state_connected) return i;
    }
    for (DWORD i = 0; i < interfaces->dwNumberOfItems; ++i) {
        if (interfaces->InterfaceInfo[i].isState != wlan_interface_state_not_ready &&
            interfaces->InterfaceInfo[i].isState != wlan_interface_state_disconnected) {
            return i;
        }
    }
    return 0;
}

WifiConnectResult connectToWifiDetailed(const std::string& ssid, const std::string& password) {
    WifiConnectResult outcome;
    std::wstring testConnect = getEnvWide(L"WW_TEST_CONNECT_WIFI");
    if (!testConnect.empty()) {
        const std::string passwordPrefix = "password:";
        bool sawPasswordMode = false;
        for (const auto& mode : splitTestConnectModes(wideToUtf8(testConnect))) {
            if (mode.rfind(passwordPrefix, 0) == 0) {
                sawPasswordMode = true;
                std::string expected = mode.substr(passwordPrefix.size());
                std::string expectedSsid;
                std::string expectedPassword = expected;
                size_t split = expected.find(':');
                if (split != std::string::npos) {
                    expectedSsid = expected.substr(0, split);
                    expectedPassword = expected.substr(split + 1);
                }
                if ((expectedSsid.empty() || expectedSsid == ssid) && password == expectedPassword) {
                    outcome.connected = true;
                    outcome.status = "connected_with_password";
                    return outcome;
                }
                continue;
            }
            if (mode == "success" || mode == ssid) {
                outcome.connected = true;
                outcome.status = "connected";
                return outcome;
            }
            if (mode == "dialog") {
                outcome.native_dialog_opened = true;
                outcome.status = "native_dialog_opened";
                outcome.error = "已打开 Windows 网络选择面板，请选择网络并输入密码";
                return outcome;
            }
            if (mode == "request") {
                outcome.connect_requested = true;
                outcome.status = "connect_requested";
                outcome.error = "Windows 已接受连接请求，仍在等待连接完成";
                return outcome;
            }
        }
        outcome.status = "failed";
        outcome.error = sawPasswordMode ? "WW_TEST_CONNECT_WIFI password mismatch" : "WW_TEST_CONNECT_WIFI requested failure";
        return outcome;
    }

    DWORD negotiated = 0;
    WlanHandle wlan;
    DWORD result = WlanOpenHandle(2, nullptr, &negotiated, &wlan.handle);
    if (result != ERROR_SUCCESS) {
        outcome.status = "failed";
        outcome.error = "无法打开 WLAN 服务";
        return outcome;
    }

    PWLAN_INTERFACE_INFO_LIST interfaces = nullptr;
    result = WlanEnumInterfaces(wlan.handle, nullptr, &interfaces);
    if (result != ERROR_SUCCESS || !interfaces || interfaces->dwNumberOfItems == 0) {
        outcome.status = "failed";
        outcome.error = "未检测到无线网卡";
        return outcome;
    }
    std::unique_ptr<WLAN_INTERFACE_INFO_LIST, decltype(&WlanFreeMemory)> interfaceGuard(interfaces, WlanFreeMemory);
    const auto& targetInterface = interfaces->InterfaceInfo[preferredInterfaceIndex(interfaces)];

    std::wstring profile = utf8ToWide(ssid);
    WLAN_CONNECTION_PARAMETERS params{};
    params.wlanConnectionMode = wlan_connection_mode_profile;
    params.strProfile = profile.c_str();
    params.dot11BssType = dot11_BSS_type_any;
    params.dwFlags = 0;

    if (!password.empty()) {
        std::wstring xml = wifiProfileXml(ssid, password);
        DWORD reason = 0;
        result = WlanSetProfile(wlan.handle, &targetInterface.InterfaceGuid, 0, xml.c_str(), nullptr, TRUE, nullptr, &reason);
        if (result != ERROR_SUCCESS) {
            outcome.native_dialog_opened = openNativeWifiFlyout();
            outcome.status = outcome.native_dialog_opened ? "native_dialog_opened" : "failed";
            outcome.error = outcome.native_dialog_opened
                ? "保存 WiFi 密码失败，已打开 Windows 网络面板。"
                : "保存 WiFi 密码失败: " + std::to_string(result);
            return outcome;
        }
    }

    result = WlanConnect(wlan.handle, &targetInterface.InterfaceGuid, &params, nullptr);
    if (result == ERROR_SUCCESS) {
        if (waitForConnectedSsid(ssid)) {
            outcome.connected = true;
            outcome.status = password.empty() ? "connected" : "connected_with_password";
        } else {
            outcome.connect_requested = true;
            outcome.status = "connect_requested";
            outcome.error = "Windows 已接受连接请求，仍在等待连接完成";
        }
        return outcome;
    }

    std::wstring command = L"wlan connect name=" + quoteArg(profile);
    ShellExecuteW(nullptr, L"open", L"netsh.exe", command.c_str(), nullptr, SW_HIDE);
    outcome.native_dialog_opened = openNativeWifiFlyout();
    outcome.connect_requested = !outcome.native_dialog_opened;
    outcome.status = outcome.native_dialog_opened ? "native_dialog_opened" : "connect_requested";
    outcome.error = outcome.native_dialog_opened
        ? "已打开 Windows 网络选择面板，请选择网络并输入密码"
        : "已请求 Windows 连接该网络，如需密码请在系统弹窗中输入";
    return outcome;
}

bool connectToWifi(const std::string& ssid, std::string* error) {
    auto outcome = connectToWifiDetailed(ssid);
    if (error) *error = outcome.error;
    return outcome.connected;
}

JsonValue availableNetworksJson() {
    JsonValue::Array array;
    for (const auto& network : listAvailableWifiNetworks()) {
        array.push_back(JsonValue::Object{
            {"ssid", network.ssid},
            {"signal_quality", network.signal_quality},
            {"connected", network.connected},
            {"secure", network.secure},
            {"auth", network.auth}
        });
    }
    return array;
}

}
