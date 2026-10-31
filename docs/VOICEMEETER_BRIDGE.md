# HXAudio Voicemeeter Bridge — experimental

This unsigned test build uses an existing Voicemeeter Input as a WASAPI loopback
source and sends processed audio to one chosen output. It does not install an
HXAudio driver or APO, change default audio devices, or uninstall Voicemeeter.
It has not been verified with a real sound card and must not be treated as a
production release.

1. Install and configure Voicemeeter separately. Start at low volume.
2. In Windows or in the application being tested, **manually** choose the
   Voicemeeter Input selected in the bridge. HXAudio never selects it for you.
3. In Voicemeeter disable every hardware/virtual bus that could play or route
   the same signal to the bridge output; otherwise direct monitoring can cause
   doubled sound or feedback. HXAudio cannot inspect or enforce Voicemeeter's
   internal routing. Do not create any other path from the chosen output back
   to the source. Device-name filters cannot detect every third-party loop.
4. Select a physical output, start the bridge and play a test signal. If the
   bridge reports a disconnect it stops processing; manually restore the normal
   Windows output, then refresh and retry when devices return.
5. For A/B, keep the same source, output, playback level and preamp; compare PEQ
   at 0 dB versus a clearly audible nonzero setting. Stop and restart after
   changing DSP controls. Toggle virtual bass separately. Listen at a safe
   level and check the input and processed peak indicators; 100% indicates
   clipping and the bridge clamps output to prevent out-of-range samples.
6. Buffer is a request in milliseconds, not measured end-to-end latency. Measure
   delay with a loopback test signal and recording on real hardware; it also
   includes Voicemeeter, Windows and device buffers. A zero input peak during
   playback means inspect source selection and Voicemeeter routing.

If setup fails, the per-user program files can be removed with Windows Apps or
the uninstaller. Close the bridge before uninstalling. No driver recovery or
audio-service restart is required. If sound is absent after stopping the bridge,
manually choose the normal physical Windows playback device. CI validates only
offline DSP and simulated data paths, not actual sound capture or playback.

## Virtual Bass overload fix (2026-10-31)

Bridge now exposes a 0–100% bass strength field (15% default). Stop and restart
processing to apply changes, just like the existing gain/PEQ controls. The bass
excitation uses `tanh(low * drive)` before evaluating any harmonic polynomial;
the wet harmonic gain is reduced by 6 dB. The legacy 0–200% DSP range remains
supported and covered by regression tests. This is a smooth saturator, so high
excitation can generate additional harmonics beyond the named preset orders.

Bridge replaces hard clipping with a unity region up to 0.85 and a C1 tanh
knee approaching 0.98. It guards after DSP and again after cubic resampling.
This is sample-peak protection, not an oversampled true-peak limiter. PEQ and
preamp still use their existing order and coefficients. Installer, uninstaller,
routing selection and APO/driver behavior are unchanged.

The running status shows cumulative counts reset at Start:
- DSP overrange: finite post-DSP samples outside ±1, before protection.
- DSP non-finite: post-DSP NaN/Inf replaced with silence.
- Soft protection: post-DSP samples above the 0.85 knee (not hard clipping).
- Hard clips: guarded samples reaching ±1 (expected zero).
- Render underruns / missing frames: software render callbacks starved after
  initial ring priming, and their silence-filled frame deficit. Silence or a
  stopped source can also increase these; these are not proof of a driver fault.
- Capture discontinuities: WASAPI DATA_DISCONTINUITY flags, separate from DSP.
- Input non-finite: invalid source samples removed before recursive DSP filters.
- Resampler protection: final-output soft-knee interventions after interpolation.

`--virtual-bass-self-test` writes `bridge-virtual-bass.log`, testing the shipping
DSP classes across 32/44.1/48/96/192 kHz, stereo versus independent mono,
block-size invariance, high bass levels, music-like transients with PEQ,
non-finite recovery, silence, impulse decay, harmonic output and guard counters.
CI also retains the existing DSP, pipeline and install/uninstall checks.
These tests do not validate real Voicemeeter capture, Realtek playback or listening
quality. Test on the real PC starting at 15% and compare the diagnostic counters
with Virtual Bass off/on before increasing strength.
