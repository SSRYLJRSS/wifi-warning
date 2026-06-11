#include "core/dyn_bcrypt.h"

namespace ww {
namespace dyn_bcrypt {

decltype(&BCryptOpenAlgorithmProvider) fn_BCryptOpenAlgorithmProvider = nullptr;
decltype(&BCryptCloseAlgorithmProvider) fn_BCryptCloseAlgorithmProvider = nullptr;
decltype(&BCryptCreateHash) fn_BCryptCreateHash = nullptr;
decltype(&BCryptDestroyHash) fn_BCryptDestroyHash = nullptr;
decltype(&BCryptHashData) fn_BCryptHashData = nullptr;
decltype(&BCryptFinishHash) fn_BCryptFinishHash = nullptr;
decltype(&BCryptGetProperty) fn_BCryptGetProperty = nullptr;

static BcryptDll g_bcryptDll;

BcryptDll& instance() {
    return g_bcryptDll;
}

bool BcryptDll::load() {
    if (dll) return true;
    dll = LoadLibraryW(L"bcrypt.dll");
    if (!dll) return false;

    #define LOAD_FN(name) \
        fn_##name = reinterpret_cast<decltype(&name)>(GetProcAddress(dll, #name)); \
        if (!fn_##name) { unload(); return false; }

    LOAD_FN(BCryptOpenAlgorithmProvider);
    LOAD_FN(BCryptCloseAlgorithmProvider);
    LOAD_FN(BCryptCreateHash);
    LOAD_FN(BCryptDestroyHash);
    LOAD_FN(BCryptHashData);
    LOAD_FN(BCryptFinishHash);
    LOAD_FN(BCryptGetProperty);

    #undef LOAD_FN

    return true;
}

void BcryptDll::unload() {
    if (dll) {
        fn_BCryptOpenAlgorithmProvider = nullptr;
        fn_BCryptCloseAlgorithmProvider = nullptr;
        fn_BCryptCreateHash = nullptr;
        fn_BCryptDestroyHash = nullptr;
        fn_BCryptHashData = nullptr;
        fn_BCryptFinishHash = nullptr;
        fn_BCryptGetProperty = nullptr;
        FreeLibrary(dll);
        dll = nullptr;
    }
}

bool isAvailable() {
    return instance().load();
}

} // namespace dyn_bcrypt
} // namespace ww
