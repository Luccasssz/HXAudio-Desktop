#pragma once

#include <windows.h>
#include <atlbase.h>
#include <atlcom.h>
#include <audioenginebaseapo.h>
#include <audioengineextensionapo.h>
#include <baseaudioprocessingobject.h>
#include <mmdeviceapi.h>
#include <atomic>
#include <string>

struct HXAudioApoProbeMapping;

extern const CLSID CLSID_HXAudioEndpointApo;

class ATL_NO_VTABLE CHXAudioEndpointApo :
    public CComObjectRootEx<CComMultiThreadModel>,
    public CComCoClass<CHXAudioEndpointApo, &CLSID_HXAudioEndpointApo>,
    public CBaseAudioProcessingObject,
    public IAudioSystemEffects
{
public:
    CHXAudioEndpointApo();
    ~CHXAudioEndpointApo() override;

    DECLARE_NO_REGISTRY()
    // The Windows audio engine creates endpoint system effects as aggregated
    // COM objects.  Rejecting aggregation makes IClassFactory succeed but
    // causes CreateInstance to fail before the APO constructor is reached.
    DECLARE_AGGREGATABLE(CHXAudioEndpointApo)

    BEGIN_COM_MAP(CHXAudioEndpointApo)
        COM_INTERFACE_ENTRY(IAudioSystemEffects)
        COM_INTERFACE_ENTRY(IAudioProcessingObject)
        COM_INTERFACE_ENTRY(IAudioProcessingObjectRT)
        COM_INTERFACE_ENTRY(IAudioProcessingObjectConfiguration)
    END_COM_MAP()

    STDMETHOD_(void, APOProcess)(UINT32 inputCount,
        APO_CONNECTION_PROPERTY** inputs, UINT32 outputCount,
        APO_CONNECTION_PROPERTY** outputs) override;
    STDMETHOD(Initialize)(UINT32 dataSize, BYTE* data) override;
    STDMETHOD(LockForProcess)(UINT32 inputCount,
        APO_CONNECTION_DESCRIPTOR** inputs, UINT32 outputCount,
        APO_CONNECTION_DESCRIPTOR** outputs) override;
    STDMETHOD(UnlockForProcess)() override;

    static const CRegAPOProperties<1> sm_RegProperties;
    static HRESULT RunOfflinePeqSelfTest();

private:
    struct Runtime;

    static DWORD WINAPI SettingsThreadEntry(void* context);
    DWORD SettingsThread();
    HRESULT ReloadSettings();
    void StopSettingsThread();
    void PublishProbe();
    void CloseProbe();

    // The settings thread only configures a slot that is not visible to the
    // real-time callback. APOProcess switches slots atomically at a packet
    // boundary and crossfades the old/new state; it never takes this lock and
    // never bypasses a packet because settings are changing.
    static constexpr LONG kRuntimeSlotCount = 3;
    enum RuntimeSlotState : LONG
    {
        RuntimeSlotIdle = 0,
        RuntimeSlotConfiguring = 1,
        RuntimeSlotActive = 2,
        RuntimeSlotPending = 3,
        RuntimeSlotTransition = 4,
    };
    CRITICAL_SECTION m_runtimeLock{};
    void* m_runtimeAllocations[kRuntimeSlotCount]{};
    Runtime* m_runtimes[kRuntimeSlotCount]{};
    std::atomic<LONG> m_runtimeSlotStates[kRuntimeSlotCount]{};
    std::atomic<LONG> m_activeRuntime{ -1 };
    std::atomic<LONG> m_pendingRuntime{ -1 };
    std::atomic<LONG> m_transitionRuntime{ -1 };
    UINT32 m_transitionFramesRemaining = 0;
    UINT32 m_transitionFramesTotal = 0;
    HANDLE m_stopEvent = nullptr;
    HANDLE m_settingsThread = nullptr;
    UINT32 m_sampleRate = 48000;
    UINT32 m_channels = 2;
    UINT32 m_bytesPerSample = sizeof(float);
    UINT32 m_validBitsPerSample = 32;
    std::wstring m_endpointId;
    std::wstring m_profileKey;
    std::atomic<LONG> m_lockedForProcess{ 0 };
    std::atomic<HRESULT> m_lastLockResult{ S_FALSE };
    std::atomic<LONG64> m_processCalls{ 0 };
    std::atomic<LONG64> m_dspProcessCalls{ 0 };
    std::atomic<LONG64> m_processedFrames{ 0 };
    std::atomic<LONG64> m_inputEnergyPico{ 0 };
    std::atomic<LONG64> m_outputEnergyPico{ 0 };
    std::atomic<DWORD> m_configuredSettingsGeneration{ 0 };
    std::atomic<DWORD> m_activeSettingsGeneration{ 0 };
    std::atomic<bool> m_probeActive{ false };
    HANDLE m_probeMappingHandle = nullptr;
    HXAudioApoProbeMapping* m_probeMapping = nullptr;
    LONG m_probeSlot = -1;
    std::wstring m_probeMappingName;
};

OBJECT_ENTRY_AUTO(CLSID_HXAudioEndpointApo, CHXAudioEndpointApo)
