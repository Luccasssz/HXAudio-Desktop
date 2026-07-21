#include "EngineBypass.h"

#include <windows.h>
#include <tlhelp32.h>
#include <objbase.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace HXAudioEngineBypass
{
    extern "C" const BYTE HXAudioLimiterUnorderedBranchOpcode;
    extern "C" const BYTE HXAudioLimiterUnequalBranchOpcode;
    extern "C" const BYTE HXAudioLimiterJumpOpcode;
    extern "C" const BYTE HXAudioLimiterNopOpcode;

    namespace
    {
        constexpr wchar_t kServiceName[] = L"HXAudioEngineBypass";
        constexpr wchar_t kServiceDisplayName[] = L"HXAudio Shared Engine AudioLimiter Bypass";
        constexpr wchar_t kRegistryPath[] = L"Software\\HXAudio\\VirtualAudio";
        constexpr SIZE_T kApoScanLength = 0x700;
        constexpr SIZE_T kLimiterScanLength = 0x180;
        constexpr SIZE_T kContextBefore = 24;
        constexpr SIZE_T kContextLength = 80;
        constexpr SIZE_T kPatchLength = 4;

        const CLSID kAudioLimiterClsid =
            { 0xd69e0717, 0xdd4b, 0x4b25, { 0x99, 0x7a, 0xda, 0x81, 0x38, 0x33, 0xb8, 0xac } };
        const IID kAudioProcessingObjectRtIid =
            { 0x9e1d6a6d, 0xddbc, 0x4e95, { 0xa4, 0xc7, 0xad, 0x64, 0xba, 0x37, 0x84, 0x6c } };

        struct PatchDescriptor
        {
            std::wstring modulePath;
            SIZE_T apoProcessRva = 0;
            SIZE_T limiterFunctionRva = 0;
            SIZE_T patchRva = 0;
            SIZE_T contextRva = 0;
            std::array<BYTE, kContextLength> context{};
            std::array<BYTE, kPatchLength> original{};
            std::array<BYTE, kPatchLength> bypass{};
        };

        SERVICE_STATUS_HANDLE g_serviceStatusHandle = nullptr;
        SERVICE_STATUS g_serviceStatus{};
        HANDLE g_stopEvent = nullptr;

        bool EnablePrivilege(const wchar_t* privilegeName)
        {
            HANDLE token = nullptr;
            if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
                return false;
            LUID luid{};
            if (!LookupPrivilegeValueW(nullptr, privilegeName, &luid))
            {
                CloseHandle(token);
                return false;
            }
            TOKEN_PRIVILEGES privileges{};
            privileges.PrivilegeCount = 1;
            privileges.Privileges[0].Luid = luid;
            privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            SetLastError(ERROR_SUCCESS);
            const bool adjusted = AdjustTokenPrivileges(token, FALSE, &privileges,
                sizeof(privileges), nullptr, nullptr) != FALSE && GetLastError() == ERROR_SUCCESS;
            CloseHandle(token);
            return adjusted;
        }

        std::wstring LastErrorText(DWORD error = GetLastError())
        {
            wchar_t* buffer = nullptr;
            FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0,
                reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
            std::wstring text = buffer ? buffer : L"Unknown error";
            if (buffer) LocalFree(buffer);
            while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n')) text.pop_back();
            return text;
        }

        std::wstring BaseName(const std::wstring& path)
        {
            const size_t slash = path.find_last_of(L"\\/");
            return slash == std::wstring::npos ? path : path.substr(slash + 1);
        }

        SIZE_T ModuleImageSize(HMODULE module)
        {
            if (!module) return 0;
            const auto* base = reinterpret_cast<const BYTE*>(module);
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return 0;
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE ||
                nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return 0;
            return nt->OptionalHeader.SizeOfImage;
        }

        BYTE* DirectCallTarget(BYTE* instruction)
        {
            int32_t displacement = 0;
            CopyMemory(&displacement, instruction + 1, sizeof(displacement));
            return instruction + 5 + displacement;
        }

        bool FindLimiterFastCopyBranch(BYTE* function, SIZE_T available,
            SIZE_T& patchOffset, SIZE_T& fastCopyOffset)
        {
            const SIZE_T scan = std::min(available, kLimiterScanLength);
            for (SIZE_T offset = 8; offset + 16 < scan; ++offset)
            {
                if (function[offset] != HXAudioLimiterUnorderedBranchOpcode ||
                    function[offset + 2] != HXAudioLimiterUnequalBranchOpcode)
                    continue;

                const ptrdiff_t slowFromUnordered = static_cast<ptrdiff_t>(offset + 2) +
                    static_cast<int8_t>(function[offset + 1]);
                const ptrdiff_t slowFromUnequal = static_cast<ptrdiff_t>(offset + 4) +
                    static_cast<int8_t>(function[offset + 3]);
                if (slowFromUnordered != slowFromUnequal ||
                    slowFromUnordered <= static_cast<ptrdiff_t>(offset + 8) ||
                    slowFromUnordered >= static_cast<ptrdiff_t>(scan))
                    continue;

                // The two floating-point result branches immediately follow a
                // UCOMISD. This excludes ordinary integer condition chains.
                bool hasFloatingCompare = false;
                for (SIZE_T index = offset >= 8 ? offset - 8 : 0;
                    index + 1 < offset; ++index)
                {
                    if (function[index] == 0x0F && function[index + 1] == 0x2E)
                    {
                        hasFloatingCompare = true;
                        break;
                    }
                }
                if (!hasFloatingCompare) continue;

                SIZE_T stateBranch = 0;
                for (SIZE_T index = offset + 4;
                    index + 1 < std::min<SIZE_T>(offset + 20, scan); ++index)
                {
                    if (function[index] != HXAudioLimiterUnequalBranchOpcode) continue;
                    const ptrdiff_t target = static_cast<ptrdiff_t>(index + 2) +
                        static_cast<int8_t>(function[index + 1]);
                    if (target == slowFromUnordered)
                    {
                        stateBranch = index;
                        break;
                    }
                }
                if (stateBranch == 0) continue;

                const SIZE_T fast = stateBranch + 2;
                if (fast <= offset + 4 || slowFromUnordered <= static_cast<ptrdiff_t>(fast + 12))
                    continue;
                const ptrdiff_t bypassDisplacement = static_cast<ptrdiff_t>(fast) -
                    static_cast<ptrdiff_t>(offset + 2);
                if (bypassDisplacement <= 0 || bypassDisplacement > INT8_MAX) continue;

                bool hasCopyCall = false;
                bool hasReturn = false;
                for (SIZE_T index = fast; index < static_cast<SIZE_T>(slowFromUnordered); ++index)
                {
                    hasCopyCall = hasCopyCall || function[index] == 0xE8;
                    hasReturn = hasReturn || function[index] == 0xC3;
                }
                if (!hasCopyCall || !hasReturn) continue;

                patchOffset = offset;
                fastCopyOffset = fast;
                return true;
            }
            return false;
        }

        bool DiscoverPatch(PatchDescriptor& descriptor, std::wstring& detail)
        {
            const HRESULT initializeResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            const bool uninitialize = SUCCEEDED(initializeResult);
            if (FAILED(initializeResult) && initializeResult != RPC_E_CHANGED_MODE)
            {
                std::wstringstream stream;
                stream << L"CoInitializeEx failed: 0x" << std::hex << initializeResult;
                detail = stream.str();
                return false;
            }

            IUnknown* processingObject = nullptr;
            const HRESULT createResult = CoCreateInstance(kAudioLimiterClsid, nullptr,
                CLSCTX_INPROC_SERVER, kAudioProcessingObjectRtIid,
                reinterpret_cast<void**>(&processingObject));
            if (FAILED(createResult) || !processingObject)
            {
                std::wstringstream stream;
                stream << L"AudioLimiter activation failed: 0x" << std::hex << createResult;
                detail = stream.str();
                if (uninitialize) CoUninitialize();
                return false;
            }

            void** vtable = *reinterpret_cast<void***>(processingObject);
            BYTE* processFunction = vtable ? reinterpret_cast<BYTE*>(vtable[3]) : nullptr;
            HMODULE module = nullptr;
            if (!processFunction || !GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(processFunction), &module))
            {
                detail = L"Unable to resolve the AudioLimiter APOProcess module.";
                processingObject->Release();
                if (uninitialize) CoUninitialize();
                return false;
            }

            wchar_t modulePath[MAX_PATH]{};
            if (!GetModuleFileNameW(module, modulePath, ARRAYSIZE(modulePath)) ||
                _wcsicmp(BaseName(modulePath).c_str(), L"audioeng.dll") != 0)
            {
                detail = L"AudioLimiter did not resolve to the Windows audioeng.dll module.";
                processingObject->Release();
                if (uninitialize) CoUninitialize();
                return false;
            }

            const SIZE_T imageSize = ModuleImageSize(module);
            BYTE* moduleBase = reinterpret_cast<BYTE*>(module);
            if (imageSize == 0 || processFunction < moduleBase ||
                processFunction >= moduleBase + imageSize)
            {
                detail = L"The active audioeng.dll image layout is invalid.";
                processingObject->Release();
                if (uninitialize) CoUninitialize();
                return false;
            }

            const SIZE_T apoAvailable = static_cast<SIZE_T>(moduleBase + imageSize - processFunction);
            const SIZE_T apoScan = std::min(apoAvailable, kApoScanLength);
            std::vector<BYTE*> callTargets;
            for (SIZE_T offset = 0; offset + 5 <= apoScan; ++offset)
            {
                if (processFunction[offset] != 0xE8) continue;
                BYTE* target = DirectCallTarget(processFunction + offset);
                if (target < moduleBase || target >= moduleBase + imageSize) continue;
                if (std::find(callTargets.begin(), callTargets.end(), target) == callTargets.end())
                    callTargets.push_back(target);
            }

            struct Candidate
            {
                BYTE* function = nullptr;
                SIZE_T patchOffset = 0;
                SIZE_T fastCopyOffset = 0;
            };
            std::vector<Candidate> candidates;
            for (BYTE* target : callTargets)
            {
                SIZE_T patchOffset = 0;
                SIZE_T fastCopyOffset = 0;
                const SIZE_T available = static_cast<SIZE_T>(moduleBase + imageSize - target);
                if (FindLimiterFastCopyBranch(target, available,
                    patchOffset, fastCopyOffset))
                    candidates.push_back({ target, patchOffset, fastCopyOffset });
            }

            if (candidates.size() != 1)
            {
                std::wstringstream stream;
                stream << L"AudioLimiter fast-copy control-flow signature count was "
                    << candidates.size() << L"; refusing an ambiguous patch.";
                detail = stream.str();
                processingObject->Release();
                if (uninitialize) CoUninitialize();
                return false;
            }

            const Candidate candidate = candidates.front();
            BYTE* patchAddress = candidate.function + candidate.patchOffset;
            if (patchAddress < moduleBase + kContextBefore ||
                patchAddress - kContextBefore + kContextLength > moduleBase + imageSize)
            {
                detail = L"AudioLimiter patch context falls outside audioeng.dll.";
                processingObject->Release();
                if (uninitialize) CoUninitialize();
                return false;
            }

            descriptor.modulePath = modulePath;
            descriptor.apoProcessRva = static_cast<SIZE_T>(processFunction - moduleBase);
            descriptor.limiterFunctionRva = static_cast<SIZE_T>(candidate.function - moduleBase);
            descriptor.patchRva = static_cast<SIZE_T>(patchAddress - moduleBase);
            descriptor.contextRva = descriptor.patchRva - kContextBefore;
            CopyMemory(descriptor.context.data(), moduleBase + descriptor.contextRva,
                descriptor.context.size());
            CopyMemory(descriptor.original.data(), patchAddress, descriptor.original.size());
            const ptrdiff_t bypassDisplacement = static_cast<ptrdiff_t>(candidate.fastCopyOffset) -
                static_cast<ptrdiff_t>(candidate.patchOffset + 2);
            descriptor.bypass = {
                HXAudioLimiterJumpOpcode,
                static_cast<BYTE>(static_cast<int8_t>(bypassDisplacement)),
                HXAudioLimiterNopOpcode,
                HXAudioLimiterNopOpcode
            };

            processingObject->Release();
            if (uninitialize) CoUninitialize();
            detail = L"AudioLimiter slow-processing branch resolved; the original fast-copy path is validated.";
            return true;
        }

        bool FindRemoteModule(DWORD processId, const wchar_t* moduleName,
            BYTE*& baseAddress, std::wstring& modulePath)
        {
            HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId);
            if (snapshot == INVALID_HANDLE_VALUE) return false;

            MODULEENTRY32W module{ sizeof(module) };
            bool found = false;
            if (Module32FirstW(snapshot, &module))
            {
                do
                {
                    if (_wcsicmp(module.szModule, moduleName) == 0)
                    {
                        baseAddress = module.modBaseAddr;
                        modulePath = module.szExePath;
                        found = true;
                        break;
                    }
                } while (Module32NextW(snapshot, &module));
            }
            CloseHandle(snapshot);
            return found;
        }

        bool PatchProcess(DWORD processId, const PatchDescriptor& descriptor,
            bool enable, std::wstring& detail)
        {
            BYTE* remoteModule = nullptr;
            std::wstring remoteModulePath;
            if (!FindRemoteModule(processId, L"audioeng.dll", remoteModule, remoteModulePath))
            {
                detail = L"audioeng.dll is not loaded yet.";
                return false;
            }
            if (_wcsicmp(remoteModulePath.c_str(), descriptor.modulePath.c_str()) != 0)
            {
                detail = L"The remote audioeng.dll path does not match the locally resolved system module.";
                return false;
            }

            HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ |
                PROCESS_VM_WRITE | PROCESS_VM_OPERATION, FALSE, processId);
            if (!process)
            {
                detail = L"OpenProcess failed: " + LastErrorText();
                return false;
            }

            BYTE* remoteContext = remoteModule + descriptor.contextRva;
            BYTE* remotePatch = remoteModule + descriptor.patchRva;
            std::array<BYTE, kContextLength> current{};
            SIZE_T bytesRead = 0;
            bool valid = ReadProcessMemory(process, remoteContext, current.data(),
                current.size(), &bytesRead) != FALSE && bytesRead == current.size();

            const SIZE_T patchIndex = descriptor.patchRva - descriptor.contextRva;
            if (valid)
            {
                for (SIZE_T index = 0; index < current.size(); ++index)
                {
                    if (index >= patchIndex && index < patchIndex + kPatchLength)
                    {
                        continue;
                    }
                    else if (current[index] != descriptor.context[index])
                    {
                        valid = false;
                        break;
                    }
                }
            }

            if (!valid)
            {
                detail = L"The remote AudioLimiter machine-code context did not match; no write was attempted.";
                CloseHandle(process);
                return false;
            }

            std::array<BYTE, kPatchLength> currentPatch{};
            CopyMemory(currentPatch.data(), current.data() + patchIndex,
                currentPatch.size());
            const auto& wanted = enable ? descriptor.bypass : descriptor.original;
            const bool knownCurrent = currentPatch == descriptor.original ||
                currentPatch == descriptor.bypass;
            if (!knownCurrent)
            {
                detail = L"The remote AudioLimiter branch bytes are neither original nor HXAudio's verified bypass.";
                CloseHandle(process);
                return false;
            }
            if (currentPatch == wanted)
            {
                detail = enable ? L"Already bypassed." : L"Already restored.";
                CloseHandle(process);
                return true;
            }

            DWORD oldProtection = 0;
            if (!VirtualProtectEx(process, remotePatch, kPatchLength,
                PAGE_EXECUTE_READWRITE, &oldProtection))
            {
                detail = L"VirtualProtectEx failed: " + LastErrorText();
                CloseHandle(process);
                return false;
            }

            SIZE_T bytesWritten = 0;
            const bool wrote = WriteProcessMemory(process, remotePatch,
                wanted.data(), wanted.size(), &bytesWritten) != FALSE &&
                bytesWritten == wanted.size();
            const DWORD writeError = wrote ? ERROR_SUCCESS : GetLastError();
            FlushInstructionCache(process, remotePatch, kPatchLength);
            DWORD ignoredProtection = 0;
            VirtualProtectEx(process, remotePatch, kPatchLength,
                oldProtection, &ignoredProtection);

            std::array<BYTE, kPatchLength> verified{};
            SIZE_T verifyRead = 0;
            const bool verifiedWrite = wrote && ReadProcessMemory(process,
                remotePatch, verified.data(), verified.size(), &verifyRead) != FALSE &&
                verifyRead == verified.size() && verified == wanted;
            CloseHandle(process);

            if (!verifiedWrite)
            {
                detail = L"The AudioLimiter machine-code write could not be verified: " +
                    LastErrorText(writeError == ERROR_SUCCESS ? ERROR_WRITE_FAULT : writeError);
                return false;
            }

            detail = enable ? L"AudioLimiter slow path bypassed through its fast-copy path."
                : L"AudioLimiter original branches restored.";
            return true;
        }

        std::vector<DWORD> AudioDeviceGraphProcesses()
        {
            std::vector<DWORD> result;
            HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snapshot == INVALID_HANDLE_VALUE) return result;
            PROCESSENTRY32W process{ sizeof(process) };
            if (Process32FirstW(snapshot, &process))
            {
                do
                {
                    if (_wcsicmp(process.szExeFile, L"audiodg.exe") == 0)
                    {
                        result.push_back(process.th32ProcessID);
                    }
                } while (Process32NextW(snapshot, &process));
            }
            CloseHandle(snapshot);
            return result;
        }

        struct PatchSummary
        {
            DWORD targets = 0;
            DWORD successful = 0;
        };

        PatchSummary ApplyToAllAudioGraphs(const PatchDescriptor& descriptor,
            bool enable, std::wstring& lastDetail)
        {
            PatchSummary summary;
            const auto processes = AudioDeviceGraphProcesses();
            summary.targets = static_cast<DWORD>(processes.size());
            for (const DWORD processId : processes)
            {
                std::wstring detail;
                if (PatchProcess(processId, descriptor, enable, detail))
                    ++summary.successful;
                else lastDetail = L"audiodg.exe PID " + std::to_wstring(processId) + L": " + detail;
            }
            return summary;
        }

        void WriteRegistryDword(const wchar_t* name, DWORD value)
        {
            HKEY key{};
            DWORD disposition = 0;
            if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kRegistryPath, 0, nullptr, 0,
                KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, &disposition) == ERROR_SUCCESS)
            {
                RegSetValueExW(key, name, 0, REG_DWORD,
                    reinterpret_cast<const BYTE*>(&value), sizeof(value));
                RegCloseKey(key);
            }
        }

        void WriteRegistryString(const wchar_t* name, const std::wstring& value)
        {
            HKEY key{};
            DWORD disposition = 0;
            if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kRegistryPath, 0, nullptr, 0,
                KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, &disposition) == ERROR_SUCCESS)
            {
                RegSetValueExW(key, name, 0, REG_SZ,
                    reinterpret_cast<const BYTE*>(value.c_str()),
                    static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
                RegCloseKey(key);
            }
        }

        void WriteStatus(DWORD state, DWORD patchedProcesses, const std::wstring& detail,
            const PatchDescriptor* descriptor = nullptr, DWORD targetProcesses = 0)
        {
            WriteRegistryDword(L"EngineBypassState", state);
            WriteRegistryDword(L"EngineBypassPatchedProcesses", patchedProcesses);
            WriteRegistryDword(L"EngineBypassTargetProcesses", targetProcesses);
            WriteRegistryString(L"EngineBypassDetail", detail);
            if (descriptor)
            {
                WriteRegistryString(L"EngineBypassAudioEng", descriptor->modulePath);
                WriteRegistryDword(L"EngineBypassFunctionRva",
                    static_cast<DWORD>(descriptor->apoProcessRva));
                WriteRegistryDword(L"EngineBypassLimiterRva",
                    static_cast<DWORD>(descriptor->limiterFunctionRva));
                WriteRegistryDword(L"EngineBypassPatchRva",
                    static_cast<DWORD>(descriptor->patchRva));
            }
        }

        void SetServiceState(DWORD state, DWORD win32ExitCode = NO_ERROR, DWORD waitHint = 0)
        {
            g_serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
            g_serviceStatus.dwCurrentState = state;
            g_serviceStatus.dwWin32ExitCode = win32ExitCode;
            g_serviceStatus.dwWaitHint = waitHint;
            g_serviceStatus.dwControlsAccepted = state == SERVICE_RUNNING
                ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
            g_serviceStatus.dwCheckPoint = (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING)
                ? g_serviceStatus.dwCheckPoint + 1 : 0;
            if (g_serviceStatusHandle) SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);
        }

        DWORD WINAPI ServiceHandler(DWORD control, DWORD, void*, void*)
        {
            if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN)
            {
                SetServiceState(SERVICE_STOP_PENDING, NO_ERROR, 5000);
                if (g_stopEvent) SetEvent(g_stopEvent);
            }
            return NO_ERROR;
        }

        void WINAPI ServiceMain(DWORD, wchar_t**)
        {
            g_serviceStatusHandle = RegisterServiceCtrlHandlerExW(kServiceName, ServiceHandler, nullptr);
            if (!g_serviceStatusHandle) return;
            SetServiceState(SERVICE_START_PENDING, NO_ERROR, 5000);

            g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!g_stopEvent)
            {
                SetServiceState(SERVICE_STOPPED, GetLastError());
                return;
            }

            PatchDescriptor descriptor;
            std::wstring detail;
            bool discovered = false;
            const ULONGLONG discoveryDeadline = GetTickCount64() + 30000;
            do
            {
                if (DiscoverPatch(descriptor, detail))
                {
                    discovered = true;
                    break;
                }
                SetServiceState(SERVICE_START_PENDING, NO_ERROR, 30000);
                if (WaitForSingleObject(g_stopEvent, 250) != WAIT_TIMEOUT) break;
            } while (GetTickCount64() < discoveryDeadline);
            if (!discovered)
            {
                WriteStatus(3, 0, detail);
                SetServiceState(SERVICE_STOPPED, ERROR_NOT_SUPPORTED);
                CloseHandle(g_stopEvent);
                g_stopEvent = nullptr;
                return;
            }

            EnablePrivilege(SE_DEBUG_NAME);
            SetServiceState(SERVICE_RUNNING);
            DWORD previousPatched = MAXDWORD;
            DWORD previousTargets = MAXDWORD;
            std::wstring previousDetail;
            for (;;)
            {
                std::wstring patchDetail;
                const PatchSummary patched = ApplyToAllAudioGraphs(descriptor,
                    true, patchDetail);
                const bool complete = patched.targets != 0 &&
                    patched.successful == patched.targets;
                const std::wstring currentDetail = complete
                    ? L"Every active Windows Shared Audio Engine graph has the real AudioLimiter slow path bypassed and verified."
                    : (patched.targets == 0
                        ? L"Waiting for an active audiodg.exe graph."
                        : (patchDetail.empty()
                            ? L"Not every active audiodg.exe graph was patched."
                            : patchDetail));
                if (patched.successful != previousPatched ||
                    patched.targets != previousTargets ||
                    currentDetail != previousDetail)
                {
                    WriteStatus(complete ? 1 : (patched.targets ? 3 : 2),
                        patched.successful, currentDetail, &descriptor,
                        patched.targets);
                    previousPatched = patched.successful;
                    previousTargets = patched.targets;
                    previousDetail = currentDetail;
                }
                if (WaitForSingleObject(g_stopEvent, 750) != WAIT_TIMEOUT) break;
            }

            std::wstring restoreDetail;
            const PatchSummary restored = ApplyToAllAudioGraphs(descriptor,
                false, restoreDetail);
            WriteStatus(0, 0, restored.successful != 0
                ? L"Windows Shared Audio Engine limiter behavior restored."
                : (restoreDetail.empty() ? L"Engine bypass service stopped." : restoreDetail),
                &descriptor, restored.targets);
            CloseHandle(g_stopEvent);
            g_stopEvent = nullptr;
            SetServiceState(SERVICE_STOPPED);
        }

        bool WaitForServiceState(SC_HANDLE service, DWORD wantedState, DWORD timeoutMs)
        {
            const ULONGLONG deadline = GetTickCount64() + timeoutMs;
            SERVICE_STATUS_PROCESS status{};
            DWORD needed = 0;
            while (GetTickCount64() < deadline)
            {
                if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                    reinterpret_cast<BYTE*>(&status), sizeof(status), &needed)) return false;
                if (status.dwCurrentState == wantedState) return true;
                if (wantedState == SERVICE_RUNNING && status.dwCurrentState == SERVICE_STOPPED) return false;
                Sleep(100);
            }
            return false;
        }

        struct SilentAudioGraph
        {
            bool uninitializeCom = false;
            IMMDeviceEnumerator* enumerator = nullptr;
            IMMDeviceCollection* collection = nullptr;
            IMMDevice* device = nullptr;
            IAudioClient* client = nullptr;
            IAudioRenderClient* render = nullptr;
            WAVEFORMATEX* format = nullptr;

            ~SilentAudioGraph()
            {
                if (client) client->Stop();
                if (render) render->Release();
                if (client) client->Release();
                if (device) device->Release();
                if (collection) collection->Release();
                if (enumerator) enumerator->Release();
                if (format) CoTaskMemFree(format);
                if (uninitializeCom) CoUninitialize();
            }

            bool Start()
            {
                HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                uninitializeCom = SUCCEEDED(hr);
                if (hr == RPC_E_CHANGED_MODE) hr = S_OK;
                if (SUCCEEDED(hr)) hr = CoCreateInstance(
                    __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                    __uuidof(IMMDeviceEnumerator),
                    reinterpret_cast<void**>(&enumerator));
                if (SUCCEEDED(hr)) hr = enumerator->GetDefaultAudioEndpoint(
                    eRender, eConsole, &device);
                if (FAILED(hr) && enumerator)
                {
                    hr = enumerator->EnumAudioEndpoints(eRender,
                        DEVICE_STATE_ACTIVE, &collection);
                    UINT count = 0;
                    if (SUCCEEDED(hr)) hr = collection->GetCount(&count);
                    if (SUCCEEDED(hr) && count) hr = collection->Item(0, &device);
                    else if (SUCCEEDED(hr)) hr = E_NOTFOUND;
                }
                if (SUCCEEDED(hr)) hr = device->Activate(__uuidof(IAudioClient),
                    CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
                if (SUCCEEDED(hr)) hr = client->GetMixFormat(&format);
                if (SUCCEEDED(hr)) hr = client->Initialize(
                    AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_NOPERSIST,
                    10000000, 0, format, nullptr);
                UINT32 frames = 0;
                if (SUCCEEDED(hr)) hr = client->GetBufferSize(&frames);
                if (SUCCEEDED(hr)) hr = client->GetService(
                    __uuidof(IAudioRenderClient),
                    reinterpret_cast<void**>(&render));
                BYTE* buffer = nullptr;
                if (SUCCEEDED(hr)) hr = render->GetBuffer(frames, &buffer);
                if (SUCCEEDED(hr)) hr = render->ReleaseBuffer(frames,
                    AUDCLNT_BUFFERFLAGS_SILENT);
                if (SUCCEEDED(hr)) hr = client->Start();
                return SUCCEEDED(hr);
            }
        };
    }

    int InstallService()
    {
        wchar_t executable[MAX_PATH]{};
        if (!GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable)))
        {
            std::wcerr << L"Unable to resolve the service executable: " << LastErrorText();
            return 20;
        }
        const std::wstring command = L"\"" + std::wstring(executable) + L"\" engine-bypass-service";

        SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
        if (!manager)
        {
            std::wcerr << L"Unable to open the service manager: " << LastErrorText();
            return 21;
        }

        SC_HANDLE service = CreateServiceW(manager, kServiceName, kServiceDisplayName,
            SERVICE_CHANGE_CONFIG | SERVICE_QUERY_STATUS | SERVICE_START | SERVICE_STOP | DELETE,
            SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
            command.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
        if (!service && GetLastError() == ERROR_SERVICE_EXISTS)
        {
            service = OpenServiceW(manager, kServiceName,
                SERVICE_CHANGE_CONFIG | SERVICE_QUERY_STATUS | SERVICE_START | SERVICE_STOP | DELETE);
        }
        if (!service)
        {
            std::wcerr << L"Unable to create the engine bypass service: " << LastErrorText();
            CloseServiceHandle(manager);
            return 22;
        }

        if (!ChangeServiceConfigW(service, SERVICE_NO_CHANGE, SERVICE_AUTO_START,
            SERVICE_NO_CHANGE, command.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr,
            kServiceDisplayName))
        {
            std::wcerr << L"Unable to update the engine bypass service: " << LastErrorText();
            CloseServiceHandle(service);
            CloseServiceHandle(manager);
            return 23;
        }

        SERVICE_DELAYED_AUTO_START_INFO delayed{ FALSE };
        ChangeServiceConfig2W(service, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &delayed);
        SERVICE_DESCRIPTIONW description{
            const_cast<wchar_t*>(L"Keeps the Windows Shared Audio Engine graph intact while routing its built-in AudioLimiter through the validated unity-gain fast-copy path.")
        };
        ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &description);

        WriteStatus(2, 0, L"Starting the machine-code AudioLimiter bypass and waiting for verified audio graphs.");
        if (!StartServiceW(service, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING)
        {
            std::wcerr << L"Unable to start the engine bypass service: " << LastErrorText();
            CloseServiceHandle(service);
            CloseServiceHandle(manager);
            return 24;
        }
        if (!WaitForServiceState(service, SERVICE_RUNNING, 35000))
        {
            std::wcerr << L"The engine bypass service did not reach the running state.";
            CloseServiceHandle(service);
            CloseServiceHandle(manager);
            return 25;
        }

        // A freshly started system may not have audiodg.exe until the first
        // shared render stream exists. Keep a silent stream alive during
        // verification so service startup is deterministic on every Win10/11
        // machine, including installations performed while no app is playing.
        SilentAudioGraph verificationGraph;
        verificationGraph.Start();

        bool verified = false;
        const ULONGLONG verifyDeadline = GetTickCount64() + 15000;
        while (GetTickCount64() < verifyDeadline)
        {
            HKEY key = nullptr;
            DWORD state = 0;
            DWORD patched = 0;
            DWORD targets = 0;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRegistryPath, 0,
                KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS)
            {
                DWORD type = 0;
                DWORD size = sizeof(DWORD);
                RegQueryValueExW(key, L"EngineBypassState", nullptr, &type,
                    reinterpret_cast<BYTE*>(&state), &size);
                type = 0;
                size = sizeof(DWORD);
                RegQueryValueExW(key, L"EngineBypassPatchedProcesses", nullptr,
                    &type, reinterpret_cast<BYTE*>(&patched), &size);
                type = 0;
                size = sizeof(DWORD);
                RegQueryValueExW(key, L"EngineBypassTargetProcesses", nullptr,
                    &type, reinterpret_cast<BYTE*>(&targets), &size);
                RegCloseKey(key);
            }
            if (state == 1 && targets != 0 && patched == targets)
            {
                verified = true;
                break;
            }
            Sleep(100);
        }
        if (!verified)
        {
            SERVICE_STATUS status{};
            ControlService(service, SERVICE_CONTROL_STOP, &status);
            WaitForServiceState(service, SERVICE_STOPPED, 5000);
            std::wcerr << L"The AudioLimiter machine-code bypass was not verified in every active audiodg.exe graph.";
            CloseServiceHandle(service);
            CloseServiceHandle(manager);
            return 26;
        }

        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        std::wcout << L"Engine bypass service installed; every active audio graph is machine-code patched and verified.";
        return 0;
    }

    int StopServiceForUpgrade()
    {
        SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!manager) return 0;
        SC_HANDLE service = OpenServiceW(manager, kServiceName,
            SERVICE_QUERY_STATUS | SERVICE_STOP);
        if (!service)
        {
            CloseServiceHandle(manager);
            return GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST ? 0 : 27;
        }
        SERVICE_STATUS status{};
        const bool requested = ControlService(service, SERVICE_CONTROL_STOP,
            &status) || GetLastError() == ERROR_SERVICE_NOT_ACTIVE;
        const bool stopped = requested && WaitForServiceState(service,
            SERVICE_STOPPED, 15000);
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        if (!stopped)
        {
            std::wcerr << L"Unable to stop the engine bypass service for an in-place upgrade.";
            return 27;
        }
        std::wcout << L"Engine bypass service stopped for in-place upgrade; service identity preserved.";
        return 0;
    }

    int UninstallService()
    {
        SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!manager) return 0;
        SC_HANDLE service = OpenServiceW(manager, kServiceName,
            SERVICE_QUERY_STATUS | SERVICE_STOP | DELETE);
        if (!service)
        {
            CloseServiceHandle(manager);
            WriteStatus(0, 0, L"Engine bypass service is not installed.");
            return 0;
        }

        SERVICE_STATUS status{};
        if (ControlService(service, SERVICE_CONTROL_STOP, &status) ||
            GetLastError() == ERROR_SERVICE_NOT_ACTIVE)
        {
            WaitForServiceState(service, SERVICE_STOPPED, 10000);
        }
        const bool deleted = DeleteService(service) != FALSE || GetLastError() == ERROR_SERVICE_MARKED_FOR_DELETE;
        const DWORD deleteError = deleted ? ERROR_SUCCESS : GetLastError();
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        WriteStatus(0, 0, L"Engine bypass service removed.");
        if (!deleted)
        {
            std::wcerr << L"Unable to remove the engine bypass service: " << LastErrorText(deleteError);
            return 26;
        }
        std::wcout << L"Engine bypass service removed.";
        return 0;
    }

    int Probe()
    {
        PatchDescriptor descriptor;
        std::wstring detail;
        if (!DiscoverPatch(descriptor, detail))
        {
            std::wcerr << detail;
            return 30;
        }

        std::wcout << L"Module: " << descriptor.modulePath << L"\n"
            << L"AudioLimiter APOProcess RVA: 0x" << std::hex << descriptor.apoProcessRva << L"\n"
            << L"Limiter worker RVA: 0x" << descriptor.limiterFunctionRva << L"\n"
            << L"Validated branch patch RVA: 0x" << descriptor.patchRva << L"\n"
            << L"Patch:";
        for (const BYTE value : descriptor.original)
            std::wcout << L' ' << std::setw(2) << std::setfill(L'0') << static_cast<unsigned>(value);
        std::wcout << L" ->";
        for (const BYTE value : descriptor.bypass)
            std::wcout << L' ' << std::setw(2) << std::setfill(L'0') << static_cast<unsigned>(value);
        std::wcout << L"\n"
            << L"Context:";
        for (const BYTE value : descriptor.context)
        {
            std::wcout << L' ' << std::setw(2) << std::setfill(L'0') << static_cast<unsigned>(value);
        }
        std::wcout << L"\n" << detail;
        return 0;
    }

    int RunServiceDispatcher()
    {
        SERVICE_TABLE_ENTRYW table[] =
        {
            { const_cast<wchar_t*>(kServiceName), ServiceMain },
            { nullptr, nullptr }
        };
        if (!StartServiceCtrlDispatcherW(table))
        {
            std::wcerr << L"Unable to connect to the service control dispatcher: " << LastErrorText();
            return 31;
        }
        return 0;
    }
}
