$ErrorActionPreference = 'Stop'
$exe = (Resolve-Path 'recovery-registry-fixture.exe').Path
function Test-Command([string]$label, [string[]]$argsList, [int]$expected) {
    $p = Start-Process $exe -ArgumentList $argsList -Wait -PassThru
    $line = "$label exit code: $($p.ExitCode) (expected $expected)"
    Write-Host $line; Add-Content hxaudio-recovery-process.log $line
    if ($p.ExitCode -ne $expected) { throw "$label returned $($p.ExitCode), expected $expected" }
}
try {
    Test-Command 'normal init' @('init') 0
    Test-Command 'normal restore' @('run') 0
    Test-Command 'archive retained' @('check') 0
    Test-Command 'repeat restore' @('run') 0
    Test-Command 'repeat archive' @('check') 0
    foreach ($boundary in 1..7) {
        Test-Command "progress-$boundary init" @('init') 0
        Test-Command "progress-$boundary kill" @('run', "$boundary", '0') 92
        Test-Command "progress-$boundary resume" @('run') 0
        Test-Command "progress-$boundary archive" @('check') 0
    }
    foreach ($boundary in 1..2) {
        Test-Command "write-$boundary init" @('init') 0
        Test-Command "write-$boundary kill" @('run', '0', "$boundary") 91
        Test-Command "write-$boundary resume" @('run') 0
        Test-Command "write-$boundary archive" @('check') 0
    }
    Test-Command 'driver init' @('init') 0
    Test-Command 'driver update' @('driver-update') 0
    Test-Command 'driver mismatch' @('run') 5
    Test-Command 'driver values retained' @('check-installed') 0
    Test-Command 'conflict init' @('init') 0
    Test-Command 'conflict change' @('conflict') 0
    Test-Command 'conflict refusal' @('run') 1
    Test-Command 'conflict values retained' @('check-conflict') 0
    Test-Command 'corrupt init' @('init') 0
    Test-Command 'corrupt hash' @('corrupt') 0
    Test-Command 'corrupt refusal' @('run') 3
    Test-Command 'corrupt values retained' @('check-installed') 0
    'PASS isolated HKCU child-process interruption and replay' | Tee-Object hxaudio-recovery-process.log -Append
} finally {
    & $exe clean
}
