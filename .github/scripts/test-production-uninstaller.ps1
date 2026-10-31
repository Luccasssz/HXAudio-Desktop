$ErrorActionPreference = 'Stop'
$setup = (Resolve-Path 'artifacts/HXAudio-APO-Setup-0.0.0-harness-x64.exe').Path
$base = Join-Path $env:RUNNER_TEMP ('hxaudio-prod-mock-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $base | Out-Null
function RunExe([string]$exe, [string[]]$argsList, [string]$label) {
    $p = Start-Process -FilePath $exe -ArgumentList $argsList -Wait -PassThru
    $line = "$label exit code: $($p.ExitCode)"
    Write-Host $line; Add-Content hxaudio-production-uninstaller.log $line
    return $p.ExitCode
}
foreach ($case in @('success','stop','ui','bypass','apo','virtual','verify')) {
    $app = Join-Path $base $case
    $setupCode = RunExe $setup @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART',("/DIR=`"$app`"")) "$case setup"
    if ($setupCode -ne 0) { throw "$case setup failed" }
    $uninstaller = Join-Path $app 'unins000.exe'
    $helper = Join-Path $app 'HXAudioDriverSetup.exe'
    $control = Join-Path $app 'HXAudioControl.exe'
    $sentinel = Join-Path $app 'recovery-sentinel.txt'
    Set-Content $sentinel 'do not erase on failure'
    if ($case -eq 'verify') { Set-Content (Join-Path $app 'apo.done') '1' }
    if ($case -ne 'success') { Set-Content (Join-Path $app "fail_$case") 'fault' }
    $log = Join-Path $base "$case-first.log"
    $code = RunExe $uninstaller @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART',("/LOG=`"$log`"")) "$case first"
    Copy-Item $log "hxaudio-production-$case-first.log"
    if ($case -eq 'success') {
        if ($code -ne 0 -or (Test-Path $helper) -or (Test-Path $uninstaller)) { throw 'normal uninstall failed' }
        continue
    }
    if ($code -eq 0 -or !(Test-Path $helper) -or !(Test-Path $control) -or
        !(Test-Path $uninstaller) -or !(Test-Path $sentinel)) { throw "$case did not preserve recovery files" }
    if ($case -eq 'virtual' -and !(Test-Path (Join-Path $app 'apo.done'))) { throw 'APO phase was not reached' }
    $calls = @(Get-Content (Join-Path $app 'calls.log'))
    if (@($calls | Where-Object { $_ -eq 'apo-uninstall' }).Count -ne 0) { throw 'duplicate APO command' }
    if ($case -eq 'virtual') {
        $expected = @('--stop','--shutdown-ui','engine-bypass-uninstall','purge')
        if (($calls -join ',') -ne ($expected -join ',')) { throw 'unexpected cleanup sequence' }
        if (@(Get-Content (Join-Path $app 'apo.done')).Count -ne 1) { throw 'APO cleanup repeated' }
    }
    Remove-Item (Join-Path $app "fail_$case")
    $retryLog = Join-Path $base "$case-retry.log"
    $retry = RunExe $uninstaller @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART',("/LOG=`"$retryLog`"")) "$case retry"
    Copy-Item $retryLog "hxaudio-production-$case-retry.log"
    if ($retry -ne 0 -or (Test-Path $helper) -or (Test-Path $uninstaller)) { throw "$case retry failed" }
}
'PASS production Inno uninstall callback with disposable mock backends' | Tee-Object hxaudio-production-uninstaller.log -Append
