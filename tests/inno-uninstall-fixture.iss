[Setup]
AppId={{0D0AA46E-C4E7-4A4C-99EA-8C65450AD965}
AppName=HXAudio isolated uninstall fixture
AppVersion=0.0.1
DefaultDirName={localappdata}\HXAudioUninstallFixture
PrivilegesRequired=lowest
DisableProgramGroupPage=yes
OutputDir=..\artifacts
OutputBaseFilename=inno-uninstall-fixture
Uninstallable=yes

[Files]
Source: "inno-uninstall-fixture.txt"; DestDir: "{app}"

[UninstallDelete]
Type: filesandordirs; Name: "{app}"

[Code]
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep <> usUninstall then Exit;
  try
    if FileExists(ExpandConstant('{app}\fail.flag')) then
      RaiseException('Injected recovery failure');
  except
    Log('Uninstall aborted: ' + GetExceptionMessage);
    Abort;
  end;
end;
