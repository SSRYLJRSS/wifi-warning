#pragma once
// Dynamic loader for bcrypt.dll - reduces PE import footprint.

#include <windows.h>
#include <bcrypt.h>
#include <string>
#include <vector>

namespace ww {
namespace dyn_bcrypt {

struct BcryptDll {
    HMODULE dll = nullptr;
    bool load();
    void unload();
    bool loaded() const { return dll != nullptr; }
    ~BcryptDll() { unload(); }
    BcryptDll() = default;
    BcryptDll(const BcryptDll&) = delete;
    BcryptDll& operator=(const BcryptDll&) = delete;
};

BcryptDll& instance();

extern decltype(&BCryptOpenAlgorithmProvider) fn_BCryptOpenAlgorithmProvider;
extern decltype(&BCryptCloseAlgorithmProvider) fn_BCryptCloseAlgorithmProvider;
extern decltype(&BCryptCreateHash) fn_BCryptCreateHash;
extern decltype(&BCryptDestroyHash) fn_BCryptDestroyHash;
extern decltype(&BCryptHashData) fn_BCryptHashData;
extern decltype(&BCryptFinishHash) fn_BCryptFinishHash;
extern decltype(&BCryptGetProperty) fn_BCryptGetProperty;

bool isAvailable();

} // namespace dyn_bcrypt
} // namespace ww
