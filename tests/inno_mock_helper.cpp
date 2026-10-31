#include "../DriverSetup/SafetyLogic.h"
#include <windows.h>
#include <string>
#include <fstream>
static std::wstring Dir() {
    wchar_t path[MAX_PATH]{}; GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path); return s.substr(0, s.find_last_of(L"\\/"));
}
static bool Exists(const std::wstring& name) {
    return GetFileAttributesW((Dir() + L"\\" + name).c_str()) != INVALID_FILE_ATTRIBUTES;
}
static void Mark(const std::wstring& name) {
    std::ofstream out(Dir() + L"\\" + name, std::ios::app); out << "1\n";
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 98;
    const std::wstring command = argv[1];
    { std::wofstream out(Dir() + L"\\calls.log", std::ios::app); out << command << L"\n"; }
    if (command == L"--stop") return Exists(L"fail_stop") ? 11 : 0;
    if (command == L"--shutdown-ui") return Exists(L"fail_ui") ? 12 : 0;
    if (command == L"engine-bypass-uninstall") return Exists(L"fail_bypass") ? 13 : 0;
    if (command != L"purge") return 99;
    return HXAudioSafety::Purge([] {
        if (Exists(L"fail_apo")) return 14;
        if (Exists(L"apo.done")) return Exists(L"fail_verify") ? 16 : 0;
        Mark(L"apo.done"); return 0;
    }, [] { return Exists(L"fail_virtual") ? 15 : 0; }, [] { return 0; });
}
