#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <shellapi.h>
#include <wlanapi.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static char* dupRange(const char* begin, size_t len) {
    char* out = static_cast<char*>(malloc(len + 1));
    if (!out) return nullptr;
    if (len) memcpy(out, begin, len);
    out[len] = 0;
    return out;
}

static void freeStr(char* value) {
    if (value) free(value);
}

static char* wideToUtf8(const wchar_t* value) {
    if (!value || !*value) return dupRange("", 0);
    int needed = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return dupRange("", 0);
    char* out = static_cast<char*>(malloc(static_cast<size_t>(needed)));
    if (!out) return nullptr;
    WideCharToMultiByte(CP_UTF8, 0, value, -1, out, needed, nullptr, nullptr);
    return out;
}

static wchar_t* utf8ToWide(const char* value) {
    if (!value || !*value) {
        wchar_t* out = static_cast<wchar_t*>(malloc(sizeof(wchar_t)));
        if (out) out[0] = 0;
        return out;
    }
    int needed = MultiByteToWideChar(CP_UTF8, 0, value, -1, nullptr, 0);
    if (needed <= 0) return nullptr;
    wchar_t* out = static_cast<wchar_t*>(malloc(sizeof(wchar_t) * static_cast<size_t>(needed)));
    if (!out) return nullptr;
    MultiByteToWideChar(CP_UTF8, 0, value, -1, out, needed);
    return out;
}

static char* argValue(int argc, wchar_t** argv, const wchar_t* name) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (wcscmp(argv[i], name) == 0) return wideToUtf8(argv[i + 1]);
    }
    return dupRange("", 0);
}

static bool hasArg(int argc, wchar_t** argv, const wchar_t* name) {
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], name) == 0) return true;
    }
    return false;
}

static wchar_t* envWide(const wchar_t* name) {
    DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (!needed) return nullptr;
    wchar_t* out = static_cast<wchar_t*>(malloc(sizeof(wchar_t) * needed));
    if (!out) return nullptr;
    DWORD written = GetEnvironmentVariableW(name, out, needed);
    if (written >= needed) out[needed - 1] = 0;
    return out;
}

static wchar_t* joinWide3(const wchar_t* a, const wchar_t* b, const wchar_t* c) {
    size_t la = wcslen(a);
    size_t lb = wcslen(b);
    size_t lc = wcslen(c);
    wchar_t* out = static_cast<wchar_t*>(malloc(sizeof(wchar_t) * (la + lb + lc + 1)));
    if (!out) return nullptr;
    memcpy(out, a, sizeof(wchar_t) * la);
    memcpy(out + la, b, sizeof(wchar_t) * lb);
    memcpy(out + la + lb, c, sizeof(wchar_t) * lc);
    out[la + lb + lc] = 0;
    return out;
}

static wchar_t* defaultConfigPath() {
    wchar_t* appData = envWide(L"APPDATA");
    if (!appData) return joinWide3(L".", L"\\WiFiWarning", L"\\config.json");
    wchar_t* out = joinWide3(appData, L"\\WiFiWarning", L"\\config.json");
    free(appData);
    return out;
}

static char* readUtf8File(const wchar_t* path) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return dupRange("", 0);
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 8 * 1024 * 1024) {
        CloseHandle(file);
        return dupRange("", 0);
    }
    char* data = static_cast<char*>(malloc(static_cast<size_t>(size.QuadPart) + 1));
    if (!data) {
        CloseHandle(file);
        return dupRange("", 0);
    }
    DWORD read = 0;
    ReadFile(file, data, static_cast<DWORD>(size.QuadPart), &read, nullptr);
    data[read] = 0;
    CloseHandle(file);
    return data;
}

static bool isEscaped(const char* text, size_t pos) {
    size_t count = 0;
    while (pos > 0 && text[--pos] == '\\') ++count;
    return (count % 2) == 1;
}

static size_t findStringEnd(const char* text, size_t len, size_t quote) {
    for (size_t i = quote + 1; i < len; ++i) {
        if (text[i] == '"' && !isEscaped(text, i)) return i;
    }
    return SIZE_MAX;
}

static size_t findMatching(const char* text, size_t len, size_t open, char close) {
    char start = text[open];
    int depth = 0;
    for (size_t i = open; i < len; ++i) {
        if (text[i] == '"') {
            size_t end = findStringEnd(text, len, i);
            if (end == SIZE_MAX) return SIZE_MAX;
            i = end;
            continue;
        }
        if (text[i] == start) ++depth;
        if (text[i] == close && --depth == 0) return i;
    }
    return SIZE_MAX;
}

static const char* findNeedleBounded(const char* text, const char* needle, size_t begin, size_t end) {
    size_t needleLen = strlen(needle);
    if (needleLen == 0 || end < begin || end - begin < needleLen) return nullptr;
    for (size_t i = begin; i + needleLen <= end; ++i) {
        if (memcmp(text + i, needle, needleLen) == 0) return text + i;
    }
    return nullptr;
}

static size_t valueStart(const char* text, size_t len, const char* key, size_t begin, size_t end) {
    if (end > len) end = len;
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char* found = findNeedleBounded(text, needle, begin, end);
    while (found) {
        size_t pos = static_cast<size_t>(found - text);
        const char* colon = static_cast<const char*>(memchr(text + pos + strlen(needle), ':', end - pos - strlen(needle)));
        if (!colon) return SIZE_MAX;
        size_t value = static_cast<size_t>(colon - text) + 1;
        while (value < end && (text[value] == ' ' || text[value] == '\t' || text[value] == '\r' || text[value] == '\n')) ++value;
        if (value < end) return value;
        found = findNeedleBounded(text, needle, pos + strlen(needle), end);
    }
    return SIZE_MAX;
}

static char* jsonStringAt(const char* text, size_t len, size_t quote) {
    if (quote == SIZE_MAX || quote >= len || text[quote] != '"') return dupRange("", 0);
    size_t end = findStringEnd(text, len, quote);
    if (end == SIZE_MAX) return dupRange("", 0);
    char* out = static_cast<char*>(malloc(end - quote + 1));
    if (!out) return nullptr;
    size_t j = 0;
    for (size_t i = quote + 1; i < end; ++i) {
        if (text[i] == '\\' && i + 1 < end) {
            char e = text[++i];
            if (e == 'n') out[j++] = '\n';
            else if (e == 'r') out[j++] = '\r';
            else if (e == 't') out[j++] = '\t';
            else out[j++] = e;
        } else {
            out[j++] = text[i];
        }
    }
    out[j] = 0;
    return out;
}

static char* jsonString(const char* text, size_t len, const char* key, size_t begin, size_t end) {
    return jsonStringAt(text, len, valueStart(text, len, key, begin, end));
}

static bool jsonBool(const char* text, size_t len, const char* key, bool fallback) {
    size_t value = valueStart(text, len, key, 0, len);
    if (value == SIZE_MAX) return fallback;
    if (strncmp(text + value, "true", 4) == 0) return true;
    if (strncmp(text + value, "false", 5) == 0) return false;
    return fallback;
}

static int jsonInt(const char* text, size_t len, const char* key, int fallback) {
    size_t value = valueStart(text, len, key, 0, len);
    if (value == SIZE_MAX) return fallback;
    return atoi(text + value);
}

static bool hasActiveBypass(const char* config, size_t len) {
    long long bypassUntil = atoll(config + valueStart(config, len, "bypass_until_epoch", 0, len));
    if (bypassUntil <= 0) return false;
    time_t now = time(nullptr);
    return now < (time_t)bypassUntil;
}

static bool arrayBounds(const char* text, size_t len, const char* key, size_t begin, size_t end, size_t* outBegin, size_t* outEnd) {
    size_t value = valueStart(text, len, key, begin, end);
    if (value == SIZE_MAX || value >= len || text[value] != '[') return false;
    size_t close = findMatching(text, len, value, ']');
    if (close == SIZE_MAX) return false;
    *outBegin = value;
    *outEnd = close;
    return true;
}

static char* lowerCopy(const char* value) {
    size_t len = strlen(value);
    char* out = dupRange(value, len);
    if (!out) return nullptr;
    for (size_t i = 0; i < len; ++i) out[i] = static_cast<char>(tolower(static_cast<unsigned char>(out[i])));
    return out;
}

static char* currentSsid() {
    wchar_t* testSsid = envWide(L"WW_TEST_CURRENT_SSID");
    if (testSsid && testSsid[0]) {
        bool disconnected = wcscmp(testSsid, L"<no-adapter>") == 0 || wcscmp(testSsid, L"<disconnected>") == 0;
        char* out = disconnected ? dupRange("", 0) : wideToUtf8(testSsid);
        free(testSsid);
        return out;
    }
    free(testSsid);

    HANDLE handle = nullptr;
    DWORD negotiated = 0;
    if (WlanOpenHandle(2, nullptr, &negotiated, &handle) != ERROR_SUCCESS) return dupRange("", 0);
    PWLAN_INTERFACE_INFO_LIST interfaces = nullptr;
    if (WlanEnumInterfaces(handle, nullptr, &interfaces) != ERROR_SUCCESS || !interfaces) {
        WlanCloseHandle(handle, nullptr);
        return dupRange("", 0);
    }
    char* ssid = dupRange("", 0);
    for (DWORD i = 0; i < interfaces->dwNumberOfItems && (!ssid || !ssid[0]); ++i) {
        if (interfaces->InterfaceInfo[i].isState != wlan_interface_state_connected) continue;
        DWORD size = 0;
        PWLAN_CONNECTION_ATTRIBUTES attrs = nullptr;
        WLAN_OPCODE_VALUE_TYPE opcode{};
        if (WlanQueryInterface(handle, &interfaces->InterfaceInfo[i].InterfaceGuid, wlan_intf_opcode_current_connection, nullptr, &size, reinterpret_cast<PVOID*>(&attrs), &opcode) == ERROR_SUCCESS && attrs) {
            freeStr(ssid);
            DOT11_SSID dot11 = attrs->wlanAssociationAttributes.dot11Ssid;
            ssid = dupRange(reinterpret_cast<const char*>(dot11.ucSSID), dot11.uSSIDLength);
            WlanFreeMemory(attrs);
        }
    }
    WlanFreeMemory(interfaces);
    WlanCloseHandle(handle, nullptr);
    return ssid;
}

struct CurrentNetwork {
    char* type;
    char* id;
};

static void freeNetwork(CurrentNetwork* network) {
    if (!network) return;
    freeStr(network->type);
    freeStr(network->id);
    network->type = nullptr;
    network->id = nullptr;
}


static CurrentNetwork* getAllActiveNetworks(int* outCount) {
    CurrentNetwork* networks = static_cast<CurrentNetwork*>(malloc(sizeof(CurrentNetwork) * 16));
    if (!networks) return nullptr;
    int count = 0;

    wchar_t* testType = envWide(L"WW_TEST_CURRENT_NETWORK_TYPE");
    wchar_t* testId = envWide(L"WW_TEST_CURRENT_NETWORK_ID");
    if ((testType && testType[0]) || (testId && testId[0])) {
        if (testId && testId[0] && wcscmp(testId, L"<no-adapter>") != 0 && wcscmp(testId, L"<disconnected>") != 0) {
            networks[count].type = (testType && testType[0]) ? wideToUtf8(testType) : dupRange("wired", 5);
            networks[count].id = wideToUtf8(testId);
            ++count;
        }
        free(testType); free(testId);
        *outCount = count;
        return networks;
    }
    free(testType); free(testId);

    char* ssid = currentSsid();
    if (ssid && ssid[0]) {
        networks[count].type = dupRange("wifi", 4);
        networks[count].id = ssid;
        ++count;
    }
    freeStr(ssid);

    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG size = 16 * 1024;
    IP_ADAPTER_ADDRESSES* addresses = static_cast<IP_ADAPTER_ADDRESSES*>(malloc(size));
    if (!addresses) { *outCount = count; return networks; }
    ULONG result = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, addresses, &size);
    if (result == ERROR_BUFFER_OVERFLOW) {
        free(addresses);
        addresses = static_cast<IP_ADAPTER_ADDRESSES*>(malloc(size));
        result = addresses ? GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, addresses, &size) : ERROR_NOT_ENOUGH_MEMORY;
    }
    if (result == NO_ERROR) {
        for (IP_ADAPTER_ADDRESSES* item = addresses; item; item = item->Next) {
            if (item->IfType != IF_TYPE_ETHERNET_CSMACD) continue;
            if (item->OperStatus != IfOperStatusUp || !item->FirstUnicastAddress) continue;
            if (count < 16) {
                networks[count].type = dupRange("wired", 5);
                networks[count].id = item->FriendlyName ? wideToUtf8(item->FriendlyName) : dupRange(item->AdapterName ? item->AdapterName : "", strlen(item->AdapterName ? item->AdapterName : ""));
                ++count;
            }
        }
    }
    free(addresses);
    *outCount = count;
    return networks;
}

static void freeAllNetworks(CurrentNetwork* networks, int count) {
    if (!networks) return;
    for (int i = 0; i < count; ++i) freeNetwork(&networks[i]);
    free(networks);
}

static CurrentNetwork currentNetwork() __attribute__((unused));
static CurrentNetwork currentNetwork() {
    CurrentNetwork network{};
    wchar_t* testType = envWide(L"WW_TEST_CURRENT_NETWORK_TYPE");
    wchar_t* testId = envWide(L"WW_TEST_CURRENT_NETWORK_ID");
    if ((testType && testType[0]) || (testId && testId[0])) {
        network.type = (testType && testType[0]) ? wideToUtf8(testType) : dupRange("wired", 5);
        bool disconnected = !testId || !testId[0] || wcscmp(testId, L"<no-adapter>") == 0 || wcscmp(testId, L"<disconnected>") == 0;
        network.id = disconnected ? dupRange("", 0) : wideToUtf8(testId);
        free(testType);
        free(testId);
        return network;
    }
    free(testType);
    free(testId);

    char* ssid = currentSsid();
    if (ssid && ssid[0]) {
        network.type = dupRange("wifi", 4);
        network.id = ssid;
        return network;
    }
    freeStr(ssid);

    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG size = 16 * 1024;
    IP_ADAPTER_ADDRESSES* addresses = static_cast<IP_ADAPTER_ADDRESSES*>(malloc(size));
    if (!addresses) {
        network.type = dupRange("", 0);
        network.id = dupRange("", 0);
        return network;
    }
    ULONG result = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, addresses, &size);
    if (result == ERROR_BUFFER_OVERFLOW) {
        free(addresses);
        addresses = static_cast<IP_ADAPTER_ADDRESSES*>(malloc(size));
        result = addresses ? GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, addresses, &size) : ERROR_NOT_ENOUGH_MEMORY;
    }
    if (result == NO_ERROR) {
        for (IP_ADAPTER_ADDRESSES* item = addresses; item; item = item->Next) {
            if (item->IfType != IF_TYPE_ETHERNET_CSMACD) continue;
            if (item->OperStatus != IfOperStatusUp || !item->FirstUnicastAddress) continue;
            network.type = dupRange("wired", 5);
            network.id = item->FriendlyName ? wideToUtf8(item->FriendlyName) : dupRange(item->AdapterName ? item->AdapterName : "", strlen(item->AdapterName ? item->AdapterName : ""));
            break;
        }
    }
    free(addresses);
    if (!network.type) network.type = dupRange("", 0);
    if (!network.id) network.id = dupRange("", 0);
    return network;
}

struct Match {
    char* ruleId;
    char* appName;
};

static bool ruleBlocks(const char* config, size_t len, const char* networkType, const char* networkId, const char* appPath, const char* requestedRule, Match* match) {
    size_t rulesBegin = 0;
    size_t rulesEnd = 0;
    if (!arrayBounds(config, len, "rules", 0, len, &rulesBegin, &rulesEnd)) return false;

    char* appLower = lowerCopy(appPath);
    bool found = false;
    size_t pos = rulesBegin + 1;
    while (!found && pos < rulesEnd) {
        const char* object = static_cast<const char*>(memchr(config + pos, '{', rulesEnd - pos));
        if (!object) break;
        size_t objectBegin = static_cast<size_t>(object - config);
        size_t objectEnd = findMatching(config, len, objectBegin, '}');
        if (objectEnd == SIZE_MAX || objectEnd > rulesEnd) break;

        char* ruleId = jsonString(config, len, "id", objectBegin, objectEnd);
        char* ruleSsid = jsonString(config, len, "ssid", objectBegin, objectEnd);
        char* ruleNetworkType = jsonString(config, len, "network_type", objectBegin, objectEnd);
        char* ruleNetworkId = jsonString(config, len, "network_id", objectBegin, objectEnd);
        if (!ruleNetworkType || !ruleNetworkType[0]) {
            freeStr(ruleNetworkType);
            ruleNetworkType = dupRange("wifi", 4);
        }
        if (!ruleNetworkId || !ruleNetworkId[0]) {
            freeStr(ruleNetworkId);
            ruleNetworkId = dupRange(ruleSsid, strlen(ruleSsid));
        }
        bool ruleMatches = (!requestedRule || !requestedRule[0] || strcmp(ruleId, requestedRule) == 0)
            && strcmp(ruleNetworkType, networkType) == 0
            && strcmp(ruleNetworkId, networkId) == 0;
        if (ruleMatches) {
            size_t appsBegin = 0;
            size_t appsEnd = 0;
            if (arrayBounds(config, len, "blocked_apps", objectBegin, objectEnd, &appsBegin, &appsEnd)) {
                size_t appPos = appsBegin + 1;
                while (!found && appPos < appsEnd) {
                    const char* appObject = static_cast<const char*>(memchr(config + appPos, '{', appsEnd - appPos));
                    if (!appObject) break;
                    size_t appBegin = static_cast<size_t>(appObject - config);
                    size_t appEnd = findMatching(config, len, appBegin, '}');
                    if (appEnd == SIZE_MAX || appEnd > appsEnd) break;
                    char* original = jsonString(config, len, "original_path", appBegin, appEnd);
                    char* originalLower = lowerCopy(original);
                    if (originalLower && appLower && strcmp(originalLower, appLower) == 0) {
                        match->ruleId = dupRange(ruleId, strlen(ruleId));
                        match->appName = jsonString(config, len, "name", appBegin, appEnd);
                        found = true;
                    }
                    freeStr(original);
                    freeStr(originalLower);
                    appPos = appEnd + 1;
                }
            }
        }
        freeStr(ruleId);
        freeStr(ruleSsid);
        freeStr(ruleNetworkType);
        freeStr(ruleNetworkId);
        pos = objectEnd + 1;
    }
    freeStr(appLower);
    return found;
}


static bool ruleBlocksAnyNetwork(const char* config, size_t len, CurrentNetwork* networks, int networkCount, const char* appPath, const char* requestedRule, Match* match) {
    for (int i = 0; i < networkCount; ++i) {
        if (ruleBlocks(config, len, networks[i].type, networks[i].id, appPath, requestedRule, match)) {
            return true;
        }
    }
    return false;
}

static bool httpAlive(int port) {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        WSACleanup();
        return false;
    }
    DWORD timeout = 120;
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

static bool shellOpenWithArgs(const char* target, const char* arguments) {
    if (GetEnvironmentVariableW(L"WW_TEST_SHELLOPEN_FAIL", nullptr, 0) > 0) return false;
    wchar_t* wide = utf8ToWide(target);
    if (!wide) return false;
    wchar_t* wideArgs = (arguments && arguments[0]) ? utf8ToWide(arguments) : nullptr;
    HINSTANCE result = ShellExecuteW(nullptr, L"open", wide, wideArgs, nullptr, SW_SHOWNORMAL);
    free(wideArgs);
    free(wide);
    return reinterpret_cast<intptr_t>(result) > 32;
}

static bool shellOpen(const char* target) {
    return shellOpenWithArgs(target, "");
}

static void writeFallbackMarker(const char* reason) {
    wchar_t* marker = envWide(L"WW_TEST_FALLBACK_FILE");
    if (!marker || !marker[0]) {
        free(marker);
        return;
    }
    HANDLE file = CreateFileW(marker, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    free(marker);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, reason, static_cast<DWORD>(strlen(reason)), &written, nullptr);
    CloseHandle(file);
}

static void showFallbackWarning(bool serviceAlive) {
    writeFallbackMarker(serviceAlive ? "browser_open_failed" : "service_unavailable");
    if (GetEnvironmentVariableW(L"WW_TEST_NO_MESSAGEBOX", nullptr, 0) > 0) return;
    MessageBoxW(
        nullptr,
        serviceAlive
            ? L"WiFi 提醒无法打开浏览器警告页，应用将被允许启动。"
            : L"WiFi 提醒服务未运行，应用将被允许启动。",
        L"WiFi 提醒",
        MB_OK | MB_ICONWARNING);
}

static void writeStdout(const char* text) {
    DWORD written = 0;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out == INVALID_HANDLE_VALUE || out == nullptr) return;
    WriteFile(out, text, static_cast<DWORD>(strlen(text)), &written, nullptr);
}

static const char* baseName(const char* path) {
    const char* slash1 = strrchr(path, '\\');
    const char* slash2 = strrchr(path, '/');
    const char* slash = slash1 > slash2 ? slash1 : slash2;
    return slash ? slash + 1 : path;
}

static void appendJsonEscaped(char* out, size_t cap, size_t* used, const char* value) {
    for (const char* p = value; *p && *used + 2 < cap; ++p) {
        if (*p == '"' || *p == '\\') {
            out[(*used)++] = '\\';
            out[(*used)++] = *p;
        } else if (*p == '\n') {
            out[(*used)++] = '\\';
            out[(*used)++] = 'n';
        } else {
            out[(*used)++] = *p;
        }
    }
    out[*used] = 0;
}

static void appendText(char* out, size_t cap, size_t* used, const char* value) {
    while (*value && *used + 1 < cap) out[(*used)++] = *value++;
    out[*used] = 0;
}

static void timestamp(char* out, size_t cap, bool dateOnly) {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    if (dateOnly) snprintf(out, cap, "%04u-%02u-%02u", t.wYear, t.wMonth, t.wDay);
    else snprintf(out, cap, "%04u-%02u-%02uT%02u:%02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}

static void logBlocked(const char* networkId, const char* app, const char* ruleId) {
    wchar_t* appData = envWide(L"APPDATA");
    wchar_t* root = appData ? joinWide3(appData, L"\\WiFiWarning", L"") : joinWide3(L".", L"\\WiFiWarning", L"");
    if (appData) free(appData);
    if (!root) return;
    CreateDirectoryW(root, nullptr);
    wchar_t* logs = joinWide3(root, L"\\logs", L"");
    CreateDirectoryW(logs, nullptr);
    char day[16]{};
    timestamp(day, sizeof(day), true);
    wchar_t* dayWide = utf8ToWide(day);
    wchar_t* file = joinWide3(logs, L"\\", dayWide);
    wchar_t* fileJsonl = joinWide3(file, L".jsonl", L"");
    free(root);
    free(logs);
    free(dayWide);
    free(file);
    if (!fileJsonl) return;

    HANDLE handle = CreateFileW(fileJsonl, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    free(fileJsonl);
    if (handle == INVALID_HANDLE_VALUE) return;

    char now[32]{};
    timestamp(now, sizeof(now), false);
    char line[2048]{};
    size_t used = 0;
    appendText(line, sizeof(line), &used, "{\"action\":\"blocked\",\"app\":\"");
    appendJsonEscaped(line, sizeof(line), &used, baseName(app));
    appendText(line, sizeof(line), &used, "\",\"rule_id\":\"");
    appendJsonEscaped(line, sizeof(line), &used, ruleId);
    appendText(line, sizeof(line), &used, "\",\"ssid\":\"");
    appendJsonEscaped(line, sizeof(line), &used, networkId);
    appendText(line, sizeof(line), &used, "\",\"timestamp\":\"");
    appendText(line, sizeof(line), &used, now);
    appendText(line, sizeof(line), &used, "\"}\n");
    DWORD written = 0;
    WriteFile(handle, line, static_cast<DWORD>(strlen(line)), &written, nullptr);
    CloseHandle(handle);
}

static char hexDigit(unsigned char value) {
    return static_cast<char>(value < 10 ? '0' + value : 'A' + value - 10);
}

static char* urlEncode(const char* value) {
    size_t len = strlen(value);
    char* out = static_cast<char*>(malloc(len * 3 + 1));
    if (!out) return nullptr;
    size_t used = 0;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = static_cast<unsigned char>(value[i]);
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out[used++] = static_cast<char>(c);
        else if (c == ' ') out[used++] = '+';
        else {
            out[used++] = '%';
            out[used++] = hexDigit(c >> 4);
            out[used++] = hexDigit(c & 15);
        }
    }
    out[used] = 0;
    return out;
}

static char* makeWarningUrl(int port, const char* appPath, const char* appName, const char* appArgs, const char* networkType, const char* networkId, const char* ruleId) {
    char* eApp = urlEncode(appPath);
    char* eName = urlEncode(appName);
    char* eArgs = urlEncode(appArgs ? appArgs : "");
    char* eType = urlEncode(networkType);
    char* eSsid = urlEncode(networkId);
    char* eRule = urlEncode(ruleId);
    size_t needed = strlen(eApp) + strlen(eName) + strlen(eArgs) + strlen(eType) + strlen(eSsid) + strlen(eRule) + 210;
    char* out = static_cast<char*>(malloc(needed));
    if (out) {
        snprintf(out, needed, "http://localhost:%d/warning?app=%s&appName=%s&appArgs=%s&networkType=%s&ssid=%s&networkId=%s&ruleId=%s", port, eApp, eName, eArgs, eType, eSsid, eSsid, eRule);
    }
    freeStr(eApp);
    freeStr(eName);
    freeStr(eArgs);
    freeStr(eType);
    freeStr(eSsid);
    freeStr(eRule);
    return out;
}

static void printDryAllow(const char* reason, const char* ssid = nullptr) {
    char out[512]{};
    if (ssid) snprintf(out, sizeof(out), "{\"decision\":\"allow\",\"reason\":\"%s\",\"ssid\":\"%s\"}\n", reason, ssid);
    else snprintf(out, sizeof(out), "{\"decision\":\"allow\",\"reason\":\"%s\"}\n", reason);
    writeStdout(out);
}

static void printDryBlock(const char* ruleId, const char* appName, const char* networkType, const char* networkId, const char* url) {
    char out[4096]{};
    size_t used = 0;
    appendText(out, sizeof(out), &used, "{\"decision\":\"block\",\"reason\":\"matching_rule\",\"rule_id\":\"");
    appendJsonEscaped(out, sizeof(out), &used, ruleId);
    appendText(out, sizeof(out), &used, "\",\"app\":\"");
    appendJsonEscaped(out, sizeof(out), &used, appName);
    appendText(out, sizeof(out), &used, "\",\"network_type\":\"");
    appendJsonEscaped(out, sizeof(out), &used, networkType);
    appendText(out, sizeof(out), &used, "\",\"ssid\":\"");
    appendJsonEscaped(out, sizeof(out), &used, networkId);
    appendText(out, sizeof(out), &used, "\",\"url\":\"");
    appendJsonEscaped(out, sizeof(out), &used, url);
    appendText(out, sizeof(out), &used, "\"}\n");
    writeStdout(out);
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    char* appPath = argValue(argc, argv, L"--app");
    char* appArgs = argValue(argc, argv, L"--app-args");
    char* ruleId = argValue(argc, argv, L"--rule");
    if (!ruleId || !ruleId[0]) {
        freeStr(ruleId);
        ruleId = argValue(argc, argv, L"--rule-id");
    }
    char* overrideSsid = argValue(argc, argv, L"--ssid");
    char* overrideNetworkType = argValue(argc, argv, L"--network-type");
    char* overrideNetworkId = argValue(argc, argv, L"--network-id");
    char* configOverride = argValue(argc, argv, L"--config");
    bool dryRun = hasArg(argc, argv, L"--dry-run");
    bool noLaunch = hasArg(argc, argv, L"--no-launch");
    LocalFree(argv);

    if (!appPath || !appPath[0]) {
        if (dryRun) writeStdout("{\"decision\":\"error\",\"reason\":\"missing_app\"}\n");
        else MessageBoxW(nullptr, L"缺少要启动的应用路径。", L"WiFi 提醒", MB_OK | MB_ICONERROR);
        freeStr(appPath);
        freeStr(appArgs);
        freeStr(ruleId);
        freeStr(overrideSsid);
        freeStr(overrideNetworkType);
        freeStr(overrideNetworkId);
        freeStr(configOverride);
        return 2;
    }

    wchar_t* configPath = (configOverride && configOverride[0]) ? utf8ToWide(configOverride) : defaultConfigPath();
    char* config = readUtf8File(configPath);
    size_t configLen = config ? strlen(config) : 0;
    free(configPath);

    if (!config || !config[0] || !jsonBool(config, configLen, "protection_enabled", true)) {
        if (!dryRun && !noLaunch) shellOpenWithArgs(appPath, appArgs);
        if (dryRun) printDryAllow("config_or_disabled");
        freeStr(config);
        freeStr(appPath);
        freeStr(appArgs);
        freeStr(ruleId);
        freeStr(overrideSsid);
        freeStr(overrideNetworkType);
        freeStr(overrideNetworkId);
        freeStr(configOverride);
        return 0;
    }

    if (hasActiveBypass(config, configLen)) {
        if (!dryRun && !noLaunch) shellOpenWithArgs(appPath, appArgs);
        if (dryRun) printDryAllow("active_bypass");
        freeStr(config);
        freeStr(appPath);
        freeStr(appArgs);
        freeStr(ruleId);
        freeStr(overrideSsid);
        freeStr(overrideNetworkType);
        freeStr(overrideNetworkId);
        freeStr(configOverride);
        return 0;
    }

    // Collect all active networks (WiFi + wired) for multi-network matching
    int activeCount = 0;
    CurrentNetwork* activeNetworks = nullptr;
    if (overrideNetworkId && overrideNetworkId[0]) {
        activeNetworks = static_cast<CurrentNetwork*>(malloc(sizeof(CurrentNetwork)));
        activeNetworks[0].type = (overrideNetworkType && overrideNetworkType[0]) ? dupRange(overrideNetworkType, strlen(overrideNetworkType)) : dupRange("wired", 5);
        activeNetworks[0].id = dupRange(overrideNetworkId, strlen(overrideNetworkId));
        activeCount = 1;
    } else if (overrideSsid && overrideSsid[0]) {
        activeNetworks = static_cast<CurrentNetwork*>(malloc(sizeof(CurrentNetwork)));
        activeNetworks[0].type = dupRange("wifi", 4);
        activeNetworks[0].id = dupRange(overrideSsid, strlen(overrideSsid));
        activeCount = 1;
    } else {
        activeNetworks = getAllActiveNetworks(&activeCount);
    }
    if (activeCount == 0) {
        if (!dryRun && !noLaunch) shellOpenWithArgs(appPath, appArgs);
        if (dryRun) printDryAllow("no_wifi");
        freeAllNetworks(activeNetworks, activeCount);
        freeStr(config);
        freeStr(appPath);
        freeStr(appArgs);
        freeStr(ruleId);
        freeStr(overrideSsid);
        freeStr(overrideNetworkType);
        freeStr(overrideNetworkId);
        freeStr(configOverride);
        return 0;
    }

    Match match{};
    if (!ruleBlocksAnyNetwork(config, configLen, activeNetworks, activeCount, appPath, ruleId, &match)) {
        if (!dryRun && !noLaunch) shellOpenWithArgs(appPath, appArgs);
        if (dryRun) printDryAllow("no_matching_rule", activeCount > 0 ? activeNetworks[0].id : "");
        freeAllNetworks(activeNetworks, activeCount);
        freeStr(config);
        freeStr(appPath);
        freeStr(appArgs);
        freeStr(ruleId);
        freeStr(overrideSsid);
        freeStr(overrideNetworkType);
        freeStr(overrideNetworkId);
        freeStr(configOverride);
        return 0;
    }

    const char* matchedNetworkId = "";
    const char* matchedNetworkType = "";
    // Find which network matched
    for (int n = 0; n < activeCount; ++n) {
        Match tmp{};
        if (ruleBlocks(config, configLen, activeNetworks[n].type, activeNetworks[n].id, appPath, ruleId, &tmp)) {
            matchedNetworkId = activeNetworks[n].id;
            matchedNetworkType = activeNetworks[n].type;
            break;
        }
    }

    if (!dryRun) logBlocked(matchedNetworkId, appPath, match.ruleId);
    int port = jsonInt(config, configLen, "http_port", 18765);
    if (port <= 0) port = 18765;

    const char* appName = (match.appName && match.appName[0]) ? match.appName : baseName(appPath);
    char* url = makeWarningUrl(port, appPath, appName, appArgs, matchedNetworkType, matchedNetworkId, match.ruleId);

    if (dryRun) {
        printDryBlock(match.ruleId, appName, matchedNetworkType, matchedNetworkId, url ? url : "");
    } else if (url && httpAlive(port)) {
        if (!shellOpen(url)) {
            showFallbackWarning(true);
            if (!noLaunch) shellOpenWithArgs(appPath, appArgs);
        }
    } else {
        showFallbackWarning(false);
        if (!noLaunch) shellOpenWithArgs(appPath, appArgs);
    }

    freeStr(url);
    freeStr(match.ruleId);
    freeStr(match.appName);
    freeAllNetworks(activeNetworks, activeCount);
    freeStr(config);
    freeStr(appPath);
    freeStr(appArgs);
    freeStr(ruleId);
    freeStr(overrideSsid);
    freeStr(overrideNetworkType);
    freeStr(overrideNetworkId);
    freeStr(configOverride);
    return 0;
}
