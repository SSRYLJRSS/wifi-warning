#include "core/shortcut_manager.h"

#include "core/util.h"

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <filesystem>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace ww {

struct ComInit {
    HRESULT hr;
    ComInit() : hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComInit() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

static std::string normalizeShortcutPath(const std::string& path) {
    std::error_code ec;
    fs::path p(utf8ToWide(path));
    auto canonical = fs::weakly_canonical(p, ec);
    if (!ec) return toLowerAscii(wideToUtf8(canonical.wstring()));
    return toLowerAscii(path);
}

static bool readShortcut(const std::wstring& path, ShortcutCandidate& out) {
    ComInit com;
    if (FAILED(com.hr)) return false;

    IShellLinkW* link = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(&link));
    if (FAILED(hr)) return false;

    IPersistFile* file = nullptr;
    hr = link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file));
    if (FAILED(hr)) {
        link->Release();
        return false;
    }

    hr = file->Load(path.c_str(), STGM_READ);
    if (FAILED(hr)) {
        file->Release();
        link->Release();
        return false;
    }

    wchar_t buffer[MAX_PATH]{};
    WIN32_FIND_DATAW data{};
    if (SUCCEEDED(link->GetPath(buffer, MAX_PATH, &data, SLGP_RAWPATH))) out.target_path = wideToUtf8(buffer);
    if (SUCCEEDED(link->GetArguments(buffer, MAX_PATH))) out.arguments = wideToUtf8(buffer);
    if (SUCCEEDED(link->GetWorkingDirectory(buffer, MAX_PATH))) out.working_dir = wideToUtf8(buffer);
    if (SUCCEEDED(link->GetDescription(buffer, MAX_PATH))) out.description = wideToUtf8(buffer);
    int iconIndex = 0;
    if (SUCCEEDED(link->GetIconLocation(buffer, MAX_PATH, &iconIndex))) {
        out.icon_path = wideToUtf8(buffer);
        out.icon_index = iconIndex;
    }
    out.path = wideToUtf8(path);

    file->Release();
    link->Release();
    return !out.target_path.empty();
}

bool readShortcutFile(const std::string& shortcutPath, ShortcutCandidate& out) {
    return readShortcut(utf8ToWide(shortcutPath), out);
}

static bool createShortcut(const std::wstring& path, const std::wstring& target, const std::wstring& args, const ShortcutCandidate& source, std::string* error) {
    ComInit com;
    if (FAILED(com.hr)) {
        if (error) *error = "COM 初始化失败";
        return false;
    }

    IShellLinkW* link = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(&link));
    if (FAILED(hr)) {
        if (error) *error = "无法创建快捷方式对象";
        return false;
    }

    link->SetPath(target.c_str());
    link->SetArguments(args.c_str());
    if (!source.working_dir.empty()) link->SetWorkingDirectory(utf8ToWide(source.working_dir).c_str());
    if (!source.description.empty()) link->SetDescription(utf8ToWide(source.description).c_str());
    if (!source.icon_path.empty()) link->SetIconLocation(utf8ToWide(source.icon_path).c_str(), source.icon_index);
    else link->SetIconLocation(utf8ToWide(source.target_path).c_str(), source.icon_index);

    IPersistFile* file = nullptr;
    hr = link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file));
    if (SUCCEEDED(hr)) hr = file->Save(path.c_str(), TRUE);
    if (file) file->Release();
    link->Release();

    if (FAILED(hr) && error) *error = "保存快捷方式失败";
    return SUCCEEDED(hr);
}

bool createShortcutFile(const std::string& shortcutPath, const std::string& targetPath, const std::string& arguments, const ShortcutCandidate& source, std::string* error) {
    return createShortcut(utf8ToWide(shortcutPath), utf8ToWide(targetPath), utf8ToWide(arguments), source, error);
}

static std::wstring normalizeRootForCompare(const std::wstring& root) {
    std::error_code ec;
    fs::path path(root);
    auto normalized = fs::weakly_canonical(path, ec);
    if (ec) {
        ec.clear();
        normalized = fs::absolute(path, ec);
    }
    if (ec) normalized = path.lexically_normal();
    std::wstring text = normalized.wstring();
    while (text.size() > 3 && (text.back() == L'\\' || text.back() == L'/')) text.pop_back();
    return text;
}

static void addShortcutRoot(std::vector<std::wstring>& roots, const std::wstring& root) {
    if (root.empty()) return;
    std::wstring normalized = normalizeRootForCompare(root);
    for (const auto& existing : roots) {
        if (CompareStringOrdinal(normalizeRootForCompare(existing).c_str(), -1, normalized.c_str(), -1, TRUE) == CSTR_EQUAL) return;
    }
    roots.push_back(root);
}

static std::wstring knownFolderPath(REFKNOWNFOLDERID folderId) {
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(folderId, KF_FLAG_DEFAULT, nullptr, &path)) || !path) return L"";
    std::wstring result(path);
    CoTaskMemFree(path);
    return result;
}

static std::vector<std::wstring> shortcutRoots() {
    std::vector<std::wstring> roots;
    std::wstring overrideRoots = getEnvWide(L"WW_SHORTCUT_ROOTS");
    if (!overrideRoots.empty()) {
        std::wstringstream stream(overrideRoots);
        std::wstring root;
        while (std::getline(stream, root, L';')) {
            addShortcutRoot(roots, root);
        }
        return roots;
    }

    std::wstring user = getEnvWide(L"USERPROFILE");
    std::wstring appData = getEnvWide(L"APPDATA");
    std::wstring programData = getEnvWide(L"PROGRAMDATA");
    addShortcutRoot(roots, knownFolderPath(FOLDERID_Desktop));
    if (!user.empty()) addShortcutRoot(roots, (fs::path(user) / L"Desktop").wstring());
    addShortcutRoot(roots, knownFolderPath(FOLDERID_StartMenu));
    if (!appData.empty()) addShortcutRoot(roots, (fs::path(appData) / L"Microsoft\\Windows\\Start Menu").wstring());
    addShortcutRoot(roots, knownFolderPath(FOLDERID_CommonStartMenu));
    if (!programData.empty()) addShortcutRoot(roots, (fs::path(programData) / L"Microsoft\\Windows\\Start Menu").wstring());
    if (!appData.empty()) addShortcutRoot(roots, (fs::path(appData) / L"Microsoft\\Internet Explorer\\Quick Launch\\User Pinned\\TaskBar").wstring());
    return roots;
}

std::vector<ShortcutCandidate> findShortcutsForApp(const std::string& targetExe) {
    std::vector<ShortcutCandidate> shortcuts;
    std::string normalizedTarget = normalizeShortcutPath(targetExe);
    for (const auto& root : shortcutRoots()) {
        std::error_code ec;
        if (!fs::exists(root, ec)) continue;
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
        fs::recursive_directory_iterator end;
        while (!ec && it != end) {
            const auto entry = *it;
            std::error_code entryEc;
            if (entry.is_regular_file(entryEc) && toLowerAscii(wideToUtf8(entry.path().extension().wstring())) == ".lnk") {
                ShortcutCandidate candidate;
                if (readShortcut(entry.path().wstring(), candidate) && normalizeShortcutPath(candidate.target_path) == normalizedTarget) {
                    shortcuts.push_back(candidate);
                }
            }
            ec.clear();
            it.increment(ec);
        }
    }
    return shortcuts;
}

std::vector<std::string> shortcutScanRoots() {
    std::vector<std::string> roots;
    for (const auto& root : shortcutRoots()) roots.push_back(wideToUtf8(root));
    return roots;
}

static fs::path shortcutBackupRoot() {
    std::wstring overrideRoot = getEnvWide(L"WW_SHORTCUT_BACKUP_ROOT");
    fs::path root = overrideRoot.empty() ? fs::path(executableDir()) / L"shortcut-backups" : fs::path(overrideRoot);
    ensureDirectory(root.wstring());
    return root;
}

static fs::path backupPathForShortcut(const std::string& shortcutPath) {
    std::string hash = sha256Hex(normalizeShortcutPath(shortcutPath));
    if (hash.empty()) hash = sha256Hex(shortcutPath);
    return shortcutBackupRoot() / utf8ToWide(hash + ".lnk");
}

static std::string copyFileError(const char* action, const std::error_code& ec) {
    return std::string(action) + ": " + ec.message();
}

static std::string win32FileError(const char* action) {
    std::error_code ec(static_cast<int>(GetLastError()), std::system_category());
    return copyFileError(action, ec);
}

static bool copyFileNoOverwrite(const fs::path& source, const fs::path& target, std::string* error, const char* action) {
    if (CopyFileW(source.wstring().c_str(), target.wstring().c_str(), TRUE)) return true;
    if (error) *error = win32FileError(action);
    return false;
}

static bool copyFileOverwrite(const fs::path& source, const fs::path& target, std::string* error, const char* action) {
    if (CopyFileW(source.wstring().c_str(), target.wstring().c_str(), FALSE)) return true;
    if (error) *error = win32FileError(action);
    return false;
}

ShortcutOperationResult replaceShortcut(const ShortcutCandidate& shortcut, const std::string& launcherPath, const std::string& appPath, const std::string& ruleId) {
    ShortcutOperationResult result;
    result.shortcut = shortcut.path;
    fs::path shortcutPath(utf8ToWide(shortcut.path));
    fs::path backupPath = backupPathForShortcut(shortcut.path);
    result.backup = wideToUtf8(backupPath.wstring());

    if (!getEnvWide(L"WW_TEST_SHORTCUT_REPLACE_FAIL").empty()) {
        result.error = "test shortcut replace failure";
        return result;
    }

    std::error_code ec;
    if (!fs::exists(backupPath, ec)) {
        ensureDirectory(backupPath.parent_path().wstring());
        if (!copyFileNoOverwrite(shortcutPath, backupPath, &result.error, "备份原快捷方式失败")) return result;
    }

    std::wstring args = L"--app " + quoteArg(utf8ToWide(appPath)) + L" --rule " + quoteArg(utf8ToWide(ruleId));
    if (!shortcut.arguments.empty()) args += L" --app-args " + quoteArg(utf8ToWide(shortcut.arguments));
    if (!createShortcut(shortcutPath.wstring(), utf8ToWide(launcherPath), args, shortcut, &result.error)) {
        ec.clear();
        if (result.error.empty()) {
            std::string restoreError;
            if (!copyFileOverwrite(backupPath, shortcutPath, &restoreError, "恢复原快捷方式失败")) result.error = restoreError;
        } else {
            std::string restoreError;
            copyFileOverwrite(backupPath, shortcutPath, &restoreError, "恢复原快捷方式失败");
        }
        return result;
    }

    result.ok = true;
    return result;
}

ShortcutOperationResult replaceShortcutByPath(const std::string& shortcutPath, const std::string& launcherPath, const std::string& ruleId, ShortcutCandidate* sourceOut) {
    ShortcutOperationResult result;
    result.shortcut = shortcutPath;

    ShortcutCandidate shortcut;
    fs::path originalPath(utf8ToWide(shortcutPath));
    fs::path backupPath = backupPathForShortcut(shortcutPath);
    fs::path legacyBackupPath = originalPath;
    legacyBackupPath += L".backup";
    std::string sourcePath = fileExists(backupPath.wstring()) ? wideToUtf8(backupPath.wstring()) : (fileExists(legacyBackupPath.wstring()) ? wideToUtf8(legacyBackupPath.wstring()) : shortcutPath);
    if (!readShortcutFile(sourcePath, shortcut)) {
        result.error = "读取快捷方式失败";
        return result;
    }
    if (!fileExists(backupPath.wstring()) && fileExists(legacyBackupPath.wstring())) {
        std::error_code ec;
        ensureDirectory(backupPath.parent_path().wstring());
        if (!copyFileOverwrite(legacyBackupPath, backupPath, &result.error, "迁移快捷方式备份失败")) return result;
    }
    shortcut.path = shortcutPath;
    if (sourceOut) *sourceOut = shortcut;
    result = replaceShortcut(shortcut, launcherPath, shortcut.target_path, ruleId);
    if (result.ok && fileExists(legacyBackupPath.wstring())) {
        std::error_code ec;
        fs::remove(legacyBackupPath, ec);
    }
    return result;
}

ShortcutOperationResult restoreShortcut(const ShortcutReplacement& replacement) {
    ShortcutOperationResult result;
    result.shortcut = replacement.original_lnk;
    result.backup = replacement.backup_lnk;
    fs::path original(utf8ToWide(replacement.original_lnk));
    fs::path backup(utf8ToWide(replacement.backup_lnk));
    std::error_code ec;

    if (!fs::exists(backup, ec)) {
        result.error = "备份文件不存在";
        return result;
    }
    ensureDirectory(original.parent_path().wstring());
    if (!copyFileOverwrite(backup, original, &result.error, "还原快捷方式失败")) return result;
    ec.clear();
    fs::remove(backup, ec);
    result.ok = true;
    return result;
}

std::vector<ShortcutOperationResult> restoreShortcutsForRule(const AppConfig& config, const std::string& ruleId) {
    std::vector<ShortcutOperationResult> results;
    std::set<std::string> seen;
    for (const auto& rule : config.rules) {
        if (rule.id != ruleId) continue;
        for (const auto& app : rule.blocked_apps) {
            for (const auto& shortcut : app.replaced_shortcuts) {
                if (!seen.insert(toLowerAscii(shortcut.original_lnk)).second) continue;
                results.push_back(restoreShortcut(shortcut));
            }
        }
    }
    return results;
}

std::vector<ShortcutOperationResult> restoreAllShortcuts(const AppConfig& config) {
    std::vector<ShortcutOperationResult> results;
    std::set<std::string> seen;
    for (const auto& rule : config.rules) {
        for (const auto& app : rule.blocked_apps) {
            for (const auto& shortcut : app.replaced_shortcuts) {
                if (!seen.insert(toLowerAscii(shortcut.original_lnk)).second) continue;
                results.push_back(restoreShortcut(shortcut));
            }
        }
    }
    return results;
}

void clearRestoredShortcutRecords(AppConfig& config, const std::vector<ShortcutOperationResult>& results) {
    std::set<std::string> restored;
    for (const auto& result : results) {
        if (result.ok) restored.insert(toLowerAscii(result.shortcut));
    }
    if (restored.empty()) return;

    for (auto& rule : config.rules) {
        for (auto& app : rule.blocked_apps) {
            std::vector<ShortcutReplacement> kept;
            for (const auto& shortcut : app.replaced_shortcuts) {
                if (restored.find(toLowerAscii(shortcut.original_lnk)) == restored.end()) kept.push_back(shortcut);
            }
            app.replaced_shortcuts = std::move(kept);
        }
    }
}

int reconcileShortcutRecords(AppConfig& config) {
    int removed = 0;
    for (auto& rule : config.rules) {
        for (auto& app : rule.blocked_apps) {
            std::vector<ShortcutReplacement> kept;
            for (const auto& shortcut : app.replaced_shortcuts) {
                const bool originalExists = fileExists(utf8ToWide(shortcut.original_lnk));
                const bool backupExists = fileExists(utf8ToWide(shortcut.backup_lnk));
                if (originalExists && backupExists) {
                    kept.push_back(shortcut);
                } else {
                    ++removed;
                }
            }
            app.replaced_shortcuts = std::move(kept);
        }
    }
    return removed;
}

}
