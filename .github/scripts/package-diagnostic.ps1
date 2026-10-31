$ErrorActionPreference = 'Stop'
$stage = Join-Path $PWD 'diagnostic-package'
New-Item -ItemType Directory -Path $stage -Force | Out-Null
$inputs = @('ControlPanel\x64\Release\HXAudioControl.exe', 'DriverSetup\x64\Release\HXAudioDriverSetup.exe', 'Apo\x64\Release\HXAudioApo.dll')
foreach ($inputFile in $inputs) { Copy-Item $inputFile $stage }
$files = Get-ChildItem $stage -File
$cert = $null
try {
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=HXAudio CI Diagnostic ONLY - Untrusted' -CertStoreLocation 'Cert:\CurrentUser\My' -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 -NotAfter (Get-Date).AddDays(7) -KeyExportPolicy NonExportable
    Export-Certificate -Cert $cert -FilePath (Join-Path $stage 'CI-UNTRUSTED-public.cer') | Out-Null
    $sdkBin = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    $signTool = Get-ChildItem "$sdkBin\*\x64\signtool.exe" -File | Sort-Object { [version]$_.Directory.Parent.Name } -Descending | Select-Object -First 1 -ExpandProperty FullName
    if (-not $signTool) { throw 'signtool unavailable' }
    foreach ($file in $files) {
        & $signTool sign /fd SHA256 /s My /sha1 $cert.Thumbprint $file.FullName
        if ($LASTEXITCODE -ne 0) { throw "Signing failed: $file" }
        $sig = Get-AuthenticodeSignature -LiteralPath $file.FullName
        if (-not $sig.SignerCertificate -or $sig.SignerCertificate.Thumbprint -ne $cert.Thumbprint) { throw "Unexpected signature: $file" }
        "$( $file.Name ): $($sig.Status); $($sig.StatusMessage); signer=$($sig.SignerCertificate.Thumbprint)" | Tee-Object -FilePath 'hxaudio-signature.log' -Append
        # /pa checks trust under normal Windows policy; a self-signed CI cert
        # is deliberately not imported into Trusted Root or Trusted Publishers.
        & $signTool verify /pa /v $file.FullName 2>&1 | Tee-Object -FilePath 'hxaudio-signature.log' -Append
        "trust verification exit: $LASTEXITCODE" | Tee-Object -FilePath 'hxaudio-signature.log' -Append
    }
    Get-FileHash -Algorithm SHA256 $files.FullName | Format-List | Out-File hxaudio-signed-hashes.log
    $exe = Join-Path $stage 'HXAudioDriverSetup.exe'
    $dll = Join-Path $stage 'HXAudioApo.dll'
    & $exe apo-diagnostic-self-test 2>&1 | Tee-Object -FilePath hxaudio-signed-diagnostic.log
    if ($LASTEXITCODE -ne 0) { throw "Signed diagnostic self-test failed: $LASTEXITCODE" }
    & $exe apo-self-test $dll 2>&1 | Tee-Object -FilePath hxaudio-signed-apo.log
    if ($LASTEXITCODE -ne 0) { throw "Signed APO self-test failed: $LASTEXITCODE" }
    @'
CI DIAGNOSTIC ONLY. SELF-SIGNED AND UNTRUSTED. NOT AN INSTALLER.
The private key was generated on an ephemeral runner and is not included.
Signature does not establish publisher identity or trust. Do not import the certificate.
Real-machine installation, APO registration, audio registry changes and service restarts require separate explicit approval.
'@ | Set-Content -LiteralPath (Join-Path $stage 'READ-ME-NOT-FOR-INSTALL.txt') -Encoding utf8
    Copy-Item hxaudio-signature.log,hxaudio-signed-hashes.log,hxaudio-signed-diagnostic.log,hxaudio-signed-apo.log $stage
    Compress-Archive -Path "$stage\*" -DestinationPath 'HXAudio-CI-Diagnostic-UNTRUSTED.zip' -Force
} finally {
    if ($cert) { Remove-Item -Path "Cert:\CurrentUser\My\$($cert.Thumbprint)" -Force -ErrorAction Continue }
}
