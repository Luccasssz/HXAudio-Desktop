$ErrorActionPreference='Stop'
$sha = if ($env:GITHUB_SHA) { $env:GITHUB_SHA } else { (& git rev-parse HEAD).Trim() }
if ($sha -notmatch '^[0-9a-f]{40}$') { throw 'Cannot determine commit SHA' }
$run = if ($env:GITHUB_RUN_NUMBER) { [int]$env:GITHUB_RUN_NUMBER } else { 0 }
$attempt = if ($env:GITHUB_RUN_ATTEMPT) { $env:GITHUB_RUN_ATTEMPT } else { 'local' }
$id = "0.1.0-dev+$($sha.Substring(0,7)).$run.$attempt"
$title = "HXAudio Voicemeeter Bridge $id"
Set-Content Bridge/BuildVersion.h "#define HX_BUILD_ID L`"$id`"`n#define HX_WINDOW_TITLE L`"$title`"" -Encoding ascii
Set-Content Installer/BuildVersion.iss "#define BuildId `"$id`"`n#define ShortSha `"$($sha.Substring(0,7))`"" -Encoding ascii
@"
#include <windows.h>
1 VERSIONINFO
FILEVERSION 0,1,0,$run
PRODUCTVERSION 0,1,0,$run
BEGIN
 BLOCK "StringFileInfo"
 BEGIN
  BLOCK "040904b0"
  BEGIN
   VALUE "FileVersion", "$id"
   VALUE "ProductVersion", "$id"
   VALUE "Comments", "SHA $sha; run $run; attempt $attempt"
  END
 END
 BLOCK "VarFileInfo"
 BEGIN
  VALUE "Translation", 0x409, 1200
 END
END
"@ | Set-Content Bridge/BuildVersion.rc -Encoding ascii
"$id; SHA=$sha; UTC=$([DateTime]::UtcNow.ToString('o'))" | Set-Content bridge-version.log
