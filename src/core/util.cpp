#include "core/util.h"

#include <windows.h>
#include <bcrypt.h>
#include <psapi.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace ww {

std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) return L"";
    int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    UINT codePage = CP_UTF8;
    if (needed <= 0) {
        needed = MultiByteToWideChar(CP_ACP, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
        codePage = CP_ACP;
    }
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(codePage, 0, value.data(), static_cast<int>(value.size()), out.data(), needed);
    return out;
}

std::string wideToUtf8(const std::wstring& value) {
    if (value.empty()) return "";
    int needed = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

std::wstring getEnvWide(const wchar_t* name) {
    DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0) return L"";
    std::wstring value(needed, L'\0');
    DWORD written = GetEnvironmentVariableW(name, value.data(), needed);
    value.resize(written);
    return value;
}

// v1.9: All data stored alongside the executable (portable, no external files)
std::wstring appDataRoot() {
    return executableDir();
}

std::wstring appConfigPath() {
    fs::path path(executableDir());
    path /= L"config.json";
    return path.wstring();
}

std::wstring logsDir() {
    fs::path path(executableDir());
    path /= L"logs";
    return path.wstring();
}

std::wstring executablePath() {
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    while (size == buffer.size()) {
        buffer.resize(buffer.size() * 2);
        size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    }
    buffer.resize(size);
    return buffer;
}

std::wstring executableDir() {
    fs::path path(executablePath());
    return path.parent_path().wstring();
}

std::wstring currentWorkingDir() {
    return fs::current_path().wstring();
}

bool ensureDirectory(const std::wstring& path) {
    std::error_code ec;
    return fs::exists(path, ec) || fs::create_directories(path, ec);
}

bool fileExists(const std::wstring& path) {
    std::error_code ec;
    return fs::exists(path, ec);
}

std::string readTextFileUtf8(const std::wstring& path) {
    std::ifstream in(fs::path(path), std::ios::binary);
    if (!in) return "";
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool writeTextFileUtf8(const std::wstring& path, const std::string& text) {
    fs::path file(path);
    ensureDirectory(file.parent_path().wstring());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return out.good();
}

std::string nowIsoLocal() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_s(&local, &t);
    std::ostringstream ss;
    ss << std::put_time(&local, "%Y-%m-%dT%H:%M:%S");
    return ss.str();
}

std::string dateStampLocal() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_s(&local, &t);
    std::ostringstream ss;
    ss << std::put_time(&local, "%Y-%m-%d");
    return ss.str();
}

std::int64_t nowUnixSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string pathBaseNameUtf8(const std::string& path) {
    fs::path p(utf8ToWide(path));
    std::wstring stem = p.filename().wstring();
    return wideToUtf8(stem);
}

std::string toLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::wstring quoteArg(const std::wstring& arg) {
    std::wstring out = L"\"";
    for (wchar_t ch : arg) {
        if (ch == L'\"') out += L'\\';
        out += ch;
    }
    out += L"\"";
    return out;
}

std::string urlEncode(const std::string& value) {
    std::ostringstream escaped;
    escaped << std::hex << std::uppercase;
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else if (c == ' ') {
            escaped << '+';
        } else {
            escaped << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(c);
        }
    }
    return escaped.str();
}

std::string urlDecode(const std::string& value) {
    std::string out;
    for (size_t i = 0; i < value.size(); ++i) {
        char c = value[i];
        if (c == '+') {
            out.push_back(' ');
        } else if (c == '%' && i + 2 < value.size()) {
            auto hex = value.substr(i + 1, 2);
            char decoded = static_cast<char>(std::strtol(hex.c_str(), nullptr, 16));
            out.push_back(decoded);
            i += 2;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string sha256Hex(const std::string& value) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectLength = 0;
    DWORD dataLength = 0;
    std::vector<unsigned char> object;
    std::vector<unsigned char> digest(32);

    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return "";
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &dataLength, 0) != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return "";
    }
    object.resize(objectLength);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectLength, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return "";
    }
    BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(value.data())), static_cast<ULONG>(value.size()), 0);
    BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);

    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (unsigned char b : digest) ss << std::setw(2) << static_cast<int>(b);
    return ss.str();
}

std::optional<int> parseInt(const std::string& value) {
    try {
        size_t idx = 0;
        int parsed = std::stoi(value, &idx);
        if (idx != value.size()) return std::nullopt;
        return parsed;
    } catch (...) {
        return std::nullopt;
    }
}

// v1.9: Removed trimCurrentProcessWorkingSet — SetProcessWorkingSetSize(-1,-1)
// is a well-known anti-pattern that forces pages back to disk and hurts
// performance on subsequent allocations. The OS manages the working set
// efficiently on its own.
void trimCurrentProcessWorkingSet() {
    // No-op: let Windows manage memory naturally.
}

}
