#pragma once

namespace HXAudioApoManager
{
    // Stops the endpoint guard without detaching the APO.  Installers use this
    // before replacing the helper executable; Install() later performs the
    // verified in-place APO upgrade and audio-engine reload.
    int PrepareForUpgrade();
    int MigrateDspSettings();
    int Install(const wchar_t* dllPath);
    int InstallForVirtualEffects(const wchar_t* dllPath);
    int Uninstall();
    int Refresh();
    int VerifyInstalled();
    int VerifyVirtualEffectsInstalled();
    int LiveTest();
    int DiagnosticExport();
    int DiagnosticSelfTest();
    int SelfTest(const wchar_t* dllPath);
    int RunServiceDispatcher();
}
