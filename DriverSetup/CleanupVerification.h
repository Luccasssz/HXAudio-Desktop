#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <cwctype>
#include <cstring>

// Read-only, fail-closed verification. `root` can be an isolated test hive;
// callers separately check the service and both machine/user COM registrations.
namespace HXAudioCleanupVerification {
constexpr wchar_t clsid[] = L"{C4D1BA43-C7A8-4B47-8DCF-6DCF554D0F59}";
constexpr wchar_t render[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render";
constexpr wchar_t backups[] = L"Software\\HXAudio\\Apo\\Backup";
constexpr wchar_t components[] = L"Software\\HXAudio\\Apo\\ComponentBackup";
constexpr wchar_t registration[] = L"Software\\Classes\\CLSID\\{C4D1BA43-C7A8-4B47-8DCF-6DCF554D0F59}";
constexpr wchar_t engineRegistration[] = L"Software\\Classes\\AudioEngine\\AudioProcessingObjects\\{C4D1BA43-C7A8-4B47-8DCF-6DCF554D0F59}";
inline bool AbsentKey(HKEY root, const wchar_t* path, REGSAM view = 0) {
    HKEY key = nullptr;
    LONG e = RegOpenKeyExW(root, path, 0, KEY_READ | view, &key);
    if (e == ERROR_SUCCESS) RegCloseKey(key);
    return e == ERROR_FILE_NOT_FOUND;
}
inline bool EmptyBackup(HKEY root, const wchar_t* path, bool component) {
    HKEY key = nullptr;
    LONG e = RegOpenKeyExW(root, path, 0, KEY_READ, &key);
    if (e == ERROR_FILE_NOT_FOUND) return true;
    if (e != ERROR_SUCCESS) return false;
    DWORD subkeys = 0, values = 0;
    e = RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subkeys, nullptr,
        nullptr, &values, nullptr, nullptr, nullptr, nullptr);
    bool ok = e == ERROR_SUCCESS && subkeys == 0 && values == 0;
    if (e == ERROR_SUCCESS && subkeys == 0 && component && values == 1) {
        DWORD type = 0, size = sizeof(DWORD), value = 0;
        ok = RegQueryValueExW(key, L"Next", nullptr, &type,
            reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS &&
            type == REG_DWORD && size == sizeof(DWORD);
    }
    RegCloseKey(key);
    return ok;
}
inline bool ContainsClsid(const std::vector<BYTE>& data) {
    const BYTE guid[] = {0x43,0xba,0xd1,0xc4,0xa8,0xc7,0x47,0x4b,
        0x8d,0xcf,0x6d,0xcf,0x55,0x4d,0x0f,0x59};
    for (size_t i = 0; i + sizeof(guid) <= data.size(); ++i)
        if (std::memcmp(data.data() + i, guid, sizeof(guid)) == 0) return true;
    const size_t n = sizeof(clsid) / sizeof(wchar_t) - 1;
    for (size_t offset = 0; offset + n * sizeof(wchar_t) <= data.size(); offset += sizeof(wchar_t)) {
        bool match = true;
        for (size_t i = 0; i < n; ++i) {
            wchar_t c = 0;
            std::memcpy(&c, data.data() + offset + i * sizeof(wchar_t), sizeof(c));
            if (std::towupper(c) != std::towupper(clsid[i])) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}
inline bool CheckValues(HKEY key) {
    DWORD count = 0, maxName = 0, maxData = 0;
    if (RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        &count, &maxName, &maxData, nullptr, nullptr) != ERROR_SUCCESS) return false;
    std::vector<wchar_t> name(static_cast<size_t>(maxName) + 2);
    std::vector<BYTE> data(static_cast<size_t>(maxData) + 2);
    for (DWORD i = 0; i < count; ++i) {
        DWORD n = static_cast<DWORD>(name.size()), d = static_cast<DWORD>(data.size()), type = 0;
        if (RegEnumValueW(key, i, name.data(), &n, nullptr, &type, data.data(), &d) != ERROR_SUCCESS)
            return false;
        const std::wstring valueName(name.data(), n);
        const wchar_t* fxPrefix = L"{D04E05A6-594B-4FB6-A80D-01AF5EED7D1D},";
        if (_wcsnicmp(valueName.c_str(), fxPrefix, wcslen(fxPrefix)) == 0 &&
            type != REG_SZ && type != REG_MULTI_SZ && type != REG_EXPAND_SZ) return false;
        if ((type == REG_SZ || type == REG_MULTI_SZ || type == REG_EXPAND_SZ) &&
            (d < sizeof(wchar_t) || d % sizeof(wchar_t) != 0 || data[d-1] != 0 || data[d-2] != 0)) return false;
        std::vector<BYTE> value(data.begin(), data.begin() + d);
        if (ContainsClsid(value)) return false;
    }
    DWORD n = static_cast<DWORD>(name.size());
    return RegEnumValueW(key, count, name.data(), &n, nullptr, nullptr, nullptr, nullptr) == ERROR_NO_MORE_ITEMS;
}
inline bool RegistryClean(HKEY root) {
    if (!EmptyBackup(root, backups, false)) return false;
    HKEY endpoints = nullptr;
    if (RegOpenKeyExW(root, render, 0, KEY_READ, &endpoints) != ERROR_SUCCESS) return false;
    bool ok = true;
    for (DWORD i = 0; ok; ++i) {
        wchar_t name[512]{}; DWORD n = ARRAYSIZE(name);
        LONG e = RegEnumKeyExW(endpoints, i, name, &n, nullptr, nullptr, nullptr, nullptr);
        if (e == ERROR_NO_MORE_ITEMS) break;
        if (e != ERROR_SUCCESS) { ok = false; break; }
        HKEY endpoint = nullptr;
        e = RegOpenKeyExW(endpoints, name, 0, KEY_READ, &endpoint);
        if (e != ERROR_SUCCESS) { ok = false; break; }
        HKEY fx = nullptr;
        e = RegOpenKeyExW(endpoint, L"FxProperties", 0, KEY_READ, &fx);
        if (e == ERROR_SUCCESS) { ok = CheckValues(fx); RegCloseKey(fx); }
        else if (e != ERROR_FILE_NOT_FOUND) ok = false;
        RegCloseKey(endpoint);
    }
    RegCloseKey(endpoints);
    return ok;
}
}
