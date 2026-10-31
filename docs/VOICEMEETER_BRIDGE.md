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
