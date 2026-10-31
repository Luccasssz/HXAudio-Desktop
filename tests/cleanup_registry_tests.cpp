#include "../DriverSetup/CleanupVerification.h"
#include <iostream>
using namespace HXAudioCleanupVerification;
int main() {
    const wchar_t* path = L"Software\\HXAudioCleanupTest-88e";
    RegDeleteTreeW(HKEY_CURRENT_USER, path);
    HKEY root = nullptr, key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, nullptr, REG_OPTION_VOLATILE,
        KEY_ALL_ACCESS, nullptr, &root, nullptr) != ERROR_SUCCESS) return 1;
    if (RegistryClean(root)) return 2; // Missing enumeration root is unknown.
    if (RegCreateKeyExW(root, render, 0, nullptr, REG_OPTION_VOLATILE,
        KEY_ALL_ACCESS, nullptr, &key, nullptr) != ERROR_SUCCESS) return 3;
    RegCloseKey(key);
    if (!RegistryClean(root)) return 4;
    const std::wstring fxPath = std::wstring(render) + L"\\endpoint\\FxProperties";
    if (RegCreateKeyExW(root, fxPath.c_str(), 0, nullptr, REG_OPTION_VOLATILE,
        KEY_ALL_ACCESS, nullptr, &key, nullptr) != ERROR_SUCCESS) return 5;
    if (RegSetValueExW(key, L"effects", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(clsid), sizeof(clsid)) != ERROR_SUCCESS) return 6;
    if (RegistryClean(root)) return 7;
    if (RegDeleteValueW(key, L"effects") != ERROR_SUCCESS) return 8;
    RegCloseKey(key);
    if (!RegistryClean(root)) return 9;
    const std::wstring orphan = std::wstring(backups) + L"\\missing-endpoint";
    if (RegCreateKeyExW(root, orphan.c_str(), 0, nullptr, REG_OPTION_VOLATILE,
        KEY_ALL_ACCESS, nullptr, &key, nullptr) != ERROR_SUCCESS) return 10;
    RegCloseKey(key);
    if (RegistryClean(root)) return 11;
    RegCloseKey(root);
    if (RegDeleteTreeW(HKEY_CURRENT_USER, path) != ERROR_SUCCESS) return 12;
    std::cout << "PASS isolated HKCU read-only cleanup checks\n";
    return 0;
}
