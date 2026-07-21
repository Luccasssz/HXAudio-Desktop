# HXAudio

APO version: 1.0.1b (internal test)  
Author: Huber_HaYu

## APO global mode (recommended)

The recommended mode installs `HXAudioApo.dll` as a Windows endpoint audio
processing object. It does not install a kernel driver and does not require
test-signing mode or disabling Secure Boot.

The installer removes the HXAudio virtual device before enabling this mode. A
LocalSystem endpoint guard attaches HXAudio as the only post-mix endpoint
effect on every selected terminal hardware render endpoint and monitors the
MMDevices render tree for devices that are connected later. Software
mixer/cable endpoints remain intermediate transport nodes so the signal is
processed once at the physical output. SFX, MFX, EFX, composite and offload
software-effect values are backed up per endpoint and removed while APO mode
is active, then restored exactly during a mode switch or uninstall.

APO installation and mode switching are transactional.  A LocalSystem endpoint
guard performs protected `MMDevices` changes, rebuilds the Windows audio
endpoint stack, and verifies every active render endpoint before Setup reports
success.  Covering an existing APO installation uses a content-addressed DLL
side by side, so the image currently loaded by `audiodg.exe` never forces a
reboot.  Switching from the virtual backend first stops its router, removes the
root device and driver package, and then completes the same verified APO flow.

The APO processes the endpoint's 32-bit floating-point stream in place. Its
real-time callback performs no registry access, allocation, or blocking wait.
DSP settings are stored in the machine settings key; a non-real-time watcher
reloads them while the audio graph is running. APO mode leaves the native
Windows shared-audio limiter enabled and does not install the former engine
bypass service.

## Virtual sound-card mode

This compatibility mode installs `HXAudio Virtual Output` and routes its
loopback stream to one or more selected physical endpoints. It remains useful
when a separate virtual playback endpoint is required. Because its kernel
driver uses a personal self-signed certificate, it requires Windows
test-signing mode and Secure Boot must not block that policy.

The router uses shared event-driven WASAPI clients, per-output render workers,
and a 32-bit float DSP bus. Device invalidation and MMDevice changes rebuild
the session. A bounded silence watchdog also rebuilds an orphaned loopback
client after a player destroys one song session and opens the next, preventing
a formally valid old session from leaving subsequent songs silent.

Virtual mode also installs the HXAudio endpoint APO as a unity-gain effect-slot
owner on terminal physical outputs. This isolates Dolby, DTS, Nahimic,
MaxxAudio and other OEM software APO components while leaving the virtual
router's existing PEQ and virtual-bass path unchanged. In `Virtual` mode the
endpoint APO is verified sample-transparent, so DSP is not applied twice.

## DSP and control panel

Both modes use the same `HXAudio 控制面板`, total gain, PEQ and virtual
bass settings. In APO mode each endpoint can be enabled or bypassed
independently, and the endpoint guard applies that selection to existing and
newly inserted USB, wired, Bluetooth and other Windows render endpoints.

The PEQ implements peak, low-shelf, high-shelf, low-pass, high-pass and notch
biquads. Total gain is limited to -30 dB through +30 dB, while every PEQ band
and the editor Y-axis are limited to -40 dB through +40 dB. Older profiles are
scaled proportionally into the new PEQ range.
Virtual bass supports the 2/3/4 and 3/5/7 presets plus three custom positive
floating-point multipliers. APO mode sends the processed floating-point stream
back to the Windows shared audio engine with its native limiter enabled.
Neither backend changes the Windows endpoint
volume automatically; endpoint volume changes only when the user operates a
device fader.

## Build

From an elevated PowerShell prompt:

```powershell
.\build-release.ps1
```

The signed APO version-1.0.1b installer is built with
`.\build-apo-release.ps1` and written to:

```text
artifacts\HXAudio-APO-Setup-1.0.1b-x64.exe
```
