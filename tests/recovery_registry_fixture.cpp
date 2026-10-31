#include "../DriverSetup/RecoveryFingerprint.h"
#include <windows.h>
#include <cstdlib>
#include <iostream>
using namespace HXAudioRecovery;
static constexpr wchar_t rootPath[] = L"Software\\HXAudio\\CIRecoveryFixture-Only";
static Record Fixture() {
    Record r; r.schema = 1; r.transactionId = L"isolated-transaction";
    r.identity = {L"fixture-endpoint", L"fixture-device", L"fixture.inf", L"1.0"};
    r.driverProvider = L"fixture-provider"; r.installedConfirmed = true;
    r.properties = {
        {L"effect", {true, REG_BINARY, {1,2,3}}, {true, REG_BINARY, {1,2,3,4}}, {}},
        {L"association", {false, 0, {}}, {true, REG_BINARY, {7,8}}, {}}
    };
    for (const auto& p : r.properties) r.expectedInstalled.push_back(p.installed);
    return r;
}
static bool SetBytes(HKEY key, const wchar_t* name, const std::vector<uint8_t>& value) {
    return RegSetValueExW(key, name, 0, REG_BINARY, value.empty() ? nullptr : value.data(),
        static_cast<DWORD>(value.size())) == ERROR_SUCCESS;
}
static bool GetBytes(HKEY key, const wchar_t* name, std::vector<uint8_t>& value) {
    DWORD type = 0, size = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS || type != REG_BINARY) return false;
    value.resize(size);
    return RegQueryValueExW(key, name, nullptr, &type, value.empty() ? nullptr : value.data(), &size) == ERROR_SUCCESS && type == REG_BINARY;
}
struct FixtureAdapter : Adapter {
    HKEY root = nullptr, values = nullptr;
    int crashSave = 0, crashWrite = 0, saves = 0, writes = 0;
    bool TrustedOrigin() override { return root && values; } // fixture namespace only; never production authority
    bool ValidFingerprint(const Record& r) override {
        std::vector<uint8_t> stored, canonical, hash, expected;
        return GetBytes(root, L"Record", stored) && CanonicalRecord(r, canonical) && stored == canonical &&
            GetBytes(root, L"Fingerprint", hash) && ComputeFingerprint(r, expected) && hash == expected;
    }
    bool ReadIdentity(Identity& id, std::wstring& provider) override {
        DWORD type = 0, version = 0, size = sizeof(version);
        if (RegQueryValueExW(root, L"DriverVersion", nullptr, &type, reinterpret_cast<BYTE*>(&version), &size) != ERROR_SUCCESS ||
            type != REG_DWORD || size != sizeof(version)) return false;
        id = Fixture().identity; if (version != 1) id.driverVersion = L"changed";
        provider = L"fixture-provider"; return true;
    }
    bool ReadValue(const std::wstring& name, RawValue& out) override {
        DWORD type = 0, size = 0;
        LONG e = RegQueryValueExW(values, name.c_str(), nullptr, &type, nullptr, &size);
        if (e == ERROR_FILE_NOT_FOUND) { out = {}; return true; }
        if (e != ERROR_SUCCESS) return false;
        out = {}; out.exists = true; out.type = type; out.bytes.resize(size);
        return RegQueryValueExW(values, name.c_str(), nullptr, &type,
            out.bytes.empty() ? nullptr : out.bytes.data(), &size) == ERROR_SUCCESS && type == out.type;
    }
    bool WriteValue(const std::wstring& name, const RawValue& value) override {
        LONG e = value.exists ? RegSetValueExW(values, name.c_str(), 0, value.type,
            value.bytes.empty() ? nullptr : value.bytes.data(), static_cast<DWORD>(value.bytes.size())) :
            RegDeleteValueW(values, name.c_str());
        if (!value.exists && e == ERROR_FILE_NOT_FOUND) e = ERROR_SUCCESS;
        if (e != ERROR_SUCCESS || RegFlushKey(values) != ERROR_SUCCESS) return false;
        if (++writes == crashWrite) TerminateProcess(GetCurrentProcess(), 91);
        return true;
    }
    bool SaveProgress(Phase phase, uint32_t index) override {
        const DWORD progress[] = {static_cast<DWORD>(phase), index};
        if (RegSetValueExW(root, L"Progress", 0, REG_BINARY,
            reinterpret_cast<const BYTE*>(progress), sizeof(progress)) != ERROR_SUCCESS || RegFlushKey(root) != ERROR_SUCCESS) return false;
        if (++saves == crashSave) TerminateProcess(GetCurrentProcess(), 92);
        return true;
    }
};
int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 2;
    const std::wstring command = argv[1];
    if (command == L"init") RegDeleteTreeW(HKEY_CURRENT_USER, rootPath);
    HKEY root = nullptr, values = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, rootPath, 0, nullptr, REG_OPTION_VOLATILE,
        KEY_ALL_ACCESS, nullptr, &root, nullptr) != ERROR_SUCCESS ||
        RegCreateKeyExW(root, L"FixtureValues", 0, nullptr, REG_OPTION_VOLATILE,
        KEY_ALL_ACCESS, nullptr, &values, nullptr) != ERROR_SUCCESS) return 3;
    FixtureAdapter adapter; adapter.root = root; adapter.values = values;
    const Record record = Fixture(); int result = 0;
    if (command == L"init") {
        std::vector<uint8_t> canonical, hash;
        DWORD version = 1;
        if (!CanonicalRecord(record, canonical) || !ComputeFingerprint(record, hash) ||
            !SetBytes(root, L"Record", canonical) || !SetBytes(root, L"Fingerprint", hash) ||
            RegSetValueExW(root, L"DriverVersion", 0, REG_DWORD, reinterpret_cast<BYTE*>(&version), sizeof(version)) != ERROR_SUCCESS) result = 4;
        for (const auto& p : record.properties) if (!adapter.WriteValue(p.name, p.installed)) result = 4;
        if (RegFlushKey(root) != ERROR_SUCCESS) result = 4;
    } else if (command == L"run") {
        if (argc >= 3) adapter.crashSave = _wtoi(argv[2]);
        if (argc >= 4) adapter.crashWrite = _wtoi(argv[3]);
        result = static_cast<int>(Run(record, adapter));
    } else if (command == L"driver-update") {
        DWORD version = 2;
        if (RegSetValueExW(root, L"DriverVersion", 0, REG_DWORD, reinterpret_cast<BYTE*>(&version), sizeof(version)) != ERROR_SUCCESS) result = 4;
    } else if (command == L"corrupt") {
        if (!SetBytes(root, L"Fingerprint", {1,2,3})) result = 4;
    } else if (command == L"conflict") {
        if (!SetBytes(values, L"effect", {99})) result = 4;
    } else if (command == L"check") {
        DWORD progress[2]{}, type = 0, size = sizeof(progress);
        if (RegQueryValueExW(root, L"Progress", nullptr, &type, reinterpret_cast<BYTE*>(progress), &size) != ERROR_SUCCESS ||
            type != REG_BINARY || size != sizeof(progress) || progress[0] != static_cast<DWORD>(Phase::Archived)) result = 5;
        for (const auto& p : record.properties) { RawValue v; if (!adapter.ReadValue(p.name, v) || !HXAudioRestorePlan::Equal(v, p.original)) result = 5; }
        if (!adapter.ValidFingerprint(record)) result = 5;
    } else if (command == L"check-installed" || command == L"check-conflict") {
        for (const auto& p : record.properties) {
            RawValue expected = p.installed, actual;
            if (command == L"check-conflict" && p.name == L"effect") expected.bytes = {99};
            if (!adapter.ReadValue(p.name, actual) || !HXAudioRestorePlan::Equal(actual, expected)) result = 5;
        }
    } else if (command != L"clean") result = 2;
    RegCloseKey(values); RegCloseKey(root);
    if (command == L"clean") RegDeleteTreeW(HKEY_CURRENT_USER, rootPath);
    return result;
}
