#pragma once
// Dynamic loader for version.dll - only used for exe version info display.

#include <windows.h>

namespace ww {
namespace dyn_version {

struct VersionDll {
    HMODULE dll = nullptr;
    bool load();
    void unload();
    bool loaded() const { return dll != nullptr; }
    ~VersionDll() { unload(); }
    VersionDll() = default;
    VersionDll(const VersionDll&) = delete;
    VersionDll& operator=(const VersionDll&) = delete;
};

VersionDll& instance();

extern decltype(&GetFileVersionInfoSizeW) fn_GetFileVersionInfoSizeW;
extern decltype(&GetFileVersionInfoW) fn_GetFileVersionInfoW;
extern decltype(&VerQueryValueW) fn_VerQueryValueW;

bool isAvailable();

} // namespace dyn_version
} // namespace ww
