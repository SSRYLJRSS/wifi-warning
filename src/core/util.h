#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace ww {

std::wstring utf8ToWide(const std::string& value);
std::string wideToUtf8(const std::wstring& value);

std::wstring getEnvWide(const wchar_t* name);
std::wstring appDataRoot();
std::wstring appConfigPath();
std::wstring logsDir();
std::wstring executablePath();
std::wstring executableDir();
std::wstring currentWorkingDir();

bool ensureDirectory(const std::wstring& path);
bool fileExists(const std::wstring& path);
std::string readTextFileUtf8(const std::wstring& path);
bool writeTextFileUtf8(const std::wstring& path, const std::string& text);

std::string nowIsoLocal();
std::string dateStampLocal();
std::int64_t nowUnixSeconds();
std::string pathBaseNameUtf8(const std::string& path);
std::string toLowerAscii(std::string value);
std::wstring quoteArg(const std::wstring& arg);
std::string urlEncode(const std::string& value);
std::string urlDecode(const std::string& value);
std::string sha256Hex(const std::string& value);
std::optional<int> parseInt(const std::string& value);
void trimCurrentProcessWorkingSet();

}
