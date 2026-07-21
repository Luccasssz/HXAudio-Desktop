[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$version = '1.0.1b'
$msbuild = 'D:\Visual Studio\Community\MSBuild\Current\Bin\amd64\MSBuild.exe'
$signTool = 'D:\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe'
$inno = Join-Path $root '.tools\InnoSetup7\ISCC.exe'
$artifacts = Join-Path $root 'artifacts'
$staging = Join-Path $artifacts 'apo-staging'
$controlProject = Join-Path $root 'ControlPanel\HXAudioControl.vcxproj'
$setupProject = Join-Path $root 'DriverSetup\HXAudioDriverSetup.vcxproj'
$apoProject = Join-Path $root 'Apo\HXAudioApo.vcxproj'
$installerScript = Join-Path $root 'Installer\HXAudioApoOnly.iss'
$installer = Join-Path $artifacts "HXAudio-APO-Setup-$version-x64.exe"
$virtualInstaller = Join-Path $artifacts "HXAudio-Setup-$version-x64.exe"

foreach ($required in @($msbuild, $signTool, $inno, $installerScript)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Required APO build input was not found: $required"
    }
}

$virtualHashBefore = $null
if (Test-Path -LiteralPath $virtualInstaller) {
    $virtualHashBefore = (Get-FileHash -Algorithm SHA256 -LiteralPath $virtualInstaller).Hash
}

if (Test-Path -LiteralPath $staging) {
    Remove-Item -LiteralPath $staging -Recurse -Force
}
if (Test-Path -LiteralPath $installer) {
    Remove-Item -LiteralPath $installer -Force
}
New-Item -ItemType Directory -Path $staging -Force | Out-Null

$commonBuildArgs = @('/m', '/p:Configuration=Release', '/p:Platform=x64',
    '/p:SpectreMitigation=false', '/v:minimal')

& $msbuild $controlProject '/t:Rebuild' @commonBuildArgs
if ($LASTEXITCODE -ne 0) { throw 'Control panel build failed.' }

& $msbuild $setupProject '/t:Rebuild' @commonBuildArgs
if ($LASTEXITCODE -ne 0) { throw 'APO setup helper build failed.' }

& $msbuild $apoProject '/t:Rebuild' @commonBuildArgs
if ($LASTEXITCODE -ne 0) { throw 'Global endpoint APO build failed.' }

$subject = 'CN=HXAudio Desktop Internal Test'
$certificate = Get-ChildItem Cert:\CurrentUser\My |
    Where-Object {
        $_.Subject -eq $subject -and $_.HasPrivateKey -and
        $_.NotAfter -gt (Get-Date).AddYears(20)
    } |
    Sort-Object NotAfter -Descending |
    Select-Object -First 1

if (-not $certificate) {
    $certificate = New-SelfSignedCertificate `
        -Type CodeSigningCert `
        -Subject $subject `
        -FriendlyName 'HXAudio Desktop Internal Test Certificate' `
        -CertStoreLocation 'Cert:\CurrentUser\My' `
        -KeyAlgorithm RSA `
        -KeyLength 3072 `
        -HashAlgorithm SHA256 `
        -KeyExportPolicy Exportable `
        -NotAfter (Get-Date).AddYears(30)
}

$thumbprint = $certificate.Thumbprint
$controlBinary = Join-Path $root 'ControlPanel\x64\Release\HXAudioControl.exe'
$setupBinary = Join-Path $root 'DriverSetup\x64\Release\HXAudioDriverSetup.exe'
$apoBinary = Join-Path $root 'Apo\x64\Release\HXAudioApo.dll'

foreach ($binary in @($controlBinary, $setupBinary, $apoBinary)) {
    & $signTool sign /v /fd SHA256 /s My /sha1 $thumbprint $binary
    if ($LASTEXITCODE -ne 0) { throw "Self-signing failed: $binary" }
}

Copy-Item -LiteralPath $controlBinary -Destination $staging
Copy-Item -LiteralPath $setupBinary -Destination $staging
$apoBuildId = (Get-FileHash -Algorithm SHA256 -LiteralPath $apoBinary).Hash.Substring(0, 16)
$apoFileName = "HXAudioApo-$apoBuildId.dll"
Copy-Item -LiteralPath $apoBinary -Destination (Join-Path $staging $apoFileName)

# APO-only means exactly these three binaries.  A driver package, certificate,
# or kernel binary in this directory is a release-blocking error.
$allowedStageNames = @('HXAudioControl.exe', 'HXAudioDriverSetup.exe', $apoFileName)
$unexpected = Get-ChildItem -LiteralPath $staging -File -Recurse |
    Where-Object { $_.Name -notin $allowedStageNames }
if ($unexpected) {
    throw "Unexpected file in APO-only staging: $($unexpected.FullName -join ', ')"
}

$installerSource = Get-Content -LiteralPath $installerScript -Raw
foreach ($forbidden in @('HXAudioVirtualAudio.inf', 'HXAudioVirtualAudio.sys',
    'HXAudioVirtualAudio.cat', 'TESTSIGNING', 'bcdedit.exe', 'SecureBoot')) {
    if ($installerSource.Contains($forbidden)) {
        throw "APO-only installer contains a forbidden virtual-driver operation: $forbidden"
    }
}

& $controlBinary --dsp-self-test
if ($LASTEXITCODE -ne 0) { throw 'DSP self-test failed.' }
& $controlBinary --main-ui-smoke
if ($LASTEXITCODE -ne 0) { throw 'Main UI smoke test failed.' }
& $controlBinary --peq-ui-smoke
if ($LASTEXITCODE -ne 0) { throw 'PEQ UI smoke test failed.' }
& $controlBinary --virtual-bass-ui-smoke
if ($LASTEXITCODE -ne 0) { throw 'Virtual bass UI smoke test failed.' }
& $setupBinary apo-self-test $apoBinary
if ($LASTEXITCODE -ne 0) { throw 'APO COM and registration self-test failed.' }

& $inno "/DMyAppVersion=$version" "/DMyApoFileName=$apoFileName" $installerScript
if ($LASTEXITCODE -ne 0) { throw 'APO-only installer compilation failed.' }
if (-not (Test-Path -LiteralPath $installer)) {
    throw "APO-only installer was not produced: $installer"
}

& $signTool sign /v /fd SHA256 /s My /sha1 $thumbprint $installer
if ($LASTEXITCODE -ne 0) { throw 'Self-signing the APO-only installer failed.' }

$signature = Get-AuthenticodeSignature -LiteralPath $installer
if (-not $signature.SignerCertificate -or
    $signature.SignerCertificate.Thumbprint -ne $thumbprint) {
    throw 'The APO-only installer signature could not be verified.'
}

if ($virtualHashBefore) {
    $virtualHashAfter = (Get-FileHash -Algorithm SHA256 -LiteralPath $virtualInstaller).Hash
    if ($virtualHashAfter -ne $virtualHashBefore) {
        throw 'The existing virtual-driver installer was modified by the APO build.'
    }
}

$hash = Get-FileHash -Algorithm SHA256 -LiteralPath $installer
$releaseInfo = [ordered]@{
    Version = $version
    PackageMode = 'APO-only'
    Windows = 'Windows 10 and Windows 11 x64'
    EndpointPolicy = 'All current and newly connected terminal hardware render endpoints'
    IncludesVirtualAudioDevice = $false
    RequiresSecureBootChange = $false
    RequiresTestSigning = $false
    Subject = $certificate.Subject
    Thumbprint = $thumbprint
    Installer = $installer
    InstallerSHA256 = $hash.Hash
    ApoFileName = $apoFileName
    PreservedVirtualInstallerSHA256 = $virtualHashBefore
}
$apoReleaseInfo = Join-Path $artifacts 'apo-release-info.json'
$releaseInfo | ConvertTo-Json | Set-Content -LiteralPath $apoReleaseInfo -Encoding utf8

Write-Host "APO installer: $installer"
Write-Host "SHA256: $($hash.Hash)"
if ($virtualHashBefore) {
    Write-Host "Existing virtual installer preserved: $virtualHashBefore"
}
