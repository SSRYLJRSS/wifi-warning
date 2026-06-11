#include "core/dyn_version.h"

namespace ww {
namespace dyn_version {

decltype(&GetFileVersionInfoSizeW) fn_GetFileVersionInfoSizeW = nullptr;
decltype(&GetFileVersionInfoW) fn_GetFileVersionInfoW = nullptr;
decltype(&VerQueryValueW) fn_VerQueryValueW = nullptr;

static VersionDll g_versionDll;

VersionDll& instance() {
    return g_versionDll;
}

bool VersionDll::load() {
    if (dll) return true;
    dll = LoadLibraryW(L"version.dll");
    if (!dll) return false;

    #define LOAD_FN(name) \
        fn_##name = reinterpret_cast<decltype(&name)>(GetProcAddress(dll, #name)); \
        if (!fn_##name) { unload(); return false; }

    LOAD_FN(GetFileVersionInfoSizeW);
    LOAD_FN(GetFileVersionInfoW);
    LOAD_FN(VerQueryValueW);

    #undef LOAD_FN

    return true;
}

void VersionDll::unload() {
    if (dll) {
        fn_GetFileVersionInfoSizeW = nullptr;
        fn_GetFileVersionInfoW = nullptr;
        fn_VerQueryValueW = nullptr;
        FreeLibrary(dll);
        dll = nullptr;
    }
}

bool isAvailable() {
    return instance().load();
}

} // namespace dyn_version
} // namespace ww
