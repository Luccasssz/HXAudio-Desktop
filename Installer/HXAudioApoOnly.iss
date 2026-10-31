#ifndef MyAppVersion
  #error MyAppVersion must be supplied by build-apo-release.ps1
#endif
#ifndef MyApoFileName
  #error MyApoFileName must be supplied by build-apo-release.ps1
#endif

[Setup]
#ifdef UninstallHarness
AppId={{274FBF47-FF51-4953-9941-F67AE1CE9CC3}
#else
AppId={{7FD886C3-0B66-4D5D-A045-9EF09623B416}
#endif
AppName=HXAudio Desktop (APO)
AppVersion={#MyAppVersion}
AppPublisher=Huber Lab
DefaultDirName={autopf}\HXAudio
DefaultGroupName=HXAudio
DisableProgramGroupPage=yes
UninstallDisplayName=HXAudio Desktop (APO)
UninstallDisplayIcon={app}\HXAudio.ico
#ifdef UninstallHarness
PrivilegesRequired=lowest
#else
PrivilegesRequired=admin
#endif
SetupArchitecture=x64
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
WizardStyle=modern
SetupIconFile=..\ControlPanel\assets\HXAudio.ico
OutputDir=..\artifacts
OutputBaseFilename=HXAudio-APO-Setup-{#MyAppVersion}-x64
Compression=lzma2/ultra64
SolidCompression=yes
SetupLogging=yes
CloseApplications=yes
RestartIfNeededByRun=no
MinVersion=10.0.10240

[Languages]
Name: "chinesesimp"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "快捷方式："

[Files]
Source: "..\artifacts\apo-staging\HXAudioControl.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\ControlPanel\assets\HXAudio.ico"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\artifacts\apo-staging\HXAudioDriverSetup.exe"; DestDir: "{app}"; Flags: ignoreversion
; The APO image name is derived from its SHA-256 content. It is immutable:
; an existing same-name file is already byte-identical and may be mapped by
; audiodg.exe, so Setup must reuse it instead of requesting an overwrite.
Source: "..\artifacts\apo-staging\{#MyApoFileName}"; DestDir: "{app}"; Flags: ignoreversion onlyifdoesntexist
Source: "..\artifacts\apo-staging\HXAudioControl.exe"; DestDir: "{tmp}"; DestName: "HXAudioApoControlPolicy.exe"; Flags: dontcopy
Source: "..\artifacts\apo-staging\HXAudioDriverSetup.exe"; DestDir: "{tmp}"; DestName: "HXAudioApoSetupPolicy.exe"; Flags: dontcopy

#ifndef UninstallHarness
[InstallDelete]
; PrepareToInstall has already stopped these binaries before this list runs.
Type: files; Name: "{app}\HXAudioControl.exe"
Type: files; Name: "{app}\HXAudioDriverSetup.exe"
Type: files; Name: "{app}\HXAudio.ico"
Type: files; Name: "{app}\HXAudioVirtualAudio.cer"
Type: filesandordirs; Name: "{app}\Driver"
Type: filesandordirs; Name: "{autopf}\HXAudio Virtual Audio"
Type: files; Name: "{group}\HXAudio 虚拟音频控制面板.lnk"
Type: files; Name: "{autodesktop}\HXAudio 虚拟音频控制面板.lnk"
Type: files; Name: "{group}\HXAudio 控制面板.lnk"
Type: files; Name: "{autodesktop}\HXAudio 控制面板.lnk"
Type: files; Name: "{group}\HXAudio Desktop.lnk"
Type: files; Name: "{autodesktop}\HXAudio Desktop.lnk"
#endif

[UninstallDelete]
#ifndef UninstallHarness
Type: files; Name: "{autodesktop}\HXAudio Desktop.lnk"
Type: files; Name: "{group}\HXAudio Desktop.lnk"
Type: files; Name: "{autodesktop}\HXAudio 控制面板.lnk"
Type: files; Name: "{group}\HXAudio 控制面板.lnk"
Type: files; Name: "{group}\卸载 HXAudio Desktop.lnk"
#endif
Type: filesandordirs; Name: "{app}"

#ifndef UninstallHarness
[Registry]
Root: HKLM64; Subkey: "Software\HXAudio\VirtualAudio"; Permissions: users-modify
Root: HKLM64; Subkey: "Software\HXAudio\VirtualAudio\DspNotify"; ValueType: dword; ValueName: "Generation"; ValueData: "0"; Permissions: users-modify
Root: HKLM64; Subkey: "Software\HXAudio\Apo\RuntimeStatus"; Permissions: users-modify

[Icons]
Name: "{group}\HXAudio 控制面板"; Filename: "{app}\HXAudioControl.exe"; IconFilename: "{app}\HXAudio.ico"; IconIndex: 0
Name: "{group}\卸载 HXAudio Desktop"; Filename: "{uninstallexe}"
Name: "{autodesktop}\HXAudio 控制面板"; Filename: "{app}\HXAudioControl.exe"; IconFilename: "{app}\HXAudio.ico"; IconIndex: 0; Tasks: desktopicon

[Run]
Filename: "{app}\HXAudioControl.exe"; Description: "启动 HXAudio 控制面板"; Flags: nowait postinstall skipifsilent
#endif

[Code]
var
  UpgradePrepared: Boolean;
  BackendInstalled: Boolean;
  UninstallBackendCleaned: Boolean;

function JoinCapturedLines(const Lines: TArrayOfString): String;
var
  I: Integer;
begin
  Result := '';
  for I := 0 to GetArrayLength(Lines) - 1 do
  begin
    if Result <> '' then Result := Result + #13#10;
    Result := Result + Lines[I];
  end;
end;

procedure RunChecked(const Filename, Params, StatusText: String);
var
  ExitCode: Integer;
  Output: TExecOutput;
  Detail: String;
  Started: Boolean;
begin
  if WizardForm <> nil then WizardForm.StatusLabel.Caption := StatusText;
  ExitCode := -1;
  try
    Started := ExecAndCaptureOutput(Filename, Params, '', SW_HIDE,
      ewWaitUntilTerminated, ExitCode, Output);
  except
    RaiseException(StatusText + '：' + GetExceptionMessage);
  end;
  Detail := JoinCapturedLines(Output.StdErr);
  if Detail = '' then Detail := JoinCapturedLines(Output.StdOut);
  if Length(Detail) > 1600 then Detail := Copy(Detail, 1, 1600) + '...';
  if (not Started) or (ExitCode <> 0) then
  begin
    if Detail <> '' then Detail := #13#10 + #13#10 + Detail;
    RaiseException(StatusText + '失败（错误代码：' + IntToStr(ExitCode) + '）。' + Detail);
  end;
end;

procedure RunUninstallChecked(const Filename, Params, Description: String);
var
  ExitCode: Integer;
begin
  if not FileExists(Filename) then
    RaiseException(Description + '失败：所需组件不存在。请重新安装本版本后再卸载。');
  ExitCode := -1;
  if (not Exec(Filename, Params, '', SW_HIDE,
    ewWaitUntilTerminated, ExitCode)) or (ExitCode <> 0) then
    RaiseException(Description + '失败（错误代码：' + IntToStr(ExitCode) +
      '）。为避免系统无声，清理已中止。');
end;

procedure RemoveLegacyCertificate();
var
  ExitCode: Integer;
begin
#ifndef UninstallHarness
  ExecWithNativeSysDir(ExpandConstant('{sys}\certutil.exe'),
    '-delstore TrustedPublisher "HXAudio Virtual Audio Personal Test"',
    '', SW_HIDE, ewWaitUntilTerminated, ExitCode);
  ExecWithNativeSysDir(ExpandConstant('{sys}\certutil.exe'),
    '-delstore Root "HXAudio Virtual Audio Personal Test"',
    '', SW_HIDE, ewWaitUntilTerminated, ExitCode);
#endif
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ControlPolicy: String;
  SetupPolicy: String;
begin
  Result := '';
  NeedsRestart := False;
#ifdef UninstallHarness
  Exit;
#endif
  if UpgradePrepared then Exit;

  ExtractTemporaryFile('HXAudioApoControlPolicy.exe');
  ExtractTemporaryFile('HXAudioApoSetupPolicy.exe');
  ControlPolicy := ExpandConstant('{tmp}\HXAudioApoControlPolicy.exe');
  SetupPolicy := ExpandConstant('{tmp}\HXAudioApoSetupPolicy.exe');

  try
    RunChecked(ControlPolicy, '--stop',
      '正在恢复物理默认输出并停止旧版 HXAudio 路由');
    RunChecked(ControlPolicy, '--shutdown-ui',
      '正在关闭旧版 HXAudio Desktop 控制面板与托盘');
    RunChecked(SetupPolicy, 'prepare-apo-install',
      '正在无重启移除旧虚拟声卡并恢复 Windows 音频服务');
    RemoveLegacyCertificate();
  except
    Result := GetExceptionMessage;
    Exit;
  end;

  UpgradePrepared := True;
end;

procedure InstallApoBackend();
var
  SetupHelper: String;
  ControlPanel: String;
begin
  if BackendInstalled then Exit;
  SetupHelper := ExpandConstant('{app}\HXAudioDriverSetup.exe');
  ControlPanel := ExpandConstant('{app}\HXAudioControl.exe');

  RunChecked(SetupHelper,
    'apo-install "' + ExpandConstant('{app}\{#MyApoFileName}') + '"',
    '正在为所有当前及后续连接的输出设备安装 HXAudio APO');
  RunChecked(SetupHelper, 'engine-bypass-uninstall',
    '正在恢复 Windows 音频引擎的原生动态压缩组件');
  RunChecked(ControlPanel, '--sync-apo-settings',
    '正在同步 PEQ 与声音处理参数');
  RunChecked(SetupHelper, 'apo-refresh',
    '正在应用按设备 APO 接管选择');
  RunChecked(SetupHelper, 'apo-status',
    '正在验证多设备 APO 处理链');
  BackendInstalled := True;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
#ifndef UninstallHarness
  if CurStep = ssPostInstall then InstallApoBackend();
#endif
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  ControlPanel: String;
  SetupHelper: String;
begin
  if (CurUninstallStep <> usUninstall) or UninstallBackendCleaned then Exit;

  try
  ControlPanel := ExpandConstant('{app}\HXAudioControl.exe');
  SetupHelper := ExpandConstant('{app}\HXAudioDriverSetup.exe');
  RunUninstallChecked(ControlPanel, '--stop', '停止 HXAudio 音频路由');
  RunUninstallChecked(ControlPanel, '--shutdown-ui', '关闭 HXAudio Desktop');
  RunUninstallChecked(SetupHelper, 'engine-bypass-uninstall',
    '恢复 Windows 音频引擎的原始动态处理');
  { purge owns both backend cleanups; do not invoke apo-uninstall twice. }
  RunUninstallChecked(SetupHelper, 'purge', '清理 HXAudio 系统配置');
  RemoveLegacyCertificate();
  UninstallBackendCleaned := True;
  except
    Log('Uninstall aborted: ' + GetExceptionMessage);
    Abort;
  end;
end;
