#pragma once

#include <windows.h>

constexpr DWORD kHXAudioApoProbeMagic = 0x50415848; // "HXAP"
constexpr DWORD kHXAudioApoProbeVersion = 3;
constexpr DWORD kHXAudioApoProbeSlots = 64;
constexpr DWORD kHXAudioApoProbeEndpointCharacters = 384;

struct HXAudioApoProbeSlot
{
    volatile LONG claimed;
    DWORD processId;
    UINT64 objectId;
    WCHAR endpointId[kHXAudioApoProbeEndpointCharacters];
    volatile LONG initialized;
    volatile LONG locked;
    HRESULT lastLockResult;
    DWORD sampleRate;
    DWORD channels;
    DWORD bytesPerSample;
    DWORD validBitsPerSample;
    volatile LONG64 processCalls;
    volatile LONG64 dspProcessCalls;
    volatile LONG64 processedFrames;
    volatile LONG64 inputEnergyPico;
    volatile LONG64 outputEnergyPico;
};

struct HXAudioApoProbeMapping
{
    DWORD magic;
    DWORD version;
    DWORD size;
    DWORD slotCount;
    volatile LONG64 dllGetClassObjectCalls;
    volatile LONG64 dllGetClassObjectSuccesses;
    volatile LONG64 constructorCalls;
    volatile LONG64 initializeCalls;
    volatile LONG64 initializeSuccesses;
    volatile LONG64 lockCalls;
    volatile LONG64 lockSuccesses;
    HRESULT lastInitializeResult;
    HRESULT lastLockResult;
    HRESULT lastDllGetClassObjectResult;
    DWORD lastInitializeDataSize;
    GUID lastRequestedClsid;
    GUID lastRequestedIid;
    HXAudioApoProbeSlot slots[kHXAudioApoProbeSlots];
};

enum class HXAudioApoProbePhase
{
    DllGetClassObject,
    DllGetClassObjectExit,
    Constructor,
    InitializeEnter,
    InitializeExit,
    LockEnter,
    LockExit,
};

// This intentionally opens the short-lived diagnostic mapping synchronously.
// The normal APO path does nothing when no live test has published a mapping
// name, and the real-time APOProcess callback never calls this helper.
inline void HXAudioApoRecordProbePhase(HXAudioApoProbePhase phase,
    HRESULT result = S_FALSE, DWORD initializeDataSize = 0,
    const GUID* requestedClsid = nullptr, const GUID* requestedIid = nullptr)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\HXAudio\\VirtualAudio", 0,
        KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) return;

    WCHAR mappingName[256]{};
    DWORD type = 0;
    DWORD bytes = sizeof(mappingName);
    const LONG query = RegQueryValueExW(key, L"ApoProbeMapping", nullptr,
        &type, reinterpret_cast<BYTE*>(mappingName), &bytes);
    RegCloseKey(key);
    if (query != ERROR_SUCCESS || type != REG_SZ || !mappingName[0]) return;

    HANDLE mappingHandle = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE,
        FALSE, mappingName);
    if (!mappingHandle) return;
    auto* mapping = static_cast<HXAudioApoProbeMapping*>(MapViewOfFile(
        mappingHandle, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
        sizeof(HXAudioApoProbeMapping)));
    if (mapping && mapping->magic == kHXAudioApoProbeMagic &&
        mapping->version == kHXAudioApoProbeVersion &&
        mapping->size == sizeof(HXAudioApoProbeMapping))
    {
        switch (phase)
        {
        case HXAudioApoProbePhase::DllGetClassObject:
            InterlockedIncrement64(&mapping->dllGetClassObjectCalls);
            break;
        case HXAudioApoProbePhase::DllGetClassObjectExit:
            mapping->lastDllGetClassObjectResult = result;
            if (requestedClsid) mapping->lastRequestedClsid = *requestedClsid;
            if (requestedIid) mapping->lastRequestedIid = *requestedIid;
            if (SUCCEEDED(result))
                InterlockedIncrement64(&mapping->dllGetClassObjectSuccesses);
            break;
        case HXAudioApoProbePhase::Constructor:
            InterlockedIncrement64(&mapping->constructorCalls);
            break;
        case HXAudioApoProbePhase::InitializeEnter:
            InterlockedIncrement64(&mapping->initializeCalls);
            mapping->lastInitializeDataSize = initializeDataSize;
            break;
        case HXAudioApoProbePhase::InitializeExit:
            mapping->lastInitializeResult = result;
            if (SUCCEEDED(result))
                InterlockedIncrement64(&mapping->initializeSuccesses);
            break;
        case HXAudioApoProbePhase::LockEnter:
            InterlockedIncrement64(&mapping->lockCalls);
            break;
        case HXAudioApoProbePhase::LockExit:
            mapping->lastLockResult = result;
            if (SUCCEEDED(result))
                InterlockedIncrement64(&mapping->lockSuccesses);
            break;
        }
    }
    if (mapping) UnmapViewOfFile(mapping);
    CloseHandle(mappingHandle);
}
