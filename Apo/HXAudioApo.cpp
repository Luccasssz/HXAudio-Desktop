#include "HXAudioApo.h"
#include "HXAudioApoProbe.h"
#include "..\Shared\HXAudioDspHeadroom.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cwchar>
#include <limits>
#include <new>
#include <string>
#include <vector>

#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <devpkey.h>
#include <audioengineextensionapo.h>
#include <ks.h>
#include <ksmedia.h>

#pragma comment(lib, "audiobaseprocessingobject.lib")
#pragma comment(lib, "audioeng.lib")

// {C4D1BA43-C7A8-4B47-8DCF-6DCF554D0F59}
const CLSID CLSID_HXAudioEndpointApo =
{ 0xc4d1ba43, 0xc7a8, 0x4b47, { 0x8d, 0xcf, 0x6d, 0xcf, 0x55, 0x4d, 0x0f, 0x59 } };

namespace
{
    constexpr wchar_t kSettingsPath[] = L"Software\\HXAudio\\VirtualAudio";
    constexpr wchar_t kSettingsNotifyPath[] =
        L"Software\\HXAudio\\VirtualAudio\\DspNotify";
    constexpr wchar_t kRuntimeStatusPath[] =
        L"Software\\HXAudio\\Apo\\RuntimeStatus";
    constexpr UINT32 kMaximumChannels = 32;
    constexpr UINT32 kMaximumPeqBands = 256;
    constexpr double kPi = 3.14159265358979323846;
    enum class PeqType
    {
        Peak,
        LowShelf,
        HighShelf,
        LowPass,
        HighPass,
        Notch,
    };

    struct PeqBand
    {
        bool enabled = true;
        PeqType type = PeqType::Peak;
        double frequency = 1000.0;
        double gainDb = 0.0;
        double q = 0.9;
    };

    struct Settings
    {
        DWORD generation = 0;
        bool processingEnabled = true;
        bool virtualBassEnabled = false;
        DWORD virtualBassMode = 0;
        std::array<double, 3> virtualBassMultipliers{ 2.0, 3.0, 4.0 };
        double virtualBassAmount = 0.0;
        double virtualBassCutoff = 80.0;
        double virtualBassSubsonic = 18.0;
        double preampDb = 0.0;
        std::vector<PeqBand> bands;
    };

    struct Coefficients
    {
        double b0 = 1.0;
        double b1 = 0.0;
        double b2 = 0.0;
        double a1 = 0.0;
        double a2 = 0.0;
    };

    DWORD ReadDword(HKEY key, const wchar_t* name, DWORD fallback)
    {
        DWORD type = 0;
        DWORD size = sizeof(DWORD);
        DWORD value = fallback;
        return RegQueryValueExW(key, name, nullptr, &type,
            reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS &&
            type == REG_DWORD ? value : fallback;
    }

    std::wstring ReadString(HKEY key, const wchar_t* name)
    {
        DWORD type = 0;
        DWORD size = 0;
        if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS ||
            type != REG_SZ || size < sizeof(wchar_t)) return {};
        std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 1, L'\0');
        if (RegQueryValueExW(key, name, nullptr, &type,
            reinterpret_cast<BYTE*>(buffer.data()), &size) != ERROR_SUCCESS) return {};
        return buffer.data();
    }

    std::vector<std::wstring> ReadMultiString(HKEY key, const wchar_t* name)
    {
        std::vector<std::wstring> values;
        DWORD type = 0;
        DWORD size = 0;
        if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS ||
            type != REG_MULTI_SZ || size < 2 * sizeof(wchar_t)) return values;
        std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 2, L'\0');
        if (RegQueryValueExW(key, name, nullptr, &type,
            reinterpret_cast<BYTE*>(buffer.data()), &size) != ERROR_SUCCESS) return values;
        for (const wchar_t* item = buffer.data(); *item; item += wcslen(item) + 1)
            values.emplace_back(item);
        return values;
    }

    bool IsMultiString(HKEY key, const wchar_t* name)
    {
        DWORD type = 0;
        DWORD size = 0;
        return RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) ==
            ERROR_SUCCESS && type == REG_MULTI_SZ;
    }

    DWORD ReadSettingsGeneration()
    {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kSettingsNotifyPath, 0,
            KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) return 0;
        const DWORD generation = ReadDword(key, L"Generation", 0);
        RegCloseKey(key);
        return generation;
    }

    std::wstring RegistrySuffix(const std::wstring& value)
    {
        uint64_t hash = 1469598103934665603ull;
        for (const wchar_t character : value)
        {
            hash ^= static_cast<uint16_t>(character);
            hash *= 1099511628211ull;
        }
        wchar_t suffix[17]{};
        swprintf_s(suffix, L"%016llX", static_cast<unsigned long long>(hash));
        return suffix;
    }

    std::wstring DevicePropertyString(IMMDevice* device,
        const PROPERTYKEY& property)
    {
        if (!device) return {};
        CComPtr<IPropertyStore> store;
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
        return result;
    }

    std::wstring DeviceProfileKey(IMMDevice* device)
    {
        if (!device) return {};
        const std::wstring name = DevicePropertyString(device,
            PKEY_Device_FriendlyName);
        CComPtr<IPropertyStore> store;
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
            return result;
        }
        PropVariantClear(&container);
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

    void WriteDiagnosticDword(const wchar_t* name, DWORD value)
    {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRuntimeStatusPath, 0,
            KEY_SET_VALUE | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS)
        {
            RegSetValueExW(key, name, 0, REG_DWORD,
                reinterpret_cast<const BYTE*>(&value), sizeof(value));
            RegCloseKey(key);
        }
    }

    bool ParseDouble(const std::wstring& text, double& value)
    {
        if (text.empty()) return false;
        wchar_t* end = nullptr;
        const double parsed = wcstod(text.c_str(), &end);
        while (end && *end && iswspace(*end)) ++end;
        if (!end || end == text.c_str() || *end || !std::isfinite(parsed)) return false;
        value = parsed;
        return true;
    }

    std::vector<std::wstring> Split(const std::wstring& text, wchar_t separator)
    {
        std::vector<std::wstring> fields;
        size_t start = 0;
        while (start <= text.size())
        {
            const size_t at = text.find(separator, start);
            fields.push_back(text.substr(start,
                at == std::wstring::npos ? std::wstring::npos : at - start));
            if (at == std::wstring::npos) break;
            start = at + 1;
        }
        return fields;
    }

    bool ParseBand(const std::wstring& encoded, PeqBand& band)
    {
        const auto fields = Split(encoded, L'|');
        if (fields.size() != 5) return false;
        if (fields[0] == L"1") band.enabled = true;
        else if (fields[0] == L"0") band.enabled = false;
        else return false;

        if (fields[1] == L"PK") band.type = PeqType::Peak;
        else if (fields[1] == L"LS") band.type = PeqType::LowShelf;
        else if (fields[1] == L"HS") band.type = PeqType::HighShelf;
        else if (fields[1] == L"LP") band.type = PeqType::LowPass;
        else if (fields[1] == L"HP") band.type = PeqType::HighPass;
        else if (fields[1] == L"NO") band.type = PeqType::Notch;
        else return false;

        if (!ParseDouble(fields[2], band.frequency) || band.frequency <= 0.0 ||
            !ParseDouble(fields[3], band.gainDb) ||
            !ParseDouble(fields[4], band.q) || band.q <= 0.0) return false;
        band.gainDb = HXAudioDsp::ClampPeqGainDb(band.gainDb);
        return true;
    }

    Settings LoadSettings(const std::wstring& profileKey = {},
        const std::wstring& endpointId = {})
    {
        Settings settings;
        settings.generation = ReadSettingsGeneration();
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kSettingsPath, 0,
            KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) return settings;

        const std::wstring installationMode = ReadString(key, L"InstallationMode");
        settings.processingEnabled =
            installationMode.rfind(L"Virtual", 0) != 0;

        const DWORD amount = std::clamp<DWORD>(ReadDword(key,
            L"DspVirtualBassAmount", 0), 0, 200);
        settings.virtualBassEnabled = ReadDword(key, L"DspVirtualBassEnabled",
            amount ? 1 : 0) != 0;
        settings.virtualBassMode = std::min<DWORD>(
            ReadDword(key, L"DspVirtualBassMode", 0), 2);
        constexpr std::array<const wchar_t*, 3> multiplierNames{
            L"DspVirtualBassMultiplier1", L"DspVirtualBassMultiplier2",
            L"DspVirtualBassMultiplier3" };
        for (size_t index = 0; index < settings.virtualBassMultipliers.size(); ++index)
        {
            double multiplier = settings.virtualBassMultipliers[index];
            if (ParseDouble(ReadString(key, multiplierNames[index]), multiplier) &&
                multiplier > 0.0 && std::isfinite(multiplier * kPi))
                settings.virtualBassMultipliers[index] = multiplier;
        }
        settings.virtualBassAmount = amount / 100.0;
        if (!ParseDouble(ReadString(key, L"DspVirtualBassCutoffHzText"),
            settings.virtualBassCutoff))
            settings.virtualBassCutoff = std::max<DWORD>(1,
                ReadDword(key, L"DspVirtualBassCutoffHz", 80));
        if (!ParseDouble(ReadString(key, L"DspVirtualBassSubsonicHzText"),
            settings.virtualBassSubsonic))
            settings.virtualBassSubsonic = ReadDword(key, L"DspVirtualBassSubsonicHz", 18);
        if (!ParseDouble(ReadString(key, L"DspPreampDb"), settings.preampDb))
        {
            const int encoded = static_cast<int>(ReadDword(key, L"DspPreampOffset", 240));
            settings.preampDb = std::clamp(encoded - 240, -240, 240) / 10.0;
        }
        settings.preampDb = HXAudioDsp::ClampGainDb(settings.preampDb);

        bool bandsConfigured = false;
        const auto loadBands = [&](const std::wstring& identity)
        {
            if (identity.empty()) return false;
            const std::wstring suffix = RegistrySuffix(identity);
            const std::wstring idName = L"DspPeqDeviceId_" + suffix;
            const std::wstring bandsName = L"DspPeqBands_" + suffix;
            if (_wcsicmp(ReadString(key, idName.c_str()).c_str(),
                identity.c_str()) != 0 || !IsMultiString(key, bandsName.c_str()))
                return false;
            for (const auto& encoded : ReadMultiString(key, bandsName.c_str()))
            {
                PeqBand band;
                if (ParseBand(encoded, band)) settings.bands.push_back(band);
                if (settings.bands.size() == kMaximumPeqBands) break;
            }
            return true;
        };
        bandsConfigured = loadBands(profileKey);
        if (!bandsConfigured && !endpointId.empty() && endpointId != profileKey)
            bandsConfigured = loadBands(endpointId);
        if (!bandsConfigured && IsMultiString(key, L"DspPeqBands"))
        {
            bandsConfigured = true;
            for (const auto& encoded : ReadMultiString(key, L"DspPeqBands"))
            {
                PeqBand band;
                if (ParseBand(encoded, band)) settings.bands.push_back(band);
                if (settings.bands.size() == kMaximumPeqBands) break;
            }
        }
        if (!bandsConfigured)
        {
            const auto legacy = [&](const wchar_t* name) -> double
            {
                const int encoded = static_cast<int>(ReadDword(key, name, 120));
                return std::clamp(encoded - 120, -120, 120) / 10.0;
            };
            settings.bands.push_back({ true, PeqType::LowShelf, 120.0,
                legacy(L"DspBassOffset"), 0.7071067811865476 });
            settings.bands.push_back({ true, PeqType::Peak, 1000.0,
                legacy(L"DspMidrangeOffset"), 0.9 });
            settings.bands.push_back({ true, PeqType::HighShelf, 8000.0,
                legacy(L"DspTrebleOffset"), 0.7071067811865476 });
        }
        RegCloseKey(key);
        return settings;
    }

    Coefficients Normalize(double b0, double b1, double b2,
        double a0, double a1, double a2)
    {
        if (!std::isfinite(a0) || std::abs(a0) < std::numeric_limits<double>::min())
            return {};
        Coefficients result{ b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
        if (!std::isfinite(result.b0) || !std::isfinite(result.b1) ||
            !std::isfinite(result.b2) || !std::isfinite(result.a1) ||
            !std::isfinite(result.a2)) return {};
        return result;
    }

    bool Representable(double sampleRate, double frequency, double q)
    {
        return std::isfinite(sampleRate) && sampleRate > 0.0 &&
            std::isfinite(frequency) && frequency > 0.0 && frequency < sampleRate * 0.5 &&
            std::isfinite(q) && q > 0.0;
    }

    Coefficients MakePass(double sampleRate, double frequency, double q, bool high)
    {
        if (!Representable(sampleRate, frequency, q)) return {};
        const double omega = 2.0 * kPi * frequency / sampleRate;
        const double cosine = std::cos(omega);
        const double alpha = std::sin(omega) / (2.0 * q);
        if (high)
            return Normalize((1.0 + cosine) / 2.0, -(1.0 + cosine),
                (1.0 + cosine) / 2.0, 1.0 + alpha, -2.0 * cosine, 1.0 - alpha);
        return Normalize((1.0 - cosine) / 2.0, 1.0 - cosine,
            (1.0 - cosine) / 2.0, 1.0 + alpha, -2.0 * cosine, 1.0 - alpha);
    }

    Coefficients MakePeak(double sampleRate, const PeqBand& band)
    {
        const double gainDb = HXAudioDsp::ClampPeqGainDb(band.gainDb);
        if (!Representable(sampleRate, band.frequency, band.q) ||
            std::abs(gainDb) < 0.00001) return {};
        const double omega = 2.0 * kPi * band.frequency / sampleRate;
        const double cosine = std::cos(omega);
        const double alpha = std::sin(omega) / (2.0 * band.q);
        const double amplitude = std::pow(10.0, gainDb / 40.0);
        return Normalize(1.0 + alpha * amplitude, -2.0 * cosine,
            1.0 - alpha * amplitude, 1.0 + alpha / amplitude,
            -2.0 * cosine, 1.0 - alpha / amplitude);
    }

    Coefficients MakeShelf(double sampleRate, const PeqBand& band, bool high)
    {
        const double gainDb = HXAudioDsp::ClampPeqGainDb(band.gainDb);
        if (!Representable(sampleRate, band.frequency, band.q) ||
            std::abs(gainDb) < 0.00001) return {};
        const double amplitude = std::pow(10.0, gainDb / 40.0);
        const double omega = 2.0 * kPi * band.frequency / sampleRate;
        const double cosine = std::cos(omega);
        const double alpha = std::sin(omega) / (2.0 * band.q);
        const double beta = 2.0 * std::sqrt(amplitude) * alpha;
        if (high)
            return Normalize(
                amplitude * ((amplitude + 1.0) + (amplitude - 1.0) * cosine + beta),
                -2.0 * amplitude * ((amplitude - 1.0) + (amplitude + 1.0) * cosine),
                amplitude * ((amplitude + 1.0) + (amplitude - 1.0) * cosine - beta),
                (amplitude + 1.0) - (amplitude - 1.0) * cosine + beta,
                2.0 * ((amplitude - 1.0) - (amplitude + 1.0) * cosine),
                (amplitude + 1.0) - (amplitude - 1.0) * cosine - beta);
        return Normalize(
            amplitude * ((amplitude + 1.0) - (amplitude - 1.0) * cosine + beta),
            2.0 * amplitude * ((amplitude - 1.0) - (amplitude + 1.0) * cosine),
            amplitude * ((amplitude + 1.0) - (amplitude - 1.0) * cosine - beta),
            (amplitude + 1.0) + (amplitude - 1.0) * cosine + beta,
            -2.0 * ((amplitude - 1.0) + (amplitude + 1.0) * cosine),
            (amplitude + 1.0) + (amplitude - 1.0) * cosine - beta);
    }

    Coefficients MakeNotch(double sampleRate, const PeqBand& band)
    {
        if (!Representable(sampleRate, band.frequency, band.q)) return {};
        const double omega = 2.0 * kPi * band.frequency / sampleRate;
        const double cosine = std::cos(omega);
        const double alpha = std::sin(omega) / (2.0 * band.q);
        return Normalize(1.0, -2.0 * cosine, 1.0,
            1.0 + alpha, -2.0 * cosine, 1.0 - alpha);
    }

    Coefficients MakeBand(double sampleRate, const PeqBand& band)
    {
        switch (band.type)
        {
        case PeqType::Peak: return MakePeak(sampleRate, band);
        case PeqType::LowShelf: return MakeShelf(sampleRate, band, false);
        case PeqType::HighShelf: return MakeShelf(sampleRate, band, true);
        case PeqType::LowPass: return MakePass(sampleRate, band.frequency, band.q, false);
        case PeqType::HighPass: return MakePass(sampleRate, band.frequency, band.q, true);
        case PeqType::Notch: return MakeNotch(sampleRate, band);
        }
        return {};
    }
}

struct CHXAudioEndpointApo::Runtime
{
    static constexpr size_t kCustomTransferSize = 1025;

    struct Biquad
    {
        Coefficients coefficients{};
        double* z1 = nullptr;
        double* z2 = nullptr;
        UINT32 stateChannels = 0;

        void Bind(double*& state, UINT32 channelCount)
        {
            stateChannels = channelCount;
            z1 = state;
            state += channelCount;
            z2 = state;
            state += channelCount;
        }

        void Configure(const Coefficients& next)
        {
            coefficients = next;
            if (z1) ZeroMemory(z1, static_cast<size_t>(stateChannels) * sizeof(double));
            if (z2) ZeroMemory(z2, static_cast<size_t>(stateChannels) * sizeof(double));
        }

        float Process(float input, UINT32 channel)
        {
            const double output = coefficients.b0 * input + z1[channel];
            z1[channel] = coefficients.b1 * input - coefficients.a1 * output + z2[channel];
            z2[channel] = coefficients.b2 * input - coefficients.a2 * output;
            return static_cast<float>(output);
        }
    };

    UINT32 channels = 2;
    DWORD settingsGeneration = 0;
    bool processingEnabled = true;
    double preamp = 1.0;
    double staticHeadroomDb = 0.0;
    bool bassEnabled = false;
    DWORD bassMode = 0;
    std::array<double, 3> bassMultipliers{ 2.0, 3.0, 4.0 };
    std::array<std::array<float, kCustomTransferSize>, 3> customTransfer{};
    double bassAmount = 0.0;
    UINT32 bandCount = 0;
    UINT32 bandCapacity = 0;
    Biquad subsonic[2];
    Biquad lowPass[2];
    Biquad dryHighPass[4];
    Biquad harmonicHighPass[4];
    Biquad* bands = nullptr;

    static size_t RequiredBytes(UINT32 channelCount, UINT32 peqCapacity)
    {
        const size_t bandOffset = (sizeof(Runtime) + alignof(Biquad) - 1) &
            ~(alignof(Biquad) - 1);
        const size_t bandBytes = static_cast<size_t>(peqCapacity) * sizeof(Biquad);
        const size_t stateOffset = (bandOffset + bandBytes + alignof(double) - 1) &
            ~(alignof(double) - 1);
        constexpr size_t fixedFilterCount = 12;
        const size_t stateValues = (fixedFilterCount + peqCapacity) *
            static_cast<size_t>(channelCount) * 2;
        if (stateValues > (std::numeric_limits<size_t>::max() - stateOffset) /
            sizeof(double)) return 0;
        return stateOffset + stateValues * sizeof(double);
    }

    void InitializeStorage(UINT32 channelCount, UINT32 peqCapacity)
    {
        channels = channelCount;
        bandCapacity = peqCapacity;
        auto address = reinterpret_cast<uintptr_t>(this) + sizeof(Runtime);
        address = (address + alignof(Biquad) - 1) & ~(alignof(Biquad) - 1);
        bands = reinterpret_cast<Biquad*>(address);
        for (UINT32 index = 0; index < bandCapacity; ++index)
            new(&bands[index]) Biquad{};
        address += static_cast<size_t>(bandCapacity) * sizeof(Biquad);
        address = (address + alignof(double) - 1) & ~(alignof(double) - 1);
        double* state = reinterpret_cast<double*>(address);
        for (auto& filter : subsonic) filter.Bind(state, channels);
        for (auto& filter : lowPass) filter.Bind(state, channels);
        for (auto& filter : dryHighPass) filter.Bind(state, channels);
        for (auto& filter : harmonicHighPass) filter.Bind(state, channels);
        for (UINT32 index = 0; index < bandCapacity; ++index)
            bands[index].Bind(state, channels);
    }

    void Configure(UINT32 sampleRate, UINT32 channelCount, const Settings& settings)
    {
        channels = std::clamp<UINT32>(channelCount, 1, kMaximumChannels);
        settingsGeneration = settings.generation;
        processingEnabled = settings.processingEnabled;
        const double rate = std::max<double>(sampleRate, 8000.0);
        bassEnabled = settings.virtualBassEnabled;
        bassMode = settings.virtualBassMode;
        bassMultipliers = settings.virtualBassMultipliers;
        if (bassMode == 2)
        {
            for (size_t harmonic = 0; harmonic < bassMultipliers.size(); ++harmonic)
            {
                const double multiplier = bassMultipliers[harmonic];
                const double atSilence = std::cos(multiplier * kPi * 0.5);
                for (size_t step = 0; step < kCustomTransferSize; ++step)
                {
                    const double x = -1.0 + 2.0 * step /
                        static_cast<double>(kCustomTransferSize - 1);
                    customTransfer[harmonic][step] = static_cast<float>(
                        std::cos(multiplier * std::acos(x)) - atSilence);
                }
            }
        }
        bassAmount = std::clamp(settings.virtualBassAmount, 0.0, 2.0);

        constexpr double q = 0.7071067811865476;
        const double cutoff = std::clamp(settings.virtualBassCutoff,
            20.0, rate * 0.45);
        const double subsonicFrequency = std::clamp(settings.virtualBassSubsonic,
            0.0, std::max(0.0, cutoff * 0.95));
        const Coefficients subsonicCoefficients = subsonicFrequency > 0.0
            ? MakePass(rate, subsonicFrequency, q, true) : Coefficients{};
        for (auto& filter : this->subsonic) filter.Configure(subsonicCoefficients);
        for (auto& filter : lowPass) filter.Configure(MakePass(rate, cutoff, q, false));
        for (auto& filter : dryHighPass) filter.Configure(MakePass(rate, cutoff, q, true));
        for (auto& filter : harmonicHighPass) filter.Configure(MakePass(rate, cutoff, q, true));

        bandCount = 0;
        for (const auto& band : settings.bands)
        {
            if (!band.enabled || bandCount == bandCapacity) continue;
            bands[bandCount++].Configure(MakeBand(rate, band));
        }
        for (UINT32 i = bandCount; i < bandCapacity; ++i)
            bands[i].Configure({});

        staticHeadroomDb = 0.0;
        const double gain = std::pow(10.0,
            HXAudioDsp::ClampGainDb(settings.preampDb) / 20.0);
        preamp = std::isfinite(gain) ? gain : 0.0;
    }

    float ProcessBass(float input, UINT32 channel)
    {
        if (!bassEnabled) return input;
        float cleaned = subsonic[0].Process(input, channel);
        cleaned = subsonic[1].Process(cleaned, channel);
        if (bassAmount <= 0.0) return cleaned;

        float low = lowPass[0].Process(cleaned, channel);
        low = lowPass[1].Process(low, channel);
        float high = cleaned;
        for (auto& filter : dryHighPass) high = filter.Process(high, channel);

        const double drive = 1.0 + 7.5 * bassAmount;
        const double driven = static_cast<double>(low) * drive;
        const double x = driven;
        const double x2 = x * x;
        const double x3 = x2 * x;
        const double x4 = x2 * x2;
        double generated = 0.0;
        if (bassMode == 1)
        {
            const double x5 = x4 * x;
            const double x7 = x5 * x2;
            generated = (0.62 * (4.0 * x3 - 3.0 * x) +
                0.28 * (16.0 * x5 - 20.0 * x3 + 5.0 * x) +
                0.10 * (64.0 * x7 - 112.0 * x5 + 56.0 * x3 - 7.0 * x)) / drive;
        }
        else if (bassMode == 0)
        {
            generated = (0.58 * (2.0 * x2) +
                0.29 * (4.0 * x3 - 3.0 * x) +
                0.13 * (8.0 * x4 - 8.0 * x2)) / drive;
        }
        else
        {
            constexpr std::array<double, 3> weights{ 0.58, 0.29, 0.13 };
            for (size_t index = 0; index < bassMultipliers.size(); ++index)
            {
                const double multiplier = bassMultipliers[index];
                const double shaped = std::cos(multiplier *
                    (x + kPi * 0.5)) -
                    std::cos(multiplier * kPi * 0.5);
                generated += weights[index] * shaped;
            }
            generated /= drive;
        }
        float harmonics = static_cast<float>(generated * bassAmount * 2.5);
        for (auto& filter : harmonicHighPass) harmonics = filter.Process(harmonics, channel);
        const double mix = std::min(bassAmount, 1.0);
        return static_cast<float>((1.0 - mix) * cleaned + mix * high + harmonics);
    }

    float ProcessSample(float input, UINT32 channel)
    {
        if (!processingEnabled) return input;
        float value = static_cast<float>(ProcessBass(input, channel) * preamp);
        for (UINT32 band = 0; band < bandCount; ++band)
            value = bands[band].Process(value, channel);
        return value;
    }

    void Process(float* samples, UINT32 frames)
    {
        for (UINT32 frame = 0; frame < frames; ++frame)
        {
            for (UINT32 channel = 0; channel < channels; ++channel)
            {
                const size_t index = static_cast<size_t>(frame) * channels + channel;
                samples[index] = ProcessSample(samples[index], channel);
            }
        }
    }
};

#pragma warning(push)
#pragma warning(disable: 4815)
const AVRT_DATA CRegAPOProperties<1> CHXAudioEndpointApo::sm_RegProperties(
    CLSID_HXAudioEndpointApo,
    L"HXAudio Global Endpoint APO",
    L"Copyright (c) Huber_HaYu",
    1,
    0,
    // Register the object by its audio-processing contract.  IAudioSystemEffects
    // remains exposed as the endpoint-system-effect marker, but it has no
    // processing methods and must not be the APO's primary interface.
    __uuidof(IAudioProcessingObject),
    // Match the proven Equalizer APO contract: frame rate and sample container
    // must match, while the endpoint graph is allowed to negotiate its channel
    // layout.  LockForProcess validates the actual input/output layouts before
    // enabling the in-place DSP.
    static_cast<APO_FLAG>(APO_FLAG_FRAMESPERSECOND_MUST_MATCH |
        APO_FLAG_BITSPERSAMPLE_MUST_MATCH | APO_FLAG_INPLACE));
#pragma warning(pop)

HRESULT CHXAudioEndpointApo::RunOfflinePeqSelfTest()
{
    constexpr UINT32 sampleRate = 48000;
    constexpr UINT32 frames = sampleRate;
    constexpr UINT32 settleFrames = 4096;
    constexpr double amplitude = 0.001;

    Settings settings;
    settings.bands.push_back({ true, PeqType::Peak, 1000.0, 12.0, 2.0 });

    const auto measureGainDb = [&](double frequency, double& gainDb) -> bool
    {
        const size_t runtimeBytes = Runtime::RequiredBytes(1, 1);
        std::vector<BYTE> runtimeStorage(runtimeBytes);
        auto* runtime = new(runtimeStorage.data()) Runtime{};
        runtime->InitializeStorage(1, 1);
        runtime->Configure(sampleRate, 1, settings);
        std::vector<float> samples(frames);
        double inputEnergy = 0.0;
        for (UINT32 frame = 0; frame < frames; ++frame)
        {
            const double input = amplitude * std::sin(
                2.0 * kPi * frequency * frame / sampleRate);
            samples[frame] = static_cast<float>(input);
            if (frame >= settleFrames) inputEnergy += input * input;
        }
        runtime->Process(samples.data(), frames);
        double outputEnergy = 0.0;
        for (UINT32 frame = settleFrames; frame < frames; ++frame)
        {
            const double output = samples[frame];
            if (!std::isfinite(output)) return false;
            outputEnergy += output * output;
        }
        if (!(inputEnergy > 0.0) || !(outputEnergy > 0.0)) return false;
        gainDb = 10.0 * std::log10(outputEnergy / inputEnergy);
        return std::isfinite(gainDb);
    };

    double centerGain = 0.0;
    double referenceGain = 0.0;
    if (!measureGainDb(1000.0, centerGain) ||
        !measureGainDb(8000.0, referenceGain)) return E_FAIL;

    // This checks the actual production Runtime, not a duplicate formula.
    // Allow a broad tolerance so SDK/compiler floating-point differences do
    // not create false failures, while still proving frequency-selective PEQ.
    if (!(centerGain >= 11.2 && centerGain <= 12.8 &&
        referenceGain >= -0.8 && referenceGain <= 0.8 &&
        centerGain - referenceGain >= 9.5)) return E_FAIL;

    {
        Settings passthrough;
        passthrough.processingEnabled = false;
        passthrough.preampDb = 30.0;
        passthrough.virtualBassEnabled = true;
        passthrough.virtualBassAmount = 2.0;
        passthrough.bands.push_back(
            { true, PeqType::Peak, 1000.0, 40.0, 1.0 });
        const size_t runtimeBytes = Runtime::RequiredBytes(2, 1);
        std::vector<BYTE> runtimeStorage(runtimeBytes);
        auto* runtime = new(runtimeStorage.data()) Runtime{};
        runtime->InitializeStorage(2, 1);
        runtime->Configure(sampleRate, 2, passthrough);
        float samples[] = { -0.75f, 0.5f, 0.25f, -0.125f };
        const float original[] = { -0.75f, 0.5f, 0.25f, -0.125f };
        runtime->Process(samples, 2);
        for (size_t index = 0; index < ARRAYSIZE(samples); ++index)
            if (samples[index] != original[index]) return E_FAIL;
    }

    const auto validateBass = [&](DWORD mode,
        const std::array<double, 3>& multipliers, double amplitude)
    {
        Settings bassSettings;
        bassSettings.virtualBassEnabled = true;
        bassSettings.virtualBassMode = mode;
        bassSettings.virtualBassMultipliers = multipliers;
        bassSettings.virtualBassAmount = 2.0;
        bassSettings.virtualBassCutoff = 80.0;
        bassSettings.virtualBassSubsonic = 15.0;
        const size_t runtimeBytes = Runtime::RequiredBytes(1, 0);
        std::vector<BYTE> runtimeStorage(runtimeBytes);
        auto* runtime = new(runtimeStorage.data()) Runtime{};
        runtime->InitializeStorage(1, 0);
        runtime->Configure(sampleRate, 1, bassSettings);
        std::vector<float> samples(sampleRate);
        for (UINT32 frame = 0; frame < sampleRate; ++frame)
            samples[frame] = static_cast<float>(amplitude * std::sin(
                2.0 * kPi * 50.0 * frame / sampleRate));
        runtime->Process(samples.data(), sampleRate);
        double peak = 0.0;
        double energy = 0.0;
        for (const float sample : samples)
        {
            if (!std::isfinite(sample)) return false;
            peak = std::max(peak, std::abs(static_cast<double>(sample)));
            energy += static_cast<double>(sample) * sample;
        }
        return energy > 0.0 && peak > 0.0 && std::isfinite(peak);
    };

    return validateBass(1, { 3.0, 5.0, 7.0 }, 0.98) &&
        validateBass(2, { 2.5, 3.75, 6.5 }, 0.25) ? S_OK : E_FAIL;
}

CHXAudioEndpointApo::CHXAudioEndpointApo()
    : CBaseAudioProcessingObject(sm_RegProperties)
{
    HXAudioApoRecordProbePhase(HXAudioApoProbePhase::Constructor);
    WriteDiagnosticDword(L"ApoRuntimeConstructorPid", GetCurrentProcessId());
    InitializeCriticalSection(&m_runtimeLock);
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

CHXAudioEndpointApo::~CHXAudioEndpointApo()
{
    StopSettingsThread();
    CloseProbe();
    for (LONG index = 0; index < kRuntimeSlotCount; ++index)
    {
        if (m_runtimeAllocations[index])
            AERT_Free(m_runtimeAllocations[index]);
        m_runtimeAllocations[index] = nullptr;
        m_runtimes[index] = nullptr;
    }
    if (m_stopEvent) CloseHandle(m_stopEvent);
    DeleteCriticalSection(&m_runtimeLock);
}

HRESULT CHXAudioEndpointApo::Initialize(UINT32 dataSize, BYTE* data)
{
    HXAudioApoRecordProbePhase(HXAudioApoProbePhase::InitializeEnter,
        S_FALSE, dataSize);
    const auto returnInitialize = [](HRESULT result)
    {
        HXAudioApoRecordProbePhase(HXAudioApoProbePhase::InitializeExit, result);
        return result;
    };
    WriteDiagnosticDword(L"ApoRuntimeInitializeDataSize", dataSize);
    WriteDiagnosticDword(L"ApoRuntimeInitializePid", GetCurrentProcessId());
    if ((!data && dataSize) || (data && !dataSize))
    {
        WriteDiagnosticDword(L"ApoRuntimeInitializeResult",
            static_cast<DWORD>(E_INVALIDARG));
        return returnInitialize(E_INVALIDARG);
    }
    if (m_bIsInitialized)
    {
        WriteDiagnosticDword(L"ApoRuntimeInitializeResult",
            static_cast<DWORD>(APOERR_ALREADY_INITIALIZED));
        return returnInitialize(APOERR_ALREADY_INITIALIZED);
    }
    if (dataSize != sizeof(APOInitSystemEffects) &&
        dataSize != sizeof(APOInitSystemEffects2)
#ifdef __IAudioSystemEffects3_INTERFACE_DEFINED__
        && dataSize != sizeof(APOInitSystemEffects3)
#endif
        )
    {
        WriteDiagnosticDword(L"ApoRuntimeInitializeResult",
            static_cast<DWORD>(E_INVALIDARG));
        return returnInitialize(E_INVALIDARG);
    }

    const auto captureEndpoint = [&](IMMDeviceCollection* collection)
    {
        if (!collection) return;
        UINT count = 0;
        if (FAILED(collection->GetCount(&count)) || count == 0) return;
        CComPtr<IMMDevice> device;
        // APOInitSystemEffects2/3 guarantees that the last item is the actual
        // audio endpoint. nSoftwareIoDeviceInCollection is a topology device,
        // not the endpoint identity used by the control panel.
        if (FAILED(collection->Item(count - 1, &device)) || !device) return;
        LPWSTR id = nullptr;
        if (SUCCEEDED(device->GetId(&id)) && id)
        {
            m_endpointId = id;
            CoTaskMemFree(id);
        }
        m_profileKey = DeviceProfileKey(device);
    };

    if (dataSize == sizeof(APOInitSystemEffects2))
    {
        const auto* initialization = reinterpret_cast<const APOInitSystemEffects2*>(data);
        captureEndpoint(initialization->pDeviceCollection);
    }
#ifdef __IAudioSystemEffects3_INTERFACE_DEFINED__
    else if (dataSize == sizeof(APOInitSystemEffects3))
    {
        const auto* initialization = reinterpret_cast<const APOInitSystemEffects3*>(data);
        captureEndpoint(initialization->pDeviceCollection);
    }
#endif
    else
    {
        const auto* initialization = reinterpret_cast<const APOInitSystemEffects*>(data);
        captureEndpoint(initialization->pDeviceCollection);
    }

    m_bIsInitialized = true;
    WriteDiagnosticDword(L"ApoRuntimeInitializeResult", S_OK);
    if (m_stopEvent && !m_settingsThread)
        m_settingsThread = CreateThread(nullptr, 0, SettingsThreadEntry, this, 0, nullptr);
    return returnInitialize(S_OK);
}

HRESULT CHXAudioEndpointApo::LockForProcess(UINT32 inputCount,
    APO_CONNECTION_DESCRIPTOR** inputs, UINT32 outputCount,
    APO_CONNECTION_DESCRIPTOR** outputs)
{
    HXAudioApoRecordProbePhase(HXAudioApoProbePhase::LockEnter);
    const auto returnLock = [](HRESULT result)
    {
        HXAudioApoRecordProbePhase(HXAudioApoProbePhase::LockExit, result);
        return result;
    };
    if (inputCount != 1 || outputCount != 1 || !inputs || !outputs ||
        !inputs[0] || !outputs[0] || !inputs[0]->pFormat || !outputs[0]->pFormat)
    {
        m_lastLockResult.store(APOERR_NUM_CONNECTIONS_INVALID,
            std::memory_order_relaxed);
        return returnLock(APOERR_NUM_CONNECTIONS_INVALID);
    }

    UNCOMPRESSEDAUDIOFORMAT inputFormat{};
    UNCOMPRESSEDAUDIOFORMAT outputFormat{};
    HRESULT hr = inputs[0]->pFormat->GetUncompressedAudioFormat(&inputFormat);
    if (SUCCEEDED(hr))
        hr = outputs[0]->pFormat->GetUncompressedAudioFormat(&outputFormat);
    if (FAILED(hr) || inputFormat.guidFormatType != KSDATAFORMAT_SUBTYPE_IEEE_FLOAT ||
        outputFormat.guidFormatType != KSDATAFORMAT_SUBTYPE_IEEE_FLOAT ||
        inputFormat.dwSamplesPerFrame != outputFormat.dwSamplesPerFrame ||
        inputFormat.dwBytesPerSampleContainer != sizeof(float) ||
        outputFormat.dwBytesPerSampleContainer != sizeof(float) ||
        inputFormat.dwValidBitsPerSample != 32 ||
        outputFormat.dwValidBitsPerSample != 32)
    {
        const HRESULT formatResult = FAILED(hr) ? hr : APOERR_INVALID_CONNECTION_FORMAT;
        m_lastLockResult.store(formatResult, std::memory_order_relaxed);
        return returnLock(formatResult);
    }

    hr = CBaseAudioProcessingObject::LockForProcess(
        inputCount, inputs, outputCount, outputs);
    if (FAILED(hr))
    {
        m_lastLockResult.store(hr, std::memory_order_relaxed);
        return returnLock(hr);
    }

    // Endpoint APO samples are negotiated as 32-bit IEEE float by the Windows
    // audio engine, independently of the physical device's 16/24/32-bit PCM
    // format.  Never reinterpret a non-float connection as float samples.
    if (GetBytesPerSampleContainer() != sizeof(float) ||
        GetValidBitsPerSample() != 32)
    {
        CBaseAudioProcessingObject::UnlockForProcess();
        m_lastLockResult.store(APOERR_INVALID_CONNECTION_FORMAT,
            std::memory_order_relaxed);
        return returnLock(APOERR_INVALID_CONNECTION_FORMAT);
    }

    const UINT32 nextChannels = inputFormat.dwSamplesPerFrame;
    const UINT32 nextSampleRate = static_cast<UINT32>(
        std::max<FLOAT32>(GetFramesPerSecond(), 8000.0f));
    if (nextChannels < AUDIO_MIN_CHANNELS || nextChannels > AUDIO_MAX_CHANNELS)
    {
        CBaseAudioProcessingObject::UnlockForProcess();
        m_lastLockResult.store(APOERR_INVALID_CONNECTION_FORMAT,
            std::memory_order_relaxed);
        return returnLock(APOERR_INVALID_CONNECTION_FORMAT);
    }
    EnterCriticalSection(&m_runtimeLock);
    m_bytesPerSample = inputFormat.dwBytesPerSampleContainer;
    m_validBitsPerSample = inputFormat.dwValidBitsPerSample;
    m_channels = nextChannels;
    m_sampleRate = nextSampleRate;
    LeaveCriticalSection(&m_runtimeLock);
    hr = ReloadSettings();
    if (FAILED(hr))
    {
        CBaseAudioProcessingObject::UnlockForProcess();
        m_lastLockResult.store(hr, std::memory_order_relaxed);
        return returnLock(hr);
    }
    m_lastLockResult.store(S_OK, std::memory_order_relaxed);
    m_lockedForProcess.store(1, std::memory_order_release);
    return returnLock(S_OK);
}

HRESULT CHXAudioEndpointApo::UnlockForProcess()
{
    const HRESULT result = CBaseAudioProcessingObject::UnlockForProcess();
    if (SUCCEEDED(result)) m_lockedForProcess.store(0, std::memory_order_release);
    return result;
}

#pragma AVRT_CODE_BEGIN
void CHXAudioEndpointApo::APOProcess(UINT32 inputCount,
    APO_CONNECTION_PROPERTY** inputs, UINT32 outputCount,
    APO_CONNECTION_PROPERTY** outputs)
{
    if (!m_bIsLocked || inputCount != 1 || outputCount != 1 ||
        !inputs || !outputs || !inputs[0] || !outputs[0]) return;

    auto* input = inputs[0];
    auto* output = outputs[0];
    const UINT32 frames = input->u32ValidFrameCount;
    const size_t bytes = static_cast<size_t>(frames) * m_channels * sizeof(float);
    auto* inputSamples = reinterpret_cast<float*>(input->pBuffer);
    auto* outputSamples = reinterpret_cast<float*>(output->pBuffer);
    if (!inputSamples || !outputSamples) return;

    m_processCalls.fetch_add(1, std::memory_order_relaxed);
    m_processedFrames.fetch_add(frames, std::memory_order_relaxed);

    const bool silentInput = input->u32BufferFlags == BUFFER_SILENT;
    if (!silentInput && input->u32BufferFlags != BUFFER_VALID) return;

    const bool measure = m_probeActive.load(std::memory_order_relaxed);
    long double inputEnergy = 0.0;
    if (measure && !silentInput)
    {
        const size_t sampleCount = static_cast<size_t>(frames) * m_channels;
        for (size_t index = 0; index < sampleCount; ++index)
            inputEnergy += static_cast<long double>(inputSamples[index]) *
                inputSamples[index];
    }
    if (silentInput) ZeroMemory(outputSamples, bytes);
    else if (outputSamples != inputSamples) CopyMemory(outputSamples, inputSamples, bytes);

    // Adopt a completed settings slot only at a packet boundary. Slot ownership
    // is exchanged atomically with the settings thread, so the real-time path
    // never locks and a packet can never jump back to unprocessed audio.
    if (m_transitionRuntime.load(std::memory_order_acquire) < 0)
    {
        const LONG pending = m_pendingRuntime.exchange(-1,
            std::memory_order_acq_rel);
        if (pending >= 0 && pending < kRuntimeSlotCount)
        {
            LONG expected = RuntimeSlotPending;
            if (m_runtimeSlotStates[pending].compare_exchange_strong(expected,
                RuntimeSlotActive, std::memory_order_acq_rel))
            {
                const LONG previous = m_activeRuntime.exchange(pending,
                    std::memory_order_acq_rel);
                m_activeSettingsGeneration.store(
                    m_runtimes[pending]->settingsGeneration,
                    std::memory_order_release);
                if (previous >= 0 && previous < kRuntimeSlotCount &&
                    previous != pending)
                {
                    m_runtimeSlotStates[previous].store(RuntimeSlotTransition,
                        std::memory_order_release);
                    m_transitionRuntime.store(previous,
                        std::memory_order_release);
                    m_transitionFramesTotal = std::max<UINT32>(1,
                        m_sampleRate * 15 / 1000);
                    m_transitionFramesRemaining = m_transitionFramesTotal;
                }
            }
        }
    }

    const LONG activeIndex = m_activeRuntime.load(std::memory_order_acquire);
    Runtime* active = activeIndex >= 0 && activeIndex < kRuntimeSlotCount
        ? m_runtimes[activeIndex] : nullptr;
    LONG transitionIndex = m_transitionRuntime.load(std::memory_order_acquire);
    Runtime* transition = transitionIndex >= 0 && transitionIndex < kRuntimeSlotCount
        ? m_runtimes[transitionIndex] : nullptr;

    if (m_channels <= kMaximumChannels && active)
    {
        if (transition && m_transitionFramesRemaining)
        {
            for (UINT32 frame = 0; frame < frames; ++frame)
            {
                const bool crossfade = m_transitionFramesRemaining != 0;
                const double newWeight = crossfade
                    ? static_cast<double>(m_transitionFramesTotal -
                        m_transitionFramesRemaining + 1) /
                        m_transitionFramesTotal
                    : 1.0;
                for (UINT32 channel = 0; channel < m_channels; ++channel)
                {
                    const size_t index = static_cast<size_t>(frame) *
                        m_channels + channel;
                    const float inputSample = silentInput ? 0.0f : inputSamples[index];
                    const float next = active->ProcessSample(inputSample, channel);
                    if (crossfade)
                    {
                        const float previous = transition->ProcessSample(
                            inputSample, channel);
                        outputSamples[index] = static_cast<float>(previous *
                            (1.0 - newWeight) + next * newWeight);
                    }
                    else outputSamples[index] = next;
                }
                if (crossfade && --m_transitionFramesRemaining == 0)
                {
                    m_runtimeSlotStates[transitionIndex].store(RuntimeSlotIdle,
                        std::memory_order_release);
                    m_transitionRuntime.store(-1, std::memory_order_release);
                    transition = nullptr;
                    transitionIndex = -1;
                }
            }
        }
        else
        {
            active->Process(outputSamples, frames);
        }
        m_dspProcessCalls.fetch_add(1, std::memory_order_relaxed);
    }
    if (measure)
    {
        long double outputEnergy = 0.0;
        const size_t sampleCount = static_cast<size_t>(frames) * m_channels;
        for (size_t index = 0; index < sampleCount; ++index)
            outputEnergy += static_cast<long double>(outputSamples[index]) *
                outputSamples[index];
        constexpr long double scale = 1000000000000.0L;
        constexpr long double maximum = 1000000000000000000.0L;
        m_inputEnergyPico.fetch_add(static_cast<LONG64>(
            std::min(inputEnergy * scale, maximum)), std::memory_order_relaxed);
        m_outputEnergyPico.fetch_add(static_cast<LONG64>(
            std::min(outputEnergy * scale, maximum)), std::memory_order_relaxed);
    }
    output->u32BufferFlags = active ? BUFFER_VALID : input->u32BufferFlags;
    output->u32ValidFrameCount = frames;
}
#pragma AVRT_CODE_END

DWORD WINAPI CHXAudioEndpointApo::SettingsThreadEntry(void* context)
{
    return static_cast<CHXAudioEndpointApo*>(context)->SettingsThread();
}

DWORD CHXAudioEndpointApo::SettingsThread()
{
    HANDLE changedEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!changedEvent) return GetLastError();
    while (WaitForSingleObject(m_stopEvent, 0) != WAIT_OBJECT_0)
    {
        PublishProbe();
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kSettingsNotifyPath, 0,
            KEY_NOTIFY | KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        {
            if (WaitForSingleObject(m_stopEvent, 1000) == WAIT_OBJECT_0) break;
            continue;
        }
        const LONG notify = RegNotifyChangeKeyValue(key, FALSE,
            REG_NOTIFY_CHANGE_LAST_SET,
            changedEvent, TRUE);
        if (notify != ERROR_SUCCESS)
        {
            RegCloseKey(key);
            if (WaitForSingleObject(m_stopEvent, 1000) == WAIT_OBJECT_0) break;
            continue;
        }
        HANDLE events[] = { m_stopEvent, changedEvent };
        const DWORD wait = WaitForMultipleObjects(ARRAYSIZE(events), events, FALSE,
            m_probeMapping ? 100 : INFINITE);
        RegCloseKey(key);
        if (wait == WAIT_OBJECT_0) break;
        if (wait == WAIT_OBJECT_0 + 1)
        {
            HRESULT reload = ReloadSettings();
            while (reload == HRESULT_FROM_WIN32(ERROR_BUSY) &&
                WaitForSingleObject(m_stopEvent, 2) == WAIT_TIMEOUT)
                reload = ReloadSettings();
            if (SUCCEEDED(reload))
            {
                const DWORD configured = m_configuredSettingsGeneration.load(
                    std::memory_order_acquire);
                const ULONGLONG deadline = GetTickCount64() + 300;
                while (m_activeSettingsGeneration.load(
                    std::memory_order_acquire) != configured &&
                    GetTickCount64() < deadline &&
                    WaitForSingleObject(m_stopEvent, 2) == WAIT_TIMEOUT)
                {
                }
            }
        }
        if (wait == WAIT_TIMEOUT || wait == WAIT_OBJECT_0 + 1) PublishProbe();
    }
    CloseHandle(changedEvent);
    return 0;
}

HRESULT CHXAudioEndpointApo::ReloadSettings()
{
    const Settings settings = LoadSettings(m_profileKey, m_endpointId);

    EnterCriticalSection(&m_runtimeLock);
    if (m_channels > kMaximumChannels)
    {
        LeaveCriticalSection(&m_runtimeLock);
        return S_OK;
    }
    const bool incompatible = std::any_of(std::begin(m_runtimes),
        std::end(m_runtimes), [&](const Runtime* runtime)
        {
            return runtime && runtime->channels != m_channels;
        });
    if (incompatible)
    {
        // A format change is only legal while the APO is outside its process
        // lock, so no real-time callback can still reference these slots.
        if (m_lockedForProcess.load(std::memory_order_acquire))
        {
            LeaveCriticalSection(&m_runtimeLock);
            return APOERR_ALREADY_INITIALIZED;
        }
        m_activeRuntime.store(-1, std::memory_order_release);
        m_pendingRuntime.store(-1, std::memory_order_release);
        m_transitionRuntime.store(-1, std::memory_order_release);
        for (LONG index = 0; index < kRuntimeSlotCount; ++index)
        {
            if (m_runtimeAllocations[index])
                AERT_Free(m_runtimeAllocations[index]);
            m_runtimeAllocations[index] = nullptr;
            m_runtimes[index] = nullptr;
            m_runtimeSlotStates[index].store(RuntimeSlotIdle,
                std::memory_order_release);
        }
    }

    // Coalesce rapid UI writes: a slot which has not reached the audio thread
    // is withdrawn and reused for the newest complete settings snapshot.
    const LONG withdrawn = m_pendingRuntime.exchange(-1,
        std::memory_order_acq_rel);
    if (withdrawn >= 0 && withdrawn < kRuntimeSlotCount)
    {
        LONG expected = RuntimeSlotPending;
        m_runtimeSlotStates[withdrawn].compare_exchange_strong(expected,
            RuntimeSlotIdle, std::memory_order_acq_rel);
    }

    LONG selected = -1;
    for (LONG index = 0; index < kRuntimeSlotCount; ++index)
    {
        LONG expected = RuntimeSlotIdle;
        if (m_runtimeSlotStates[index].compare_exchange_strong(expected,
            RuntimeSlotConfiguring, std::memory_order_acq_rel))
        {
            selected = index;
            break;
        }
    }
    if (selected < 0)
    {
        LeaveCriticalSection(&m_runtimeLock);
        return HRESULT_FROM_WIN32(ERROR_BUSY);
    }

    const UINT32 requiredBands = static_cast<UINT32>(std::count_if(
        settings.bands.begin(), settings.bands.end(),
        [](const PeqBand& band) { return band.enabled; }));
    if (!m_runtimes[selected] ||
        m_runtimes[selected]->channels != m_channels ||
        m_runtimes[selected]->bandCapacity < requiredBands)
    {
        if (m_runtimeAllocations[selected])
            AERT_Free(m_runtimeAllocations[selected]);
        m_runtimeAllocations[selected] = nullptr;
        m_runtimes[selected] = nullptr;
        const size_t bytes = Runtime::RequiredBytes(m_channels, requiredBands);
        void* memory = nullptr;
        const HRESULT allocation = bytes
            ? AERT_Allocate(bytes, &memory) : E_OUTOFMEMORY;
        if (FAILED(allocation) || !memory)
        {
            m_runtimeSlotStates[selected].store(RuntimeSlotIdle,
                std::memory_order_release);
            LeaveCriticalSection(&m_runtimeLock);
            return FAILED(allocation) ? allocation : E_OUTOFMEMORY;
        }
        m_runtimeAllocations[selected] = memory;
        m_runtimes[selected] = new(memory) Runtime{};
        m_runtimes[selected]->InitializeStorage(m_channels, requiredBands);
    }

    m_runtimes[selected]->Configure(m_sampleRate, m_channels, settings);
    m_configuredSettingsGeneration.store(settings.generation,
        std::memory_order_release);
    WriteDiagnosticDword(L"ApoVolumeRecoveryAvailable", 0);
    WriteDiagnosticDword(L"ApoVolumeBorrowAvailable", 0);
    WriteDiagnosticDword(L"ApoEndpointAttenuationMilliDb", 0);
    WriteDiagnosticDword(L"ApoVolumeRecoveryMilliDb", 0);
    WriteDiagnosticDword(L"ApoHiddenBassReductionMilliDb", 0);
    WriteDiagnosticDword(L"ApoHiddenTrebleReductionMilliDb", 0);
    WriteDiagnosticDword(L"ApoStaticHeadroomMilliDb", static_cast<DWORD>(
        std::llround(m_runtimes[selected]->staticHeadroomDb * 1000.0)));
    const std::wstring profileSuffix = RegistrySuffix(
        m_profileKey.empty() ? m_endpointId : m_profileKey);
    WriteDiagnosticDword((L"ApoStaticHeadroomMilliDb_" + profileSuffix).c_str(),
        static_cast<DWORD>(std::llround(
            m_runtimes[selected]->staticHeadroomDb * 1000.0)));
    if (m_activeRuntime.load(std::memory_order_acquire) < 0)
    {
        m_runtimeSlotStates[selected].store(RuntimeSlotActive,
            std::memory_order_release);
        m_activeRuntime.store(selected, std::memory_order_release);
        m_activeSettingsGeneration.store(settings.generation,
            std::memory_order_release);
    }
    else
    {
        m_runtimeSlotStates[selected].store(RuntimeSlotPending,
            std::memory_order_release);
        m_pendingRuntime.store(selected, std::memory_order_release);
    }
    LeaveCriticalSection(&m_runtimeLock);
    return S_OK;
}

void CHXAudioEndpointApo::StopSettingsThread()
{
    if (m_stopEvent) SetEvent(m_stopEvent);
    if (m_settingsThread)
    {
        WaitForSingleObject(m_settingsThread, 5000);
        CloseHandle(m_settingsThread);
        m_settingsThread = nullptr;
    }
}

void CHXAudioEndpointApo::PublishProbe()
{
    HKEY key = nullptr;
    std::wstring mappingName;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kSettingsPath, 0,
        KEY_READ | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS)
    {
        mappingName = ReadString(key, L"ApoProbeMapping");
        RegCloseKey(key);
    }

    const auto writeDiagnostic = [](const wchar_t* name, DWORD value)
    {
        WriteDiagnosticDword(name, value);
    };

    const std::wstring profileSuffix = RegistrySuffix(
        m_profileKey.empty() ? m_endpointId : m_profileKey);
    writeDiagnostic((L"ApoSettingsGeneration_" + profileSuffix).c_str(),
        m_activeSettingsGeneration.load(std::memory_order_acquire));
    writeDiagnostic((L"ApoSettingsProcessId_" + profileSuffix).c_str(),
        GetCurrentProcessId());

    if (mappingName != m_probeMappingName)
    {
        CloseProbe();
        m_probeMappingName = mappingName;
        if (!mappingName.empty())
        {
            m_probeMappingHandle = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE,
                FALSE, mappingName.c_str());
            const DWORD openError = m_probeMappingHandle ? ERROR_SUCCESS : GetLastError();
            writeDiagnostic(L"ApoProbeOpenError", openError);
            writeDiagnostic(L"ApoProbePublisherPid", GetCurrentProcessId());
            writeDiagnostic(L"ApoProbePublisherInitialized", m_bIsInitialized ? 1 : 0);
            if (m_probeMappingHandle)
            {
                m_probeMapping = static_cast<HXAudioApoProbeMapping*>(MapViewOfFile(
                    m_probeMappingHandle, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
                    sizeof(HXAudioApoProbeMapping)));
                if (!m_probeMapping || m_probeMapping->magic != kHXAudioApoProbeMagic ||
                    m_probeMapping->version != kHXAudioApoProbeVersion ||
                    m_probeMapping->size != sizeof(HXAudioApoProbeMapping))
                {
                    writeDiagnostic(L"ApoProbeMapError", m_probeMapping
                        ? ERROR_INVALID_DATA : GetLastError());
                    CloseProbe();
                }
            }
        }
    }

    if (!m_probeMapping) return;
    if (m_probeSlot < 0)
    {
        const DWORD count = std::min<DWORD>(m_probeMapping->slotCount,
            kHXAudioApoProbeSlots);
        for (DWORD index = 0; index < count; ++index)
        {
            if (InterlockedCompareExchange(&m_probeMapping->slots[index].claimed,
                1, 0) == 0)
            {
                m_probeSlot = static_cast<LONG>(index);
                break;
            }
        }
    }
    if (m_probeSlot < 0) return;

    auto& slot = m_probeMapping->slots[m_probeSlot];
    slot.processId = GetCurrentProcessId();
    slot.objectId = reinterpret_cast<UINT64>(this);
    wcsncpy_s(slot.endpointId, m_endpointId.c_str(), _TRUNCATE);
    slot.initialized = m_bIsInitialized ? 1 : 0;
    slot.locked = m_lockedForProcess.load(std::memory_order_acquire);
    slot.lastLockResult = m_lastLockResult.load(std::memory_order_relaxed);
    slot.sampleRate = m_sampleRate;
    slot.channels = m_channels;
    slot.bytesPerSample = m_bytesPerSample;
    slot.validBitsPerSample = m_validBitsPerSample;
    slot.processCalls = m_processCalls.load(std::memory_order_relaxed);
    slot.dspProcessCalls = m_dspProcessCalls.load(std::memory_order_relaxed);
    slot.processedFrames = m_processedFrames.load(std::memory_order_relaxed);
    slot.inputEnergyPico = m_inputEnergyPico.load(std::memory_order_relaxed);
    slot.outputEnergyPico = m_outputEnergyPico.load(std::memory_order_relaxed);
    m_probeActive.store(true, std::memory_order_release);
    writeDiagnostic(L"ApoProbeMapError", ERROR_SUCCESS);
    writeDiagnostic(L"ApoProbeLastLockResult",
        static_cast<DWORD>(m_lastLockResult.load(std::memory_order_relaxed)));
    writeDiagnostic(L"ApoProbeProcessCalls",
        static_cast<DWORD>(m_processCalls.load(std::memory_order_relaxed)));
    writeDiagnostic(L"ApoProbeDspProcessCalls",
        static_cast<DWORD>(m_dspProcessCalls.load(std::memory_order_relaxed)));
}

void CHXAudioEndpointApo::CloseProbe()
{
    m_probeActive.store(false, std::memory_order_release);
    if (m_probeMapping && m_probeSlot >= 0 &&
        static_cast<DWORD>(m_probeSlot) < kHXAudioApoProbeSlots)
        InterlockedExchange(&m_probeMapping->slots[m_probeSlot].claimed, 0);
    m_probeSlot = -1;
    if (m_probeMapping) UnmapViewOfFile(m_probeMapping);
    if (m_probeMappingHandle) CloseHandle(m_probeMappingHandle);
    m_probeMapping = nullptr;
    m_probeMappingHandle = nullptr;
    m_probeMappingName.clear();
}
