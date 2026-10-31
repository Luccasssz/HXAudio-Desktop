#include "ApoManager.h"
#include "../Apo/HXAudioApoProbe.h"
#include "../Shared/HXAudioDspHeadroom.h"

#include <windows.h>
#include <winsvc.h>
#include <audioenginebaseapo.h>
#include <audiomediatype.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <devpkey.h>
#include <ks.h>
#include <ksmedia.h>
#include <aclapi.h>
#include <sddl.h>
#include <setupapi.h>
#include <cfgmgr32.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cwctype>
#include <iostream>
#include <string>
#include <sstream>
#include <vector>

namespace HXAudioApoManager
{
    namespace
    {
        constexpr wchar_t kServiceName[] = L"HXAudioApoGuard";
        constexpr wchar_t kServiceDisplayName[] = L"HXAudio Global APO Endpoint Guard";
        constexpr wchar_t kSettingsPath[] = L"Software\\HXAudio\\VirtualAudio";
        constexpr wchar_t kApoPath[] = L"Software\\HXAudio\\Apo";
        constexpr wchar_t kBackupPath[] = L"Software\\HXAudio\\Apo\\Backup";
        constexpr wchar_t kComponentBackupPath[] =
            L"Software\\HXAudio\\Apo\\ComponentBackup";
        constexpr wchar_t kRenderPath[] =
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render";
        constexpr wchar_t kEnumeratorNameValue[] =
            L"{A45C254E-DF1C-4EFD-8020-67D146A850E0},24";
        constexpr wchar_t kPreMixValue[] =
            L"{D04E05A6-594B-4FB6-A80D-01AF5EED7D1D},1";
        constexpr wchar_t kPostMixValue[] =
            L"{D04E05A6-594B-4FB6-A80D-01AF5EED7D1D},2";
        constexpr wchar_t kAssociationValue[] =
            L"{D04E05A6-594B-4FB6-A80D-01AF5EED7D1D},0";
        constexpr wchar_t kSingleEndpointValue[] =
            L"{D04E05A6-594B-4FB6-A80D-01AF5EED7D1D},7";
        constexpr wchar_t kSingleStreamValue[] =
            L"{D04E05A6-594B-4FB6-A80D-01AF5EED7D1D},5";
        constexpr wchar_t kSingleModeValue[] =
            L"{D04E05A6-594B-4FB6-A80D-01AF5EED7D1D},6";
        constexpr wchar_t kOffloadStreamValue[] =
            L"{D04E05A6-594B-4FB6-A80D-01AF5EED7D1D},11";
        constexpr wchar_t kOffloadModeValue[] =
            L"{D04E05A6-594B-4FB6-A80D-01AF5EED7D1D},12";
        constexpr wchar_t kCompositeStreamValue[] =
            L"{D04E05A6-594B-4FB6-A80D-01AF5EED7D1D},13";
        constexpr wchar_t kCompositeModeValue[] =
            L"{D04E05A6-594B-4FB6-A80D-01AF5EED7D1D},14";
        constexpr wchar_t kCompositeValue[] =
            L"{D04E05A6-594B-4FB6-A80D-01AF5EED7D1D},15";
        constexpr wchar_t kCompositeOffloadStreamValue[] =
            L"{D04E05A6-594B-4FB6-A80D-01AF5EED7D1D},19";
        constexpr wchar_t kCompositeOffloadModeValue[] =
            L"{D04E05A6-594B-4FB6-A80D-01AF5EED7D1D},20";
        constexpr wchar_t kDisableSysFxValue[] =
            L"{1DA5D803-D492-4EDD-8C23-E0C0FFEE7F0E},5";
        constexpr wchar_t kEfxProcessingModesValue[] =
            L"{D3993A3F-99C2-4402-B5EC-A92A0367664B},7";
        constexpr wchar_t kSfxProcessingModesValue[] =
            L"{D3993A3F-99C2-4402-B5EC-A92A0367664B},5";
        constexpr wchar_t kMfxProcessingModesValue[] =
            L"{D3993A3F-99C2-4402-B5EC-A92A0367664B},6";
        constexpr wchar_t kOffloadSfxProcessingModesValue[] =
            L"{D3993A3F-99C2-4402-B5EC-A92A0367664B},11";
        constexpr wchar_t kOffloadMfxProcessingModesValue[] =
            L"{D3993A3F-99C2-4402-B5EC-A92A0367664B},12";
        constexpr wchar_t kDefaultProcessingMode[] =
            L"{C18E2F7E-933D-4965-B7D1-1EEF228D2AF3}";
        constexpr wchar_t kCommunicationsProcessingMode[] =
            L"{98951333-B9CD-48B1-A0A3-FF40682D73F7}";
        constexpr wchar_t kSpeechProcessingMode[] =
            L"{FC1CFC9B-B9D6-4CFA-B5E0-4BB2166878B2}";
        constexpr wchar_t kNotificationProcessingMode[] =
            L"{9CF2A70B-F377-403B-BD6B-360863E0355C}";
        constexpr wchar_t kMediaProcessingMode[] =
            L"{4780004E-7133-41D8-8C74-660DADD2C0EE}";
        constexpr wchar_t kMovieProcessingMode[] =
            L"{B26FEB0D-EC94-477C-9494-D1AB8E753F6E}";
        constexpr wchar_t kApoClsid[] =
            L"{C4D1BA43-C7A8-4B47-8DCF-6DCF554D0F59}";
        constexpr DWORD kBackupSchemaVersion = 7;
        const GUID kAudioProcessingObjectClass =
            { 0x5989fce8, 0x9cd0, 0x467d, { 0x8a, 0x6a, 0x54, 0x19, 0xe3, 0x15, 0x29, 0xd4 } };
        const DEVPROPKEY kDeviceParentProperty =
            { { 0x4340a6c5, 0x93fa, 0x4706,
                { 0x97, 0x2c, 0x7b, 0x64, 0x80, 0x08, 0xa5, 0xa7 } }, 8 };

        struct BackedValue
        {
            const wchar_t* name;
            const wchar_t* prefix;
        };

        struct RawRegistryValue
        {
            std::wstring name;
            DWORD type = REG_NONE;
            std::vector<BYTE> data;
        };

        constexpr BackedValue kEffectValues[] =
        {
            { kPreMixValue, L"PreMix" },
            { kPostMixValue, L"PostMix" },
            { kSingleStreamValue, L"SingleStream" },
            { kSingleModeValue, L"SingleMode" },
            { kSingleEndpointValue, L"SingleEndpoint" },
            { kOffloadStreamValue, L"OffloadStream" },
            { kOffloadModeValue, L"OffloadMode" },
            { kCompositeStreamValue, L"CompositeStream" },
            { kCompositeModeValue, L"CompositeMode" },
            { kCompositeValue, L"Composite" },
            { kCompositeOffloadStreamValue, L"CompositeOffloadStream" },
            { kCompositeOffloadModeValue, L"CompositeOffloadMode" },
        };

        constexpr BackedValue kProcessingModeValues[] =
        {
            { kSfxProcessingModesValue, L"SfxModes" },
            { kMfxProcessingModesValue, L"MfxModes" },
            { kEfxProcessingModesValue, L"EfxModes" },
            { kOffloadSfxProcessingModesValue, L"OffloadSfxModes" },
            { kOffloadMfxProcessingModesValue, L"OffloadMfxModes" },
        };
        constexpr DWORD kServiceControlApply = 128;
        constexpr DWORD kServiceControlRestore = 129;
        constexpr DWORD kOperationNone = 0;
        constexpr DWORD kOperationApply = 1;
        constexpr DWORD kOperationRestore = 2;
        constexpr DWORD kGuardStopped = 0;
        constexpr DWORD kGuardReady = 1;
        constexpr DWORD kGuardWorking = 2;
        constexpr DWORD kGuardError = 3;
        constexpr DWORD kDeviceStateActive = 0x00000001;
        constexpr DWORD kStageNone = 0;
        constexpr DWORD kStageEnumerate = 1;
        constexpr DWORD kStageOpenFxProperties = 2;
        constexpr DWORD kStageCreateFxProperties = 3;
        constexpr DWORD kStageCreateBackup = 4;
        constexpr DWORD kStageWriteBackup = 5;
        constexpr DWORD kStageWriteComposite = 6;
        constexpr DWORD kStageEnableEnhancements = 7;
        constexpr DWORD kStageVerify = 8;
        constexpr DWORD kStageRestore = 9;
        constexpr DWORD kStageWriteProcessingModes = 10;
        constexpr DWORD kStageExternalAssociations = 11;
        constexpr DWORD kStageApoComponents = 12;
        constexpr DWORD kStageEndpointVolume = 13;

        struct EndpointSummary
        {
            DWORD enumerated = 0;
            DWORD succeeded = 0;
            DWORD failed = 0;
            DWORD active = 0;
            DWORD activeSucceeded = 0;
            DWORD firstError = ERROR_SUCCESS;
            DWORD failureStage = kStageNone;
            DWORD verifyFailureMask = 0;
            std::wstring failureEndpoint;
        };

        void PreserveVerificationSummary(EndpointSummary& summary, const EndpointSummary& verified)
        {
            summary = verified;
        }

        // Shared by the real guard and deterministic offline fault injection.
        template<class Attempt> bool RunApplyAttempts(Attempt attempt)
        {
            for (DWORD i = 0; i < 5; ++i)
                if (attempt(i)) return true;
            return false;
        }

        template<class RestorePrevious, class RestoreClean>
        bool RunFailedInstallRollback(RestorePrevious previous, RestoreClean clean)
        {
            if (previous()) return true;
            clean();
            return false;
        }

        struct InstallFailureDiagnostic
        {
            bool available = false;
            DWORD error = 0, stage = 0, mask = 0;
            std::wstring endpoint;
            DWORD exitCode = 0;
            DWORD win32Known = 0;
            std::wstring phase;
            DWORD rollbackStatus = 0; // 0 not attempted, 1 old restored, 2 fallback attempted, 3 old restore failed
            DWORD rollbackCode = 0; // first observed fallback exit code, 0 means none observed
        };

        SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
        SERVICE_STATUS g_status{};
        HANDLE g_stopEvent = nullptr;
        HANDLE g_commandEvent = nullptr;
        volatile LONG g_pendingOperation = kOperationNone;

        std::wstring LastErrorText(DWORD error = GetLastError())
        {
            wchar_t* buffer = nullptr;
            FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0,
                reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
            std::wstring text = buffer ? buffer : L"Unknown error";
            if (buffer) LocalFree(buffer);
            while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n'))
                text.pop_back();
            return text;
        }

        bool SetString(HKEY root, const wchar_t* path, const wchar_t* name,
            const std::wstring& value)
        {
            HKEY key = nullptr;
            DWORD disposition = 0;
            if (RegCreateKeyExW(root, path, 0, nullptr, 0,
                KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, &disposition) != ERROR_SUCCESS)
                return false;
            const LONG result = RegSetValueExW(key, name, 0, REG_SZ,
                reinterpret_cast<const BYTE*>(value.c_str()),
                static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
            RegCloseKey(key);
            return result == ERROR_SUCCESS;
        }

        std::wstring ReadString(HKEY root, const wchar_t* path, const wchar_t* name)
        {
            HKEY key = nullptr;
            if (RegOpenKeyExW(root, path, 0,
                KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) return {};
            DWORD type = 0;
            DWORD size = 0;
            if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS ||
                type != REG_SZ || size < sizeof(wchar_t))
            {
                RegCloseKey(key);
                return {};
            }
            std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 1, L'\0');
            const LONG result = RegQueryValueExW(key, name, nullptr, &type,
                reinterpret_cast<BYTE*>(buffer.data()), &size);
            RegCloseKey(key);
            return result == ERROR_SUCCESS ? std::wstring(buffer.data()) : std::wstring();
        }

        bool QueryRaw(HKEY key, const wchar_t* name, DWORD& type,
            std::vector<BYTE>& data)
        {
            type = 0;
            DWORD size = 0;
            const LONG query = RegQueryValueExW(key, name, nullptr, &type, nullptr, &size);
            if (query != ERROR_SUCCESS) return false;
            data.resize(size);
            if (!size) return true;
            return RegQueryValueExW(key, name, nullptr, &type, data.data(), &size) == ERROR_SUCCESS;
        }

        bool SetDword(HKEY key, const wchar_t* name, DWORD value)
        {
            return RegSetValueExW(key, name, 0, REG_DWORD,
                reinterpret_cast<const BYTE*>(&value), sizeof(value)) == ERROR_SUCCESS;
        }

        bool QueryDword(HKEY key, const wchar_t* name, DWORD& value)
        {
            DWORD type = 0;
            DWORD size = sizeof(value);
            return RegQueryValueExW(key, name, nullptr, &type,
                reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS &&
                type == REG_DWORD && size == sizeof(value);
        }

        DWORD ReadSettingsDword(const wchar_t* name, DWORD fallback = 0)
        {
            HKEY key = nullptr;
            DWORD value = fallback;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kSettingsPath, 0,
                KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS)
            {
                QueryDword(key, name, value);
                RegCloseKey(key);
            }
            return value;
        }

        bool SetSettingsDword(const wchar_t* name, DWORD value)
        {
            HKEY key = nullptr;
            DWORD disposition = 0;
            if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kSettingsPath, 0, nullptr, 0,
                KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, &disposition) != ERROR_SUCCESS)
                return false;
            const bool result = SetDword(key, name, value);
            RegCloseKey(key);
            return result;
        }

        bool ConsumeForceAudioRestart()
        {
            HKEY key = nullptr;
            DWORD value = 0;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kSettingsPath, 0,
                KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS)
            {
                QueryDword(key, L"ApoForceAudioRestart", value);
                RegDeleteValueW(key, L"ApoForceAudioRestart");
                RegCloseKey(key);
            }
            return value != 0;
        }

        void WriteGuardStatus(DWORD state, DWORD error,
            const EndpointSummary* summary, bool completeOperation)
        {
            HKEY key = nullptr;
            DWORD disposition = 0;
            if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kSettingsPath, 0, nullptr, 0,
                KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key,
                &disposition) != ERROR_SUCCESS) return;

            SetDword(key, L"ApoGuardState", state);
            SetDword(key, L"ApoLastError", error);
            if (summary)
            {
                SetDword(key, L"ApoEnumeratedEndpoints", summary->enumerated);
                SetDword(key, L"ApoAttachedEndpoints", summary->succeeded);
                SetDword(key, L"ApoFailedEndpoints", summary->failed);
                SetDword(key, L"ApoActiveEndpoints", summary->active);
                SetDword(key, L"ApoActiveAttachedEndpoints", summary->activeSucceeded);
                SetDword(key, L"ApoFailureStage", summary->failureStage);
                SetDword(key, L"ApoVerifyFailureMask", summary->verifyFailureMask);
                const auto& endpoint = summary->failureEndpoint;
                RegSetValueExW(key, L"ApoFailureEndpoint", 0, REG_SZ,
                    reinterpret_cast<const BYTE*>(endpoint.c_str()),
                    static_cast<DWORD>((endpoint.size() + 1) * sizeof(wchar_t)));
            }
            if (completeOperation)
            {
                DWORD serial = 0;
                QueryDword(key, L"ApoOperationSerial", serial);
                SetDword(key, L"ApoOperationSerial", serial + 1);
            }
            RegCloseKey(key);
        }

        void ClearGuardStatus()
        {
            HKEY key = nullptr;
            DWORD disposition = 0;
            if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kSettingsPath, 0, nullptr, 0,
                KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, &disposition) != ERROR_SUCCESS)
                return;
            const wchar_t* names[] =
            {
                L"ApoGuardState", L"ApoLastError", L"ApoEnumeratedEndpoints",
                L"ApoAttachedEndpoints", L"ApoFailedEndpoints", L"ApoActiveEndpoints",
                L"ApoActiveAttachedEndpoints", L"ApoOperationSerial",
                L"ApoForceAudioRestart", L"ApoFailureStage", L"ApoFailureEndpoint",
                L"ApoVerifyFailureMask"
            };
            for (const auto* name : names) RegDeleteValueW(key, name);
            RegCloseKey(key);
        }

        bool BackupValue(HKEY backup, HKEY source, const wchar_t* sourceName,
            const wchar_t* prefix)
        {
            DWORD type = 0;
            std::vector<BYTE> data;
            const bool present = QueryRaw(source, sourceName, type, data);
            const std::wstring presentName = std::wstring(prefix) + L"Present";
            const std::wstring typeName = std::wstring(prefix) + L"Type";
            const std::wstring dataName = std::wstring(prefix) + L"Data";
            if (!SetDword(backup, presentName.c_str(), present ? 1 : 0)) return false;
            if (!present) return true;
            if (!SetDword(backup, typeName.c_str(), type)) return false;
            return RegSetValueExW(backup, dataName.c_str(), 0, REG_BINARY,
                data.empty() ? nullptr : data.data(), static_cast<DWORD>(data.size())) == ERROR_SUCCESS;
        }

        bool RestoreValue(HKEY backup, HKEY target, const wchar_t* targetName,
            const wchar_t* prefix, bool& changed)
        {
            const std::wstring presentName = std::wstring(prefix) + L"Present";
            const std::wstring typeName = std::wstring(prefix) + L"Type";
            const std::wstring dataName = std::wstring(prefix) + L"Data";
            DWORD present = 0;
            if (!QueryDword(backup, presentName.c_str(), present)) return false;
            if (!present)
            {
                DWORD currentType = 0;
                std::vector<BYTE> currentData;
                if (!QueryRaw(target, targetName, currentType, currentData))
                    return true;
                const LONG result = RegDeleteValueW(target, targetName);
                if (result == ERROR_SUCCESS) changed = true;
                return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
            }
            DWORD type = 0;
            if (!QueryDword(backup, typeName.c_str(), type)) return false;
            DWORD storedType = 0;
            std::vector<BYTE> data;
            if (!QueryRaw(backup, dataName.c_str(), storedType, data) || storedType != REG_BINARY)
                return false;
            DWORD currentType = 0;
            std::vector<BYTE> currentData;
            if (QueryRaw(target, targetName, currentType, currentData) &&
                currentType == type && currentData == data) return true;
            const LONG result = RegSetValueExW(target, targetName, 0, type,
                data.empty() ? nullptr : data.data(), static_cast<DWORD>(data.size()));
            if (result == ERROR_SUCCESS) changed = true;
            return result == ERROR_SUCCESS;
        }

        std::vector<std::wstring> DecodeStrings(DWORD type, const std::vector<BYTE>& data)
        {
            std::vector<std::wstring> values;
            if ((type != REG_SZ && type != REG_MULTI_SZ) ||
                data.size() < sizeof(wchar_t)) return values;
            const wchar_t* begin = reinterpret_cast<const wchar_t*>(data.data());
            const size_t characters = data.size() / sizeof(wchar_t);
            if (type == REG_SZ)
            {
                values.emplace_back(begin, wcsnlen(begin, characters));
                return values;
            }
            const wchar_t* end = begin + characters;
            for (const wchar_t* item = begin; item < end && *item; )
            {
                const size_t remaining = static_cast<size_t>(end - item);
                const size_t length = wcsnlen(item, remaining);
                if (length == remaining) break;
                values.emplace_back(item, length);
                item += length + 1;
            }
            return values;
        }

        std::vector<BYTE> EncodeMultiString(const std::vector<std::wstring>& values)
        {
            size_t characters = 1;
            for (const auto& value : values) characters += value.size() + 1;
            characters = std::max<size_t>(characters, 2);
            std::vector<BYTE> data(characters * sizeof(wchar_t), 0);
            wchar_t* cursor = reinterpret_cast<wchar_t*>(data.data());
            for (const auto& value : values)
            {
                CopyMemory(cursor, value.c_str(), value.size() * sizeof(wchar_t));
                cursor += value.size() + 1;
            }
            return data;
        }

        bool StartsWithInsensitive(const std::wstring& value,
            const wchar_t* prefix);
        bool SetRegistryStringValue(HKEY key, const wchar_t* name,
            const std::wstring& value);

        bool TryParseFiniteDouble(const std::wstring& text, double& value)
        {
            const wchar_t* cursor = text.c_str();
            while (*cursor && iswspace(*cursor)) ++cursor;
            if (!*cursor) return false;

            errno = 0;
            wchar_t* end = nullptr;
            const double parsed = wcstod(cursor, &end);
            if (end == cursor || !std::isfinite(parsed)) return false;
            while (*end && iswspace(*end)) ++end;
            if (*end) return false;
            value = parsed;
            return true;
        }

        bool ClampStoredGain(std::wstring& text)
        {
            double value = 0.0;
            if (!TryParseFiniteDouble(text, value))
            {
                if (text == L"0") return false;
                text = L"0";
                return true;
            }

            const double clamped = HXAudioDsp::ClampGainDb(value);
            if (clamped == value) return false;
            text = clamped > 0.0 ? L"30" : clamped < 0.0 ? L"-30" : L"0";
            return true;
        }

        bool ClampStoredPeqBand(std::wstring& encoded)
        {
            const size_t first = encoded.find(L'|');
            const size_t second = first == std::wstring::npos
                ? std::wstring::npos : encoded.find(L'|', first + 1);
            const size_t third = second == std::wstring::npos
                ? std::wstring::npos : encoded.find(L'|', second + 1);
            const size_t fourth = third == std::wstring::npos
                ? std::wstring::npos : encoded.find(L'|', third + 1);
            if (third == std::wstring::npos || fourth == std::wstring::npos ||
                encoded.find(L'|', fourth + 1) != std::wstring::npos) return false;

            std::wstring gain = encoded.substr(third + 1, fourth - third - 1);
            double parsed = 0.0;
            if (!TryParseFiniteDouble(gain, parsed)) return false;
            const double clamped = HXAudioDsp::ClampPeqGainDb(parsed);
            if (clamped == parsed) return false;
            gain = clamped > 0.0 ? L"40" : clamped < 0.0 ? L"-40" : L"0";
            encoded.replace(third + 1, fourth - third - 1, gain);
            return true;
        }

        bool MigrateStoredDspGainRange(bool& changed)
        {
            changed = false;
            HKEY settings = nullptr;
            DWORD disposition = 0;
            if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kSettingsPath, 0, nullptr, 0,
                KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr,
                &settings, &disposition) != ERROR_SUCCESS) return false;

            bool ok = true;
            DWORD type = 0;
            std::vector<BYTE> data;
            if (QueryRaw(settings, L"DspPreampDb", type, data) && type == REG_SZ &&
                data.size() >= sizeof(wchar_t))
            {
                const wchar_t* raw = reinterpret_cast<const wchar_t*>(data.data());
                std::wstring value(raw, wcsnlen(raw, data.size() / sizeof(wchar_t)));
                if (ClampStoredGain(value))
                {
                    ok = SetRegistryStringValue(settings, L"DspPreampDb", value);
                    changed = ok;
                }
            }

            DWORD valueCount = 0;
            DWORD maximumName = 0;
            if (ok && RegQueryInfoKeyW(settings, nullptr, nullptr, nullptr, nullptr,
                nullptr, nullptr, &valueCount, &maximumName, nullptr, nullptr,
                nullptr) != ERROR_SUCCESS) ok = false;

            std::vector<std::wstring> names;
            if (ok)
            {
                std::vector<wchar_t> name(static_cast<size_t>(maximumName) + 2, L'\0');
                for (DWORD index = 0; index < valueCount; ++index)
                {
                    DWORD length = static_cast<DWORD>(name.size());
                    if (RegEnumValueW(settings, index, name.data(), &length, nullptr,
                        nullptr, nullptr, nullptr) == ERROR_SUCCESS)
                    {
                        std::wstring valueName(name.data(), length);
                        if (_wcsicmp(valueName.c_str(), L"DspPeqBands") == 0 ||
                            StartsWithInsensitive(valueName, L"DspPeqBands_"))
                            names.push_back(std::move(valueName));
                    }
                }
            }

            for (const auto& name : names)
            {
                if (!ok) break;
                DWORD bandsType = 0;
                std::vector<BYTE> bandsData;
                if (!QueryRaw(settings, name.c_str(), bandsType, bandsData) ||
                    bandsType != REG_MULTI_SZ) continue;
                auto bands = DecodeStrings(bandsType, bandsData);
                bool bandsChanged = false;
                for (auto& band : bands)
                    bandsChanged = ClampStoredPeqBand(band) || bandsChanged;
                if (!bandsChanged) continue;
                const auto encoded = EncodeMultiString(bands);
                ok = RegSetValueExW(settings, name.c_str(), 0, REG_MULTI_SZ,
                    encoded.data(), static_cast<DWORD>(encoded.size())) == ERROR_SUCCESS;
                changed = changed || ok;
            }

            RegCloseKey(settings);
            return ok;
        }

        bool StartsWithInsensitive(const std::wstring& value,
            const wchar_t* prefix)
        {
            const size_t length = wcslen(prefix);
            return value.size() >= length &&
                _wcsnicmp(value.c_str(), prefix, length) == 0;
        }

        bool IsExternalApoAssociation(DWORD type, const std::vector<BYTE>& data)
        {
            if (type != REG_SZ && type != REG_EXPAND_SZ && type != REG_MULTI_SZ)
                return false;
            const auto values = DecodeStrings(type == REG_EXPAND_SZ ? REG_SZ : type, data);
            return std::any_of(values.begin(), values.end(), [](const std::wstring& value)
            {
                return StartsWithInsensitive(value, L"SWD\\DRIVERENUM\\");
            });
        }

        std::vector<RawRegistryValue> ExternalApoAssociations(HKEY fx)
        {
            std::vector<RawRegistryValue> result;
            DWORD valueCount = 0;
            DWORD maximumName = 0;
            if (RegQueryInfoKeyW(fx, nullptr, nullptr, nullptr, nullptr, nullptr,
                nullptr, &valueCount, &maximumName, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
                return result;
            std::vector<wchar_t> name(static_cast<size_t>(maximumName) + 2, L'\0');
            for (DWORD index = 0; index < valueCount; ++index)
            {
                DWORD length = static_cast<DWORD>(name.size());
                const LONG enumerate = RegEnumValueW(fx, index, name.data(), &length,
                    nullptr, nullptr, nullptr, nullptr);
                if (enumerate != ERROR_SUCCESS) continue;
                RawRegistryValue value;
                value.name.assign(name.data(), length);
                if (QueryRaw(fx, value.name.c_str(), value.type, value.data) &&
                    IsExternalApoAssociation(value.type, value.data))
                    result.push_back(std::move(value));
            }
            return result;
        }

        bool SetRegistryStringValue(HKEY key, const wchar_t* name,
            const std::wstring& value)
        {
            return RegSetValueExW(key, name, 0, REG_SZ,
                reinterpret_cast<const BYTE*>(value.c_str()),
                static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
        }

        bool QueryRegistryStringValue(HKEY key, const wchar_t* name,
            std::wstring& value)
        {
            DWORD type = 0;
            std::vector<BYTE> data;
            if (!QueryRaw(key, name, type, data) || type != REG_SZ ||
                data.size() < sizeof(wchar_t)) return false;
            const wchar_t* text = reinterpret_cast<const wchar_t*>(data.data());
            value.assign(text, wcsnlen(text, data.size() / sizeof(wchar_t)));
            return true;
        }

        bool BackupExternalApoAssociations(HKEY backup, HKEY fx)
        {
            const auto values = ExternalApoAssociations(fx);
            HKEY list = nullptr;
            DWORD disposition = 0;
            if (RegCreateKeyExW(backup, L"ExternalAssociations", 0, nullptr, 0,
                KEY_READ | KEY_WRITE, nullptr, &list, &disposition) != ERROR_SUCCESS)
                return false;

            DWORD count = 0;
            QueryDword(list, L"Count", count);
            for (const auto& value : values)
            {
                bool exists = false;
                for (DWORD index = 0; index < count; ++index)
                {
                    const std::wstring itemName = std::to_wstring(index);
                    HKEY item = nullptr;
                    if (RegOpenKeyExW(list, itemName.c_str(), 0, KEY_READ,
                        &item) != ERROR_SUCCESS) continue;
                    std::wstring storedName;
                    const bool read = QueryRegistryStringValue(item, L"Name", storedName);
                    RegCloseKey(item);
                    if (read && _wcsicmp(storedName.c_str(), value.name.c_str()) == 0)
                    {
                        exists = true;
                        break;
                    }
                }
                if (exists) continue;

                const std::wstring itemName = std::to_wstring(count);
                HKEY item = nullptr;
                if (RegCreateKeyExW(list, itemName.c_str(), 0, nullptr, 0,
                    KEY_WRITE, nullptr, &item, &disposition) != ERROR_SUCCESS)
                {
                    RegCloseKey(list);
                    return false;
                }
                const bool stored = SetRegistryStringValue(item, L"Name", value.name) &&
                    SetDword(item, L"Type", value.type) &&
                    RegSetValueExW(item, L"Data", 0, REG_BINARY,
                        value.data.empty() ? nullptr : value.data.data(),
                        static_cast<DWORD>(value.data.size())) == ERROR_SUCCESS;
                RegCloseKey(item);
                if (!stored)
                {
                    RegCloseKey(list);
                    return false;
                }
                ++count;
            }
            const bool saved = SetDword(list, L"Count", count);
            RegCloseKey(list);
            return saved;
        }

        bool RestoreExternalApoAssociations(HKEY backup, HKEY fx, bool& changed)
        {
            HKEY list = nullptr;
            if (RegOpenKeyExW(backup, L"ExternalAssociations", 0, KEY_READ,
                &list) != ERROR_SUCCESS) return true;
            DWORD count = 0;
            if (!QueryDword(list, L"Count", count))
            {
                RegCloseKey(list);
                return false;
            }
            bool ok = true;
            for (DWORD index = 0; index < count && ok; ++index)
            {
                const std::wstring itemName = std::to_wstring(index);
                HKEY item = nullptr;
                if (RegOpenKeyExW(list, itemName.c_str(), 0, KEY_READ,
                    &item) != ERROR_SUCCESS)
                {
                    ok = false;
                    break;
                }
                std::wstring name;
                DWORD type = 0;
                DWORD storedType = 0;
                std::vector<BYTE> data;
                ok = QueryRegistryStringValue(item, L"Name", name) &&
                    QueryDword(item, L"Type", type) &&
                    QueryRaw(item, L"Data", storedType, data) &&
                    storedType == REG_BINARY;
                RegCloseKey(item);
                if (!ok) break;

                DWORD currentType = 0;
                std::vector<BYTE> currentData;
                if (QueryRaw(fx, name.c_str(), currentType, currentData) &&
                    currentType == type && currentData == data) continue;
                ok = RegSetValueExW(fx, name.c_str(), 0, type,
                    data.empty() ? nullptr : data.data(),
                    static_cast<DWORD>(data.size())) == ERROR_SUCCESS;
                if (ok) changed = true;
            }
            RegCloseKey(list);
            return ok;
        }

        bool RemoveExternalApoAssociations(HKEY fx, bool& changed)
        {
            bool ok = true;
            for (const auto& value : ExternalApoAssociations(fx))
            {
                const LONG result = RegDeleteValueW(fx, value.name.c_str());
                if (result == ERROR_SUCCESS) changed = true;
                else if (result != ERROR_FILE_NOT_FOUND) ok = false;
            }
            return ok;
        }

        bool ExternalApoAssociationsAbsent(HKEY fx)
        {
            return ExternalApoAssociations(fx).empty();
        }

        bool ContainsClsid(const std::vector<std::wstring>& values)
        {
            return std::any_of(values.begin(), values.end(), [](const std::wstring& value)
            {
                return _wcsicmp(value.c_str(), kApoClsid) == 0;
            });
        }

        bool EndpointIsActive(HKEY renderRoot, const std::wstring& endpoint)
        {
            HKEY key = nullptr;
            DWORD state = 0;
            if (RegOpenKeyExW(renderRoot, endpoint.c_str(), 0,
                KEY_QUERY_VALUE, &key) == ERROR_SUCCESS)
            {
                QueryDword(key, L"DeviceState", state);
                RegCloseKey(key);
            }
            return (state & kDeviceStateActive) != 0;
        }

        bool EndpointIsTerminalHardware(HKEY renderRoot,
            const std::wstring& endpoint)
        {
            HKEY properties = nullptr;
            const std::wstring path = endpoint + L"\\Properties";
            if (RegOpenKeyExW(renderRoot, path.c_str(), 0, KEY_QUERY_VALUE,
                &properties) != ERROR_SUCCESS) return true;
            DWORD type = 0;
            std::vector<BYTE> data;
            std::wstring enumerator;
            if (QueryRaw(properties, kEnumeratorNameValue, type, data))
            {
                const auto values = DecodeStrings(type, data);
                if (!values.empty()) enumerator = values.front();
            }
            RegCloseKey(properties);
            // ROOT\MEDIA endpoints are software mixers/cables. Processing
            // them as well as their terminal hardware output applies HXAudio
            // twice and they do not instantiate post-mix endpoint effects.
            return _wcsicmp(enumerator.c_str(), L"ROOT") != 0;
        }

        std::wstring EndpointRegistryKeyFromId(const std::wstring& id)
        {
            const size_t separator = id.rfind(L"}.{" );
            if (separator != std::wstring::npos)
                return id.substr(separator + 2);
            const size_t brace = id.rfind(L'{');
            return brace == std::wstring::npos ? id : id.substr(brace);
        }

        std::wstring DevicePropertyString(IMMDevice* device,
            const PROPERTYKEY& property)
        {
            if (!device) return {};
            IPropertyStore* store = nullptr;
            PROPVARIANT value{};
            PropVariantInit(&value);
            std::wstring result;
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) && store &&
                SUCCEEDED(store->GetValue(property, &value)))
            {
                if (value.vt == VT_LPWSTR && value.pwszVal) result = value.pwszVal;
                else if (value.vt == VT_BSTR && value.bstrVal) result = value.bstrVal;
            }
            PropVariantClear(&value);
            if (store) store->Release();
            return result;
        }

        std::wstring DeviceProfileKey(IMMDevice* device)
        {
            if (!device) return {};
            const std::wstring name = DevicePropertyString(device,
                PKEY_Device_FriendlyName);
            IPropertyStore* store = nullptr;
            PROPVARIANT container{};
            PropVariantInit(&container);
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) && store &&
                SUCCEEDED(store->GetValue(PKEY_Device_ContainerId, &container)) &&
                container.vt == VT_CLSID && container.puuid)
            {
                wchar_t text[64]{};
                StringFromGUID2(*container.puuid, text, ARRAYSIZE(text));
                const std::wstring result = L"CONTAINER|" + std::wstring(text) +
                    L"|" + name;
                PropVariantClear(&container);
                store->Release();
                return result;
            }
            PropVariantClear(&container);
            if (store) store->Release();

            const std::wstring instance = DevicePropertyString(device,
                PKEY_Device_InstanceId);
            if (!instance.empty() && instance.rfind(L"SWD\\MMDEVAPI\\", 0) != 0)
                return L"INSTANCE|" + instance + L"|" + name;
            LPWSTR id = nullptr;
            std::wstring endpointId;
            if (SUCCEEDED(device->GetId(&id)) && id)
            {
                endpointId = id;
                CoTaskMemFree(id);
            }
            return name.empty() ? L"ENDPOINT|" + endpointId : L"NAME|" + name;
        }

        struct EndpointVolumeSnapshot
        {
            std::wstring endpointId;
            std::wstring profileKey;
            float volume = 1.0f;
            BOOL muted = FALSE;
        };

        bool CaptureEndpointVolumes(std::vector<EndpointVolumeSnapshot>& snapshots)
        {
            snapshots.clear();
            IMMDeviceEnumerator* enumerator = nullptr;
            IMMDeviceCollection* collection = nullptr;
            HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                reinterpret_cast<void**>(&enumerator));
            if (SUCCEEDED(hr)) hr = enumerator->EnumAudioEndpoints(eRender,
                DEVICE_STATE_ACTIVE, &collection);
            UINT count = 0;
            if (SUCCEEDED(hr)) hr = collection->GetCount(&count);
            for (UINT index = 0; SUCCEEDED(hr) && index < count; ++index)
            {
                IMMDevice* device = nullptr;
                IAudioEndpointVolume* endpointVolume = nullptr;
                LPWSTR endpointId = nullptr;
                float volume = 1.0f;
                BOOL muted = FALSE;
                if (SUCCEEDED(collection->Item(index, &device)) && device &&
                    SUCCEEDED(device->GetId(&endpointId)) && endpointId &&
                    SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume),
                        CLSCTX_ALL, nullptr,
                        reinterpret_cast<void**>(&endpointVolume))) && endpointVolume &&
                    SUCCEEDED(endpointVolume->GetMasterVolumeLevelScalar(&volume)) &&
                    SUCCEEDED(endpointVolume->GetMute(&muted)))
                {
                    snapshots.push_back({ endpointId, DeviceProfileKey(device),
                        std::clamp(volume, 0.0f, 1.0f), muted });
                }
                if (endpointVolume) endpointVolume->Release();
                if (endpointId) CoTaskMemFree(endpointId);
                if (device) device->Release();
            }
            if (collection) collection->Release();
            if (enumerator) enumerator->Release();
            return SUCCEEDED(hr);
        }

        bool RestoreEndpointVolumes(const std::vector<EndpointVolumeSnapshot>& snapshots,
            std::wstring& failedEndpoint)
        {
            failedEndpoint.clear();
            if (snapshots.empty()) return true;
            std::vector<bool> restored(snapshots.size(), false);
            const GUID eventContext =
                { 0xe4e95d9c, 0x4943, 0x48ee, { 0xa4, 0xd7, 0x66, 0xec, 0x44, 0x73, 0x25, 0xcb } };
            const ULONGLONG deadline = GetTickCount64() + 15000;
            ULONGLONG stableSince = 0;
            do
            {
                std::fill(restored.begin(), restored.end(), false);
                IMMDeviceEnumerator* enumerator = nullptr;
                IMMDeviceCollection* collection = nullptr;
                HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                    CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                    reinterpret_cast<void**>(&enumerator));
                if (SUCCEEDED(hr)) hr = enumerator->EnumAudioEndpoints(eRender,
                    DEVICE_STATE_ACTIVE, &collection);
                UINT count = 0;
                if (SUCCEEDED(hr)) hr = collection->GetCount(&count);
                for (UINT deviceIndex = 0; SUCCEEDED(hr) && deviceIndex < count;
                    ++deviceIndex)
                {
                    IMMDevice* device = nullptr;
                    LPWSTR endpointId = nullptr;
                    if (FAILED(collection->Item(deviceIndex, &device)) || !device ||
                        FAILED(device->GetId(&endpointId)) || !endpointId)
                    {
                        if (endpointId) CoTaskMemFree(endpointId);
                        if (device) device->Release();
                        continue;
                    }
                    const std::wstring id(endpointId);
                    const std::wstring profile = DeviceProfileKey(device);
                    size_t match = snapshots.size();
                    for (size_t index = 0; index < snapshots.size(); ++index)
                    {
                        if (_wcsicmp(
                            snapshots[index].endpointId.c_str(), id.c_str()) == 0)
                        {
                            match = index;
                            break;
                        }
                    }
                    if (match == snapshots.size() && !profile.empty())
                    {
                        for (size_t index = 0; index < snapshots.size(); ++index)
                        {
                            if (!snapshots[index].profileKey.empty() &&
                                _wcsicmp(snapshots[index].profileKey.c_str(),
                                    profile.c_str()) == 0)
                            {
                                match = index;
                                break;
                            }
                        }
                    }
                    if (match != snapshots.size())
                    {
                        IAudioEndpointVolume* endpointVolume = nullptr;
                        if (SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume),
                            CLSCTX_ALL, nullptr,
                            reinterpret_cast<void**>(&endpointVolume))) && endpointVolume)
                        {
                            float currentVolume = -1.0f;
                            BOOL currentMute = FALSE;
                            bool ok = SUCCEEDED(endpointVolume->GetMasterVolumeLevelScalar(
                                &currentVolume)) && SUCCEEDED(endpointVolume->GetMute(
                                    &currentMute));
                            if (ok && std::abs(currentVolume - snapshots[match].volume) > 0.0005f)
                                ok = SUCCEEDED(endpointVolume->SetMasterVolumeLevelScalar(
                                    snapshots[match].volume, &eventContext));
                            if (ok && currentMute != snapshots[match].muted)
                                ok = SUCCEEDED(endpointVolume->SetMute(
                                    snapshots[match].muted, &eventContext));
                            float verifiedVolume = -1.0f;
                            BOOL verifiedMute = FALSE;
                            ok = ok && SUCCEEDED(endpointVolume->GetMasterVolumeLevelScalar(
                                &verifiedVolume)) && SUCCEEDED(endpointVolume->GetMute(
                                    &verifiedMute)) &&
                                std::abs(verifiedVolume - snapshots[match].volume) <= 0.001f &&
                                verifiedMute == snapshots[match].muted;
                            restored[match] = ok;
                            endpointVolume->Release();
                        }
                    }
                    CoTaskMemFree(endpointId);
                    device->Release();
                }
                if (collection) collection->Release();
                if (enumerator) enumerator->Release();
                if (std::all_of(restored.begin(), restored.end(),
                    [](bool value) { return value; }))
                {
                    if (stableSince == 0) stableSince = GetTickCount64();
                    if (GetTickCount64() - stableSince >= 5000) return true;
                }
                else stableSince = 0;
                Sleep(200);
            } while (GetTickCount64() < deadline);

            for (size_t index = 0; index < snapshots.size(); ++index)
            {
                if (!restored[index])
                {
                    failedEndpoint = snapshots[index].endpointId;
                    break;
                }
            }
            return false;
        }

        bool ContainsInsensitive(const std::vector<std::wstring>& values,
            const std::wstring& wanted)
        {
            return std::any_of(values.begin(), values.end(),
                [&](const std::wstring& value)
                {
                    return _wcsicmp(value.c_str(), wanted.c_str()) == 0;
                });
        }

        struct EndpointSelection
        {
            bool configured = false;
            std::vector<std::wstring> endpointKeys;

            bool Includes(const std::wstring& endpoint) const
            {
                return !configured || ContainsInsensitive(endpointKeys, endpoint);
            }
        };

        EndpointSelection LoadEndpointSelection()
        {
            EndpointSelection selection;
            HKEY settings = nullptr;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kSettingsPath, 0,
                KEY_QUERY_VALUE | KEY_WOW64_64KEY, &settings) != ERROR_SUCCESS)
                return selection;

            std::wstring installationMode;
            QueryRegistryStringValue(settings, L"InstallationMode",
                installationMode);
            if (StartsWithInsensitive(installationMode, L"Virtual"))
            {
                RegCloseKey(settings);
                return selection;
            }

            DWORD endpointType = 0;
            DWORD profileType = 0;
            std::vector<BYTE> endpointData;
            std::vector<BYTE> profileData;
            const bool endpointsPresent = QueryRaw(settings, L"TargetEndpoints",
                endpointType, endpointData) && endpointType == REG_MULTI_SZ;
            const bool profilesPresent = QueryRaw(settings, L"TargetEndpointProfiles",
                profileType, profileData) && profileType == REG_MULTI_SZ;
            RegCloseKey(settings);
            selection.configured = endpointsPresent || profilesPresent;
            if (!selection.configured) return selection;

            if (endpointsPresent)
            {
                for (const auto& id : DecodeStrings(endpointType, endpointData))
                {
                    const std::wstring key = EndpointRegistryKeyFromId(id);
                    if (!key.empty() && !ContainsInsensitive(selection.endpointKeys, key))
                        selection.endpointKeys.push_back(key);
                }
            }
            const std::vector<std::wstring> profiles = profilesPresent
                ? DecodeStrings(profileType, profileData) : std::vector<std::wstring>{};
            if (profiles.empty()) return selection;

            const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            const bool uninitialize = SUCCEEDED(co);
            IMMDeviceEnumerator* enumerator = nullptr;
            IMMDeviceCollection* collection = nullptr;
            HRESULT hr = co == RPC_E_CHANGED_MODE ? S_OK : co;
            if (SUCCEEDED(hr)) hr = CoCreateInstance(__uuidof(MMDeviceEnumerator),
                nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                reinterpret_cast<void**>(&enumerator));
            if (SUCCEEDED(hr)) hr = enumerator->EnumAudioEndpoints(eRender,
                DEVICE_STATEMASK_ALL, &collection);
            UINT count = 0;
            if (SUCCEEDED(hr)) hr = collection->GetCount(&count);
            for (UINT index = 0; SUCCEEDED(hr) && index < count; ++index)
            {
                IMMDevice* device = nullptr;
                LPWSTR id = nullptr;
                if (SUCCEEDED(collection->Item(index, &device)) && device &&
                    ContainsInsensitive(profiles, DeviceProfileKey(device)) &&
                    SUCCEEDED(device->GetId(&id)) && id)
                {
                    const std::wstring key = EndpointRegistryKeyFromId(id);
                    if (!key.empty() && !ContainsInsensitive(selection.endpointKeys, key))
                        selection.endpointKeys.push_back(key);
                }
                if (id) CoTaskMemFree(id);
                if (device) device->Release();
            }
            if (collection) collection->Release();
            if (enumerator) enumerator->Release();
            if (uninitialize) CoUninitialize();
            return selection;
        }

        std::wstring ParentDeviceInstanceId(const std::wstring& instanceId)
        {
            if (instanceId.empty()) return {};
            DEVINST node = 0;
            if (CM_Locate_DevNodeW(&node, const_cast<wchar_t*>(instanceId.c_str()),
                CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return {};

            // Software MMDEVAPI endpoints expose their audio-adapter parent
            // through DEVPKEY_Device_Parent.  CM_Get_Parent can instead return
            // the generic software-device tree, which does not match the
            // parent recorded on DCH AudioProcessingObject components.
            DEVPROPTYPE type = 0;
            ULONG size = 0;
            CONFIGRET property = CM_Get_DevNode_PropertyW(node,
                &kDeviceParentProperty, &type, nullptr, &size, 0);
            if (property == CR_BUFFER_SMALL && type == DEVPROP_TYPE_STRING &&
                size >= sizeof(wchar_t))
            {
                std::vector<BYTE> data(size, 0);
                property = CM_Get_DevNode_PropertyW(node, &kDeviceParentProperty,
                    &type, data.data(), &size, 0);
                if (property == CR_SUCCESS && type == DEVPROP_TYPE_STRING)
                {
                    const wchar_t* value =
                        reinterpret_cast<const wchar_t*>(data.data());
                    const std::wstring parent(value, wcsnlen(value,
                        data.size() / sizeof(wchar_t)));
                    if (!parent.empty()) return parent;
                }
            }

            DEVINST parent = 0;
            if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS) return {};
            wchar_t id[MAX_DEVICE_ID_LEN]{};
            return CM_Get_Device_IDW(parent, id, ARRAYSIZE(id), 0) == CR_SUCCESS
                ? std::wstring(id) : std::wstring();
        }

        std::wstring PhysicalParentForEndpoint(const std::wstring& endpoint)
        {
            const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            const bool uninitialize = SUCCEEDED(co);
            HRESULT hr = co == RPC_E_CHANGED_MODE ? S_OK : co;
            IMMDeviceEnumerator* enumerator = nullptr;
            IMMDeviceCollection* collection = nullptr;
            if (SUCCEEDED(hr)) hr = CoCreateInstance(__uuidof(MMDeviceEnumerator),
                nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                reinterpret_cast<void**>(&enumerator));
            if (SUCCEEDED(hr)) hr = enumerator->EnumAudioEndpoints(eRender,
                DEVICE_STATEMASK_ALL, &collection);
            UINT count = 0;
            if (SUCCEEDED(hr)) hr = collection->GetCount(&count);
            std::wstring parent;
            for (UINT index = 0; SUCCEEDED(hr) && index < count && parent.empty(); ++index)
            {
                IMMDevice* device = nullptr;
                LPWSTR id = nullptr;
                if (SUCCEEDED(collection->Item(index, &device)) && device &&
                    SUCCEEDED(device->GetId(&id)) && id &&
                    _wcsicmp(EndpointRegistryKeyFromId(id).c_str(), endpoint.c_str()) == 0)
                {
                    const std::wstring instance = DevicePropertyString(device,
                        PKEY_Device_InstanceId);
                    parent = StartsWithInsensitive(instance, L"SWD\\MMDEVAPI\\")
                        ? ParentDeviceInstanceId(instance) : instance;
                }
                if (id) CoTaskMemFree(id);
                if (device) device->Release();
            }
            if (collection) collection->Release();
            if (enumerator) enumerator->Release();
            if (uninitialize) CoUninitialize();
            return parent;
        }

        struct ApoComponentBackup
        {
            std::wstring keyName;
            std::wstring instanceId;
            bool wasDisabled = false;
        };

        std::vector<ApoComponentBackup> ReadApoComponentBackups()
        {
            std::vector<ApoComponentBackup> result;
            HKEY root = nullptr;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kComponentBackupPath, 0,
                KEY_READ | KEY_WOW64_64KEY, &root) != ERROR_SUCCESS) return result;
            DWORD index = 0;
            wchar_t name[64]{};
            while (true)
            {
                DWORD length = ARRAYSIZE(name);
                const LONG enumerate = RegEnumKeyExW(root, index++, name, &length,
                    nullptr, nullptr, nullptr, nullptr);
                if (enumerate == ERROR_NO_MORE_ITEMS) break;
                if (enumerate != ERROR_SUCCESS) continue;
                HKEY item = nullptr;
                if (RegOpenKeyExW(root, name, 0, KEY_READ, &item) != ERROR_SUCCESS)
                    continue;
                ApoComponentBackup backup;
                backup.keyName.assign(name, length);
                DWORD disabled = 0;
                if (QueryRegistryStringValue(item, L"InstanceId", backup.instanceId) &&
                    QueryDword(item, L"WasDisabled", disabled))
                {
                    backup.wasDisabled = disabled != 0;
                    result.push_back(std::move(backup));
                }
                RegCloseKey(item);
            }
            RegCloseKey(root);
            return result;
        }

        bool RecordApoComponent(const std::wstring& instanceId, bool wasDisabled)
        {
            const auto existing = ReadApoComponentBackups();
            if (std::any_of(existing.begin(), existing.end(), [&](const auto& item)
                { return _wcsicmp(item.instanceId.c_str(), instanceId.c_str()) == 0; }))
                return true;

            HKEY root = nullptr;
            DWORD disposition = 0;
            if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kComponentBackupPath, 0, nullptr, 0,
                KEY_READ | KEY_WRITE | KEY_WOW64_64KEY, nullptr, &root,
                &disposition) != ERROR_SUCCESS) return false;
            DWORD next = 0;
            QueryDword(root, L"Next", next);
            const std::wstring keyName = std::to_wstring(next++);
            HKEY item = nullptr;
            const LONG created = RegCreateKeyExW(root, keyName.c_str(), 0, nullptr, 0,
                KEY_WRITE, nullptr, &item, &disposition);
            bool ok = created == ERROR_SUCCESS;
            if (ok)
            {
                ok = SetRegistryStringValue(item, L"InstanceId", instanceId) &&
                    SetDword(item, L"WasDisabled", wasDisabled ? 1 : 0) &&
                    SetDword(root, L"Next", next);
                RegCloseKey(item);
            }
            RegCloseKey(root);
            return ok;
        }

        void DeleteApoComponentBackup(const std::wstring& keyName)
        {
            HKEY root = nullptr;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kComponentBackupPath, 0,
                KEY_WRITE | KEY_WOW64_64KEY, &root) == ERROR_SUCCESS)
            {
                RegDeleteTreeW(root, keyName.c_str());
                RegCloseKey(root);
            }
        }

        bool DevNodeDisabled(DEVINST node, bool& disabled)
        {
            ULONG status = 0;
            ULONG problem = 0;
            if (CM_Get_DevNode_Status(&status, &problem, node, 0) != CR_SUCCESS)
                return false;
            disabled = problem == CM_PROB_DISABLED;
            return true;
        }

        bool SetApoComponentEnabled(const std::wstring& instanceId, bool enabled)
        {
            DEVINST node = 0;
            CONFIGRET located = CM_Locate_DevNodeW(&node,
                const_cast<wchar_t*>(instanceId.c_str()), CM_LOCATE_DEVNODE_NORMAL);
            if (located != CR_SUCCESS)
                located = CM_Locate_DevNodeW(&node,
                    const_cast<wchar_t*>(instanceId.c_str()), CM_LOCATE_DEVNODE_PHANTOM);
            if (located != CR_SUCCESS) return located == CR_NO_SUCH_DEVNODE;
            const CONFIGRET result = enabled
                ? CM_Enable_DevNode(node, 0)
                : CM_Disable_DevNode(node, CM_DISABLE_PERSIST);
            if (result != CR_SUCCESS) return false;
            for (DWORD attempt = 0; attempt < 20; ++attempt)
            {
                bool disabled = false;
                if (DevNodeDisabled(node, disabled) && disabled == !enabled) return true;
                Sleep(50);
            }
            return false;
        }

        struct PresentApoComponent
        {
            std::wstring instanceId;
            std::wstring parentId;
            DEVINST node = 0;
            bool disabled = false;
        };

        std::wstring DeviceParentId(DEVINST node)
        {
            DEVPROPTYPE type = 0;
            ULONG size = 0;
            CONFIGRET property = CM_Get_DevNode_PropertyW(node,
                &kDeviceParentProperty, &type, nullptr, &size, 0);
            if (property == CR_BUFFER_SMALL && type == DEVPROP_TYPE_STRING &&
                size >= sizeof(wchar_t))
            {
                std::vector<BYTE> data(size, 0);
                property = CM_Get_DevNode_PropertyW(node, &kDeviceParentProperty,
                    &type, data.data(), &size, 0);
                if (property == CR_SUCCESS && type == DEVPROP_TYPE_STRING)
                {
                    const wchar_t* value = reinterpret_cast<const wchar_t*>(data.data());
                    return std::wstring(value, wcsnlen(value,
                        data.size() / sizeof(wchar_t)));
                }
            }

            DEVINST parent = 0;
            wchar_t parentId[MAX_DEVICE_ID_LEN]{};
            if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS ||
                CM_Get_Device_IDW(parent, parentId, ARRAYSIZE(parentId), 0) != CR_SUCCESS)
                return {};
            return parentId;
        }

        std::wstring ApoComponentParentId(const std::wstring& instanceId)
        {
            DEVINST node = 0;
            CONFIGRET located = CM_Locate_DevNodeW(&node,
                const_cast<wchar_t*>(instanceId.c_str()), CM_LOCATE_DEVNODE_NORMAL);
            if (located != CR_SUCCESS)
                located = CM_Locate_DevNodeW(&node,
                    const_cast<wchar_t*>(instanceId.c_str()), CM_LOCATE_DEVNODE_PHANTOM);
            return located == CR_SUCCESS ? DeviceParentId(node) : std::wstring();
        }

        std::vector<PresentApoComponent> PresentApoComponents()
        {
            std::vector<PresentApoComponent> result;
            HDEVINFO set = SetupDiGetClassDevsW(&kAudioProcessingObjectClass,
                nullptr, nullptr, DIGCF_PRESENT);
            if (set == INVALID_HANDLE_VALUE) return result;
            SP_DEVINFO_DATA device{ sizeof(device) };
            for (DWORD index = 0; SetupDiEnumDeviceInfo(set, index, &device); ++index)
            {
                wchar_t instanceId[MAX_DEVICE_ID_LEN]{};
                if (!SetupDiGetDeviceInstanceIdW(set, &device, instanceId,
                    ARRAYSIZE(instanceId), nullptr)) continue;
                const std::wstring parentId = DeviceParentId(device.DevInst);
                if (parentId.empty()) continue;
                bool disabled = false;
                if (!DevNodeDisabled(device.DevInst, disabled)) continue;
                result.push_back({ instanceId, parentId, device.DevInst, disabled });
            }
            SetupDiDestroyDeviceInfoList(set);
            return result;
        }

        bool ContainsInsensitive(const std::vector<std::wstring>& values,
            const std::wstring& wanted);

        bool ApplyApoComponentIsolation(const std::vector<std::wstring>& targetParents,
            bool& changed)
        {
            bool ok = true;
            std::vector<PresentApoComponent> components;
            for (DWORD pass = 0; pass < 8; ++pass)
            {
                components = PresentApoComponents();
                bool foundEnabled = false;
                for (const auto& component : components)
                {
                    if (!ContainsInsensitive(targetParents, component.parentId)) continue;
                    if (!RecordApoComponent(component.instanceId, component.disabled))
                    {
                        ok = false;
                        continue;
                    }
                    if (!component.disabled)
                    {
                        foundEnabled = true;
                        if (SetApoComponentEnabled(component.instanceId, false)) changed = true;
                        else ok = false;
                    }
                }
                if (!foundEnabled || !ok) break;
                Sleep(150);
            }

            components = PresentApoComponents();
            for (const auto& component : components)
                if (ContainsInsensitive(targetParents, component.parentId) &&
                    !component.disabled) ok = false;

            for (const auto& backup : ReadApoComponentBackups())
            {
                const auto component = std::find_if(components.begin(), components.end(),
                    [&](const auto& item)
                    {
                        return _wcsicmp(item.instanceId.c_str(),
                            backup.instanceId.c_str()) == 0;
                    });
                if (component != components.end() &&
                    ContainsInsensitive(targetParents, component->parentId)) continue;
                if (component == components.end())
                {
                    const std::wstring parent = ApoComponentParentId(backup.instanceId);
                    if (!parent.empty() && ContainsInsensitive(targetParents, parent))
                        continue;
                }
                if (!backup.wasDisabled && !SetApoComponentEnabled(backup.instanceId, true))
                {
                    ok = false;
                    continue;
                }
                if (!backup.wasDisabled) changed = true;
                DeleteApoComponentBackup(backup.keyName);
            }
            return ok;
        }

        bool VerifyApoComponentIsolation(const std::vector<std::wstring>& targetParents)
        {
            for (const auto& component : PresentApoComponents())
                if (ContainsInsensitive(targetParents, component.parentId) &&
                    !component.disabled) return false;
            return true;
        }

        bool RestoreAllApoComponents(bool& changed)
        {
            bool ok = true;
            for (const auto& backup : ReadApoComponentBackups())
            {
                if (!backup.wasDisabled)
                {
                    if (!SetApoComponentEnabled(backup.instanceId, true))
                    {
                        ok = false;
                        continue;
                    }
                    changed = true;
                }
                DeleteApoComponentBackup(backup.keyName);
            }
            return ok;
        }

        bool EnablePrivilege(const wchar_t* name)
        {
            HANDLE token = nullptr;
            if (!OpenProcessToken(GetCurrentProcess(),
                TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return false;
            LUID luid{};
            if (!LookupPrivilegeValueW(nullptr, name, &luid))
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
                sizeof(privileges), nullptr, nullptr) != FALSE;
            const DWORD error = GetLastError();
            CloseHandle(token);
            return adjusted && error == ERROR_SUCCESS;
        }

        DWORD CurrentTokenSid(std::vector<BYTE>& storage, PSID& sid)
        {
            sid = nullptr;
            HANDLE token = nullptr;
            if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
                return GetLastError();
            DWORD size = 0;
            GetTokenInformation(token, TokenUser, nullptr, 0, &size);
            DWORD error = GetLastError();
            if (error != ERROR_INSUFFICIENT_BUFFER || size == 0)
            {
                CloseHandle(token);
                return error;
            }
            storage.resize(size);
            if (!GetTokenInformation(token, TokenUser, storage.data(), size, &size))
            {
                error = GetLastError();
                CloseHandle(token);
                return error;
            }
            CloseHandle(token);
            sid = reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid;
            return IsValidSid(sid) ? ERROR_SUCCESS : ERROR_INVALID_SID;
        }

        SECURITY_INFORMATION DaclSecurityInformation(PSECURITY_DESCRIPTOR descriptor)
        {
            SECURITY_DESCRIPTOR_CONTROL control = 0;
            DWORD revision = 0;
            GetSecurityDescriptorControl(descriptor, &control, &revision);
            return DACL_SECURITY_INFORMATION |
                ((control & SE_DACL_PROTECTED) != 0
                    ? PROTECTED_DACL_SECURITY_INFORMATION
                    : UNPROTECTED_DACL_SECURITY_INFORMATION);
        }

        DWORD CreateFxPropertiesWithPreservedSecurity(HKEY renderRoot,
            const std::wstring& endpoint, REGSAM access, HKEY* fx)
        {
            *fx = nullptr;
            HKEY endpointKey = nullptr;
            LONG result = RegOpenKeyExW(renderRoot, endpoint.c_str(), 0,
                KEY_CREATE_SUB_KEY, &endpointKey);
            if (result == ERROR_SUCCESS)
            {
                DWORD disposition = 0;
                result = RegCreateKeyExW(endpointKey, L"FxProperties", 0, nullptr, 0,
                    access, nullptr, fx, &disposition);
                RegCloseKey(endpointKey);
                return result;
            }
            if (result != ERROR_ACCESS_DENIED) return result;

            // MMDevices deliberately grants administrators value-level access but
            // not subkey creation.  A few generic/USB endpoints legitimately have
            // no FxProperties key.  Change only that endpoint's DACL for the duration
            // of key creation and restore its byte-for-byte owner/DACL afterwards.
            // The temporary ACE is non-inheritable, so the new key receives only the
            // endpoint's original Windows service ACLs.
            EnablePrivilege(SE_TAKE_OWNERSHIP_NAME);
            EnablePrivilege(SE_RESTORE_NAME);

            bool ownerChanged = false;
            HKEY securityKey = nullptr;
            result = RegOpenKeyExW(renderRoot, endpoint.c_str(), 0,
                READ_CONTROL | WRITE_DAC, &securityKey);

            PSECURITY_DESCRIPTOR originalDescriptor = nullptr;
            PSID originalOwner = nullptr;
            PACL originalDacl = nullptr;
            if (result != ERROR_SUCCESS)
            {
                HKEY ownerKey = nullptr;
                result = RegOpenKeyExW(renderRoot, endpoint.c_str(), 0,
                    READ_CONTROL | WRITE_OWNER, &ownerKey);
                if (result != ERROR_SUCCESS) return result;
                result = GetSecurityInfo(ownerKey, SE_REGISTRY_KEY,
                    OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                    &originalOwner, nullptr, &originalDacl, nullptr,
                    &originalDescriptor);
                if (result == ERROR_SUCCESS)
                {
                    std::vector<BYTE> sidStorage;
                    PSID currentSid = nullptr;
                    result = CurrentTokenSid(sidStorage, currentSid);
                    if (result == ERROR_SUCCESS)
                    {
                        result = SetSecurityInfo(ownerKey, SE_REGISTRY_KEY,
                            OWNER_SECURITY_INFORMATION, currentSid, nullptr,
                            nullptr, nullptr);
                        ownerChanged = result == ERROR_SUCCESS;
                    }
                }
                RegCloseKey(ownerKey);
                if (result == ERROR_SUCCESS)
                    result = RegOpenKeyExW(renderRoot, endpoint.c_str(), 0,
                        READ_CONTROL | WRITE_DAC, &securityKey);
            }
            else
            {
                result = GetSecurityInfo(securityKey, SE_REGISTRY_KEY,
                    OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                    &originalOwner, nullptr, &originalDacl, nullptr,
                    &originalDescriptor);
            }

            PACL writableDacl = nullptr;
            if (result == ERROR_SUCCESS)
            {
                std::vector<BYTE> sidStorage;
                PSID currentSid = nullptr;
                result = CurrentTokenSid(sidStorage, currentSid);
                if (result == ERROR_SUCCESS)
                {
                    EXPLICIT_ACCESSW entry{};
                    entry.grfAccessPermissions = KEY_CREATE_SUB_KEY;
                    entry.grfAccessMode = GRANT_ACCESS;
                    entry.grfInheritance = NO_INHERITANCE;
                    entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
                    entry.Trustee.TrusteeType = TRUSTEE_IS_USER;
                    entry.Trustee.ptstrName = reinterpret_cast<LPWSTR>(currentSid);
                    result = SetEntriesInAclW(1, &entry, originalDacl, &writableDacl);
                }
            }
            if (result == ERROR_SUCCESS)
            {
                result = SetSecurityInfo(securityKey, SE_REGISTRY_KEY,
                    DaclSecurityInformation(originalDescriptor), nullptr, nullptr,
                    writableDacl, nullptr);
            }
            if (result == ERROR_SUCCESS)
            {
                HKEY createKey = nullptr;
                result = RegOpenKeyExW(renderRoot, endpoint.c_str(), 0,
                    KEY_CREATE_SUB_KEY, &createKey);
                if (result == ERROR_SUCCESS)
                {
                    DWORD disposition = 0;
                    result = RegCreateKeyExW(createKey, L"FxProperties", 0,
                        nullptr, 0, access, nullptr, fx, &disposition);
                    RegCloseKey(createKey);
                }
            }

            DWORD restoreResult = ERROR_SUCCESS;
            if (securityKey && originalDescriptor)
            {
                restoreResult = SetSecurityInfo(securityKey, SE_REGISTRY_KEY,
                    DaclSecurityInformation(originalDescriptor), nullptr, nullptr,
                    originalDacl, nullptr);
            }
            if (securityKey) RegCloseKey(securityKey);
            if (writableDacl) LocalFree(writableDacl);

            if (ownerChanged && originalOwner)
            {
                HKEY ownerKey = nullptr;
                const LONG ownerOpen = RegOpenKeyExW(renderRoot, endpoint.c_str(), 0,
                    WRITE_OWNER, &ownerKey);
                if (ownerOpen == ERROR_SUCCESS)
                {
                    const DWORD ownerRestore = SetSecurityInfo(ownerKey,
                        SE_REGISTRY_KEY, OWNER_SECURITY_INFORMATION,
                        originalOwner, nullptr, nullptr, nullptr);
                    if (restoreResult == ERROR_SUCCESS) restoreResult = ownerRestore;
                    RegCloseKey(ownerKey);
                }
                else if (restoreResult == ERROR_SUCCESS) restoreResult = ownerOpen;
            }
            if (originalDescriptor) LocalFree(originalDescriptor);
            if (result == ERROR_SUCCESS && restoreResult != ERROR_SUCCESS)
            {
                if (*fx) { RegCloseKey(*fx); *fx = nullptr; }
                return restoreResult;
            }
            return result;
        }

        DWORD OpenEndpointFx(HKEY renderRoot, const std::wstring& endpoint,
            REGSAM access, bool create, HKEY* fx, DWORD* stage = nullptr)
        {
            if (stage) *stage = kStageOpenFxProperties;
            const std::wstring path = endpoint + L"\\FxProperties";
            LONG result = RegOpenKeyExW(renderRoot, path.c_str(), 0, access, fx);
            if (result == ERROR_FILE_NOT_FOUND && create)
            {
                if (stage) *stage = kStageCreateFxProperties;
                result = CreateFxPropertiesWithPreservedSecurity(renderRoot,
                    endpoint, access, fx);
            }
            return result;
        }

        bool ReadCompositeEffects(HKEY fx, std::vector<std::wstring>& effects)
        {
            DWORD type = 0;
            std::vector<BYTE> data;
            effects.clear();
            if (QueryRaw(fx, kCompositeValue, type, data))
                effects = DecodeStrings(type, data);
            effects.erase(std::remove_if(effects.begin(), effects.end(),
                [](const std::wstring& value) { return value.empty(); }), effects.end());
            return !effects.empty();
        }

        bool DeleteValueIfPresent(HKEY key, const wchar_t* name, bool& changed)
        {
            DWORD type = 0;
            std::vector<BYTE> data;
            if (!QueryRaw(key, name, type, data)) return true;
            const LONG result = RegDeleteValueW(key, name);
            if (result == ERROR_SUCCESS) changed = true;
            return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
        }

        bool ValueContainsApo(HKEY key, const wchar_t* name)
        {
            DWORD type = 0;
            std::vector<BYTE> data;
            return QueryRaw(key, name, type, data) &&
                ContainsClsid(DecodeStrings(type, data));
        }

        bool ValueContainsOnlyApo(HKEY key, const wchar_t* name)
        {
            DWORD type = 0;
            std::vector<BYTE> data;
            if (!QueryRaw(key, name, type, data)) return false;
            const auto values = DecodeStrings(type, data);
            return values.size() == 1 &&
                _wcsicmp(values.front().c_str(), kApoClsid) == 0;
        }

        bool ValueIsAbsent(HKEY key, const wchar_t* name)
        {
            DWORD type = 0;
            std::vector<BYTE> data;
            return !QueryRaw(key, name, type, data);
        }

        const std::vector<std::wstring>& RequiredRenderProcessingModes()
        {
            static const std::vector<std::wstring> modes{
                kDefaultProcessingMode,
            };
            return modes;
        }

        bool ProcessingModesCoverRenderModes(HKEY key, const wchar_t* name)
        {
            DWORD type = 0;
            std::vector<BYTE> data;
            if (!QueryRaw(key, name, type, data)) return false;
            const auto modes = DecodeStrings(type, data);
            const auto& required = RequiredRenderProcessingModes();
            // EFX is downstream of the mode mix. Windows specifies only
            // DEFAULT for PKEY_EFX_ProcessingModes_Supported_For_Streaming.
            return type == REG_MULTI_SZ && modes.size() == 1 &&
                required.size() == 1 && _wcsicmp(modes[0].c_str(), required[0].c_str()) == 0;
        }

        DWORD RestoreEndpoint(HKEY renderRoot, const std::wstring& endpoint,
            bool& changed, DWORD& stage);
        bool RemoveApoFromValue(HKEY fx, const wchar_t* name, bool& changed);
        bool RemoveApoFromComposite(HKEY fx, bool& changed);

        DWORD ApplyEndpoint(HKEY renderRoot, const std::wstring& endpoint,
            bool& changed, DWORD& stage)
        {
            changed = false;
            // The legacy writer removes other effect slots and component
            // associations. Do not touch even an apparently empty endpoint:
            // safe OEM coexistence has not been implemented.
            (void)renderRoot; (void)endpoint;
            stage = kStageExternalAssociations;
            return ERROR_NOT_SUPPORTED;
#if 0
            HKEY fx = nullptr;
            LONG result = OpenEndpointFx(renderRoot, endpoint,
                KEY_QUERY_VALUE | KEY_SET_VALUE, true, &fx, &stage);
            if (result != ERROR_SUCCESS) return result;

            const std::wstring backupPath = std::wstring(kBackupPath) + L"\\" + endpoint;
            HKEY backup = nullptr;
            DWORD disposition = 0;
            stage = kStageCreateBackup;
            result = RegCreateKeyExW(HKEY_LOCAL_MACHINE, backupPath.c_str(), 0, nullptr, 0,
                KEY_READ | KEY_WRITE | KEY_WOW64_64KEY, nullptr, &backup,
                &disposition);
            if (result != ERROR_SUCCESS)
            {
                RegCloseKey(fx);
                return result;
            }

            DWORD complete = 0;
            DWORD schemaVersion = 0;
            const bool completeBackup = QueryDword(backup, L"Complete", complete) &&
                complete == 1;
            if (completeBackup) QueryDword(backup, L"SchemaVersion", schemaVersion);
            stage = kStageWriteBackup;
            bool backupOk = true;
            if (!completeBackup)
            {
                for (const auto& value : kEffectValues)
                    backupOk = backupOk && BackupValue(backup, fx,
                        value.name, value.prefix);
                for (const auto& value : kProcessingModeValues)
                    backupOk = backupOk && BackupValue(backup, fx,
                        value.name, value.prefix);
                backupOk = backupOk &&
                    BackupValue(backup, fx, kDisableSysFxValue, L"Disable") &&
                    BackupValue(backup, fx, kAssociationValue, L"Association") &&
                    BackupExternalApoAssociations(backup, fx) &&
                    SetDword(backup, L"SchemaVersion", kBackupSchemaVersion) &&
                    SetDword(backup, L"Complete", 1);
                schemaVersion = kBackupSchemaVersion;
            }
            else if (schemaVersion < kBackupSchemaVersion)
            {
                if (schemaVersion < 2)
                {
                    backupOk = backupOk && BackupValue(backup, fx,
                        kEfxProcessingModesValue, L"EfxModes");
                    schemaVersion = 2;
                }
                if (schemaVersion < 3)
                {
                    backupOk = backupOk && BackupValue(backup, fx,
                        kSingleEndpointValue, L"SingleEndpoint");
                    schemaVersion = 3;
                }
                if (schemaVersion < 4)
                {
                    backupOk = backupOk && BackupValue(backup, fx,
                        kSfxProcessingModesValue, L"SfxModes") &&
                        BackupValue(backup, fx, kSingleStreamValue,
                            L"SingleStream");
                    schemaVersion = 4;
                }
                if (schemaVersion < 5)
                {
                    backupOk = backupOk && BackupValue(backup, fx,
                        kAssociationValue, L"Association");
                    schemaVersion = 5;
                }
                // Schema 5 already preserves HXAudio's old SFX/EFX writes.
                // Capture only properties that version 1.0.0 never touched, so
                // the original OEM chain remains exact across this migration.
                if (schemaVersion < 6)
                {
                    const BackedValue additions[] =
                    {
                        { kPreMixValue, L"PreMix" },
                        { kPostMixValue, L"PostMix" },
                        { kSingleModeValue, L"SingleMode" },
                        { kOffloadStreamValue, L"OffloadStream" },
                        { kOffloadModeValue, L"OffloadMode" },
                        { kCompositeStreamValue, L"CompositeStream" },
                        { kCompositeModeValue, L"CompositeMode" },
                        { kCompositeOffloadStreamValue, L"CompositeOffloadStream" },
                        { kCompositeOffloadModeValue, L"CompositeOffloadMode" },
                        { kMfxProcessingModesValue, L"MfxModes" },
                        { kOffloadSfxProcessingModesValue, L"OffloadSfxModes" },
                        { kOffloadMfxProcessingModesValue, L"OffloadMfxModes" },
                    };
                    for (const auto& value : additions)
                        backupOk = backupOk && BackupValue(backup, fx,
                            value.name, value.prefix);
                    schemaVersion = 6;
                }
                if (schemaVersion < 7)
                    backupOk = backupOk && BackupExternalApoAssociations(backup, fx);
                backupOk = backupOk && SetDword(backup, L"SchemaVersion",
                    kBackupSchemaVersion);
                schemaVersion = kBackupSchemaVersion;
            }
            if (!backupOk || schemaVersion != kBackupSchemaVersion)
            {
                RegCloseKey(backup);
                RegCloseKey(fx);
                return ERROR_WRITE_FAULT;
            }

            // DCH drivers can add association properties after the endpoint's
            // first installation (for example after an OEM component update).
            // Merge every newly observed association into the original-state
            // backup before removing it from the active graph.
            if (!BackupExternalApoAssociations(backup, fx))
            {
                RegCloseKey(backup);
                RegCloseKey(fx);
                return ERROR_WRITE_FAULT;
            }

            // The association is endpoint topology metadata, not an effect.
            // Preserve it exactly while replacing every software effect slot.
            if (!RestoreValue(backup, fx, kAssociationValue,
                L"Association", changed))
            {
                RegCloseKey(backup);
                RegCloseKey(fx);
                return ERROR_WRITE_FAULT;
            }

            stage = kStageExternalAssociations;
            if (!RemoveExternalApoAssociations(fx, changed))
            {
                RegCloseKey(backup);
                RegCloseKey(fx);
                return ERROR_WRITE_FAULT;
            }

            stage = kStageWriteComposite;
            for (const auto& value : kEffectValues)
            {
                if (_wcsicmp(value.name, kSingleEndpointValue) == 0) continue;
                if (!DeleteValueIfPresent(fx, value.name, changed))
                {
                    RegCloseKey(backup);
                    RegCloseKey(fx);
                    return ERROR_WRITE_FAULT;
                }
            }
            if (!ValueContainsOnlyApo(fx, kSingleEndpointValue))
            {
                result = RegSetValueExW(fx, kSingleEndpointValue, 0, REG_SZ,
                    reinterpret_cast<const BYTE*>(kApoClsid), sizeof(kApoClsid));
                if (result != ERROR_SUCCESS)
                {
                    RegCloseKey(backup);
                    RegCloseKey(fx);
                    return result;
                }
                changed = true;
            }

            stage = kStageWriteProcessingModes;
            for (const auto& value : kProcessingModeValues)
            {
                if (_wcsicmp(value.name, kEfxProcessingModesValue) == 0) continue;
                if (!DeleteValueIfPresent(fx, value.name, changed))
                {
                    RegCloseKey(backup);
                    RegCloseKey(fx);
                    return ERROR_WRITE_FAULT;
                }
            }
            if (!ProcessingModesCoverRenderModes(fx,
                kEfxProcessingModesValue))
            {
                const auto defaultMode = EncodeMultiString(
                    RequiredRenderProcessingModes());
                result = RegSetValueExW(fx, kEfxProcessingModesValue, 0,
                    REG_MULTI_SZ, defaultMode.data(),
                    static_cast<DWORD>(defaultMode.size()));
                if (result != ERROR_SUCCESS)
                {
                    RegCloseKey(backup);
                    RegCloseKey(fx);
                    return result;
                }
                changed = true;
            }

            DWORD disableType = 0;
            std::vector<BYTE> disableData;
            DWORD disabled = 1;
            if (QueryRaw(fx, kDisableSysFxValue, disableType, disableData) &&
                disableType == REG_DWORD && disableData.size() == sizeof(DWORD))
                CopyMemory(&disabled, disableData.data(), sizeof(disabled));
            if (disabled != 0)
            {
                stage = kStageEnableEnhancements;
                const DWORD enabled = 0;
                result = RegSetValueExW(fx, kDisableSysFxValue, 0, REG_DWORD,
                    reinterpret_cast<const BYTE*>(&enabled), sizeof(enabled));
                if (result != ERROR_SUCCESS)
                {
                    RegCloseKey(backup);
                    RegCloseKey(fx);
                    return result;
                }
                changed = true;
            }

            stage = kStageVerify;
            DWORD verifiedDisabled = 1;
            DWORD verifiedType = 0;
            std::vector<BYTE> verifiedData;
            if (QueryRaw(fx, kDisableSysFxValue, verifiedType, verifiedData) &&
                verifiedType == REG_DWORD && verifiedData.size() == sizeof(DWORD))
                CopyMemory(&verifiedDisabled, verifiedData.data(), sizeof(verifiedDisabled));
            bool exclusive = ValueContainsOnlyApo(fx, kSingleEndpointValue) &&
                ProcessingModesCoverRenderModes(fx,
                    kEfxProcessingModesValue) &&
                verifiedDisabled == 0 && ExternalApoAssociationsAbsent(fx);
            for (const auto& value : kEffectValues)
                if (_wcsicmp(value.name, kSingleEndpointValue) != 0)
                    exclusive = exclusive && ValueIsAbsent(fx, value.name);
            for (const auto& value : kProcessingModeValues)
                if (_wcsicmp(value.name, kEfxProcessingModesValue) != 0)
                    exclusive = exclusive && ValueIsAbsent(fx, value.name);
            RegCloseKey(backup);
            RegCloseKey(fx);
            return exclusive ? ERROR_SUCCESS : ERROR_INVALID_DATA;
        }

#endif
        }

        DWORD VerifyEndpoint(HKEY renderRoot, const std::wstring& endpoint,
            DWORD& stage, DWORD& failureMask)
        {
            failureMask = 0;
            stage = kStageOpenFxProperties;
            HKEY fx = nullptr;
            const LONG result = OpenEndpointFx(renderRoot, endpoint,
                KEY_QUERY_VALUE, false, &fx, &stage);
            if (result != ERROR_SUCCESS) return result;
            DWORD disabled = 1;
            QueryDword(fx, kDisableSysFxValue, disabled);
            // Report independent conditions instead of hiding them behind
            // a short-circuiting boolean expression and ERROR_NOT_FOUND.
            if (!ValueContainsOnlyApo(fx, kSingleEndpointValue))
                failureMask |= 1u;
            if (!ProcessingModesCoverRenderModes(fx, kEfxProcessingModesValue))
                failureMask |= 2u;
            if (disabled != 0)
                failureMask |= 4u;
            if (!ExternalApoAssociationsAbsent(fx))
                failureMask |= 8u;
            for (const auto& value : kEffectValues)
                if (_wcsicmp(value.name, kSingleEndpointValue) != 0 &&
                    !ValueIsAbsent(fx, value.name))
                    failureMask |= 16u;
            for (const auto& value : kProcessingModeValues)
                if (_wcsicmp(value.name, kEfxProcessingModesValue) != 0 &&
                    !ValueIsAbsent(fx, value.name))
                    failureMask |= 32u;
            RegCloseKey(fx);
            stage = kStageVerify;
            return failureMask == 0 ? ERROR_SUCCESS : ERROR_NOT_FOUND;
        }

        DWORD VerifyEndpointDetached(HKEY renderRoot,
            const std::wstring& endpoint, DWORD& stage)
        {
            stage = kStageOpenFxProperties;
            HKEY fx = nullptr;
            const LONG result = OpenEndpointFx(renderRoot, endpoint,
                KEY_QUERY_VALUE, false, &fx, &stage);
            if (result == ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;
            if (result != ERROR_SUCCESS) return result;
            bool detached = true;
            for (const auto& value : kEffectValues)
                detached = detached && !ValueContainsApo(fx, value.name);
            RegCloseKey(fx);
            stage = kStageVerify;
            return detached ? ERROR_SUCCESS : ERROR_ALREADY_EXISTS;
        }

        bool EnumerateEndpoints(bool apply, EndpointSummary& summary, bool& changed,
            DWORD& error, bool* endpointChangedResult = nullptr,
            bool* componentChangedResult = nullptr)
        {
            summary = {};
            changed = false;
            bool endpointChanges = false;
            bool componentChanges = false;
            error = ERROR_SUCCESS;
            const EndpointSelection selection = LoadEndpointSelection();
            std::vector<std::wstring> targetParents;
            HKEY render = nullptr;
            const LONG open = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRenderPath, 0,
                KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE | KEY_WOW64_64KEY, &render);
            if (open != ERROR_SUCCESS)
            {
                error = open;
                return false;
            }

            DWORD index = 0;
            wchar_t name[512]{};
            while (true)
            {
                DWORD length = ARRAYSIZE(name);
                const LONG result = RegEnumKeyExW(render, index++, name, &length,
                    nullptr, nullptr, nullptr, nullptr);
                if (result == ERROR_NO_MORE_ITEMS) break;
                if (result != ERROR_SUCCESS)
                {
                    ++summary.failed;
                    if (summary.firstError == ERROR_SUCCESS)
                    {
                        summary.firstError = result;
                        summary.failureStage = kStageEnumerate;
                    }
                    continue;
                }
                const std::wstring endpoint(name, length);
                const bool active = EndpointIsActive(render, endpoint);
                const bool shouldAttach = selection.Includes(endpoint) &&
                    EndpointIsTerminalHardware(render, endpoint);
                if (shouldAttach)
                {
                    const std::wstring parent = PhysicalParentForEndpoint(endpoint);
                    if (!parent.empty() && !ContainsInsensitive(targetParents, parent))
                        targetParents.push_back(parent);
                }
                ++summary.enumerated;
                if (active) ++summary.active;
                bool endpointChanged = false;
                DWORD endpointStage = kStageNone;
                DWORD endpointVerifyMask = 0;
                const DWORD endpointError = apply
                    ? (shouldAttach
                        ? ApplyEndpoint(render, endpoint, endpointChanged, endpointStage)
                        : RestoreEndpoint(render, endpoint, endpointChanged, endpointStage))
                    : (shouldAttach
                        ? VerifyEndpoint(render, endpoint, endpointStage, endpointVerifyMask)
                        : VerifyEndpointDetached(render, endpoint, endpointStage));
                if (endpointError == ERROR_SUCCESS)
                {
                    if (shouldAttach)
                    {
                        ++summary.succeeded;
                        if (active) ++summary.activeSucceeded;
                    }
                    endpointChanges = endpointChanges || endpointChanged;
                }
                else
                {
                    ++summary.failed;
                    if (summary.firstError == ERROR_SUCCESS)
                    {
                        summary.firstError = endpointError;
                        summary.failureStage = endpointStage;
                        summary.verifyFailureMask = endpointVerifyMask;
                        summary.failureEndpoint = endpoint;
                    }
                }
            }
            RegCloseKey(render);

            if (summary.failed == 0)
            {
                bool componentsOk = false;
                if (apply)
                    componentsOk = ApplyApoComponentIsolation(targetParents,
                        componentChanges);
                else
                    componentsOk = VerifyApoComponentIsolation(targetParents);
                if (!componentsOk)
                {
                    ++summary.failed;
                    summary.firstError = ERROR_NOT_SUPPORTED;
                    summary.failureStage = kStageApoComponents;
                }
            }

            if (summary.failed != 0)
            {
                error = summary.firstError != ERROR_SUCCESS
                    ? summary.firstError : ERROR_PARTIAL_COPY;
                return false;
            }
            changed = endpointChanges || componentChanges;
            if (endpointChangedResult) *endpointChangedResult = endpointChanges;
            if (componentChangedResult) *componentChangedResult = componentChanges;
            return true;
        }

        bool RemoveApoFromComposite(HKEY fx, bool& changed)
        {
            return RemoveApoFromValue(fx, kCompositeValue, changed);
        }

        bool RemoveApoFromValue(HKEY fx, const wchar_t* name, bool& changed)
        {
            DWORD type = 0;
            std::vector<BYTE> data;
            if (!QueryRaw(fx, name, type, data)) return true;
            auto effects = DecodeStrings(type, data);
            if (!ContainsClsid(effects)) return true;
            effects.erase(std::remove_if(effects.begin(), effects.end(),
                [](const std::wstring& value)
                {
                    return _wcsicmp(value.c_str(), kApoClsid) == 0;
                }), effects.end());
            LONG result = ERROR_SUCCESS;
            if (effects.empty()) result = RegDeleteValueW(fx, name);
            else
            {
                const auto encoded = EncodeMultiString(effects);
                result = RegSetValueExW(fx, name, 0, REG_MULTI_SZ,
                    encoded.data(), static_cast<DWORD>(encoded.size()));
            }
            if (result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS;
            if (result == ERROR_SUCCESS) changed = true;
            return result == ERROR_SUCCESS;
        }

        DWORD RestoreEndpoint(HKEY renderRoot, const std::wstring& endpoint,
            bool& changed, DWORD& stage)
        {
            changed = false;
            stage = kStageRestore;
            HKEY fx = nullptr;
            const LONG fxOpen = OpenEndpointFx(renderRoot, endpoint,
                KEY_QUERY_VALUE | KEY_SET_VALUE, false, &fx, &stage);
            if (fxOpen == ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;
            if (fxOpen != ERROR_SUCCESS) return fxOpen;

            const std::wstring backupPath = std::wstring(kBackupPath) + L"\\" + endpoint;
            HKEY backup = nullptr;
            const bool hasBackup = RegOpenKeyExW(HKEY_LOCAL_MACHINE, backupPath.c_str(), 0,
                KEY_READ | KEY_WOW64_64KEY, &backup) == ERROR_SUCCESS;
            DWORD complete = 0;
            bool ok = true;
            bool restoredCompleteSchema = false;
            if (hasBackup && QueryDword(backup, L"Complete", complete) && complete == 1)
            {
                DWORD schemaVersion = 0;
                QueryDword(backup, L"SchemaVersion", schemaVersion);
                if (schemaVersion >= kBackupSchemaVersion)
                {
                    for (const auto& value : kEffectValues)
                        ok = ok && RestoreValue(backup, fx, value.name,
                            value.prefix, changed);
                    for (const auto& value : kProcessingModeValues)
                        ok = ok && RestoreValue(backup, fx, value.name,
                            value.prefix, changed);
                    ok = ok && RestoreValue(backup, fx, kDisableSysFxValue,
                        L"Disable", changed) && RestoreValue(backup, fx,
                            kAssociationValue, L"Association", changed) &&
                        RestoreExternalApoAssociations(backup, fx, changed);
                    restoredCompleteSchema = ok;
                }
                else
                {
                    ok = RestoreValue(backup, fx, kCompositeValue, L"Composite",
                        changed) && RestoreValue(backup, fx, kDisableSysFxValue,
                            L"Disable", changed);
                    if (ok && schemaVersion >= 2)
                        ok = RestoreValue(backup, fx, kEfxProcessingModesValue,
                            L"EfxModes", changed);
                    if (ok && schemaVersion >= 3)
                        ok = RestoreValue(backup, fx, kSingleEndpointValue,
                            L"SingleEndpoint", changed);
                    if (ok && schemaVersion >= 4)
                        ok = RestoreValue(backup, fx, kSfxProcessingModesValue,
                            L"SfxModes", changed) && RestoreValue(backup, fx,
                                kSingleStreamValue, L"SingleStream", changed);
                    if (ok && schemaVersion >= 5)
                        ok = RestoreValue(backup, fx, kAssociationValue,
                            L"Association", changed);
                }
            }
            // Without a complete schema-7 backup, remove HXAudio from every
            // possible effect slot while leaving all unrelated OEM entries intact.
            if (ok && !restoredCompleteSchema)
                for (const auto& value : kEffectValues)
                    ok = ok && RemoveApoFromValue(fx, value.name, changed);
            if (ok && !restoredCompleteSchema)
                for (const auto& value : kEffectValues)
                    ok = ok && !ValueContainsApo(fx, value.name);
            if (backup) RegCloseKey(backup);
            RegCloseKey(fx);
            return ok ? ERROR_SUCCESS : ERROR_WRITE_FAULT;
        }

        bool RestoreAllEndpoints(EndpointSummary& summary, bool& changed, DWORD& error)
        {
            summary = {};
            changed = false;
            error = ERROR_SUCCESS;
            HKEY render = nullptr;
            const LONG open = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRenderPath, 0,
                KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE | KEY_WOW64_64KEY, &render);
            if (open != ERROR_SUCCESS)
            {
                error = open;
                return false;
            }
            DWORD index = 0;
            wchar_t name[512]{};
            while (true)
            {
                DWORD length = ARRAYSIZE(name);
                const LONG result = RegEnumKeyExW(render, index++, name, &length,
                    nullptr, nullptr, nullptr, nullptr);
                if (result == ERROR_NO_MORE_ITEMS) break;
                if (result != ERROR_SUCCESS)
                {
                    ++summary.failed;
                    if (summary.firstError == ERROR_SUCCESS)
                    {
                        summary.firstError = result;
                        summary.failureStage = kStageEnumerate;
                    }
                    continue;
                }
                const std::wstring endpoint(name, length);
                const bool active = EndpointIsActive(render, endpoint);
                ++summary.enumerated;
                if (active) ++summary.active;
                bool endpointChanged = false;
                DWORD endpointStage = kStageNone;
                const DWORD endpointError = RestoreEndpoint(render, endpoint,
                    endpointChanged, endpointStage);
                if (endpointError == ERROR_SUCCESS)
                {
                    ++summary.succeeded;
                    if (active) ++summary.activeSucceeded;
                    changed = changed || endpointChanged;
                }
                else
                {
                    ++summary.failed;
                    if (summary.firstError == ERROR_SUCCESS)
                    {
                        summary.firstError = endpointError;
                        summary.failureStage = endpointStage;
                        summary.failureEndpoint = endpoint;
                    }
                }
            }
            RegCloseKey(render);
            if (summary.failed == 0 && !RestoreAllApoComponents(changed))
            {
                ++summary.failed;
                summary.firstError = ERROR_NOT_SUPPORTED;
                summary.failureStage = kStageApoComponents;
            }
            if (summary.failed != 0)
            {
                error = summary.firstError != ERROR_SUCCESS
                    ? summary.firstError : ERROR_PARTIAL_COPY;
                return false;
            }
            return true;
        }

        using RegisterFunction = HRESULT (STDAPICALLTYPE*)();

        HRESULT CallDllExport(const std::wstring& path, const char* name)
        {
            if (path.empty()) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
            HMODULE module = LoadLibraryExW(path.c_str(), nullptr,
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!module) module = LoadLibraryW(path.c_str());
            if (!module) return HRESULT_FROM_WIN32(GetLastError());
            auto function = reinterpret_cast<RegisterFunction>(GetProcAddress(module, name));
            const HRESULT result = function ? function() : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
            FreeLibrary(module);
            return result;
        }

        void DeleteUnusedApoDlls(const std::wstring& referencePath,
            const std::wstring& keepPath)
        {
            const size_t slash = referencePath.find_last_of(L"\\/");
            if (slash == std::wstring::npos) return;
            const std::wstring directory = referencePath.substr(0, slash + 1);
            const std::wstring pattern = directory + L"HXAudioApo*.dll";
            WIN32_FIND_DATAW data{};
            HANDLE find = FindFirstFileW(pattern.c_str(), &data);
            if (find == INVALID_HANDLE_VALUE) return;
            do
            {
                if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
                const std::wstring candidate = directory + data.cFileName;
                if (!keepPath.empty() && _wcsicmp(candidate.c_str(), keepPath.c_str()) == 0)
                    continue;
                DeleteFileW(candidate.c_str());
            } while (FindNextFileW(find, &data));
            FindClose(find);
        }

        bool WaitForService(SC_HANDLE service, DWORD wanted, DWORD timeout)
        {
            const ULONGLONG deadline = GetTickCount64() + timeout;
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

        bool ServiceIsRunning(SC_HANDLE service)
        {
            SERVICE_STATUS_PROCESS status{};
            DWORD bytes = 0;
            return QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                reinterpret_cast<BYTE*>(&status), sizeof(status), &bytes) &&
                status.dwCurrentState == SERVICE_RUNNING;
        }

        bool StopServiceTree(SC_HANDLE manager, const std::wstring& name,
            std::vector<std::wstring>& stopped)
        {
            if (std::find_if(stopped.begin(), stopped.end(), [&](const std::wstring& item)
                { return _wcsicmp(item.c_str(), name.c_str()) == 0; }) != stopped.end())
                return true;
            SC_HANDLE service = OpenServiceW(manager, name.c_str(),
                SERVICE_QUERY_STATUS | SERVICE_ENUMERATE_DEPENDENTS |
                SERVICE_STOP | SERVICE_START);
            if (!service) return false;
            if (!ServiceIsRunning(service))
            {
                CloseServiceHandle(service);
                return true;
            }

            DWORD bytesNeeded = 0;
            DWORD count = 0;
            EnumDependentServicesW(service, SERVICE_ACTIVE, nullptr, 0,
                &bytesNeeded, &count);
            if (GetLastError() == ERROR_MORE_DATA && bytesNeeded != 0)
            {
                std::vector<BYTE> buffer(bytesNeeded);
                auto entries = reinterpret_cast<ENUM_SERVICE_STATUSW*>(buffer.data());
                if (EnumDependentServicesW(service, SERVICE_ACTIVE, entries, bytesNeeded,
                    &bytesNeeded, &count))
                {
                    for (DWORD i = 0; i < count; ++i)
                    {
                        if (!StopServiceTree(manager, entries[i].lpServiceName, stopped))
                        {
                            CloseServiceHandle(service);
                            return false;
                        }
                    }
                }
            }

            SERVICE_STATUS status{};
            const bool requested = ControlService(service, SERVICE_CONTROL_STOP, &status) ||
                GetLastError() == ERROR_SERVICE_NOT_ACTIVE;
            const bool stoppedOk = requested && WaitForService(service, SERVICE_STOPPED, 20000);
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
                ok = (started && WaitForService(service, SERVICE_RUNNING, 20000)) && ok;
                CloseServiceHandle(service);
            }
            return ok;
        }

        bool EnsureServiceRunning(SC_HANDLE manager, const wchar_t* name)
        {
            SC_HANDLE service = OpenServiceW(manager, name,
                SERVICE_QUERY_STATUS | SERVICE_START);
            if (!service) return false;
            const bool started = ServiceIsRunning(service) || StartServiceW(service, 0, nullptr) ||
                GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;
            const bool running = started && WaitForService(service, SERVICE_RUNNING, 20000);
            CloseServiceHandle(service);
            return running;
        }

        bool RestartSharedAudioEngine()
        {
            SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
            if (!manager) return false;
            std::vector<std::wstring> stopped;
            // A binary-only APO handoff does not require rematerializing the
            // endpoint devices. Restart AudioSrv and its dependents so new
            // audiodg graphs resolve the content-addressed DLL, while the
            // EndpointBuilder-owned device objects stay continuously present.
            const bool stoppedOk = StopServiceTree(manager, L"AudioSrv", stopped);
            const bool startedOk = stoppedOk && StartStoppedServices(manager, stopped);
            const bool builderRunning = EnsureServiceRunning(manager,
                L"AudioEndpointBuilder");
            const bool audioRunning = builderRunning && EnsureServiceRunning(manager,
                L"AudioSrv");
            CloseServiceHandle(manager);
            return startedOk && audioRunning;
        }

        bool RestartEndpointStack()
        {
            SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
            if (!manager) return false;
            std::vector<std::wstring> stopped;
            // FX registry values are consumed by AudioEndpointBuilder when it
            // materializes each endpoint's effect property store.  Restarting
            // AudioSrv alone recreates audiodg but can reuse that stale store,
            // yielding a loaded DLL that is never inserted into the graph.
            // Stop the builder and its dependent services as one tree, then
            // start them in reverse order so every Windows 10/11 endpoint is
            // rebuilt from the new Composite FX configuration without a reboot.
            const bool stoppedOk = StopServiceTree(manager,
                L"AudioEndpointBuilder", stopped);
            const bool startedOk = stoppedOk && StartStoppedServices(manager, stopped);
            const bool builderRunning = EnsureServiceRunning(manager, L"AudioEndpointBuilder");
            const bool audioRunning = builderRunning && EnsureServiceRunning(manager, L"AudioSrv");
            CloseServiceHandle(manager);
            return startedOk && audioRunning;
        }

        void SetServiceStatusValue(DWORD state, DWORD error = NO_ERROR, DWORD hint = 0)
        {
            g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
            g_status.dwCurrentState = state;
            g_status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP : 0;
            g_status.dwWin32ExitCode = error;
            g_status.dwWaitHint = hint;
            SetServiceStatus(g_statusHandle, &g_status);
        }

        DWORD PerformApplyOperation()
        {
            WriteGuardStatus(kGuardWorking, ERROR_IO_PENDING, nullptr, false);
            EndpointSummary summary{};
            summary.failed = 1;
            summary.firstError = ERROR_NOT_SUPPORTED;
            summary.failureStage = kStageExternalAssociations;
            WriteGuardStatus(kGuardError, ERROR_NOT_SUPPORTED, &summary, true);
            return ERROR_NOT_SUPPORTED;
#if 0
            DWORD error = ERROR_GEN_FAILURE;
            std::vector<EndpointVolumeSnapshot> volumeSnapshots;
            if (!CaptureEndpointVolumes(volumeSnapshots))
            {
                summary.failed = 1;
                summary.firstError = ERROR_READ_FAULT;
                summary.failureStage = kStageEndpointVolume;
                WriteGuardStatus(kGuardError, summary.firstError, &summary, true);
                return summary.firstError;
            }
            bool forceAudioRestart = ConsumeForceAudioRestart();
            bool endpointStackRebuilt = false;
            const bool applied = RunApplyAttempts([&](DWORD attempt)
            {
                bool changed = false;
                bool endpointChanged = false;
                bool componentChanged = false;
                if (!EnumerateEndpoints(true, summary, changed, error,
                    &endpointChanged, &componentChanged))
                {
                    if (attempt != 4) Sleep(300);
                    return false;
                }
                bool restarted = true;
                if (endpointChanged && !endpointStackRebuilt)
                {
                    restarted = RestartEndpointStack();
                    endpointStackRebuilt = restarted;
                }
                else if (changed || forceAudioRestart)
                {
                    restarted = RestartSharedAudioEngine();
                }
                if (!restarted)
                {
                    error = ERROR_SERVICE_NOT_ACTIVE;
                    if (attempt != 4) Sleep(300);
                    return false;
                }
                std::wstring volumeFailure;
                if ((changed || forceAudioRestart) &&
                    !RestoreEndpointVolumes(volumeSnapshots, volumeFailure))
                {
                    error = ERROR_WRITE_FAULT;
                    summary.failed = 1;
                    summary.firstError = error;
                    summary.failureStage = kStageEndpointVolume;
                    summary.failureEndpoint = volumeFailure;
                    if (attempt != 4) Sleep(300);
                    return false;
                }
                if (changed || forceAudioRestart)
                {
                    forceAudioRestart = false;
                    Sleep(componentChanged ? 750 : 500);
                }
                EndpointSummary verified{};
                bool ignoredChanged = false;
                if (EnumerateEndpoints(false, verified, ignoredChanged, error))
                {
                    summary = verified;
                    return true;
                }
                // Preserve the *verification* failure. Otherwise a successful
                // ApplyEndpoint summary overwrites its stage, endpoint, and
                // failed count with zeros when verification returns 1168.
                PreserveVerificationSummary(summary, verified);
                return false;
            });
            if (applied)
            {
                WriteGuardStatus(kGuardReady, ERROR_SUCCESS, &summary, true);
                return ERROR_SUCCESS;
            }
            WriteGuardStatus(kGuardError, error, &summary, true);
            return error;
        }

#endif
        }

        DWORD PerformRestoreOperation()
        {
            WriteGuardStatus(kGuardWorking, ERROR_IO_PENDING, nullptr, false);
            EndpointSummary summary{};
            bool changed = false;
            DWORD error = ERROR_SUCCESS;
            std::vector<EndpointVolumeSnapshot> volumeSnapshots;
            if (!CaptureEndpointVolumes(volumeSnapshots))
            {
                summary.failed = 1;
                summary.firstError = ERROR_READ_FAULT;
                summary.failureStage = kStageEndpointVolume;
                WriteGuardStatus(kGuardError, summary.firstError, &summary, true);
                return summary.firstError;
            }
            if (!RestoreAllEndpoints(summary, changed, error))
            {
                WriteGuardStatus(kGuardError, error, &summary, true);
                return error;
            }
            if (changed && !RestartEndpointStack()) error = ERROR_SERVICE_NOT_ACTIVE;
            if (error == ERROR_SUCCESS && changed)
            {
                std::wstring volumeFailure;
                if (!RestoreEndpointVolumes(volumeSnapshots, volumeFailure))
                {
                    error = ERROR_WRITE_FAULT;
                    summary.failed = 1;
                    summary.firstError = error;
                    summary.failureStage = kStageEndpointVolume;
                    summary.failureEndpoint = volumeFailure;
                }
            }
            if (error == ERROR_SUCCESS)
            {
                // EndpointBuilder can rewrite effect stores while rebuilding.
                // Remove only our CLSID once more without touching the OEM chain.
                EndpointSummary verified{};
                bool finalChanged = false;
                DWORD finalError = ERROR_SUCCESS;
                if (!RestoreAllEndpoints(verified, finalChanged, finalError)) error = finalError;
                else summary = verified;
            }
            WriteGuardStatus(error == ERROR_SUCCESS ? kGuardStopped : kGuardError,
                error, &summary, true);
            return error;
        }

        DWORD PerformOperation(DWORD operation)
        {
            if (operation == kOperationRestore) return PerformRestoreOperation();
            return PerformApplyOperation();
        }

        DWORD WINAPI ServiceControl(DWORD control, DWORD, void*, void*)
        {
            if (control == SERVICE_CONTROL_STOP && g_stopEvent)
            {
                SetServiceStatusValue(SERVICE_STOP_PENDING, NO_ERROR, 5000);
                SetEvent(g_stopEvent);
            }
            else if ((control == kServiceControlApply ||
                control == kServiceControlRestore) && g_commandEvent)
            {
                InterlockedExchange(&g_pendingOperation,
                    control == kServiceControlRestore ? kOperationRestore : kOperationApply);
                SetEvent(g_commandEvent);
            }
            return NO_ERROR;
        }

        void WINAPI ServiceMain(DWORD argc, wchar_t** argv)
        {
            g_statusHandle = RegisterServiceCtrlHandlerExW(kServiceName,
                ServiceControl, nullptr);
            if (!g_statusHandle) return;
            SetServiceStatusValue(SERVICE_START_PENDING, NO_ERROR, 5000);
            g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            g_commandEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            HANDLE registryEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            HANDLE settingsEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!g_stopEvent || !g_commandEvent || !registryEvent || !settingsEvent)
            {
                SetServiceStatusValue(SERVICE_STOPPED, GetLastError());
                if (settingsEvent) CloseHandle(settingsEvent);
                if (registryEvent) CloseHandle(registryEvent);
                if (g_commandEvent) CloseHandle(g_commandEvent);
                if (g_stopEvent) CloseHandle(g_stopEvent);
                return;
            }
            SetServiceStatusValue(SERVICE_RUNNING);
            const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            const bool uninitialize = SUCCEEDED(co);

            DWORD operation = kOperationApply;
            if (argc >= 2 && argv[1] && _wcsicmp(argv[1], L"restore") == 0)
                operation = kOperationRestore;
            PerformOperation(operation);
            DWORD selectionSerial = ReadSettingsDword(
                L"ApoEndpointSelectionSerial", 0);

            while (WaitForSingleObject(g_stopEvent, 0) != WAIT_OBJECT_0)
            {
                HKEY render = nullptr;
                HKEY settingsNotify = nullptr;
                if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRenderPath, 0,
                    KEY_NOTIFY | KEY_READ | KEY_WOW64_64KEY, &render) != ERROR_SUCCESS)
                {
                    if (WaitForSingleObject(g_stopEvent, 1000) == WAIT_OBJECT_0) break;
                    continue;
                }
                if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kSettingsPath, 0,
                    KEY_NOTIFY | KEY_READ | KEY_WOW64_64KEY,
                    &settingsNotify) != ERROR_SUCCESS)
                {
                    RegCloseKey(render);
                    if (WaitForSingleObject(g_stopEvent, 1000) == WAIT_OBJECT_0) break;
                    continue;
                }
                const LONG notify = RegNotifyChangeKeyValue(render, TRUE,
                    REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET,
                    registryEvent, TRUE);
                const LONG settingsNotifyResult = RegNotifyChangeKeyValue(
                    settingsNotify, TRUE,
                    REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET,
                    settingsEvent, TRUE);
                if (notify != ERROR_SUCCESS || settingsNotifyResult != ERROR_SUCCESS)
                {
                    RegCloseKey(settingsNotify);
                    RegCloseKey(render);
                    if (WaitForSingleObject(g_stopEvent, 1000) == WAIT_OBJECT_0) break;
                    continue;
                }
                HANDLE events[] = { g_stopEvent, g_commandEvent, registryEvent,
                    settingsEvent };
                const DWORD wait = WaitForMultipleObjects(ARRAYSIZE(events), events,
                    FALSE, INFINITE);
                RegCloseKey(settingsNotify);
                RegCloseKey(render);
                if (WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0) break;
                if (wait == WAIT_OBJECT_0 + 1)
                {
                    const DWORD requested = static_cast<DWORD>(InterlockedExchange(
                        &g_pendingOperation, kOperationNone));
                    if (requested != kOperationNone) PerformOperation(requested);
                }
                else if (wait == WAIT_OBJECT_0 + 2)
                {
                    // Endpoint creation writes several values in succession. Debounce them
                    // so only one complete rescan/rebuild occurs.
                    WaitForSingleObject(g_stopEvent, 300);
                    if (WaitForSingleObject(g_stopEvent, 0) != WAIT_OBJECT_0)
                        PerformApplyOperation();
                }
                else if (wait == WAIT_OBJECT_0 + 3)
                {
                    const DWORD latest = ReadSettingsDword(
                        L"ApoEndpointSelectionSerial", 0);
                    if (latest != selectionSerial)
                    {
                        selectionSerial = latest;
                        WaitForSingleObject(g_stopEvent, 100);
                        if (WaitForSingleObject(g_stopEvent, 0) != WAIT_OBJECT_0)
                            PerformApplyOperation();
                    }
                }
            }

            HKEY settings = nullptr;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kSettingsPath, 0,
                KEY_SET_VALUE | KEY_WOW64_64KEY, &settings) == ERROR_SUCCESS)
            {
                SetDword(settings, L"ApoGuardState", 0);
                RegCloseKey(settings);
            }
            if (uninitialize) CoUninitialize();
            CloseHandle(settingsEvent);
            CloseHandle(registryEvent);
            CloseHandle(g_commandEvent);
            CloseHandle(g_stopEvent);
            g_commandEvent = nullptr;
            g_stopEvent = nullptr;
            SetServiceStatusValue(SERVICE_STOPPED);
        }

        int StopServiceOnly()
        {
            SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
            if (!manager) return 0;
            SC_HANDLE service = OpenServiceW(manager, kServiceName,
                SERVICE_QUERY_STATUS | SERVICE_STOP);
            if (!service)
            {
                CloseServiceHandle(manager);
                return 0;
            }
            SERVICE_STATUS status{};
            const bool requested = ControlService(service, SERVICE_CONTROL_STOP, &status) ||
                GetLastError() == ERROR_SERVICE_NOT_ACTIVE;
            const bool stopped = requested && WaitForService(service, SERVICE_STOPPED, 15000);
            CloseServiceHandle(service);
            CloseServiceHandle(manager);
            return stopped ? 0 : 55;
        }

        int StartOperationService(DWORD operation, DWORD& baselineSerial)
        {
            wchar_t executable[MAX_PATH]{};
            if (!GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable))) return 51;
            const std::wstring command = L"\"" + std::wstring(executable) + L"\" apo-service";
            SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr,
                SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
            if (!manager) return 52;
            SC_HANDLE service = CreateServiceW(manager, kServiceName,
                kServiceDisplayName, SERVICE_QUERY_STATUS | SERVICE_START |
                SERVICE_STOP | DELETE | SERVICE_CHANGE_CONFIG |
                SERVICE_USER_DEFINED_CONTROL,
                SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
                SERVICE_ERROR_NORMAL, command.c_str(), nullptr, nullptr, nullptr,
                L"LocalSystem", nullptr);
            if (!service && GetLastError() == ERROR_SERVICE_EXISTS)
                service = OpenServiceW(manager, kServiceName, SERVICE_QUERY_STATUS |
                    SERVICE_START | SERVICE_STOP | DELETE | SERVICE_CHANGE_CONFIG |
                    SERVICE_USER_DEFINED_CONTROL);
            if (!service)
            {
                CloseServiceHandle(manager);
                return 53;
            }
            ChangeServiceConfigW(service, SERVICE_NO_CHANGE, SERVICE_AUTO_START,
                SERVICE_NO_CHANGE, command.c_str(), nullptr, nullptr, nullptr,
                L"LocalSystem", nullptr, kServiceDisplayName);
            SERVICE_DESCRIPTIONW description{ const_cast<wchar_t*>(
                L"Automatically attaches the HXAudio endpoint APO to current and newly connected render devices while preserving each device's original effect chain.") };
            ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &description);
            baselineSerial = ReadSettingsDword(L"ApoOperationSerial", 0);
            SERVICE_STATUS_PROCESS current{};
            DWORD bytes = 0;
            const bool queried = QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                reinterpret_cast<BYTE*>(&current), sizeof(current), &bytes) != FALSE;
            bool started = false;
            if (queried && current.dwCurrentState == SERVICE_RUNNING)
            {
                SERVICE_STATUS ignored{};
                started = ControlService(service,
                    operation == kOperationRestore ? kServiceControlRestore : kServiceControlApply,
                    &ignored) != FALSE;
            }
            else
            {
                const wchar_t* argument = operation == kOperationRestore ? L"restore" : L"apply";
                started = StartServiceW(service, 1, &argument) ||
                    GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;
            }
            const bool running = started && WaitForService(service, SERVICE_RUNNING, 15000);
            CloseServiceHandle(service);
            CloseServiceHandle(manager);
            return running ? 0 : 54;
        }

        bool WriteInstallFailureDiagnostic(HKEY key, const InstallFailureDiagnostic& diagnostic)
        {
            if (!diagnostic.available) return false;
            return SetDword(key, L"ApoInstallFailureValid", 0) &&
                SetDword(key, L"ApoInstallFailureExitCode", diagnostic.exitCode) &&
                SetDword(key, L"ApoInstallFailureWin32Known", diagnostic.win32Known) &&
                SetRegistryStringValue(key, L"ApoInstallFailurePhase", diagnostic.phase) &&
                SetDword(key, L"ApoInstallFailureRollbackStatus", diagnostic.rollbackStatus) &&
                SetDword(key, L"ApoInstallFailureRollbackCode", diagnostic.rollbackCode) &&
                SetDword(key, L"ApoInstallFailureError", diagnostic.error) &&
                SetDword(key, L"ApoInstallFailureStage", diagnostic.stage) &&
                SetDword(key, L"ApoInstallFailureMask", diagnostic.mask) &&
                SetRegistryStringValue(key, L"ApoInstallFailureEndpoint", diagnostic.endpoint) &&
                SetDword(key, L"ApoInstallFailureValid", 1);
        }

        bool SaveInstallFailureDiagnostic(const InstallFailureDiagnostic& diagnostic)
        {
            HKEY key = nullptr;
            if (!diagnostic.available || RegCreateKeyExW(HKEY_LOCAL_MACHINE, kSettingsPath,
                0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key,
                nullptr) != ERROR_SUCCESS) return false;
            const bool ok = WriteInstallFailureDiagnostic(key, diagnostic);
            RegCloseKey(key);
            return ok;
        }

        int WaitForOperation(DWORD baselineSerial, DWORD expectedState, DWORD timeout,
            InstallFailureDiagnostic* failure = nullptr)
        {
            const ULONGLONG deadline = GetTickCount64() + timeout;
            while (GetTickCount64() < deadline)
            {
                const DWORD serial = ReadSettingsDword(L"ApoOperationSerial", 0);
                const DWORD state = ReadSettingsDword(L"ApoGuardState", kGuardStopped);
                const DWORD error = ReadSettingsDword(L"ApoLastError", ERROR_IO_PENDING);
                if (serial != baselineSerial)
                {
                    if (state == expectedState && error == ERROR_SUCCESS) return 0;
                    if (state == kGuardError)
                    {
                        const DWORD stage = ReadSettingsDword(L"ApoFailureStage",
                            kStageNone);
                        const DWORD mask = ReadSettingsDword(L"ApoVerifyFailureMask", 0);
                        const std::wstring endpoint = ReadString(HKEY_LOCAL_MACHINE,
                            kSettingsPath, L"ApoFailureEndpoint");
                        if (failure) *failure = { true, error, stage, mask, endpoint };
                        // Print the actionable ASCII fields before any
                        // localized Win32 text, which can break wcerr output.
                        std::wcerr << L"HXAudio APO failed: Win32=" << error
                            << L", stage=" << stage << L", verifyMask=" << mask;
                        if (!endpoint.empty())
                            std::wcerr << L", endpoint=" << endpoint;
                        std::wcerr << std::endl;
                        std::wcerr << L"Windows message: " << LastErrorText(error)
                            << std::endl;
                        return 56;
                    }
                }
                Sleep(100);
            }
            std::wcerr << L"Timed out waiting for HXAudio APO endpoint operation.";
            return 57;
        }

        int RemoveService()
        {
            SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
            if (!manager) return 0;
            SC_HANDLE service = OpenServiceW(manager, kServiceName,
                SERVICE_QUERY_STATUS | SERVICE_STOP | DELETE);
            if (!service)
            {
                CloseServiceHandle(manager);
                return 0;
            }
            SERVICE_STATUS status{};
            if (ControlService(service, SERVICE_CONTROL_STOP, &status) ||
                GetLastError() == ERROR_SERVICE_NOT_ACTIVE)
                WaitForService(service, SERVICE_STOPPED, 15000);
            const bool removed = DeleteService(service) ||
                GetLastError() == ERROR_SERVICE_MARKED_FOR_DELETE;
            CloseServiceHandle(service);
            CloseServiceHandle(manager);
            return removed ? 0 : 55;
        }
    }

    int PrepareForUpgrade()
    {
        return StopServiceOnly();
    }

    int MigrateDspSettings()
    {
        bool changed = false;
        if (!MigrateStoredDspGainRange(changed))
        {
            std::wcerr << L"Unable to migrate stored preamp/PEQ gains into the supported ranges.";
            return 45;
        }
        if (changed)
            std::wcout << L"Stored preamp/PEQ gains were migrated into the supported ranges.";
        return 0;
    }

    int InstallWithMode(const wchar_t* dllPath, const wchar_t* finalMode,
        const wchar_t* installingMode)
    {
        InstallFailureDiagnostic installFailure{};
        if (!SetSettingsDword(L"ApoInstallFailureValid", 0))
            std::wcerr << L"Unable to invalidate previous install diagnostic." << std::endl;
        const auto reportFailure = [&](int code, const wchar_t* phase) {
            installFailure.available = true;
            installFailure.exitCode = static_cast<DWORD>(code);
            installFailure.phase = phase;
            if (!SaveInstallFailureDiagnostic(installFailure))
                std::wcerr << L"Unable to save install failure diagnostic." << std::endl;
            std::wcerr << std::dec << L"HXAudio install failed: exit=" << code << L", phase=" << phase << std::endl;
            return code;
        };
        wchar_t fullPath[MAX_PATH]{};
        if (!dllPath || !GetFullPathNameW(dllPath, ARRAYSIZE(fullPath), fullPath, nullptr))
            return reportFailure(40, L"validate-path");
        if (GetFileAttributesW(fullPath) == INVALID_FILE_ATTRIBUTES)
            return reportFailure(40, L"validate-file");
        // No installation until an OEM-preserving integration is verified.
        return reportFailure(58, L"oem-coexistence-unverified");
#if 0
        const std::wstring oldDll = ReadString(HKEY_LOCAL_MACHINE, kApoPath, L"DllPath");
        const std::wstring oldMode = ReadString(HKEY_LOCAL_MACHINE,
            kSettingsPath, L"InstallationMode");
        const bool hadWorkingApo = !oldDll.empty() &&
            GetFileAttributesW(oldDll.c_str()) != INVALID_FILE_ATTRIBUTES &&
            (_wcsicmp(oldMode.c_str(), L"APO") == 0 ||
                _wcsicmp(oldMode.c_str(), L"Virtual") == 0);

        const auto restorePreviousApo = [&]()
        {
            CallDllExport(fullPath, "DllUnregisterServer");
            if (!hadWorkingApo) return false;
            if (FAILED(CallDllExport(oldDll, "DllRegisterServer"))) return false;
            if (!SetString(HKEY_LOCAL_MACHINE, kApoPath, L"DllPath", oldDll) ||
                !SetString(HKEY_LOCAL_MACHINE, kSettingsPath,
                    L"InstallationMode", oldMode) ||
                !SetSettingsDword(L"ApoForceAudioRestart", 1)) return false;
            DWORD rollbackBaseline = 0;
            int rollback = StartOperationService(kOperationApply,
                rollbackBaseline);
            if (rollback == 0)
                rollback = WaitForOperation(rollbackBaseline, kGuardReady,
                    90000);
            return rollback == 0;
        };

        const auto rollbackAppliedInstall = [&]() {
            // A covering install is a two-version transaction: keep the old
            // registered APO active unless the new graph reaches GuardReady.
            return RunFailedInstallRollback(restorePreviousApo, [&]()
            {
                DWORD restoreBaseline = ReadSettingsDword(
                    L"ApoOperationSerial", 0);
                int restoreResult = StartOperationService(kOperationRestore, restoreBaseline);
                if (restoreResult == 0)
                    restoreResult = WaitForOperation(restoreBaseline, kGuardStopped, 90000);
                if (restoreResult != 0) installFailure.rollbackCode = static_cast<DWORD>(restoreResult);
                const int removeResult = RemoveService();
                if (removeResult != 0 && installFailure.rollbackCode == 0)
                    installFailure.rollbackCode = static_cast<DWORD>(removeResult);
                if (FAILED(CallDllExport(fullPath, "DllUnregisterServer")) && installFailure.rollbackCode == 0)
                    installFailure.rollbackCode = 41;
                if (!hadWorkingApo)
                {
                    RegDeleteTreeW(HKEY_LOCAL_MACHINE, kApoPath);
                    HKEY settings = nullptr;
                    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kSettingsPath, 0,
                        KEY_SET_VALUE | KEY_WOW64_64KEY, &settings) == ERROR_SUCCESS)
                    {
                        RegDeleteValueW(settings, L"InstallationMode");
                        RegDeleteValueW(settings, L"ApoForceAudioRestart");
                        RegCloseKey(settings);
                    }
                    if (!oldMode.empty())
                        SetString(HKEY_LOCAL_MACHINE, kSettingsPath,
                            L"InstallationMode", oldMode);
                }
            });
        };

        // Always stop an older guard process before replacing its registration.
        // This also makes an in-place APO upgrade load the just-installed binary.
        const int stopResult = StopServiceOnly();
        if (stopResult != 0) return reportFailure(stopResult, L"stop-guard");
        bool migratedSettings = false;
        if (!MigrateStoredDspGainRange(migratedSettings))
        {
            std::wcerr << L"Unable to migrate stored preamp/PEQ gains into the supported ranges.";
            return reportFailure(45, L"migrate-settings");
        }
        const HRESULT registration = CallDllExport(fullPath, "DllRegisterServer");
        if (FAILED(registration))
        {
            std::wcerr << L"Unable to register HXAudio APO: 0x" << std::hex << registration;
            return reportFailure(41, L"register-apo");
        }
        if (!SetString(HKEY_LOCAL_MACHINE, kApoPath, L"DllPath", fullPath) ||
            !SetString(HKEY_LOCAL_MACHINE, kSettingsPath, L"InstallationMode",
                installingMode))
        {
            reportFailure(42, L"write-install-state");
            installFailure.rollbackStatus = restorePreviousApo() ? 1u : 3u;
            return reportFailure(42, L"write-install-state");
        }

        ClearGuardStatus();
        if (!SetSettingsDword(L"ApoForceAudioRestart", 1))
        {
            reportFailure(44, L"force-restart-flag");
            installFailure.rollbackStatus = restorePreviousApo() ? 1u : 3u;
            return reportFailure(44, L"force-restart-flag");
        }
        DWORD baseline = 0;
        int serviceResult = StartOperationService(kOperationApply, baseline);
        if (serviceResult == 0)
            serviceResult = WaitForOperation(baseline, kGuardReady, 90000, &installFailure);
        if (serviceResult != 0)
        {
            installFailure.win32Known = installFailure.available ? 1u : 0u;
            reportFailure(serviceResult, serviceResult == 56 ? L"verify-guard" :
                serviceResult == 57 ? L"guard-timeout" : L"start-guard");
            const bool previousRestored = rollbackAppliedInstall();
            installFailure.rollbackStatus = previousRestored ? 1u : 2u;
            return reportFailure(serviceResult, serviceResult == 56 ? L"verify-guard" :
                serviceResult == 57 ? L"guard-timeout" : L"start-guard");
        }
        if (!SetString(HKEY_LOCAL_MACHINE, kSettingsPath, L"InstallationMode", finalMode))
        {
            reportFailure(43, L"commit-mode");
            installFailure.rollbackStatus = rollbackAppliedInstall() ? 1u : 2u;
            return reportFailure(43, L"commit-mode");
        }
        const DWORD count = ReadSettingsDword(L"ApoAttachedEndpoints", 0);
        DeleteUnusedApoDlls(fullPath, fullPath);
        std::wcout << L"HXAudio APO installed on " << count << L" render endpoints.";
        if (migratedSettings)
            std::wcout << L" Stored preamp/PEQ gains were migrated into the supported ranges.";
        return 0;
    }

#endif
    }

    int Install(const wchar_t* dllPath)
    {
        return InstallWithMode(dllPath, L"APO", L"APO-Installing");
    }

    int InstallForVirtualEffects(const wchar_t* dllPath)
    {
        return InstallWithMode(dllPath, L"Virtual",
            L"Virtual-EffectsInstalling");
    }

    int Uninstall()
    {
        const std::wstring dll = ReadString(HKEY_LOCAL_MACHINE, kApoPath, L"DllPath");
        HKEY apoKey = nullptr;
        const bool apoStateExists = !dll.empty() ||
            RegOpenKeyExW(HKEY_LOCAL_MACHINE, kApoPath, 0,
                KEY_READ | KEY_WOW64_64KEY, &apoKey) == ERROR_SUCCESS;
        if (apoKey) RegCloseKey(apoKey);

        if (apoStateExists)
        {
            // Restart the installed service with an explicit restore operation so
            // protected MMDevices keys are always changed by LocalSystem.
            const int stopResult = StopServiceOnly();
            if (stopResult != 0) return stopResult;
            DWORD baseline = ReadSettingsDword(L"ApoOperationSerial", 0);
            int restoreResult = StartOperationService(kOperationRestore, baseline);
            if (restoreResult == 0)
                restoreResult = WaitForOperation(baseline, kGuardStopped, 90000);
            if (restoreResult != 0) return restoreResult;
        }
        RemoveService();
        if (!dll.empty()) CallDllExport(dll, "DllUnregisterServer");
        if (!dll.empty()) DeleteUnusedApoDlls(dll, L"");
        RegDeleteTreeW(HKEY_LOCAL_MACHINE, kApoPath);
        HKEY settings = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kSettingsPath, 0,
            KEY_SET_VALUE | KEY_WOW64_64KEY, &settings) == ERROR_SUCCESS)
        {
            RegDeleteValueW(settings, L"ApoAttachedEndpoints");
            RegDeleteValueW(settings, L"ApoEnumeratedEndpoints");
            RegDeleteValueW(settings, L"ApoFailedEndpoints");
            RegDeleteValueW(settings, L"ApoActiveEndpoints");
            RegDeleteValueW(settings, L"ApoActiveAttachedEndpoints");
            RegDeleteValueW(settings, L"ApoGuardState");
            RegDeleteValueW(settings, L"ApoLastError");
            RegDeleteValueW(settings, L"ApoOperationSerial");
            RegDeleteValueW(settings, L"ApoForceAudioRestart");
            RegDeleteValueW(settings, L"ApoFailureStage");
            RegDeleteValueW(settings, L"ApoFailureEndpoint");
            RegDeleteValueW(settings, L"ApoVerifyFailureMask");
            wchar_t mode[64]{};
            DWORD type = 0;
            DWORD size = sizeof(mode);
            if (RegQueryValueExW(settings, L"InstallationMode", nullptr, &type,
                reinterpret_cast<BYTE*>(mode), &size) == ERROR_SUCCESS && type == REG_SZ &&
                (_wcsicmp(mode, L"APO") == 0 ||
                    _wcsicmp(mode, L"APO-Installing") == 0 ||
                    _wcsicmp(mode, L"Virtual-EffectsInstalling") == 0))
                RegDeleteValueW(settings, L"InstallationMode");
            RegCloseKey(settings);
        }
        std::wcout << L"HXAudio APO removed and original endpoint effects restored.";
        return 0;
    }

    int Refresh()
    {
        if (ReadString(HKEY_LOCAL_MACHINE, kApoPath, L"DllPath").empty()) return 60;
        DWORD baseline = ReadSettingsDword(L"ApoOperationSerial", 0);
        int result = StartOperationService(kOperationApply, baseline);
        if (result == 0) result = WaitForOperation(baseline, kGuardReady, 90000);
        if (result != 0) return result;
        const DWORD count = ReadSettingsDword(L"ApoAttachedEndpoints", 0);
        std::wcout << L"HXAudio APO is attached to " << count << L" render endpoints.";
        return 0;
    }

    int VerifyInstalledMode(const wchar_t* expectedMode)
    {
        if (_wcsicmp(ReadString(HKEY_LOCAL_MACHINE, kSettingsPath,
            L"InstallationMode").c_str(), expectedMode) != 0) return 62;
        const std::wstring dll = ReadString(HKEY_LOCAL_MACHINE, kApoPath, L"DllPath");
        if (dll.empty() || GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES)
            return 63;
        if (ReadSettingsDword(L"ApoGuardState", kGuardStopped) != kGuardReady ||
            ReadSettingsDword(L"ApoLastError", ERROR_GEN_FAILURE) != ERROR_SUCCESS)
            return 64;

        SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!manager) return 65;
        SC_HANDLE service = OpenServiceW(manager, kServiceName, SERVICE_QUERY_STATUS);
        const bool running = service && ServiceIsRunning(service);
        if (service) CloseServiceHandle(service);
        CloseServiceHandle(manager);
        if (!running) return 66;

        EndpointSummary summary{};
        bool ignoredChanged = false;
        DWORD error = ERROR_SUCCESS;
        if (!EnumerateEndpoints(false, summary, ignoredChanged, error)) return 67;
        std::wcout << L"HXAudio APO verified on " << summary.succeeded
            << L" render endpoints (" << summary.activeSucceeded
            << L" active).";
        return 0;
    }

    int VerifyInstalled()
    {
        return VerifyInstalledMode(L"APO");
    }

    int VerifyVirtualEffectsInstalled()
    {
        return VerifyInstalledMode(L"Virtual");
    }

    int LiveTest()
    {
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;LS)",
            SDDL_REVISION_1, &descriptor, nullptr)) return 80;
        SECURITY_ATTRIBUTES security{ sizeof(security), descriptor, FALSE };
        const std::wstring mappingName = L"Global\\HXAudioApoProbe-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64());
        HANDLE mappingHandle = CreateFileMappingW(INVALID_HANDLE_VALUE, &security,
            PAGE_READWRITE, 0, sizeof(HXAudioApoProbeMapping), mappingName.c_str());
        LocalFree(descriptor);
        if (!mappingHandle) return 81;
        auto* mapping = static_cast<HXAudioApoProbeMapping*>(MapViewOfFile(
            mappingHandle, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
            sizeof(HXAudioApoProbeMapping)));
        if (!mapping)
        {
            CloseHandle(mappingHandle);
            return 82;
        }
        ZeroMemory(mapping, sizeof(*mapping));
        mapping->magic = kHXAudioApoProbeMagic;
        mapping->version = kHXAudioApoProbeVersion;
        mapping->size = sizeof(*mapping);
        mapping->slotCount = kHXAudioApoProbeSlots;

        if (!SetString(HKEY_LOCAL_MACHINE, kSettingsPath,
            L"ApoProbeMapping", mappingName))
        {
            UnmapViewOfFile(mapping);
            CloseHandle(mappingHandle);
            return 83;
        }

        // Build a fresh AudioDG graph while the diagnostic mapping already
        // exists.  Otherwise Windows may reuse a graph that cached an earlier
        // APO initialization failure and no constructor/Lock transition would
        // be observable by this test.
        if (!RestartSharedAudioEngine())
        {
            HKEY settings = nullptr;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kSettingsPath, 0,
                KEY_SET_VALUE | KEY_WOW64_64KEY, &settings) == ERROR_SUCCESS)
            {
                RegDeleteValueW(settings, L"ApoProbeMapping");
                RegCloseKey(settings);
            }
            UnmapViewOfFile(mapping);
            CloseHandle(mappingHandle);
            return 85;
        }
        Sleep(500);

        struct ProbeStream
        {
            std::wstring endpointId;
            IAudioClient* client = nullptr;
            IAudioRenderClient* render = nullptr;
            UINT32 bufferFrames = 0;
            UINT32 sampleRate = 0;
            UINT32 channels = 0;
            UINT32 bits = 0;
        };
        std::vector<ProbeStream> streams;
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool uninitialize = SUCCEEDED(hr);
        if (hr == RPC_E_CHANGED_MODE) hr = S_OK;

        IMMDeviceEnumerator* enumerator = nullptr;
        IMMDeviceCollection* collection = nullptr;
        if (SUCCEEDED(hr)) hr = CoCreateInstance(__uuidof(MMDeviceEnumerator),
            nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
            reinterpret_cast<void**>(&enumerator));
        if (SUCCEEDED(hr)) hr = enumerator->EnumAudioEndpoints(eRender,
            DEVICE_STATE_ACTIVE, &collection);

        UINT count = 0;
        if (SUCCEEDED(hr)) hr = collection->GetCount(&count);
        for (UINT index = 0; SUCCEEDED(hr) && index < count; ++index)
        {
            IMMDevice* device = nullptr;
            IAudioClient* client = nullptr;
            IAudioRenderClient* render = nullptr;
            WAVEFORMATEX* format = nullptr;
            LPWSTR endpointId = nullptr;
            UINT32 bufferFrames = 0;

            HRESULT endpointResult = collection->Item(index, &device);
            if (SUCCEEDED(endpointResult) && _wcsicmp(DevicePropertyString(
                device, PKEY_Device_EnumeratorName).c_str(), L"ROOT") == 0)
            {
                device->Release();
                continue;
            }
            if (SUCCEEDED(endpointResult)) endpointResult = device->GetId(&endpointId);
            if (SUCCEEDED(endpointResult)) endpointResult = device->Activate(
                __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                reinterpret_cast<void**>(&client));
            if (SUCCEEDED(endpointResult)) endpointResult = client->GetMixFormat(&format);
            if (SUCCEEDED(endpointResult)) endpointResult = client->Initialize(
                AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_NOPERSIST,
                10000000, 0, format, nullptr);
            if (SUCCEEDED(endpointResult)) endpointResult = client->GetBufferSize(&bufferFrames);
            if (SUCCEEDED(endpointResult)) endpointResult = client->GetService(
                __uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render));
            if (SUCCEEDED(endpointResult))
            {
                BYTE* buffer = nullptr;
                endpointResult = render->GetBuffer(bufferFrames, &buffer);
                if (SUCCEEDED(endpointResult))
                {
                    ZeroMemory(buffer, static_cast<size_t>(bufferFrames) *
                        format->nBlockAlign);
                    // A valid all-zero packet proves that the production DSP
                    // path ran, without producing an audible test signal.
                    endpointResult = render->ReleaseBuffer(bufferFrames, 0);
                }
            }
            if (SUCCEEDED(endpointResult)) endpointResult = client->Start();

            if (SUCCEEDED(endpointResult))
            {
                streams.push_back({ endpointId ? endpointId : L"", client, render,
                    bufferFrames, format->nSamplesPerSec, format->nChannels,
                    format->wBitsPerSample });
                client = nullptr;
                render = nullptr;
            }
            else
            {
                std::wcerr << L"Unable to open shared probe stream for endpoint "
                    << (endpointId ? endpointId : L"(unknown)") << L": 0x"
                    << std::hex << static_cast<unsigned long>(endpointResult) << L". ";
            }
            if (render) render->Release();
            if (client) client->Release();
            if (endpointId) CoTaskMemFree(endpointId);
            if (format) CoTaskMemFree(format);
            if (device) device->Release();
        }

        const ULONGLONG deadline = GetTickCount64() + 1800;
        while (!streams.empty() && GetTickCount64() < deadline)
        {
            for (auto& stream : streams)
            {
                UINT32 padding = 0;
                if (FAILED(stream.client->GetCurrentPadding(&padding)) ||
                    padding >= stream.bufferFrames) continue;
                const UINT32 available = stream.bufferFrames - padding;
                BYTE* buffer = nullptr;
                if (SUCCEEDED(stream.render->GetBuffer(available, &buffer)))
                {
                    const size_t blockAlign = static_cast<size_t>(stream.channels) *
                        std::max<UINT32>(stream.bits / 8, 1);
                    ZeroMemory(buffer, static_cast<size_t>(available) * blockAlign);
                    stream.render->ReleaseBuffer(available, 0);
                }
            }
            Sleep(20);
        }
        Sleep(250);

        wchar_t requestedClsid[64]{};
        wchar_t requestedIid[64]{};
        StringFromGUID2(mapping->lastRequestedClsid, requestedClsid,
            ARRAYSIZE(requestedClsid));
        StringFromGUID2(mapping->lastRequestedIid, requestedIid,
            ARRAYSIZE(requestedIid));
        std::wcout << L"\nAPO phases: class=" << mapping->dllGetClassObjectCalls
            << L"/" << mapping->dllGetClassObjectSuccesses
            << L" (hr=0x" << std::hex
            << static_cast<unsigned long>(mapping->lastDllGetClassObjectResult)
            << L", clsid=" << requestedClsid << L", iid=" << requestedIid
            << L"), " << std::dec
            << L"object=" << mapping->constructorCalls
            << L", initialize=" << mapping->initializeCalls
            << L"/" << mapping->initializeSuccesses
            << L" (last size=" << mapping->lastInitializeDataSize
            << L", hr=0x" << std::hex
            << static_cast<unsigned long>(mapping->lastInitializeResult)
            << L"), lock=" << std::dec << mapping->lockCalls
            << L"/" << mapping->lockSuccesses << L" (hr=0x" << std::hex
            << static_cast<unsigned long>(mapping->lastLockResult)
            << std::dec << L").";

        std::vector<const HXAudioApoProbeSlot*> activeSlots;
        for (DWORD index = 0; index < kHXAudioApoProbeSlots; ++index)
        {
            const auto& slot = mapping->slots[index];
            if (slot.claimed)
            {
                std::wcout << L"\nAPO instance " << slot.objectId
                    << L": endpoint=" << (slot.endpointId[0]
                        ? slot.endpointId : L"(not reported)")
                    << L", initialized=" << slot.initialized
                    << L", locked=" << slot.locked
                    << L", lockHr=0x" << std::hex
                    << static_cast<unsigned long>(slot.lastLockResult)
                    << std::dec << L", format=" << slot.sampleRate << L"/"
                    << slot.channels << L"/" << slot.validBitsPerSample
                    << L", process=" << slot.processCalls
                    << L", dsp=" << slot.dspProcessCalls << L".";
            }
            if (slot.claimed && slot.initialized && slot.locked &&
                SUCCEEDED(slot.lastLockResult) && slot.processCalls > 0 &&
                slot.dspProcessCalls > 0)
                activeSlots.push_back(&slot);
        }

        bool valid = !streams.empty() && activeSlots.size() >= streams.size();
        std::vector<bool> matchedSlots(activeSlots.size(), false);
        for (const auto& stream : streams)
        {
            size_t matchIndex = activeSlots.size();
            for (size_t index = 0; index < activeSlots.size(); ++index)
            {
                if (matchedSlots[index]) continue;
                const auto* slot = activeSlots[index];
                if ((!stream.endpointId.empty() && slot->endpointId[0] &&
                    _wcsicmp(stream.endpointId.c_str(), slot->endpointId) == 0) ||
                    (!slot->endpointId[0] && activeSlots.size() == streams.size()))
                {
                    matchIndex = index;
                    break;
                }
            }
            const bool matched = matchIndex < activeSlots.size();
            if (matched) matchedSlots[matchIndex] = true;
            else valid = false;
            std::wcout << L"\n" << stream.endpointId << L": "
                << stream.sampleRate << L" Hz, " << stream.channels
                << L" channels, " << stream.bits << L"-bit mix format, APO "
                << (matched ? L"processed" : L"not observed");
        }

        for (auto& stream : streams)
        {
            stream.client->Stop();
            stream.render->Release();
            stream.client->Release();
        }
        if (collection) collection->Release();
        if (enumerator) enumerator->Release();
        if (uninitialize) CoUninitialize();

        HKEY settings = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kSettingsPath, 0,
            KEY_SET_VALUE | KEY_WOW64_64KEY, &settings) == ERROR_SUCCESS)
        {
            RegDeleteValueW(settings, L"ApoProbeMapping");
            RegCloseKey(settings);
        }
        Sleep(150);
        UnmapViewOfFile(mapping);
        CloseHandle(mappingHandle);

        if (!valid) return 84;
        std::wcout << L"\nHXAudio APO processed valid silent packets on all "
            << streams.size() << L" active shared render endpoints.";
        return 0;
    }

    // Uses only a private, volatile HKCU key. Never enumerates real endpoints,
    // installs an APO, controls services, or opens MMDevices.
    int DiagnosticSelfTest()
    {
        const std::wstring path = L"Software\\HXAudioDiagnosticTest_" +
            std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
        HKEY root = nullptr, fx = nullptr, backup = nullptr;
        int result = 0;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr,
            REG_OPTION_VOLATILE, KEY_ALL_ACCESS, nullptr, &root, nullptr) != ERROR_SUCCESS)
            return 80;
        if (RegCreateKeyExW(root, L"endpoint\\FxProperties", 0, nullptr,
            REG_OPTION_VOLATILE, KEY_ALL_ACCESS, nullptr, &fx, nullptr) != ERROR_SUCCESS ||
            RegCreateKeyExW(root, L"backup", 0, nullptr, REG_OPTION_VOLATILE,
                KEY_ALL_ACCESS, nullptr, &backup, nullptr) != ERROR_SUCCESS) result = 81;
        if (!result)
        {
            const auto modes = EncodeMultiString(RequiredRenderProcessingModes());
            for (DWORD expected = 0; expected < 64 && !result; ++expected)
            {
                bool ok = SetRegistryStringValue(fx, kSingleEndpointValue,
                    (expected & 1) ? L"wrong" : kApoClsid);
                if (expected & 2) RegDeleteValueW(fx, kEfxProcessingModesValue);
                else ok = ok && RegSetValueExW(fx, kEfxProcessingModesValue, 0,
                    REG_MULTI_SZ, modes.data(), static_cast<DWORD>(modes.size())) == ERROR_SUCCESS;
                ok = ok && SetDword(fx, kDisableSysFxValue, (expected & 4) ? 1 : 0);
                if (expected & 8) ok = ok && SetRegistryStringValue(fx, L"testAssociation", L"SWD\\DRIVERENUM\\test");
                else RegDeleteValueW(fx, L"testAssociation");
                if (expected & 16) ok = ok && SetRegistryStringValue(fx, kPreMixValue, L"other");
                else RegDeleteValueW(fx, kPreMixValue);
                if (expected & 32) ok = ok && SetRegistryStringValue(fx, kSfxProcessingModesValue, L"other");
                else RegDeleteValueW(fx, kSfxProcessingModesValue);
                DWORD stage = 0, mask = 0;
                const DWORD error = VerifyEndpoint(root, L"endpoint", stage, mask);
                if (!ok || mask != expected || stage != kStageVerify ||
                    error != static_cast<DWORD>(expected ? ERROR_NOT_FOUND : ERROR_SUCCESS))
                {
                    std::wcerr << L"Verify case=" << expected << L" actual=" << mask
                        << L" stage=" << stage << L" error=" << error << std::endl;
                    result = 82;
                }
            }
            const auto extraModes = EncodeMultiString({kDefaultProcessingMode, kMediaProcessingMode});
            bool modeOk = RegSetValueExW(fx, kEfxProcessingModesValue, 0, REG_MULTI_SZ,
                extraModes.data(), static_cast<DWORD>(extraModes.size())) == ERROR_SUCCESS &&
                !ProcessingModesCoverRenderModes(fx, kEfxProcessingModesValue);
            const auto singleMode = EncodeMultiString({kDefaultProcessingMode});
            modeOk = modeOk && RegSetValueExW(fx, kEfxProcessingModesValue, 0, REG_MULTI_SZ,
                singleMode.data(), static_cast<DWORD>(singleMode.size())) == ERROR_SUCCESS &&
                ProcessingModesCoverRenderModes(fx, kEfxProcessingModesValue);
            if (!modeOk) result = 92;
            EndpointSummary applied{}, verified{};
            applied.succeeded = 1;
            verified.failed = 1; verified.firstError = ERROR_NOT_FOUND;
            verified.failureStage = kStageVerify; verified.verifyFailureMask = 33;
            verified.failureEndpoint = L"test-endpoint";
            PreserveVerificationSummary(applied, verified);
            if (applied.failed != 1 || applied.succeeded != 0 ||
                applied.firstError != ERROR_NOT_FOUND || applied.failureStage != kStageVerify ||
                applied.verifyFailureMask != 33 || applied.failureEndpoint != L"test-endpoint") result = 83;

            // Exercise the production backup/restore functions with exact binary
            // type and bytes, then the originally-absent-value path.
            const BYTE original[] = { 0, 1, 2, 255, 0 };
            bool changed = false;
            bool ok = RegSetValueExW(fx, L"testValue", 0, REG_BINARY, original,
                sizeof(original)) == ERROR_SUCCESS && BackupValue(backup, fx, L"testValue", L"Sample");
            ok = ok && SetDword(fx, L"testValue", 99) && RestoreValue(backup, fx, L"testValue", L"Sample", changed);
            DWORD type = 0; std::vector<BYTE> bytes;
            ok = ok && changed && QueryRaw(fx, L"testValue", type, bytes) && type == REG_BINARY &&
                bytes == std::vector<BYTE>(original, original + sizeof(original));
            ok = ok && BackupValue(backup, fx, L"missingValue", L"Missing") &&
                SetDword(fx, L"missingValue", 1);
            changed = false;
            ok = ok && RestoreValue(backup, fx, L"missingValue", L"Missing", changed) &&
                changed && ValueIsAbsent(fx, L"missingValue");
            if (!ok && !result) result = 84;

            int attempts = 0;
            bool retry = RunApplyAttempts([&](DWORD) { return ++attempts == 3; });
            if (!retry || attempts != 3) result = 86;
            attempts = 0;
            retry = RunApplyAttempts([&](DWORD) { ++attempts; return false; });
            if (retry || attempts != 5) result = 87;
            int previous = 0, clean = 0;
            bool restored = RunFailedInstallRollback([&]() { ++previous; return true; },
                [&]() { ++clean; });
            if (!restored || previous != 1 || clean != 0) result = 88;
            restored = RunFailedInstallRollback([&]() { ++previous; return false; },
                [&]() { ++clean; });
            if (restored || previous != 2 || clean != 1) result = 89;

            InstallFailureDiagnostic failure{true, ERROR_NOT_FOUND, kStageVerify, 33, L"fake-endpoint"};
            failure.exitCode = 56; failure.win32Known = 1;
            failure.phase = L"verify-guard"; failure.rollbackStatus = 2; failure.rollbackCode = 57;
            // Simulate rollback writing new guard state before saving original failure.
            ok = SetDword(root, L"ApoLastError", ERROR_SUCCESS) &&
                WriteInstallFailureDiagnostic(root, failure);
            DWORD savedError = 0, savedStage = 0, savedMask = 0, guardError = 1, valid = 0;
            DWORD savedExit = 0, savedKnown = 0, savedRollback = 0, savedRollbackCode = 0;
            std::wstring savedEndpoint, savedPhase;
            ok = ok && QueryDword(root, L"ApoInstallFailureValid", valid) && valid == 1 &&
                QueryDword(root, L"ApoInstallFailureExitCode", savedExit) && savedExit == 56 &&
                QueryDword(root, L"ApoInstallFailureWin32Known", savedKnown) && savedKnown == 1 &&
                QueryDword(root, L"ApoInstallFailureRollbackStatus", savedRollback) && savedRollback == 2 &&
                QueryDword(root, L"ApoInstallFailureRollbackCode", savedRollbackCode) && savedRollbackCode == 57 &&
                QueryRegistryStringValue(root, L"ApoInstallFailurePhase", savedPhase) && savedPhase == L"verify-guard" &&
                QueryDword(root, L"ApoInstallFailureError", savedError) &&
                QueryDword(root, L"ApoInstallFailureStage", savedStage) &&
                QueryDword(root, L"ApoInstallFailureMask", savedMask) &&
                QueryDword(root, L"ApoLastError", guardError) &&
                QueryRegistryStringValue(root, L"ApoInstallFailureEndpoint", savedEndpoint) &&
                savedError == ERROR_NOT_FOUND && savedStage == kStageVerify && savedMask == 33 &&
                savedEndpoint == L"fake-endpoint" && guardError == ERROR_SUCCESS;
            if (!ok) result = 90;

            // Integrate the real registry value backup, endpoint verifier,
            // retry policy, rollback selection and preserved diagnostic on
            // an isolated fake endpoint. No SCM or MMDevices access occurs.
            RegDeleteValueW(fx, L"testAssociation");
            RegDeleteValueW(fx, kPreMixValue);
            RegDeleteValueW(fx, kSfxProcessingModesValue);
            ok = SetDword(fx, kDisableSysFxValue, 0) &&
                SetRegistryStringValue(fx, kSingleEndpointValue, L"OEM-original") &&
                BackupValue(backup, fx, kSingleEndpointValue, L"TxnOriginal") &&
                SetRegistryStringValue(fx, kSingleEndpointValue, kApoClsid);
            RegDeleteValueW(fx, kEfxProcessingModesValue);
            DWORD failStage = 0, failMask = 0, failCode = 0;
            int runs = 0;
            const bool unexpectedlyApplied = RunApplyAttempts([&](DWORD) {
                ++runs;
                failCode = VerifyEndpoint(root, L"endpoint", failStage, failMask);
                return failCode == ERROR_SUCCESS;
            });
            const InstallFailureDiagnostic originalFailure{true, failCode, failStage, failMask, L"endpoint"};
            ok = ok && WriteInstallFailureDiagnostic(root, originalFailure);
            DWORD preRollbackError = 0;
            ok = ok && QueryDword(root, L"ApoInstallFailureError", preRollbackError) &&
                preRollbackError == ERROR_NOT_FOUND;
            bool rollbackChanged = false;
            bool fallbackWorked = false;
            const bool oldVersionRestored = RunFailedInstallRollback(
                [&]() { return false; }, [&]() {
                    fallbackWorked = RestoreValue(backup, fx, kSingleEndpointValue,
                        L"TxnOriginal", rollbackChanged) &&
                        SetDword(root, L"ApoLastError", ERROR_SUCCESS);
                });
            ok = ok && !unexpectedlyApplied && runs == 5 && failCode == ERROR_NOT_FOUND &&
                failStage == kStageVerify && failMask == 2 && !oldVersionRestored &&
                fallbackWorked && rollbackChanged && WriteInstallFailureDiagnostic(root, originalFailure);
            std::wstring restoredOem, storedEndpoint;
            DWORD storedError = 0, storedStage = 0, storedMask = 0;
            ok = ok && QueryRegistryStringValue(fx, kSingleEndpointValue, restoredOem) &&
                restoredOem == L"OEM-original" && QueryDword(root, L"ApoInstallFailureError", storedError) &&
                QueryDword(root, L"ApoInstallFailureStage", storedStage) &&
                QueryDword(root, L"ApoInstallFailureMask", storedMask) &&
                QueryRegistryStringValue(root, L"ApoInstallFailureEndpoint", storedEndpoint) &&
                storedError == ERROR_NOT_FOUND && storedStage == kStageVerify &&
                storedMask == 2 && storedEndpoint == L"endpoint";

            // Two failed verifies followed by a successful third attempt.
            ok = ok && SetRegistryStringValue(fx, kSingleEndpointValue, kApoClsid);
            runs = 0;
            const bool eventuallyApplied = RunApplyAttempts([&](DWORD) {
                ++runs;
                if (runs == 3 && RegSetValueExW(fx, kEfxProcessingModesValue, 0,
                    REG_MULTI_SZ, modes.data(), static_cast<DWORD>(modes.size())) != ERROR_SUCCESS)
                    return false;
                return VerifyEndpoint(root, L"endpoint", failStage, failMask) == ERROR_SUCCESS;
            });
            if (!ok || !eventuallyApplied || runs != 3) result = 91;

            // Fixture distilled from the user's Realtek snapshot. It includes
            // each observed composite FX slot and all eight component links.
            struct Fixture { const wchar_t* name; std::vector<std::wstring> values; };
            const Fixture fixture[] = {
                {kCompositeStreamValue, {L"{0EBD8605-17BB-4AE7-AD76-E86F99A425E9}"}},
                {kCompositeModeValue, {L"{90609662-991C-4873-BA68-8FF2D57B230B}", L"{001897D5-ADD1-4C31-90FD-34B06B8E7EE4}"}},
                {kCompositeValue, {L"{0EBD8607-17BB-4AE7-AD76-E86F99A425E9}", L"{90705486-CFCC-4D2F-9671-B62846279A2C}"}},
                {kCompositeOffloadStreamValue, {L"{0EBD8611-17BB-4AE7-AD76-E86F99A425E9}"}},
                {kCompositeOffloadModeValue, {L"{0EBD8612-17BB-4AE7-AD76-E86F99A425E9}", L"{90C0662B-E145-49D8-A0C1-670D079442BE}"}},
                {kSfxProcessingModesValue, {kDefaultProcessingMode, kCommunicationsProcessingMode, kMediaProcessingMode, kMovieProcessingMode, kNotificationProcessingMode}},
                {kMfxProcessingModesValue, {kDefaultProcessingMode, kCommunicationsProcessingMode, kMediaProcessingMode, kMovieProcessingMode}},
                {kOffloadSfxProcessingModesValue, {kDefaultProcessingMode, kMediaProcessingMode, kMovieProcessingMode, kNotificationProcessingMode}},
                {kOffloadMfxProcessingModesValue, {kDefaultProcessingMode, kMediaProcessingMode, kMovieProcessingMode}}
            };
            for (const auto& item : fixture) {
                const auto encoded = EncodeMultiString(item.values);
                ok = ok && RegSetValueExW(fx, item.name, 0, REG_MULTI_SZ,
                    encoded.data(), static_cast<DWORD>(encoded.size())) == ERROR_SUCCESS;
            }
            const wchar_t* associations[] = {
                L"{0ebd8605-17bb-4ae7-ad76-e86f99a425e9},100", L"{0ebd8607-17bb-4ae7-ad76-e86f99a425e9},100",
                L"{0ebd8611-17bb-4ae7-ad76-e86f99a425e9},100", L"{0ebd8612-17bb-4ae7-ad76-e86f99a425e9},100",
                L"{001897d5-add1-4c31-90fd-34b06b8e7ee4},100", L"{90609662-991c-4873-ba68-8ff2d57b230b},100",
                L"{90705486-cfcc-4d2f-9671-b62846279a2c},100", L"{90c0662b-e145-49d8-a0c1-670d079442be},100"
            };
            for (size_t i = 0; i < ARRAYSIZE(associations); ++i)
                ok = ok && SetRegistryStringValue(fx, associations[i], i < 4 ? L"SWD\\DRIVERENUM\\DOLBYAPO_DAX3APOSVC" :
                    i == 4 ? L"SWD\\DRIVERENUM\\ELEVOCAPO" : L"SWD\\DRIVERENUM\\REALTEKAPO");
            DWORD rebuiltStage = 0, rebuiltMask = 0;
            ok = ok && VerifyEndpoint(root, L"endpoint", rebuiltStage, rebuiltMask) == ERROR_NOT_FOUND &&
                rebuiltStage == kStageVerify && rebuiltMask == (8u | 16u | 32u);
            const auto oldSixModes = EncodeMultiString({kDefaultProcessingMode, kCommunicationsProcessingMode,
                kSpeechProcessingMode, kNotificationProcessingMode, kMediaProcessingMode, kMovieProcessingMode});
            ok = ok && RegSetValueExW(fx, kEfxProcessingModesValue, 0, REG_MULTI_SZ,
                oldSixModes.data(), static_cast<DWORD>(oldSixModes.size())) == ERROR_SUCCESS &&
                VerifyEndpoint(root, L"endpoint", rebuiltStage, rebuiltMask) == ERROR_NOT_FOUND && rebuiltMask == (2u | 8u | 16u | 32u);
            // Model endpoint rebuild restoring the audited DEFAULT-only value.
            ok = ok && RegSetValueExW(fx, kEfxProcessingModesValue, 0, REG_MULTI_SZ,
                singleMode.data(), static_cast<DWORD>(singleMode.size())) == ERROR_SUCCESS &&
                VerifyEndpoint(root, L"endpoint", rebuiltStage, rebuiltMask) == ERROR_NOT_FOUND && rebuiltMask == (8u | 16u | 32u);
            bool refusedChanged = true; DWORD refusedStage = 0;
            ok = ok && ApplyEndpoint(root, L"endpoint", refusedChanged, refusedStage) == ERROR_NOT_SUPPORTED && !refusedChanged;
            for (const auto& item : fixture) {
                DWORD actualType = 0; std::vector<BYTE> actual;
                ok = ok && QueryRaw(fx, item.name, actualType, actual) && actualType == REG_MULTI_SZ &&
                    actual == EncodeMultiString(item.values);
            }
            for (const auto* name : associations) {
                std::wstring association;
                ok = ok && QueryRegistryStringValue(fx, name, association) &&
                    StartsWithInsensitive(association, L"SWD\\DRIVERENUM\\");
            }
            if (!ok) result = 96;
        }
        // Exercise the real install entrypoint's earliest failure without
        // privileges or audio changes, redirecting HKLM to this disposable HKCU
        // tree before calling it. The null path exits before any service/DLL work.
        HKEY fakeSettings = nullptr;
        const LONG settingsCreated = RegCreateKeyExW(root, L"Software\\HXAudio\\VirtualAudio",
            0, nullptr, REG_OPTION_VOLATILE, KEY_ALL_ACCESS, nullptr, &fakeSettings, nullptr);
        if (settingsCreated != ERROR_SUCCESS) { if (!result) result = 93; }
        else
        {
            const LONG overrideResult = RegOverridePredefKey(HKEY_LOCAL_MACHINE, root);
            if (overrideResult != ERROR_SUCCESS) { if (!result) result = 94; }
            else
            {
                std::wostringstream captured;
                auto* oldBuffer = std::wcerr.rdbuf(captured.rdbuf());
                const int installExit = InstallWithMode(nullptr, L"APO", L"APO-Installing");
                std::wcerr.rdbuf(oldBuffer);
                const LONG resetResult = RegOverridePredefKey(HKEY_LOCAL_MACHINE, nullptr);
                DWORD validInstall = 0, recordedExit = 0, knownWin32 = 1;
                std::wstring phase;
                const bool recorded = QueryDword(fakeSettings, L"ApoInstallFailureValid", validInstall) &&
                    QueryDword(fakeSettings, L"ApoInstallFailureExitCode", recordedExit) &&
                    QueryDword(fakeSettings, L"ApoInstallFailureWin32Known", knownWin32) &&
                    QueryRegistryStringValue(fakeSettings, L"ApoInstallFailurePhase", phase);
                if ((resetResult != ERROR_SUCCESS || installExit != 40 || !recorded ||
                    validInstall != 1 || recordedExit != 40 || knownWin32 != 0 || phase != L"validate-path" ||
                    captured.str().find(L"exit=40") == std::wstring::npos) && !result) result = 95;
            }
            RegCloseKey(fakeSettings);
        }
        if (backup) RegCloseKey(backup);
        if (fx) RegCloseKey(fx);
        RegCloseKey(root);
        if (RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()) != ERROR_SUCCESS && !result) result = 85;
        if (!result) std::wcout << L"64 masks, isolated transaction, audited Realtek rebuild fixture, and fail-closed checks passed.\n";
        return result;
    }

    int SelfTest(const wchar_t* dllPath)
    {
        if (!dllPath) return 70;
        // AERT_Allocate locks the APO's real-time state.  Ordinary diagnostic
        // processes can have a much smaller working-set allowance than
        // audiodg.exe, so reserve a representative allowance before exercising
        // LockForProcess outside the audio engine.
        SetProcessWorkingSetSize(GetCurrentProcess(), 16u * 1024u * 1024u,
            256u * 1024u * 1024u);
        HMODULE module = LoadLibraryExW(dllPath, nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) return 71;
        using GetClassObjectFunction = HRESULT (STDAPICALLTYPE*)(
            REFCLSID, REFIID, void**);
        auto getClassObject = reinterpret_cast<GetClassObjectFunction>(
            GetProcAddress(module, "DllGetClassObject"));
        using OfflinePeqSelfTestFunction = HRESULT (STDAPICALLTYPE*)();
        auto offlinePeqSelfTest = reinterpret_cast<OfflinePeqSelfTestFunction>(
            GetProcAddress(module, "HXAudioApoOfflinePeqSelfTest"));
        if (!getClassObject || !offlinePeqSelfTest)
        {
            FreeLibrary(module);
            return 72;
        }
        const CLSID classId =
        { 0xc4d1ba43, 0xc7a8, 0x4b47, { 0x8d, 0xcf, 0x6d, 0xcf, 0x55, 0x4d, 0x0f, 0x59 } };
        IClassFactory* factory = nullptr;
        HRESULT hr = getClassObject(classId, IID_IClassFactory,
            reinterpret_cast<void**>(&factory));
        IAudioProcessingObject* apo = nullptr;
        if (SUCCEEDED(hr))
            hr = factory->CreateInstance(nullptr, __uuidof(IAudioProcessingObject),
                reinterpret_cast<void**>(&apo));
        APO_REG_PROPERTIES* properties = nullptr;
        if (SUCCEEDED(hr)) hr = apo->GetRegistrationProperties(&properties);
        bool valid = SUCCEEDED(hr) && properties && properties->clsid == classId &&
            (properties->Flags & APO_FLAG_INPLACE) != 0 &&
            properties->u32MinInputConnections == 1 &&
            properties->u32MaxInputConnections == 1 &&
            properties->u32MinOutputConnections == 1 &&
            properties->u32MaxOutputConnections == 1 &&
            properties->u32NumAPOInterfaces == 1 &&
            properties->iidAPOInterfaceList[0] == __uuidof(IAudioProcessingObject);
        IAudioSystemEffects* effects = nullptr;
        if (valid)
            valid = SUCCEEDED(apo->QueryInterface(__uuidof(IAudioSystemEffects),
                reinterpret_cast<void**>(&effects)));
        if (effects) effects->Release();
        if (properties) CoTaskMemFree(properties);
        if (apo) apo->Release();
        if (!valid)
            std::wcerr << L"APO registration contract failed; HRESULT=0x"
                << std::hex << static_cast<unsigned long>(hr) << L". ";

        struct TestFormat
        {
            UINT32 sampleRate;
            WORD channels;
            DWORD channelMask;
        };
        const std::array<TestFormat, 13> formats =
        {{
            { 8000, 1, SPEAKER_FRONT_CENTER },
            { 11025, 2, SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT },
            { 16000, 1, SPEAKER_FRONT_CENTER },
            { 22050, 2, SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT },
            { 32000, 2, SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT },
            { 44100, 1, SPEAKER_FRONT_CENTER },
            { 48000, 2, SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT },
            { 88200, 2, SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT },
            { 96000, 6, KSAUDIO_SPEAKER_5POINT1 },
            { 176400, 2, SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT },
            { 192000, 8, KSAUDIO_SPEAKER_7POINT1_SURROUND },
            { 352800, 2, SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT },
            { 384000, 8, KSAUDIO_SPEAKER_7POINT1_SURROUND },
        }};

        for (const auto& test : formats)
        {
            if (!valid) break;
            IAudioProcessingObject* processing = nullptr;
            IAudioProcessingObjectConfiguration* configuration = nullptr;
            IAudioProcessingObjectRT* realtime = nullptr;
            IAudioMediaType* mediaType = nullptr;

            hr = factory->CreateInstance(nullptr, __uuidof(IAudioProcessingObject),
                reinterpret_cast<void**>(&processing));
            if (SUCCEEDED(hr))
            {
                APOInitSystemEffects initialization{};
                initialization.APOInit.cbSize = sizeof(initialization);
                initialization.APOInit.clsid = classId;
                hr = processing->Initialize(sizeof(initialization),
                    reinterpret_cast<BYTE*>(&initialization));
            }
            if (SUCCEEDED(hr))
                hr = processing->QueryInterface(__uuidof(IAudioProcessingObjectConfiguration),
                    reinterpret_cast<void**>(&configuration));
            if (SUCCEEDED(hr))
                hr = processing->QueryInterface(__uuidof(IAudioProcessingObjectRT),
                    reinterpret_cast<void**>(&realtime));

            WAVEFORMATEXTENSIBLE wave{};
            wave.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
            wave.Format.nChannels = test.channels;
            wave.Format.nSamplesPerSec = test.sampleRate;
            wave.Format.wBitsPerSample = 32;
            wave.Format.nBlockAlign = static_cast<WORD>(test.channels * sizeof(float));
            wave.Format.nAvgBytesPerSec = test.sampleRate * wave.Format.nBlockAlign;
            wave.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
            wave.Samples.wValidBitsPerSample = 32;
            wave.dwChannelMask = test.channelMask;
            wave.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
            if (SUCCEEDED(hr))
                hr = CreateAudioMediaType(reinterpret_cast<WAVEFORMATEX*>(&wave),
                    sizeof(wave), &mediaType);

            constexpr UINT32 frames = 256;
            std::vector<float> input(static_cast<size_t>(frames) * test.channels);
            std::vector<float> output(input.size(), 0.0f);
            for (UINT32 frame = 0; frame < frames; ++frame)
            {
                const float sample = static_cast<float>(0.000001 *
                    std::sin(2.0 * 3.14159265358979323846 * 997.0 *
                        frame / test.sampleRate));
                for (WORD channel = 0; channel < test.channels; ++channel)
                    input[static_cast<size_t>(frame) * test.channels + channel] = sample;
            }

            APO_CONNECTION_DESCRIPTOR inputDescriptor{};
            inputDescriptor.Type = APO_CONNECTION_BUFFER_TYPE_EXTERNAL;
            inputDescriptor.pBuffer = reinterpret_cast<UINT_PTR>(input.data());
            inputDescriptor.u32MaxFrameCount = frames;
            inputDescriptor.pFormat = mediaType;
            inputDescriptor.u32Signature = APO_CONNECTION_DESCRIPTOR_SIGNATURE;
            APO_CONNECTION_DESCRIPTOR outputDescriptor = inputDescriptor;
            outputDescriptor.pBuffer = reinterpret_cast<UINT_PTR>(output.data());
            APO_CONNECTION_DESCRIPTOR* inputDescriptors[] = { &inputDescriptor };
            APO_CONNECTION_DESCRIPTOR* outputDescriptors[] = { &outputDescriptor };
            bool locked = false;
            if (SUCCEEDED(hr))
            {
                hr = configuration->LockForProcess(1, inputDescriptors,
                    1, outputDescriptors);
                locked = SUCCEEDED(hr);
            }

            APO_CONNECTION_PROPERTY inputProperty{};
            inputProperty.pBuffer = reinterpret_cast<UINT_PTR>(input.data());
            inputProperty.u32ValidFrameCount = frames;
            inputProperty.u32BufferFlags = BUFFER_VALID;
            inputProperty.u32Signature = APO_CONNECTION_PROPERTY_SIGNATURE;
            APO_CONNECTION_PROPERTY outputProperty{};
            outputProperty.pBuffer = reinterpret_cast<UINT_PTR>(output.data());
            outputProperty.u32BufferFlags = BUFFER_INVALID;
            outputProperty.u32Signature = APO_CONNECTION_PROPERTY_SIGNATURE;
            APO_CONNECTION_PROPERTY* inputProperties[] = { &inputProperty };
            APO_CONNECTION_PROPERTY* outputProperties[] = { &outputProperty };
            if (SUCCEEDED(hr))
                realtime->APOProcess(1, inputProperties, 1, outputProperties);

            double energy = 0.0;
            if (SUCCEEDED(hr))
            {
                valid = outputProperty.u32BufferFlags == BUFFER_VALID &&
                    outputProperty.u32ValidFrameCount == frames;
                for (const float sample : output)
                {
                    if (!std::isfinite(sample)) { valid = false; break; }
                    energy += static_cast<double>(sample) * sample;
                }
                valid = valid && energy > 0.0;
            }
            else valid = false;

            if (!valid)
            {
                std::wcerr << L"APO lifecycle failed at " << std::dec
                    << test.sampleRate << L" Hz / " << test.channels
                    << L" channels; HRESULT=0x" << std::hex
                    << static_cast<unsigned long>(hr)
                    << L", outputFlags=" << outputProperty.u32BufferFlags
                    << L", outputFrames=" << std::dec
                    << outputProperty.u32ValidFrameCount << L". ";
            }

            if (locked) configuration->UnlockForProcess();
            if (mediaType) mediaType->Release();
            if (realtime) realtime->Release();
            if (configuration) configuration->Release();
            if (processing) processing->Release();
        }

        if (valid)
        {
            const HRESULT peqResult = offlinePeqSelfTest();
            valid = SUCCEEDED(peqResult);
            if (!valid)
                std::wcerr << L"Production PEQ frequency response failed; HRESULT=0x"
                    << std::hex << static_cast<unsigned long>(peqResult) << L". ";
        }
        if (factory) factory->Release();
        FreeLibrary(module);
        if (!valid) return 73;
        std::wcout << L"HXAudio APO registration, Initialize/Lock/APOProcess formats, "
            L"and frequency-selective PEQ passed.";
        return 0;
    }

    int RunServiceDispatcher()
    {
        SERVICE_TABLE_ENTRYW table[] =
        {
            { const_cast<wchar_t*>(kServiceName), ServiceMain },
            { nullptr, nullptr },
        };
        if (!StartServiceCtrlDispatcherW(table))
        {
            std::wcerr << L"Unable to connect HXAudio APO guard to SCM: " << LastErrorText();
            return 61;
        }
        return 0;
    }
}
