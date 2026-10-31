[Setup]
AppId={{4A9A61FC-B02F-402D-A5B8-6D13277FE76C}
AppName=HXAudio Voicemeeter Bridge (experimental)
AppVersion=0.1.0
AppPublisher=HXAudio community diagnostics
DefaultDirName={localappdata}\Programs\HXAudioBridge
PrivilegesRequired=lowest
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
DisableProgramGroupPage=yes
OutputDir=..\artifacts
OutputBaseFilename=HXAudio-Voicemeeter-Bridge-experimental-x64
Compression=lzma2
SolidCompression=yes
SetupLogging=yes

[Files]
Source: "..\Bridge\x64\Release\HXAudioBridge.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\docs\VOICEMEETER_BRIDGE.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\HXAudio Voicemeeter Bridge"; Filename: "{app}\HXAudioBridge.exe"

[Code]
function InitializeUninstall(): Boolean;
begin
  Result := FindWindowByClassName('HXAudioVoiceMeeterBridge') = 0;
  if not Result then
    MsgBox('请先关闭 HXAudio Bridge，再卸载。Voicemeeter 和系统音频配置不会被修改。', mbError, MB_OK);
end;
