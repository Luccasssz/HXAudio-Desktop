#include <windows.h>
#include <setupapi.h>
#include <newdev.h>
#include <cfgmgr32.h>
#include <algorithm>
#include <string>
#include <vector>
#include <iostream>

#include "EngineBypass.h"
#include "ApoManager.h"
#include "SafetyLogic.h"

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "newdev.lib")
#pragma comment(lib, "cfgmgr32.lib")

namespace
{
    constexpr wchar_t kHardwareId[] = L"Root\\HXAudioVirtualAudio";
    constexpr wchar_t kRegistryPath[] = L"Software\\HXAudio\\VirtualAudio";
    constexpr wchar_t kRunOncePath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce";
    constexpr wchar_t kRecoveryValue[] = L"HXAudioDriverRecovery";
    constexpr DWORD kNoDeviceProblem = 0;
    const GUID kMediaClass = { 0x4d36e96c, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } };
    const DEVPROPKEY kDeviceDriverInfPath = {
        { 0xa8b865dd, 0x2e3d, 0x4094, { 0xad, 0x97, 0xe5, 0x93, 0xa7, 0x0c, 0x75, 0xd6 } }, 5 };
    const DEVPROPKEY kDeviceDriverVersion = {
        { 0xa8b865dd, 0x2e3d, 0x4094, { 0xad, 0x97, 0xe5, 0x93, 0xa7, 0x0c, 0x75, 0xd6 } }, 3 };

    struct SYSTEM_CODEINTEGRITY_INFORMATION
    {
        ULONG Length;
        ULONG CodeIntegrityOptions;
    };

    using NtQuerySystemInformationFn = LONG (NTAPI*)(ULONG, PVOID, ULONG, PULONG);
    constexpr ULONG kSystemCodeIntegrityInformation = 103;
    constexpr ULONG kCodeIntegrityTestSign = 0x02;

    std::wstring LastErrorText(DWORD error = GetLastError())
    {
        wchar_t* buffer = nullptr;
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, error, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
        std::wstring text = buffer ? buffer : L"Unknown error";
        if (buffer) LocalFree(buffer);
        return text;
    }

    bool IsAdministrator()
    {
        BOOL member = FALSE;
        SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
        PSID sid = nullptr;
        if (AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0, &sid))
        {
            CheckTokenMembership(nullptr, sid, &member);
            FreeSid(sid);
        }
        return member != FALSE;
    }

    bool HardwareIdsContain(HDEVINFO set, SP_DEVINFO_DATA& device, const wchar_t* wanted)
    {
        DWORD type = 0, required = 0;
        SetupDiGetDeviceRegistryPropertyW(set, &device, SPDRP_HARDWAREID, &type, nullptr, 0, &required);
        if (!required) return false;
        std::vector<BYTE> bytes(required + sizeof(wchar_t));
        if (!SetupDiGetDeviceRegistryPropertyW(set, &device, SPDRP_HARDWAREID, &type, bytes.data(), required, nullptr)) return false;
        const wchar_t* item = reinterpret_cast<const wchar_t*>(bytes.data());
        while (*item)
        {
            if (_wcsicmp(item, wanted) == 0) return true;
            item += wcslen(item) + 1;
        }
        return false;
    }

    struct DeviceState
    {
        bool present = false;
        bool started = false;
        DWORD problem = kNoDeviceProblem;
    };

    std::vector<DeviceState> MatchingDeviceStates()
    {
        HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
        std::vector<DeviceState> states;
        if (set == INVALID_HANDLE_VALUE) return states;
        SP_DEVINFO_DATA device{ sizeof(device) };
        for (DWORD index = 0; SetupDiEnumDeviceInfo(set, index, &device); ++index)
        {
            if (!HardwareIdsContain(set, device, kHardwareId)) continue;
            ULONG status = 0;
            ULONG problem = kNoDeviceProblem;
            const CONFIGRET result = CM_Get_DevNode_Status(&status, &problem,
                device.DevInst, 0);
            DeviceState state;
            state.present = true;
            state.problem = result == CR_SUCCESS ? problem : CM_PROB_FAILED_INSTALL;
            state.started = result == CR_SUCCESS && problem == kNoDeviceProblem &&
                (status & DN_STARTED) != 0;
            states.push_back(state);
        }
        SetupDiDestroyDeviceInfoList(set);
        return states;
    }

    bool DeviceExists()
    {
        return !MatchingDeviceStates().empty();
    }

    bool HealthyDeviceExists()
    {
        const std::vector<DeviceState> states = MatchingDeviceStates();
        return std::any_of(states.begin(), states.end(),
            [](const DeviceState& state) { return state.present && state.started; });
    }

    std::wstring LeafName(std::wstring path)
    {
        const size_t slash = path.find_last_of(L"\\/");
        if (slash != std::wstring::npos) path.erase(0, slash + 1);
        return path;
    }

    bool HealthyDeviceUsesInf(const std::wstring& publishedInf)
    {
        const std::wstring wanted = LeafName(publishedInf);
        if (wanted.empty()) return false;
        HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr,
            DIGCF_ALLCLASSES | DIGCF_PRESENT);
        if (set == INVALID_HANDLE_VALUE) return false;
        bool matched = false;
        SP_DEVINFO_DATA device{ sizeof(device) };
        for (DWORD index = 0; SetupDiEnumDeviceInfo(set, index, &device); ++index)
        {
            if (!HardwareIdsContain(set, device, kHardwareId)) continue;
            ULONG status = 0, problem = 0;
            if (CM_Get_DevNode_Status(&status, &problem, device.DevInst, 0) != CR_SUCCESS ||
                problem != kNoDeviceProblem || (status & DN_STARTED) == 0)
                continue;
            DEVPROPTYPE type = 0;
            wchar_t infPath[MAX_PATH]{};
            if (SetupDiGetDevicePropertyW(set, &device,
                &kDeviceDriverInfPath, &type,
                reinterpret_cast<BYTE*>(infPath), sizeof(infPath), nullptr, 0) &&
                type == DEVPROP_TYPE_STRING &&
                _wcsicmp(LeafName(infPath).c_str(), wanted.c_str()) == 0)
            {
                matched = true;
                break;
            }
        }
        SetupDiDestroyDeviceInfoList(set);
        return matched;
    }

    std::wstring InfDriverVersion(const wchar_t* infPath)
    {
        UINT errorLine = 0;
        HINF inf = SetupOpenInfFileW(infPath, nullptr, INF_STYLE_WIN4, &errorLine);
        if (inf == INVALID_HANDLE_VALUE) return {};
        wchar_t driverVer[256]{};
        DWORD required = 0;
        const BOOL read = SetupGetLineTextW(nullptr, inf, L"Version", L"DriverVer",
            driverVer, ARRAYSIZE(driverVer), &required);
        SetupCloseInfFile(inf);
        if (!read) return {};
        std::wstring value = driverVer;
        const size_t comma = value.find(L',');
        if (comma != std::wstring::npos) value.erase(0, comma + 1);
        const size_t first = value.find_first_not_of(L" \t");
        const size_t last = value.find_last_not_of(L" \t");
        return first == std::wstring::npos
            ? std::wstring() : value.substr(first, last - first + 1);
    }

    bool HealthyDeviceUsesVersion(const std::wstring& version)
    {
        if (version.empty()) return false;
        HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr,
            DIGCF_ALLCLASSES | DIGCF_PRESENT);
        if (set == INVALID_HANDLE_VALUE) return false;
        bool matched = false;
        SP_DEVINFO_DATA device{ sizeof(device) };
        for (DWORD index = 0; SetupDiEnumDeviceInfo(set, index, &device); ++index)
        {
            if (!HardwareIdsContain(set, device, kHardwareId)) continue;
            ULONG status = 0, problem = 0;
            if (CM_Get_DevNode_Status(&status, &problem, device.DevInst, 0) != CR_SUCCESS ||
                problem != kNoDeviceProblem || (status & DN_STARTED) == 0)
                continue;
            DEVPROPTYPE type = 0;
            wchar_t installedVersion[128]{};
            if (SetupDiGetDevicePropertyW(set, &device,
                &kDeviceDriverVersion, &type,
                reinterpret_cast<BYTE*>(installedVersion),
                sizeof(installedVersion), nullptr, 0) &&
                type == DEVPROP_TYPE_STRING &&
                _wcsicmp(installedVersion, version.c_str()) == 0)
            {
                matched = true;
                break;
            }
        }
        SetupDiDestroyDeviceInfoList(set);
        return matched;
    }

    bool WaitForHealthyDevice(DWORD timeoutMs)
    {
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        do
        {
            if (HealthyDeviceExists()) return true;
            if (GetTickCount64() >= deadline) break;
            Sleep(100);
        } while (true);
        return HealthyDeviceExists();
    }

    bool RestartMatchingDevices()
    {
        HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr,
            DIGCF_ALLCLASSES | DIGCF_PRESENT);
        if (set == INVALID_HANDLE_VALUE) return false;
        bool found = false;
        bool ok = true;
        SP_DEVINFO_DATA device{ sizeof(device) };
        for (DWORD index = 0; SetupDiEnumDeviceInfo(set, index, &device); ++index)
        {
            if (!HardwareIdsContain(set, device, kHardwareId)) continue;
            found = true;
            SP_PROPCHANGE_PARAMS change{};
            change.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
            change.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
            change.StateChange = DICS_PROPCHANGE;
            change.Scope = DICS_FLAG_GLOBAL;
            change.HwProfile = 0;
            if (!SetupDiSetClassInstallParamsW(set, &device,
                &change.ClassInstallHeader, sizeof(change)) ||
                !SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, &device))
                ok = false;
        }
        SetupDiDestroyDeviceInfoList(set);
        return found && ok;
    }

    bool CreateRootDevice()
    {
        HDEVINFO set = SetupDiCreateDeviceInfoList(&kMediaClass, nullptr);
        if (set == INVALID_HANDLE_VALUE) return false;
        SP_DEVINFO_DATA device{ sizeof(device) };
        bool ok = SetupDiCreateDeviceInfoW(set, L"HXAudioVirtualAudio", &kMediaClass,
            L"HXAudio Virtual Output", nullptr, DICD_GENERATE_ID, &device) != FALSE;
        if (ok)
        {
            const wchar_t ids[] = L"Root\\HXAudioVirtualAudio\0\0";
            ok = SetupDiSetDeviceRegistryPropertyW(set, &device, SPDRP_HARDWAREID,
                reinterpret_cast<const BYTE*>(ids), sizeof(ids)) != FALSE;
        }
        if (ok) ok = SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, &device) != FALSE;
        SetupDiDestroyDeviceInfoList(set);
        return ok;
    }

    void SavePublishedInf(const std::wstring& name)
    {
        HKEY key{};
        DWORD disposition{};
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kRegistryPath, 0, nullptr, 0, KEY_WRITE, nullptr, &key, &disposition) == ERROR_SUCCESS)
        {
            RegSetValueExW(key, L"PublishedInf", 0, REG_SZ, reinterpret_cast<const BYTE*>(name.c_str()), static_cast<DWORD>((name.size() + 1) * sizeof(wchar_t)));
            RegCloseKey(key);
        }
    }

    void SetInstallationMode(const wchar_t* mode)
    {
        HKEY key{};
        DWORD disposition{};
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kRegistryPath, 0, nullptr, 0,
            KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, &disposition) == ERROR_SUCCESS)
        {
            RegSetValueExW(key, L"InstallationMode", 0, REG_SZ,
                reinterpret_cast<const BYTE*>(mode),
                static_cast<DWORD>((wcslen(mode) + 1) * sizeof(wchar_t)));
            RegCloseKey(key);
        }
    }

    std::wstring ReadPublishedInf()
    {
        HKEY key{};
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRegistryPath, 0, KEY_READ, &key) != ERROR_SUCCESS) return {};
        wchar_t value[MAX_PATH]{};
        DWORD type{}, size = sizeof(value);
        const LONG result = RegQueryValueExW(key, L"PublishedInf", nullptr, &type, reinterpret_cast<BYTE*>(value), &size);
        RegCloseKey(key);
        return result == ERROR_SUCCESS && type == REG_SZ ? value : L"";
    }

    bool WaitForServiceState(SC_HANDLE service, DWORD wanted, DWORD timeoutMs)
    {
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        SERVICE_STATUS_PROCESS status{};
        DWORD bytes = 0;
        do
        {
            if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                reinterpret_cast<BYTE*>(&status), sizeof(status), &bytes)) return false;
            if (status.dwCurrentState == wanted) return true;
            Sleep(100);
        } while (GetTickCount64() < deadline);
        return false;
    }

    bool StopServiceTree(SC_HANDLE manager, const std::wstring& name,
        std::vector<std::wstring>& stopped)
    {
        SC_HANDLE service = OpenServiceW(manager, name.c_str(),
            SERVICE_QUERY_STATUS | SERVICE_ENUMERATE_DEPENDENTS |
            SERVICE_STOP | SERVICE_START);
        if (!service) return false;

        SERVICE_STATUS_PROCESS current{};
        DWORD bytes = 0;
        if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
            reinterpret_cast<BYTE*>(&current), sizeof(current), &bytes))
        {
            CloseServiceHandle(service);
            return false;
        }
        if (current.dwCurrentState == SERVICE_STOPPED)
        {
            CloseServiceHandle(service);
            return true;
        }
        if (current.dwCurrentState == SERVICE_STOP_PENDING)
        {
            const bool stoppedOk = WaitForServiceState(service,
                SERVICE_STOPPED, 20000);
            CloseServiceHandle(service);
            return stoppedOk;
        }

        DWORD needed = 0;
        DWORD count = 0;
        EnumDependentServicesW(service, SERVICE_ACTIVE, nullptr, 0,
            &needed, &count);
        if (GetLastError() == ERROR_MORE_DATA && needed != 0)
        {
            std::vector<BYTE> buffer(needed);
            auto entries = reinterpret_cast<ENUM_SERVICE_STATUSW*>(buffer.data());
            if (!EnumDependentServicesW(service, SERVICE_ACTIVE, entries, needed,
                &needed, &count))
            {
                CloseServiceHandle(service);
                return false;
            }
            for (DWORD index = 0; index < count; ++index)
            {
                if (!StopServiceTree(manager, entries[index].lpServiceName, stopped))
                {
                    CloseServiceHandle(service);
                    return false;
                }
            }
        }

        SERVICE_STATUS ignored{};
        const bool requested = ControlService(service, SERVICE_CONTROL_STOP, &ignored) ||
            GetLastError() == ERROR_SERVICE_NOT_ACTIVE;
        const bool stoppedOk = requested && WaitForServiceState(service,
            SERVICE_STOPPED, 20000);
        CloseServiceHandle(service);
        if (stoppedOk) stopped.push_back(name);
        return stoppedOk;
    }

    bool StartStoppedServices(SC_HANDLE manager,
        const std::vector<std::wstring>& stopped)
    {
        bool ok = true;
        for (auto item = stopped.rbegin(); item != stopped.rend(); ++item)
        {
            SC_HANDLE service = OpenServiceW(manager, item->c_str(),
                SERVICE_QUERY_STATUS | SERVICE_START);
            if (!service)
            {
                ok = false;
                continue;
            }
            const bool started = StartServiceW(service, 0, nullptr) ||
                GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;
            ok = (started && WaitForServiceState(service,
                SERVICE_RUNNING, 20000)) && ok;
            CloseServiceHandle(service);
        }
        return ok;
    }

    bool EnsureServiceRunning(SC_HANDLE manager, const wchar_t* name)
    {
        SC_HANDLE service = OpenServiceW(manager, name,
            SERVICE_QUERY_STATUS | SERVICE_START);
        if (!service) return false;
        SERVICE_STATUS_PROCESS current{};
        DWORD bytes = 0;
        const bool queried = QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
            reinterpret_cast<BYTE*>(&current), sizeof(current), &bytes) != FALSE;
        const bool started = queried && current.dwCurrentState == SERVICE_RUNNING
            ? true : (StartServiceW(service, 0, nullptr) ||
                GetLastError() == ERROR_SERVICE_ALREADY_RUNNING);
        const bool running = started && WaitForServiceState(service,
            SERVICE_RUNNING, 20000);
        CloseServiceHandle(service);
        return running;
    }

    void DisarmRecovery()
    {
        HKEY key{};
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRunOncePath, 0,
            KEY_SET_VALUE | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS)
        {
            RegDeleteValueW(key, kRecoveryValue);
            RegCloseKey(key);
        }
    }

    bool ArmRecovery()
    {
        wchar_t executable[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
        if (length == 0 || length >= ARRAYSIZE(executable)) return false;

        const std::wstring command = L"\"" + std::wstring(executable) + L"\" recover";
        HKEY key{};
        DWORD disposition{};
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kRunOncePath, 0, nullptr, 0,
            KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, &disposition) != ERROR_SUCCESS)
        {
            return false;
        }

        const LONG result = RegSetValueExW(key, kRecoveryValue, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(command.c_str()),
            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
        return result == ERROR_SUCCESS;
    }

    int Uninstall();

    int Install(const wchar_t* infArgument)
    {
        const int migrationResult = HXAudioApoManager::MigrateDspSettings();
        if (migrationResult != 0) return migrationResult;

        if (!ArmRecovery())
        {
            std::wcerr << L"Unable to arm driver recovery: " << LastErrorText();
            return 7;
        }

        wchar_t fullInf[MAX_PATH]{};
        if (!GetFullPathNameW(infArgument, ARRAYSIZE(fullInf), fullInf, nullptr))
        {
            std::wcerr << L"Invalid INF path: " << LastErrorText();
            DisarmRecovery();
            return 3;
        }

        const std::wstring requestedVersion = InfDriverVersion(fullInf);
        if (HealthyDeviceUsesVersion(requestedVersion))
        {
            DisarmRecovery();
            SetInstallationMode(L"Virtual");
            std::wcout << L"Installed; existing healthy driver version retained.";
            return 0;
        }

        wchar_t published[MAX_PATH]{};
        DWORD required{};
        wchar_t existingStorePath[MAX_PATH]{};
        bool packageAlreadyStaged = SetupGetInfDriverStoreLocationW(fullInf,
            nullptr, nullptr, existingStorePath, ARRAYSIZE(existingStorePath),
            &required) != FALSE;
        required = 0;
        if (!SetupCopyOEMInfW(fullInf, nullptr, SPOST_PATH, 0, published,
            ARRAYSIZE(published), &required, nullptr))
        {
            const DWORD copyError = GetLastError();
            if (copyError == ERROR_FILE_EXISTS)
            {
                packageAlreadyStaged = true;
            }
            else
            {
                std::wcerr << L"Unable to stage driver package: " << LastErrorText(copyError);
                DisarmRecovery();
                return 4;
            }
        }
        if (*published) SavePublishedInf(published);
        if (*published && HealthyDeviceUsesInf(published))
            packageAlreadyStaged = true;

        // SetupCopyOEMInf identifies driver packages by content. If this exact
        // package is already staged and its device is running, an overlay
        // install only needs to update user-mode files. Forcing a PnP update in
        // this state tears down a live WaveRT graph, can be vetoed by audiodg,
        // and turns an otherwise healthy reinstall into a pending-removal
        // device that requires a reboot.
        if (packageAlreadyStaged && HealthyDeviceExists())
        {
            DisarmRecovery();
            SetInstallationMode(L"Virtual");
            std::wcout << L"Installed; existing healthy driver retained.";
            return 0;
        }

        if (!DeviceExists() && !CreateRootDevice())
        {
            std::wcerr << L"Unable to create root audio device: " << LastErrorText();
            DisarmRecovery();
            return 5;
        }

        BOOL reboot = FALSE;
        if (!UpdateDriverForPlugAndPlayDevicesW(nullptr, kHardwareId, fullInf, INSTALLFLAG_FORCE, &reboot))
        {
            std::wcerr << L"Unable to bind virtual audio driver: " << LastErrorText();
            DisarmRecovery();
            return 6;
        }

        // A successful SetupAPI return only means that the transaction was
        // accepted. It may still have been vetoed and marked for reboot. Never
        // advertise a usable backend until the devnode is actually DN_STARTED.
        if (!WaitForHealthyDevice(5000))
        {
            if (!reboot)
            {
                RestartMatchingDevices();
                WaitForHealthyDevice(5000);
            }
            if (!HealthyDeviceExists())
            {
                std::wcerr << (reboot
                    ? L"Driver update is pending a Windows restart; the existing device was preserved."
                    : L"Virtual audio device did not reach the started state; the driver package was preserved for recovery.");
                DisarmRecovery();
                return reboot ? 9 : 10;
            }
        }
        DisarmRecovery();
        SetInstallationMode(L"Virtual");
        std::wcout << (reboot ? L"Installed; reboot required." : L"Installed.");
        // The standard installer owns reboot/resume.  Keep this helper's
        // success code stable so Inno Setup does not misreport 3010 as a
        // failed driver operation.
        return 0;
    }

    int Uninstall()
    {
        bool ok = HXAudioEngineBypass::UninstallService() == 0;
        bool removalRequiresRestart = false;

        HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES);
        if (set != INVALID_HANDLE_VALUE)
        {
            SP_DEVINFO_DATA device{ sizeof(device) };
            for (DWORD index = 0; SetupDiEnumDeviceInfo(set, index, &device); ++index)
            {
                if (!HardwareIdsContain(set, device, kHardwareId)) continue;
                SP_REMOVEDEVICE_PARAMS remove{};
                remove.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
                remove.ClassInstallHeader.InstallFunction = DIF_REMOVE;
                remove.Scope = DI_REMOVEDEVICE_GLOBAL;
                remove.HwProfile = 0;
                if (!SetupDiSetClassInstallParamsW(set, &device,
                    &remove.ClassInstallHeader, sizeof(remove)) ||
                    !SetupDiCallClassInstaller(DIF_REMOVE, set, &device))
                {
                    ok = false;
                    continue;
                }
                SP_DEVINSTALL_PARAMS_W parameters{ sizeof(parameters) };
                if (SetupDiGetDeviceInstallParamsW(set, &device, &parameters) &&
                    (parameters.Flags & (DI_NEEDREBOOT | DI_NEEDRESTART)) != 0)
                    removalRequiresRestart = true;
            }
            SetupDiDestroyDeviceInfoList(set);
        }

        else ok = false;

        // A vetoed/pending removal still owns the kernel image even when the
        // devnode briefly disappears from a present-only enumeration. Deleting
        // the package here leaves Windows with neither a working old device nor
        // a startable new one. Preserve the package and report the transaction
        // as incomplete; a later uninstall can finish after Windows releases it.
        if (!ok || removalRequiresRestart || DeviceExists())
        {
            // Keep recovery scheduling and PublishedInf until removal succeeds.
            SetLastError(ERROR_BUSY);
            std::wcerr << L"Virtual audio device is still in use; driver package was preserved.";
            return 8;
        }

        std::wstring published = ReadPublishedInf();
        if (!published.empty())
        {
            const size_t slash = published.find_last_of(L"\\/");
            if (slash != std::wstring::npos) published.erase(0, slash + 1);
            if (!SetupUninstallOEMInfW(published.c_str(), SUOI_FORCEDELETE, nullptr) &&
                GetLastError() != ERROR_FILE_NOT_FOUND)
                ok = false;
        }
        if (!ok) {
            std::wcerr << L"Driver package removal failed; settings and recovery data preserved.";
            return 8;
        }

        // Preserve user DSP settings and the parent key ACL across an in-place
        // mode switch. The full application uninstaller uses the explicit
        // purge command after both audio backends have been removed.
        HKEY settings{};
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRegistryPath, 0,
            KEY_SET_VALUE | KEY_WOW64_64KEY, &settings) == ERROR_SUCCESS)
        {
            RegDeleteValueW(settings, L"PublishedInf");
            RegCloseKey(settings);
        }
        DisarmRecovery();
        if (!ok)
        {
            std::wcerr << L"Unable to completely remove the HXAudio virtual audio backend: "
                << LastErrorText();
            return 8;
        }
        std::wcout << L"Uninstalled.";
        return 0;
    }

    int PrepareApoInstall()
    {
        // The guard executable lives beside the control panel and must not be
        // left running while Inno replaces that directory.
        const int guardResult = HXAudioApoManager::PrepareForUpgrade();
        if (guardResult != 0) return guardResult;

        const int bypassResult = HXAudioEngineBypass::StopServiceForUpgrade();
        if (bypassResult != 0) return bypassResult;

        // A pure APO upgrade has no virtual devnode or package to release.
        // Avoid interrupting audio in that common path.
        if (!DeviceExists() && ReadPublishedInf().empty()) return 0;

        SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!manager)
        {
            std::wcerr << L"Unable to open the Windows service manager for the APO transition: "
                << LastErrorText();
            return 80;
        }

        std::vector<std::wstring> stopped;
        bool audioStopped = StopServiceTree(manager, L"AudioSrv", stopped);
        if (!audioStopped)
        {
            // EndpointBuilder is the compatibility fallback for audio stacks
            // whose AudioSrv dependency tree cannot be stopped independently.
            if (!stopped.empty()) StartStoppedServices(manager, stopped);
            stopped.clear();
            audioStopped = StopServiceTree(manager, L"AudioEndpointBuilder", stopped);
        }
        if (!audioStopped)
        {
            EnsureServiceRunning(manager, L"AudioEndpointBuilder");
            EnsureServiceRunning(manager, L"AudioSrv");
            CloseServiceHandle(manager);
            std::wcerr << L"Unable to pause the Windows audio engine; the virtual backend was preserved.";
            return 81;
        }

        int removalResult = 8;
        for (DWORD attempt = 0; attempt < 3; ++attempt)
        {
            removalResult = Uninstall();
            if (removalResult == 0) break;
            DEVINST root = 0;
            if (CM_Locate_DevNodeW(&root, nullptr, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS)
                CM_Reenumerate_DevNode(root, CM_REENUMERATE_SYNCHRONOUS);
            Sleep(300);
        }

        const bool restarted = StartStoppedServices(manager, stopped);
        const bool builderRunning = EnsureServiceRunning(manager, L"AudioEndpointBuilder");
        const bool audioRunning = builderRunning && EnsureServiceRunning(manager, L"AudioSrv");
        CloseServiceHandle(manager);

        if (!restarted || !audioRunning)
        {
            std::wcerr << L"The virtual backend was processed, but the Windows audio service did not recover.";
            return 82;
        }
        if (removalResult != 0 || DeviceExists())
        {
            std::wcerr << L"The virtual backend could not be removed without a restart; APO installation was not started.";
            return removalResult != 0 ? removalResult : 83;
        }

        std::wcout << L"Virtual backend removed; Windows audio services restored.";
        return 0;
    }

    int PolicyStatus()
    {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        auto query = reinterpret_cast<NtQuerySystemInformationFn>(GetProcAddress(ntdll, "NtQuerySystemInformation"));
        if (!query) return 12;
        SYSTEM_CODEINTEGRITY_INFORMATION information{ sizeof(information), 0 };
        const LONG status = query(kSystemCodeIntegrityInformation, &information, sizeof(information), nullptr);
        if (status < 0) return 12;
        return (information.CodeIntegrityOptions & kCodeIntegrityTestSign) != 0 ? 0 : 10;
    }

    int DriverStatus()
    {
        const std::vector<DeviceState> states = MatchingDeviceStates();
        if (states.empty())
        {
            std::wcout << L"absent";
            return 11;
        }
        for (const DeviceState& state : states)
        {
            if (state.started)
            {
                std::wcout << L"started";
                return 0;
            }
        }
        std::wcout << L"present-not-started; problem=" << states.front().problem;
        return 10;
    }
}

int wmain(int argc, wchar_t** argv)
{
    if (argc >= 2 && _wcsicmp(argv[1], L"apo-service") == 0)
        return HXAudioApoManager::RunServiceDispatcher();
    if (argc >= 2 && _wcsicmp(argv[1], L"engine-bypass-service") == 0)
        return HXAudioEngineBypass::RunServiceDispatcher();
    if (argc >= 2 && _wcsicmp(argv[1], L"engine-bypass-probe") == 0)
        return HXAudioEngineBypass::Probe();
    if (argc >= 2 && _wcsicmp(argv[1], L"apo-diagnostic-self-test") == 0)
        return HXAudioApoManager::DiagnosticSelfTest();
    if (argc >= 3 && _wcsicmp(argv[1], L"apo-self-test") == 0)
        return HXAudioApoManager::SelfTest(argv[2]);

    if (!IsAdministrator())
    {
        std::wcerr << L"Administrator privileges are required.";
        return 2;
    }
    if (argc >= 3 && _wcsicmp(argv[1], L"install") == 0) return Install(argv[2]);
    if (argc >= 2 && _wcsicmp(argv[1], L"uninstall") == 0) return Uninstall();
    // Refuse before PrepareApoInstall can remove the virtual backend or stop
    // services. The legacy APO path cannot preserve OEM audio components yet.
    if (argc >= 2 && (_wcsicmp(argv[1], L"apo-install") == 0 ||
        _wcsicmp(argv[1], L"apo-install-effects") == 0 ||
        _wcsicmp(argv[1], L"prepare-apo-install") == 0 ||
        _wcsicmp(argv[1], L"apo-refresh") == 0))
    {
        std::wcerr << L"APO installation and refresh disabled: OEM audio coexistence is unverified.";
        return 58;
    }
    if (argc >= 2 && _wcsicmp(argv[1], L"apo-uninstall") == 0)
        return HXAudioApoManager::Uninstall();
    if (argc >= 2 && _wcsicmp(argv[1], L"apo-status") == 0)
        return HXAudioApoManager::VerifyInstalled();
    if (argc >= 2 && _wcsicmp(argv[1], L"apo-status-effects") == 0)
        return HXAudioApoManager::VerifyVirtualEffectsInstalled();
    if (argc >= 2 && _wcsicmp(argv[1], L"apo-live-test") == 0)
        return HXAudioApoManager::LiveTest();
    if (argc >= 2 && _wcsicmp(argv[1], L"purge") == 0)
    {
        return HXAudioSafety::Purge(
            [] { return HXAudioApoManager::Uninstall(); },
            [] { return Uninstall(); },
            [] {
                // Backups and settings are retained deliberately: deleting
                // across hives is not atomic and recovery data must survive.
                std::wcout << L"Audio backends cleaned; HXAudio settings and recovery backups retained.";
                return 0;
            });
    }
    if (argc >= 2 && _wcsicmp(argv[1], L"recover") == 0) return Uninstall();
    if (argc >= 2 && _wcsicmp(argv[1], L"policy") == 0) return PolicyStatus();
    if (argc >= 2 && _wcsicmp(argv[1], L"driver-status") == 0) return DriverStatus();
    if (argc >= 2 && _wcsicmp(argv[1], L"engine-bypass-install") == 0)
        return HXAudioEngineBypass::InstallService();
    if (argc >= 2 && _wcsicmp(argv[1], L"engine-bypass-uninstall") == 0)
        return HXAudioEngineBypass::UninstallService();
    std::wcerr << L"Usage: HXAudioDriverSetup install <inf> | uninstall | recover | policy | driver-status | "
        L"prepare-apo-install | apo-install <dll> | apo-install-effects <dll> | apo-uninstall | apo-refresh | apo-status | apo-status-effects | apo-live-test | apo-self-test <dll> | purge | "
        L"engine-bypass-install | engine-bypass-uninstall | engine-bypass-probe";
    return 1;
}
