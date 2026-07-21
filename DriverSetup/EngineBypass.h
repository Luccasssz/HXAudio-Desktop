#pragma once

namespace HXAudioEngineBypass
{
    int InstallService();
    int StopServiceForUpgrade();
    int UninstallService();
    int Probe();
    int RunServiceDispatcher();
}
