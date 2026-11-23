// ww-admin.exe — 需要管理员权限的 IFEO 维护助手（由主程序按需 runas 调用）
#include <windows.h>
#include <shellapi.h>

#include "core/config_manager.h"
#include "core/ifeo_manager.h"
#include "core/json.h"
#include "core/util.h"

#include <filesystem>
#include <string>

using namespace ww;

static void printText(const std::string& text) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out == INVALID_HANDLE_VALUE || !out) return;
    DWORD written = 0;
    WriteFile(out, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
}

static void writeResult(const JsonValue& value) {
    std::filesystem::path path = std::filesystem::path(executableDir()) / L"ifeo-admin-result.json";
    writeTextFileUtf8(path.wstring(), stringifyJson(value, 2));
}

static int exitWith(const JsonValue& value, bool ok) {
    writeResult(value);
    printText(stringifyJson(value, 2) + "\n");
    return ok ? 0 : 1;
}

int main() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring command;
    std::wstring configPath;
    std::wstring removeExe;
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"--ifeo-sync") command = L"sync";
        else if (arg == L"--ifeo-restore") command = L"restore";
        else if (arg == L"--ifeo-remove" && i + 1 < argc) { command = L"remove"; removeExe = argv[i + 1]; }
        else if (arg == L"--config" && i + 1 < argc) configPath = argv[i + 1];
    }
    LocalFree(argv);

    if (!isProcessElevated()) {
        return exitWith(JsonValue::Object{{"ok", false}, {"error", std::string("需要管理员权限")}}, false);
    }

    if (command == L"sync") {
        ConfigManager manager(configPath);
        AppConfig cfg = manager.load();
        IfeoApplyResult r = syncIfeo(cfg);
        JsonValue::Array exes;
        for (const auto& e : r.exes) exes.push_back(e);
        return exitWith(JsonValue::Object{
            {"ok", r.ok}, {"applied", r.applied}, {"already", r.already},
            {"removed", r.removed}, {"error", r.error}, {"exes", exes}}, r.ok);
    }
    if (command == L"restore") {
        IfeoApplyResult r = restoreAllIfeo();
        JsonValue::Array exes;
        for (const auto& e : r.exes) exes.push_back(e);
        return exitWith(JsonValue::Object{{"ok", r.ok}, {"removed", r.removed}, {"error", r.error}, {"exes", exes}}, r.ok);
    }
    if (command == L"remove") {
        bool ok = removeIfeoForExe(wideToUtf8(removeExe));
        return exitWith(JsonValue::Object{{"ok", ok}, {"exe", wideToUtf8(removeExe)}}, ok);
    }

    return exitWith(JsonValue::Object{{"ok", false}, {"error", std::string("未知命令")}}, false);
}
