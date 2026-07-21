#include "HXAudioApo.h"
#include "HXAudioApoProbe.h"

#include <string>

namespace
{
    class CHXAudioApoModule final : public CAtlDllModuleT<CHXAudioApoModule>
    {
    };

    CHXAudioApoModule g_module;
    HINSTANCE g_instance = nullptr;

    HRESULT SetRegistryString(HKEY root, const std::wstring& path,
        const wchar_t* name, const std::wstring& value)
    {
        HKEY key = nullptr;
        DWORD disposition = 0;
        const LONG open = RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0,
            KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, &disposition);
        if (open != ERROR_SUCCESS) return HRESULT_FROM_WIN32(open);
        const LONG set = RegSetValueExW(key, name, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(value.c_str()),
            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
        return HRESULT_FROM_WIN32(set);
    }
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_instance = instance;
        DisableThreadLibraryCalls(instance);
    }
    return g_module.DllMain(reason, reserved);
}

extern "C" STDAPI DllCanUnloadNow()
{
    return g_module.DllCanUnloadNow();
}

extern "C" STDAPI DllGetClassObject(REFCLSID classId, REFIID interfaceId, void** object)
{
    HXAudioApoRecordProbePhase(HXAudioApoProbePhase::DllGetClassObject);
    const HRESULT result = g_module.DllGetClassObject(classId, interfaceId, object);
    HXAudioApoRecordProbePhase(HXAudioApoProbePhase::DllGetClassObjectExit,
        result, 0, &classId, &interfaceId);
    return result;
}

extern "C" STDAPI DllRegisterServer()
{
    wchar_t modulePath[MAX_PATH]{};
    if (!GetModuleFileNameW(g_instance, modulePath, ARRAYSIZE(modulePath)))
        return HRESULT_FROM_WIN32(GetLastError());

    constexpr wchar_t classPath[] =
        L"Software\\Classes\\CLSID\\{C4D1BA43-C7A8-4B47-8DCF-6DCF554D0F59}";
    HRESULT hr = SetRegistryString(HKEY_LOCAL_MACHINE, classPath, nullptr,
        L"HXAudio Global Endpoint APO");
    if (FAILED(hr)) return hr;
    hr = SetRegistryString(HKEY_LOCAL_MACHINE,
        std::wstring(classPath) + L"\\InprocServer32", nullptr, modulePath);
    if (FAILED(hr)) return hr;
    hr = SetRegistryString(HKEY_LOCAL_MACHINE,
        std::wstring(classPath) + L"\\InprocServer32", L"ThreadingModel", L"Both");
    if (FAILED(hr)) return hr;
    return RegisterAPO(&CHXAudioEndpointApo::sm_RegProperties.m_Properties);
}

extern "C" STDAPI DllUnregisterServer()
{
    const HRESULT apoResult = UnregisterAPO(CLSID_HXAudioEndpointApo);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE,
        L"Software\\Classes\\CLSID\\{C4D1BA43-C7A8-4B47-8DCF-6DCF554D0F59}");
    return FAILED(apoResult) && apoResult != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
        ? apoResult : S_OK;
}

extern "C" STDAPI HXAudioApoOfflinePeqSelfTest()
{
    return CHXAudioEndpointApo::RunOfflinePeqSelfTest();
}
