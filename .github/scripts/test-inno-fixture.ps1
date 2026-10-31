$ErrorActionPreference = 'Stop'
$fixture = (Resolve-Path 'artifacts/inno-uninstall-fixture.exe').Path
$base = Join-Path $env:RUNNER_TEMP ('hxaudio-inno-' + [guid]::NewGuid().ToString('N'))
$app = Join-Path $base 'app'
New-Item -ItemType Directory -Force $base | Out-Null
function Invoke-Fixture($exe, $arguments, $label) {
    $p = Start-Process -FilePath $exe -ArgumentList $arguments -Wait -PassThru
    $line = "$label exit code: $($p.ExitCode)"
    Write-Host $line
    Add-Content -Path hxaudio-inno-fixture.log -Value $line
    return $p.ExitCode
}
$setupExit = Invoke-Fixture $fixture @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART',("/DIR=`"$app`"")) 'fixture install'
if ($setupExit -ne 0) { throw 'Fixture setup failed' }
$uninstaller = Join-Path $app 'unins000.exe'
$payload = Join-Path $app 'inno-uninstall-fixture.txt'
$flag = Join-Path $app 'fail.flag'
if (!(Test-Path $uninstaller) -or !(Test-Path $payload)) { throw 'Fixture missing' }
Set-Content -LiteralPath $flag -Value 'recovery data sentinel'
$failExit = Invoke-Fixture $uninstaller @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART',("/LOG=`"$base\failed-uninstall.log`"")) 'injected failure'
Copy-Item "$base\failed-uninstall.log" hxaudio-inno-failed-uninstall.log
if ($failExit -eq 0 -or !(Test-Path $payload) -or !(Test-Path $flag) -or !(Test-Path $uninstaller)) {
    throw 'Failed cleanup did not preserve fixture files or returned success'
}
Remove-Item -LiteralPath $flag
$successExit = Invoke-Fixture $uninstaller @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART',("/LOG=`"$base\successful-uninstall.log`"")) 'successful uninstall'
Copy-Item "$base\successful-uninstall.log" hxaudio-inno-successful-uninstall.log
if ($successExit -ne 0 -or (Test-Path $payload) -or (Test-Path $uninstaller)) { throw 'Fixture cleanup failed' }
'PASS isolated Inno uninstall failure preservation and retry' | Tee-Object -FilePath hxaudio-inno-fixture.log -Append
