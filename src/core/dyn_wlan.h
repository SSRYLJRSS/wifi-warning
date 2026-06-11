#pragma once
// Dynamic loader for wlanapi.dll - avoids GPS/Location permission association.

#include <windows.h>

// Forward-declare WLAN types to avoid pulling in wlanapi.h at compile link time
struct _WLAN_INTERFACE_INFO_LIST;
typedef struct _WLAN_INTERFACE_INFO_LIST WLAN_INTERFACE_INFO_LIST;
struct _WLAN_AVAILABLE_NETWORK_LIST;
typedef struct _WLAN_AVAILABLE_NETWORK_LIST WLAN_AVAILABLE_NETWORK_LIST;
struct _WLAN_PROFILE_INFO_LIST;
typedef struct _WLAN_PROFILE_INFO_LIST WLAN_PROFILE_INFO_LIST;
struct _WLAN_CONNECTION_ATTRIBUTES;
typedef struct _WLAN_CONNECTION_ATTRIBUTES WLAN_CONNECTION_ATTRIBUTES;
struct _WLAN_CONNECTION_PARAMETERS;
typedef struct _WLAN_CONNECTION_PARAMETERS WLAN_CONNECTION_PARAMETERS;

namespace ww {
namespace dyn_wlan {

// Explicit function pointer types to avoid linking wlanapi.lib
typedef DWORD (WINAPI *FnWlanOpenHandle)(DWORD, PVOID*, PDWORD, PHANDLE);
typedef DWORD (WINAPI *FnWlanCloseHandle)(HANDLE, PVOID);
typedef DWORD (WINAPI *FnWlanEnumInterfaces)(HANDLE, PVOID, WLAN_INTERFACE_INFO_LIST**);
typedef VOID  (WINAPI *FnWlanFreeMemory)(PVOID);
typedef DWORD (WINAPI *FnWlanQueryInterface)(HANDLE, const GUID*, DWORD, PVOID, PDWORD, PVOID*, PVOID);
typedef DWORD (WINAPI *FnWlanGetAvailableNetworkList)(HANDLE, const GUID*, DWORD, PVOID, WLAN_AVAILABLE_NETWORK_LIST**);
typedef DWORD (WINAPI *FnWlanGetProfileList)(HANDLE, const GUID*, PVOID, WLAN_PROFILE_INFO_LIST**);
typedef DWORD (WINAPI *FnWlanSetProfile)(HANDLE, const GUID*, DWORD, LPCWSTR, PVOID, BOOL, PVOID, DWORD*);
typedef DWORD (WINAPI *FnWlanConnect)(HANDLE, const GUID*, const WLAN_CONNECTION_PARAMETERS*, PVOID);

extern FnWlanOpenHandle fn_WlanOpenHandle;
extern FnWlanCloseHandle fn_WlanCloseHandle;
extern FnWlanEnumInterfaces fn_WlanEnumInterfaces;
extern FnWlanFreeMemory fn_WlanFreeMemory;
extern FnWlanQueryInterface fn_WlanQueryInterface;
extern FnWlanGetAvailableNetworkList fn_WlanGetAvailableNetworkList;
extern FnWlanGetProfileList fn_WlanGetProfileList;
extern FnWlanSetProfile fn_WlanSetProfile;
extern FnWlanConnect fn_WlanConnect;

struct WlanDll {
    HMODULE dll = nullptr;
    bool load();
    void unload();
    bool loaded() const { return dll != nullptr; }
    ~WlanDll() { unload(); }
    WlanDll() = default;
    WlanDll(const WlanDll&) = delete;
    WlanDll& operator=(const WlanDll&) = delete;
};

struct WlanHandle {
    WlanHandle() = default;
    HANDLE handle = nullptr;
    ~WlanHandle() {
        if (handle && fn_WlanCloseHandle) fn_WlanCloseHandle(handle, nullptr);
    }
    WlanHandle(const WlanHandle&) = delete;
    WlanHandle& operator=(const WlanHandle&) = delete;
};

WlanDll& instance();
bool isAvailable();

inline void freeMemory(void* ptr) {
    if (ptr && fn_WlanFreeMemory) fn_WlanFreeMemory(ptr);
}

} // namespace dyn_wlan
} // namespace ww
