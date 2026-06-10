#include "ui/api_handlers.h"

#include "core/auto_start.h"
#include "core/network_manager.h"
#include "core/shortcut_manager.h"
#include "core/util.h"
#include "core/wifi_detector.h"
#include "core/wifi_switcher.h"
#include "ui/browser_launcher.h"

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <winver.h>

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <optional>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace ww {

ApiHandlers::ApiHandlers(ConfigManager& config, Logger& logger) : config_(config), logger_(logger) {}

JsonValue requestBodyJson(const std::string& body) {
    std::string error;
    auto parsed = parseJson(body, &error);
    return parsed.value_or(JsonValue::Object{});
}

HttpResponse jsonResponse(const JsonValue& value, int status) {
    HttpResponse response;
    response.status = status;
    response.body = stringifyJson(value);
    return response;
}

HttpResponse errorResponse(const std::string& message, int status) {
    return jsonResponse(JsonValue::Object{
        {"ok", false},
        {"error", message}
    }, status);
}

bool ApiHandlers::handle(const HttpRequest& request, HttpResponse& response) {
    if (request.path == "/api/config" && request.method == "GET") response = configGet();
    else if (request.path == "/api/config" && request.method == "POST") response = configPost(request.body);
    else if (request.path == "/api/wifi/current" && request.method == "GET") response = wifiCurrent();
    else if (request.path == "/api/wifi/available" && request.method == "GET") response = wifiAvailable();
    else if (request.path == "/api/wifi/switch" && request.method == "POST") response = wifiSwitch(request.body);
    else if (request.path == "/api/network/current" && request.method == "GET") response = networkCurrent();
    else if (request.path == "/api/network/wired" && request.method == "GET") response = wiredAdapters();
    else if (request.path == "/api/network/wired/toggle" && request.method == "POST") response = wiredToggle(request.body);
    else if (request.path == "/api/stats" && request.method == "GET") response = stats();
    else if (request.path == "/api/bypass" && request.method == "POST") response = bypass(request.body);
    else if (request.path == "/api/apps/browse" && request.method == "POST") response = appBrowse();
    else if (request.path == "/api/shortcuts/browse" && request.method == "POST") response = shortcutBrowse();
    else if (request.path == "/api/shortcuts/read" && request.method == "POST") response = shortcutRead(request.body);
    else if (request.path == "/api/apps/icon" && request.method == "GET") response = appIcon(request);
    else if (request.path == "/api/apps/status" && request.method == "GET") response = appStatus();
    else if (request.path == "/api/apps/cleanup" && request.method == "POST") response = appCleanup(request.body);
    else if (request.path == "/api/shortcuts/scan" && request.method == "POST") response = shortcutScan(request.body);
    else if (request.path == "/api/shortcuts/replace" && request.method == "POST") response = shortcutReplace(request.body);
    else if (request.path == "/api/shortcuts/restore" && request.method == "POST") response = restoreShortcuts(request.body);
    else return false;
    return true;
}

HttpResponse ApiHandlers::configGet() {
    auto config = config_.load();
    if (reconcileShortcutRecords(config) > 0) config_.save(config);
    fs::path backupRoot = fs::path(executableDir()) / L"shortcut-backups";
    return jsonResponse(JsonValue::Object{
        {"ok", true},
        {"config", configToJson(config)},
        {"config_path", wideToUtf8(config_.path())},
        {"backup_path", wideToUtf8(backupRoot.wstring())},
        {"auto_start_registered", isAutoStartEnabled()}
    });
}

HttpResponse ApiHandlers::configPost(const std::string& body) {
    JsonValue parsed = requestBodyJson(body);
    const JsonValue* configValue = parsed.get("config");
    if (!configValue) configValue = &parsed;
    AppConfig previous = config_.load();
    AppConfig config = configFromJson(*configValue);
    if (config.settings.http_port <= 0) config.settings.http_port = 18765;

    JsonValue::Array restoreResults;
    if (previous.settings.protection_enabled && !config.settings.protection_enabled) {
        std::vector<ShortcutOperationResult> results;
        for (const auto& result : restoreAllShortcuts(previous)) {
            results.push_back(result);
            restoreResults.push_back(JsonValue::Object{
                {"ok", result.ok},
                {"shortcut", result.shortcut},
                {"backup", result.backup},
                {"error", result.error}
            });
        }
        clearRestoredShortcutRecords(config, results);
    }

    if (!config_.save(config)) return errorResponse("配置保存失败", 500);
    std::string autoStartError;
    bool autoStartSynced = setAutoStart(config.settings.auto_start, executablePath(), &autoStartError);
    return jsonResponse(JsonValue::Object{
        {"ok", true},
        {"config", configToJson(config)},
        {"restore_results", restoreResults},
        {"auto_start_synced", autoStartSynced},
        {"auto_start_registered", isAutoStartEnabled()},
        {"auto_start_error", autoStartError}
    });
}

HttpResponse ApiHandlers::wifiCurrent() {
    auto wifi = getCurrentWifi();
    return jsonResponse(JsonValue::Object{
        {"ok", true},
        {"adapter_available", wifi.adapter_available},
        {"connected", wifi.connected},
        {"ssid", wifi.ssid},
        {"interface", wifi.interface_description},
        {"error", wifi.error}
    });
}

HttpResponse ApiHandlers::wifiAvailable() {
    JsonValue::Array known;
    for (const auto& profile : getKnownWifiProfiles()) known.push_back(profile);
    return jsonResponse(JsonValue::Object{
        {"ok", true},
        {"networks", availableNetworksJson()},
        {"known_profiles", known}
    });
}

HttpResponse ApiHandlers::wifiSwitch(const std::string& body) {
    JsonValue parsed = requestBodyJson(body);
    std::string ssid = parsed.get("ssid") ? parsed.get("ssid")->asString() : "";
    std::string password = parsed.get("password") ? parsed.get("password")->asString() : "";
    if (ssid.empty()) return errorResponse("缺少 WiFi 名称");
    auto result = connectToWifiDetailed(ssid, password);
    LogRecord switchRecord;
    switchRecord.timestamp = nowIsoLocal();
    switchRecord.wifi_action = result.connected ? "switched_to" : (result.native_dialog_opened ? "native_dialog_opened" : (result.connect_requested ? "connect_requested" : "switch_failed"));
    switchRecord.target_ssid = ssid;
    logger_.write(switchRecord);
    bool accepted = result.connected || result.native_dialog_opened || result.connect_requested;
    return jsonResponse(JsonValue::Object{
        {"ok", accepted},
        {"connected", result.connected},
        {"native_dialog_opened", result.native_dialog_opened},
        {"connect_requested", result.connect_requested},
        {"status", result.status},
        {"error", result.error},
        {"password_supplied", !password.empty()},
        {"target_ssid", ssid}
    }, accepted ? 200 : 409);
}

HttpResponse ApiHandlers::networkCurrent() {
    return jsonResponse(networkStatusJson(getCurrentNetwork()));
}

HttpResponse ApiHandlers::wiredAdapters() {
    return jsonResponse(JsonValue::Object{
        {"ok", true},
        {"adapters", wiredAdaptersJson()}
    });
}

HttpResponse ApiHandlers::wiredToggle(const std::string& body) {
    JsonValue parsed = requestBodyJson(body);
    std::string adapterId = parsed.get("id") ? parsed.get("id")->asString() : "";
    bool enabled = parsed.get("enabled") ? parsed.get("enabled")->asBool(true) : true;
    if (adapterId.empty()) return errorResponse("missing adapter id");

    auto result = setWiredAdapterEnabled(adapterId, enabled);
    LogRecord record;
    record.timestamp = nowIsoLocal();
    record.wifi_action = enabled ? "wired_enable_requested" : "wired_disable_requested";
    record.target_ssid = adapterId;
    record.error = result.error;
    logger_.write(record);
    return jsonResponse(networkActionJson(result), result.ok ? 200 : 409);
}

HttpResponse ApiHandlers::stats() {
    return jsonResponse(JsonValue::Object{{"ok", true}, {"stats", logger_.statsJson(30)}});
}

HttpResponse ApiHandlers::bypass(const std::string& body) {
    JsonValue parsed = requestBodyJson(body);
    std::string password = parsed.get("password") ? parsed.get("password")->asString() : "";
    std::string app = parsed.get("app") ? parsed.get("app")->asString() : "";
    std::string appArgs = parsed.get("app_args") ? parsed.get("app_args")->asString() : "";
    std::string ruleId = parsed.get("rule_id") ? parsed.get("rule_id")->asString() : "";
    auto config = config_.load();

    if (config.settings.bypass_password.empty()) return errorResponse("尚未设置临时允许密码", 403);
    if (sha256Hex(password) != config.settings.bypass_password) return errorResponse("密码不正确", 403);

    // Set bypass_until_epoch so rule_engine and ww-launch will allow during this window
    int timeoutMin = config.settings.bypass_timeout_minutes > 0 ? config.settings.bypass_timeout_minutes : 30;
    config.settings.bypass_until_epoch = nowUnixSeconds() + static_cast<int64_t>(timeoutMin) * 60;
    config_.save(config);

    LogRecord bypassRecord;
    bypassRecord.timestamp = nowIsoLocal();
    bypassRecord.app = pathBaseNameUtf8(app);
    bypassRecord.action = "bypassed";
    bypassRecord.rule_id = ruleId;
    bypassRecord.bypass_type = "password";
    logger_.write(bypassRecord);

    bool launched = app.empty() ? false : launchAppPath(app, appArgs);
    return jsonResponse(JsonValue::Object{{"ok", true}, {"launched", launched}, {"bypass_minutes", timeoutMin}});
}

static std::string trimNullTerminated(std::wstring value) {
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    return wideToUtf8(value);
}

static std::string versionField(const std::wstring& path, const wchar_t* key) {
    DWORD handle = 0;
    DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (!size) return "";

    std::vector<unsigned char> data(size);
    if (!GetFileVersionInfoW(path.c_str(), handle, size, data.data())) return "";

    struct Translation {
        WORD language;
        WORD codepage;
    };

    Translation* translations = nullptr;
    UINT translationBytes = 0;
    std::vector<Translation> candidates;
    if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations), &translationBytes) && translations) {
        size_t count = translationBytes / sizeof(Translation);
        for (size_t i = 0; i < count; ++i) candidates.push_back(translations[i]);
    }
    candidates.push_back({0x0409, 0x04B0});

    for (const auto& item : candidates) {
        wchar_t block[128]{};
        std::swprintf(block, sizeof(block) / sizeof(block[0]), L"\\StringFileInfo\\%04x%04x\\%ls", item.language, item.codepage, key);
        wchar_t* value = nullptr;
        UINT valueChars = 0;
        if (VerQueryValueW(data.data(), block, reinterpret_cast<void**>(&value), &valueChars) && value && valueChars) {
            std::string text = trimNullTerminated(std::wstring(value, value + valueChars));
            if (!text.empty()) return text;
        }
    }
    return "";
}

static JsonValue appInfoJson(const std::string& path, const std::string& overrideName = "") {
    fs::path file(utf8ToWide(path));
    std::string name = overrideName;
    if (name.empty()) name = versionField(file.wstring(), L"FileDescription");
    if (name.empty()) name = versionField(file.wstring(), L"ProductName");
    if (name.empty()) name = wideToUtf8(file.stem().wstring());
    return JsonValue::Object{
        {"path", path},
        {"name", name},
        {"icon_path", path}
    };
}

static std::optional<JsonValue::Array> testPickedApps() {
    std::wstring overrideJson = getEnvWide(L"WW_TEST_PICK_APPS_JSON");
    if (overrideJson.empty()) return std::nullopt;

    std::string error;
    auto parsed = parseJson(wideToUtf8(overrideJson), &error);
    JsonValue::Array apps;
    if (!parsed) return apps;
    for (const auto& item : parsed->asArray()) {
        if (item.isString()) {
            apps.push_back(appInfoJson(item.asString()));
            continue;
        }
        const JsonValue* path = item.get("path");
        if (!path) path = item.get("original_path");
        if (!path) continue;
        std::string name = item.get("name") ? item.get("name")->asString() : "";
        apps.push_back(appInfoJson(path->asString(), name));
    }
    return apps;
}

static std::optional<JsonValue::Array> testPickedShortcuts() {
    std::wstring overrideJson = getEnvWide(L"WW_TEST_PICK_SHORTCUTS_JSON");
    if (overrideJson.empty()) return std::nullopt;

    std::string error;
    auto parsed = parseJson(wideToUtf8(overrideJson), &error);
    JsonValue::Array shortcuts;
    if (!parsed) return shortcuts;
    for (const auto& item : parsed->asArray()) {
        std::string path;
        if (item.isString()) {
            path = item.asString();
        } else if (const JsonValue* pathValue = item.get("path")) {
            path = pathValue->asString();
        }
        if (path.empty()) continue;
        ShortcutCandidate candidate;
        if (readShortcutFile(path, candidate)) {
            shortcuts.push_back(JsonValue::Object{
                {"path", candidate.path},
                {"target_path", candidate.target_path},
                {"arguments", candidate.arguments},
                {"working_dir", candidate.working_dir},
                {"icon_path", candidate.icon_path},
                {"icon_index", candidate.icon_index},
                {"description", candidate.description}
            });
        } else {
            shortcuts.push_back(JsonValue::Object{
                {"path", path},
                {"target_path", ""},
                {"error", "读取快捷方式失败"}
            });
        }
    }
    return shortcuts;
}

static JsonValue::Array browseFiles(bool* cancelled, std::string* error, const wchar_t* title, const wchar_t* filter) {
    JsonValue::Array apps;
    *cancelled = false;

    std::vector<wchar_t> buffer(65536, L'\0');
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrTitle = title;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.hwndOwner = GetForegroundWindow();
    ofn.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

    if (!GetOpenFileNameW(&ofn)) {
        DWORD dialogError = CommDlgExtendedError();
        if (dialogError == 0) {
            *cancelled = true;
        } else if (error) {
            *error = "打开文件选择器失败: " + std::to_string(dialogError);
        }
        return apps;
    }

    const wchar_t* first = buffer.data();
    const wchar_t* cursor = first + std::wcslen(first) + 1;
    if (*cursor == L'\0') {
        apps.push_back(wideToUtf8(first));
        return apps;
    }

    fs::path directory(first);
    while (*cursor) {
        fs::path selected = directory / cursor;
        apps.push_back(wideToUtf8(selected.wstring()));
        cursor += std::wcslen(cursor) + 1;
    }
    return apps;
}

static JsonValue::Array browseAppFiles(bool* cancelled, std::string* error) {
    if (auto testApps = testPickedApps()) return *testApps;

    JsonValue::Array apps;
    auto paths = browseFiles(cancelled, error, L"选择受限应用", L"应用程序 (*.exe)\0*.exe\0所有文件 (*.*)\0*.*\0\0");
    for (const auto& path : paths) apps.push_back(appInfoJson(path.asString()));
    return apps;
}

HttpResponse ApiHandlers::appBrowse() {
    bool cancelled = false;
    std::string error;
    JsonValue::Array apps = browseAppFiles(&cancelled, &error);
    if (!error.empty()) return errorResponse(error, 500);
    return jsonResponse(JsonValue::Object{
        {"ok", true},
        {"cancelled", cancelled},
        {"apps", apps}
    });
}

static JsonValue shortcutCandidateJson(const ShortcutCandidate& shortcut) {
    return JsonValue::Object{
        {"path", shortcut.path},
        {"target_path", shortcut.target_path},
        {"arguments", shortcut.arguments},
        {"working_dir", shortcut.working_dir},
        {"icon_path", shortcut.icon_path},
        {"icon_index", shortcut.icon_index},
        {"description", shortcut.description}
    };
}

HttpResponse ApiHandlers::shortcutBrowse() {
    bool cancelled = false;
    std::string error;
    JsonValue::Array shortcuts;
    if (auto testShortcuts = testPickedShortcuts()) {
        shortcuts = *testShortcuts;
    } else {
        auto paths = browseFiles(&cancelled, &error, L"选择要替换的快捷方式", L"快捷方式 (*.lnk)\0*.lnk\0所有文件 (*.*)\0*.*\0\0");
        if (!error.empty()) return errorResponse(error, 500);
        for (const auto& pathValue : paths) {
            ShortcutCandidate shortcut;
            if (readShortcutFile(pathValue.asString(), shortcut)) {
                shortcuts.push_back(shortcutCandidateJson(shortcut));
            } else {
                shortcuts.push_back(JsonValue::Object{
                    {"path", pathValue.asString()},
                    {"target_path", ""},
                    {"error", "读取快捷方式失败"}
                });
            }
        }
    }
    return jsonResponse(JsonValue::Object{
        {"ok", true},
        {"cancelled", cancelled},
        {"shortcuts", shortcuts}
    });
}

HttpResponse ApiHandlers::shortcutRead(const std::string& body) {
    JsonValue parsed = requestBodyJson(body);
    std::string shortcutPath = parsed.get("shortcut_path") ? parsed.get("shortcut_path")->asString() : "";
    if (shortcutPath.empty()) return errorResponse("缺少快捷方式路径");
    ShortcutCandidate shortcut;
    if (!readShortcutFile(shortcutPath, shortcut)) return errorResponse("读取快捷方式失败", 404);
    return jsonResponse(JsonValue::Object{{"ok", true}, {"shortcut", shortcutCandidateJson(shortcut)}});
}

static void appendByte(std::string& out, unsigned char value) {
    out.push_back(static_cast<char>(value));
}

static void appendU16(std::string& out, std::uint16_t value) {
    appendByte(out, static_cast<unsigned char>(value & 0xff));
    appendByte(out, static_cast<unsigned char>((value >> 8) & 0xff));
}

static void appendU32(std::string& out, std::uint32_t value) {
    appendByte(out, static_cast<unsigned char>(value & 0xff));
    appendByte(out, static_cast<unsigned char>((value >> 8) & 0xff));
    appendByte(out, static_cast<unsigned char>((value >> 16) & 0xff));
    appendByte(out, static_cast<unsigned char>((value >> 24) & 0xff));
}

static SIZE sizeForIcon(HICON icon) {
    SIZE size{GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON)};
    ICONINFO info{};
    if (!GetIconInfo(icon, &info)) return size;

    BITMAP bitmap{};
    HBITMAP source = info.hbmColor ? info.hbmColor : info.hbmMask;
    if (source && GetObjectW(source, sizeof(bitmap), &bitmap) == sizeof(bitmap)) {
        size.cx = bitmap.bmWidth;
        size.cy = info.hbmColor ? bitmap.bmHeight : bitmap.bmHeight / 2;
    }

    if (info.hbmColor) DeleteObject(info.hbmColor);
    if (info.hbmMask) DeleteObject(info.hbmMask);
    return size;
}

static std::vector<unsigned char> iconMaskBits(HICON icon, int width, int height) {
    std::vector<unsigned char> mask;
    ICONINFO info{};
    if (!GetIconInfo(icon, &info) || !info.hbmMask) {
        if (info.hbmColor) DeleteObject(info.hbmColor);
        return mask;
    }

    int stride = ((width + 31) / 32) * 4;
    mask.resize(static_cast<size_t>(stride * height));
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 1;
    bmi.bmiHeader.biCompression = BI_RGB;
    HDC dc = GetDC(nullptr);
    if (!dc || !GetDIBits(dc, info.hbmMask, 0, static_cast<UINT>(height), mask.data(), &bmi, DIB_RGB_COLORS)) {
        mask.clear();
    }
    if (dc) ReleaseDC(nullptr, dc);
    if (info.hbmColor) DeleteObject(info.hbmColor);
    if (info.hbmMask) DeleteObject(info.hbmMask);
    return mask;
}

static bool maskPixelTransparent(const std::vector<unsigned char>& mask, int width, int height, int x, int y) {
    if (mask.empty()) return false;
    int stride = ((width + 31) / 32) * 4;
    size_t row = static_cast<size_t>(height - 1 - y) * static_cast<size_t>(stride);
    size_t byteIndex = row + static_cast<size_t>(x / 8);
    if (byteIndex >= mask.size()) return false;
    unsigned char bit = static_cast<unsigned char>(0x80 >> (x % 8));
    return (mask[byteIndex] & bit) != 0;
}

static std::vector<unsigned char> drawIconPixels(HICON icon, int width, int height) {
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    std::vector<unsigned char> pixels(static_cast<size_t>(width * height * 4));
    if (!dc || !dib || !bits) {
        if (dib) DeleteObject(dib);
        if (dc) DeleteDC(dc);
        if (screen) ReleaseDC(nullptr, screen);
        return {};
    }

    HGDIOBJ old = SelectObject(dc, dib);
    std::fill(pixels.begin(), pixels.end(), 0);
    std::fill_n(static_cast<unsigned char*>(bits), pixels.size(), 0);
    DrawIconEx(dc, 0, 0, icon, width, height, 0, nullptr, DI_NORMAL);
    std::copy(static_cast<unsigned char*>(bits), static_cast<unsigned char*>(bits) + pixels.size(), pixels.begin());

    bool hasAlpha = std::any_of(pixels.begin() + 3, pixels.end(), [index = size_t{3}](unsigned char alpha) mutable {
        bool check = index % 4 == 3;
        ++index;
        return check && alpha != 0;
    });
    if (!hasAlpha) {
        auto mask = iconMaskBits(icon, width, height);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                size_t alphaIndex = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4 + 3;
                pixels[alphaIndex] = maskPixelTransparent(mask, width, height, x, y) ? 0 : 255;
            }
        }
    }

    SelectObject(dc, old);
    DeleteObject(dib);
    DeleteDC(dc);
    if (screen) ReleaseDC(nullptr, screen);
    return pixels;
}

static std::string iconToIcoBytes(HICON icon) {
    SIZE size = sizeForIcon(icon);
    int width = std::max(1, static_cast<int>(size.cx));
    int height = std::max(1, static_cast<int>(size.cy));
    auto pixels = drawIconPixels(icon, width, height);
    if (pixels.empty()) return "";

    std::string image;
    int xorStride = width * 4;
    int maskStride = ((width + 31) / 32) * 4;
    std::uint32_t pixelBytes = static_cast<std::uint32_t>(xorStride * height);
    std::uint32_t maskBytes = static_cast<std::uint32_t>(maskStride * height);

    appendU32(image, static_cast<std::uint32_t>(sizeof(BITMAPINFOHEADER)));
    appendU32(image, static_cast<std::uint32_t>(width));
    appendU32(image, static_cast<std::uint32_t>(height * 2));
    appendU16(image, 1);
    appendU16(image, 32);
    appendU32(image, BI_RGB);
    appendU32(image, pixelBytes + maskBytes);
    appendU32(image, 0);
    appendU32(image, 0);
    appendU32(image, 0);
    appendU32(image, 0);

    for (int y = height - 1; y >= 0; --y) {
        size_t row = static_cast<size_t>(y) * static_cast<size_t>(xorStride);
        image.append(reinterpret_cast<const char*>(pixels.data() + row), static_cast<size_t>(xorStride));
    }
    image.append(static_cast<size_t>(maskBytes), '\0');

    std::string ico;
    appendU16(ico, 0);
    appendU16(ico, 1);
    appendU16(ico, 1);
    appendByte(ico, width >= 256 ? 0 : static_cast<unsigned char>(width));
    appendByte(ico, height >= 256 ? 0 : static_cast<unsigned char>(height));
    appendByte(ico, 0);
    appendByte(ico, 0);
    appendU16(ico, 1);
    appendU16(ico, 32);
    appendU32(ico, static_cast<std::uint32_t>(image.size()));
    appendU32(ico, 6 + 16);
    ico += image;
    return ico;
}

static HICON iconForPath(const std::wstring& path) {
    SHFILEINFOW info{};
    UINT flags = SHGFI_ICON | SHGFI_LARGEICON;
    if (SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info), flags) && info.hIcon) return info.hIcon;
    if (SHGetFileInfoW(path.c_str(), FILE_ATTRIBUTE_NORMAL, &info, sizeof(info), flags | SHGFI_USEFILEATTRIBUTES) && info.hIcon) return info.hIcon;

    HICON largeIcon = nullptr;
    HICON smallIcon = nullptr;
    if (ExtractIconExW(path.c_str(), 0, &largeIcon, &smallIcon, 1) > 0) {
        if (smallIcon) {
            DestroyIcon(smallIcon);
            smallIcon = nullptr;
        }
        if (largeIcon) return largeIcon;
    }
    if (smallIcon) DestroyIcon(smallIcon);
    return nullptr;
}

HttpResponse ApiHandlers::appIcon(const HttpRequest& request) {
    auto it = request.query_params.find("path");
    std::string path = it == request.query_params.end() ? "" : it->second;
    if (path.empty()) return errorResponse("missing app path");
    std::wstring widePath = utf8ToWide(path);
    if (!fileExists(widePath)) return errorResponse("app path does not exist", 404);

    HICON icon = iconForPath(widePath);
    if (!icon) return errorResponse("could not read app icon", 404);
    std::string bytes = iconToIcoBytes(icon);
    DestroyIcon(icon);
    if (bytes.empty()) return errorResponse("could not encode app icon", 500);

    HttpResponse response;
    response.content_type = "image/x-icon";
    response.body = bytes;
    response.headers["X-Content-Type-Options"] = "nosniff";
    trimCurrentProcessWorkingSet();
    return response;
}

HttpResponse ApiHandlers::appStatus() {
    auto config = config_.load();
    JsonValue::Array rules;
    for (const auto& rule : config.rules) {
        JsonValue::Array apps;
        for (const auto& app : rule.blocked_apps) {
            bool exists = fileExists(utf8ToWide(app.original_path));
            apps.push_back(JsonValue::Object{
                {"name", app.name},
                {"path", app.original_path},
                {"exists", exists},
                {"missing", !exists},
                {"replaced_shortcut_count", static_cast<int>(app.replaced_shortcuts.size())}
            });
        }
        rules.push_back(JsonValue::Object{
            {"rule_id", rule.id},
            {"ssid", rule.ssid},
            {"network_type", rule.network_type.empty() ? "wifi" : rule.network_type},
            {"network_id", rule.network_id.empty() ? rule.ssid : rule.network_id},
            {"network_name", rule.network_name.empty() ? (rule.network_id.empty() ? rule.ssid : rule.network_id) : rule.network_name},
            {"apps", apps}
        });
    }
    return jsonResponse(JsonValue::Object{{"ok", true}, {"rules", rules}});
}

HttpResponse ApiHandlers::appCleanup(const std::string& body) {
    JsonValue parsed = requestBodyJson(body);
    std::string ruleId = parsed.get("rule_id") ? parsed.get("rule_id")->asString() : "";
    std::string appPath = parsed.get("app_path") ? parsed.get("app_path")->asString() : "";
    if (ruleId.empty() || appPath.empty()) return errorResponse("missing rule_id or app_path");

    auto config = config_.load();
    JsonValue::Array restoreResults;
    bool found = false;
    bool removed = false;
    std::string loweredPath = toLowerAscii(appPath);
    for (auto& rule : config.rules) {
        if (rule.id != ruleId) continue;
        auto& apps = rule.blocked_apps;
        for (auto it = apps.begin(); it != apps.end(); ++it) {
            if (toLowerAscii(it->original_path) != loweredPath) continue;
            found = true;
            if (fileExists(utf8ToWide(it->original_path))) {
                return errorResponse("app path still exists", 409);
            }
            bool restoreFailed = false;
            for (const auto& shortcut : it->replaced_shortcuts) {
                auto result = restoreShortcut(shortcut);
                if (!result.ok) restoreFailed = true;
                restoreResults.push_back(JsonValue::Object{
                    {"ok", result.ok},
                    {"shortcut", result.shortcut},
                    {"backup", result.backup},
                    {"error", result.error}
                });
            }
            if (restoreFailed) {
                return jsonResponse(JsonValue::Object{
                    {"ok", false},
                    {"error", "shortcut restore failed"},
                    {"restore_results", restoreResults},
                    {"config", configToJson(config)}
                }, 409);
            }
            apps.erase(it);
            if (!rule.app_group_id.empty()) {
                for (auto& group : config.app_groups) {
                    if (group.id != rule.app_group_id) continue;
                    auto& groupApps = group.apps;
                    groupApps.erase(std::remove_if(groupApps.begin(), groupApps.end(), [&](const auto& groupApp) {
                        return toLowerAscii(groupApp.original_path) == loweredPath;
                    }), groupApps.end());
                }
            }
            removed = true;
            break;
        }
    }
    if (!found) return errorResponse("app entry not found", 404);
    if (!config_.save(config)) return errorResponse("config save failed", 500);
    return jsonResponse(JsonValue::Object{
        {"ok", true},
        {"removed", removed},
        {"restore_results", restoreResults},
        {"config", configToJson(config)}
    });
}

HttpResponse ApiHandlers::shortcutScan(const std::string& body) {
    JsonValue parsed = requestBodyJson(body);
    std::string appPath = parsed.get("app_path") ? parsed.get("app_path")->asString() : "";
    if (appPath.empty()) return errorResponse("缺少应用路径");

    JsonValue::Array items;
    for (const auto& shortcut : findShortcutsForApp(appPath)) {
        items.push_back(JsonValue::Object{
            {"path", shortcut.path},
            {"target_path", shortcut.target_path},
            {"arguments", shortcut.arguments},
            {"working_dir", shortcut.working_dir},
            {"icon_path", shortcut.icon_path},
            {"icon_index", shortcut.icon_index},
            {"description", shortcut.description}
        });
    }
    return jsonResponse(JsonValue::Object{{"ok", true}, {"shortcuts", items}});
}

HttpResponse ApiHandlers::shortcutReplace(const std::string& body) {
    JsonValue parsed = requestBodyJson(body);
    std::string appPath = parsed.get("app_path") ? parsed.get("app_path")->asString() : "";
    std::string ruleId = parsed.get("rule_id") ? parsed.get("rule_id")->asString() : "";
    if (appPath.empty() || ruleId.empty()) return errorResponse("缺少应用路径或规则 ID");

    fs::path launcher = fs::path(executableDir()) / L"ww-launch.exe";
    if (!fileExists(launcher.wstring())) {
        launcher = fs::path(currentWorkingDir()) / L"ww-launch.exe";
    }

    auto config = config_.load();
    JsonValue::Array resultItems;
    std::vector<ShortcutReplacement> replacements;

    std::vector<std::string> selectedShortcutPaths;
    for (const auto& rule : config.rules) {
        if (rule.id != ruleId) continue;
        for (const auto& app : rule.blocked_apps) {
            if (toLowerAscii(app.original_path) != toLowerAscii(appPath)) continue;
            selectedShortcutPaths = app.shortcut_paths;
            break;
        }
    }

    auto appendReplacementResult = [&](const ShortcutOperationResult& result) {
        if (result.ok) replacements.push_back({result.shortcut, result.backup});
        if (!result.ok) {
            LogRecord record;
            record.timestamp = nowIsoLocal();
            record.app = pathBaseNameUtf8(appPath);
            record.action = "shortcut_replace_failed";
            record.rule_id = ruleId;
            record.shortcut = result.shortcut;
            record.backup = result.backup;
            record.error = result.error;
            logger_.write(record);
        }
        resultItems.push_back(JsonValue::Object{
            {"ok", result.ok},
            {"shortcut", result.shortcut},
            {"backup", result.backup},
            {"error", result.error}
        });
    };

    if (!selectedShortcutPaths.empty()) {
        for (const auto& shortcutPath : selectedShortcutPaths) {
            ShortcutCandidate source;
            auto result = replaceShortcutByPath(shortcutPath, wideToUtf8(launcher.wstring()), ruleId, &source);
            appendReplacementResult(result);
        }
    } else {
        for (const auto& shortcut : findShortcutsForApp(appPath)) {
            auto result = replaceShortcut(shortcut, wideToUtf8(launcher.wstring()), appPath, ruleId);
            appendReplacementResult(result);
        }
    }

    for (auto& rule : config.rules) {
        if (rule.id != ruleId) continue;
        for (auto& app : rule.blocked_apps) {
            if (toLowerAscii(app.original_path) != toLowerAscii(appPath)) continue;
            for (const auto& replacement : replacements) {
                auto exists = std::any_of(app.replaced_shortcuts.begin(), app.replaced_shortcuts.end(), [&](const auto& item) {
                    return toLowerAscii(item.original_lnk) == toLowerAscii(replacement.original_lnk);
                });
                if (!exists) app.replaced_shortcuts.push_back(replacement);
            }
        }
    }
    config_.save(config);

    return jsonResponse(JsonValue::Object{{"ok", true}, {"results", resultItems}, {"config", configToJson(config)}});
}

HttpResponse ApiHandlers::restoreShortcuts(const std::string& body) {
    auto config = config_.load();
    JsonValue parsed = requestBodyJson(body);
    std::string ruleId = parsed.get("rule_id") ? parsed.get("rule_id")->asString() : "";
    JsonValue::Array resultItems;
    auto results = ruleId.empty() ? restoreAllShortcuts(config) : restoreShortcutsForRule(config, ruleId);
    for (const auto& result : results) {
        resultItems.push_back(JsonValue::Object{
            {"ok", result.ok},
            {"shortcut", result.shortcut},
            {"backup", result.backup},
            {"error", result.error}
        });
    }

    clearRestoredShortcutRecords(config, results);
    config_.save(config);
    return jsonResponse(JsonValue::Object{{"ok", true}, {"results", resultItems}, {"config", configToJson(config)}});
}

}
