#pragma once

#include <string>

namespace ww {

bool setAutoStart(bool enabled, const std::wstring& exePath, std::string* error = nullptr);
bool isAutoStartEnabled();

}
