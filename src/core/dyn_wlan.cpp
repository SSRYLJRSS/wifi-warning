#include "core/dyn_wlan.h"

namespace ww {
namespace dyn_wlan {

FnWlanOpenHandle fn_WlanOpenHandle = nullptr;
FnWlanCloseHandle fn_WlanCloseHandle = nullptr;
FnWlanEnumInterfaces fn_WlanEnumInterfaces = nullptr;
FnWlanFreeMemory fn_WlanFreeMemory = nullptr;
FnWlanQueryInterface fn_WlanQueryInterface = nullptr;
FnWlanGetAvailableNetworkList fn_WlanGetAvailableNetworkList = nullptr;
FnWlanGetProfileList fn_WlanGetProfileList = nullptr;
FnWlanSetProfile fn_WlanSetProfile = nullptr;
FnWlanConnect fn_WlanConnect = nullptr;
FnWlanRegisterNotification fn_WlanRegisterNotification = nullptr;

static WlanDll g_wlanDll;

WlanDll& instance() {
    return g_wlanDll;
}

bool WlanDll::load() {
    if (dll) return true;
    dll = LoadLibraryW(L"wlanapi.dll");
    if (!dll) return false;

    #define LOAD_FN(var, name) \
        var = reinterpret_cast<decltype(var)>(GetProcAddress(dll, name)); \
        if (!var) { unload(); return false; }

    LOAD_FN(fn_WlanOpenHandle, "WlanOpenHandle");
    LOAD_FN(fn_WlanCloseHandle, "WlanCloseHandle");
    LOAD_FN(fn_WlanEnumInterfaces, "WlanEnumInterfaces");
    LOAD_FN(fn_WlanFreeMemory, "WlanFreeMemory");
    LOAD_FN(fn_WlanQueryInterface, "WlanQueryInterface");
    LOAD_FN(fn_WlanGetAvailableNetworkList, "WlanGetAvailableNetworkList");
    LOAD_FN(fn_WlanGetProfileList, "WlanGetProfileList");
    LOAD_FN(fn_WlanSetProfile, "WlanSetProfile");
    LOAD_FN(fn_WlanConnect, "WlanConnect");
    LOAD_FN(fn_WlanRegisterNotification, "WlanRegisterNotification");

    #undef LOAD_FN

    return true;
}

void WlanDll::unload() {
    if (dll) {
        fn_WlanOpenHandle = nullptr;
        fn_WlanCloseHandle = nullptr;
        fn_WlanEnumInterfaces = nullptr;
        fn_WlanFreeMemory = nullptr;
        fn_WlanQueryInterface = nullptr;
        fn_WlanGetAvailableNetworkList = nullptr;
        fn_WlanGetProfileList = nullptr;
        fn_WlanSetProfile = nullptr;
        fn_WlanConnect = nullptr;
        fn_WlanRegisterNotification = nullptr;
        FreeLibrary(dll);
        dll = nullptr;
    }
}

bool isAvailable() {
    return instance().load();
}

} // namespace dyn_wlan
} // namespace ww
