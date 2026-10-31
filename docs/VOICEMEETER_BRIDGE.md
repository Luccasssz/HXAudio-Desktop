# HXAudio Voicemeeter Bridge — experimental

This unsigned test build uses an existing Voicemeeter Input as a WASAPI loopback
source and sends processed audio to one chosen output. It does not install an
HXAudio driver or APO, change default audio devices, or uninstall Voicemeeter.
The user confirmed real Voicemeeter → HXAudio → Realtek playback and PEQ,
and that the preceding bass overload fix removed the audible crackling. This
experimental live-control revision still needs real-hardware listening validation.

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
   at 0 dB versus a clearly audible nonzero setting. Adjust DSP controls during
   playback; they apply without reopening either audio client. Toggle virtual
   bass separately. Listen at a safe level and inspect the separate DSP
   overrange, soft-protection and render/capture counters.
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

Bridge exposes a 0–100% bass strength field and slider (15% default), now live
during playback together with gain and PEQ. The bass
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


## Live DSP controls (2026-10-31)

The Start routing-confirmation dialog is removed. A permanent, non-modal warning
explains that Voicemeeter's internal routing cannot be inspected and feedback
cannot be guaranteed absent. Existing endpoint identity/name checks, duplicate
router checks and disconnect errors remain. Source, physical output, buffer,
and refresh controls are disabled while the audio session is running; these
are the only settings here that require Stop/Start.

Gain and bass strength have sliders plus numeric editors. Frequency, PEQ gain
and Q have numeric editors. Numeric typing is debounced for 60 ms; incomplete,
non-finite or out-of-range edits retain the last valid snapshot. The applied
readout comes from the audio thread, including the current ramped gain, bass
strength, enable blend, PEQ transition target, revision and transition state.
At a full telemetry queue an update is skipped rather than blocking audio.

DSP uses a fixed-capacity single-producer/single-consumer queue with acquire/
release indices. Only the capture thread owns and mutates DSP. It consumes the
latest valid snapshot at the next processing block boundary; the UI retries its
latest unsent value if the queue is full. No locks, IO, filter reconstruction,
or allocation are introduced into the DSP processing/update path.

Gain, bass strength and bypass blend ramp over 20 ms. Bass filters keep running
while bypassed to avoid stale-state transients; 0% strength fully bypasses bass.
PEQ has two session-preallocated filter banks. The inactive bank receives new
stable coefficients and cleared state, and is faded in with a 30 ms raised-cosine
crossfade. Updates during a fade are coalesced until that fade finishes, then
applied at a block boundary. Existing tanh excitation, wet gain and both peak
guards are retained. This revision does not boost the wet signal further.

Expected control latency includes UI debounce for typing, the next capture
block, 20–30 ms transition and the existing output buffering; sliders do not
wait for the typing debounce. No control update calls WASAPI initialization,
Stop/Start, endpoint activation or device reopening.

Validation includes the original DSP/bass/pipeline/install/uninstall tests plus:
- `--realtime-dsp-self-test`: five sample rates, 0/15/20/30/50/100% strength,
  full-scale bass with rapid toggles/updates, 20 ms gain convergence, full PEQ
  ranges, invalid snapshots and concurrent transport stress with 100001 attempts.
- `--realtime-ui-self-test`: real Win32 edit/slider/toggle message handlers with
  a concurrent synthetic audio thread; 1000 UI sweeps, invalid typing, latest
  target convergence, and session-control enabled/disabled behavior. It opens
  no WASAPI client and does not prove real-hardware playback.
- A harness extracts the actual production DSP definitions and runs the same
  tests under Windows AddressSanitizer, then counts allocations across 10000
  live updates (expected zero). Its memory and build results are included.

Capture discontinuities are cumulative, including startup transients; a solitary
increment is not by itself an audible fault. Render missing-data callbacks can
also occur when the source is silent/stopped. Neither count is labeled as DSP
clipping, nor treated as proof of a persistent hardware fault.

## Experimental cleaner bass (multiband A/B)

The original small-speaker optimization remains fully intact and is the
default when the new **清晰低频试验** checkbox is unchecked. The checkbox is
only audible with Virtual Bass enabled and Small Speaker Optimization checked.
It crossfades between the two continuously warmed synthesis banks over ~20 ms,
so listening tests need not stop or reopen the Voicemeeter/WASAPI session.
The old bass generator and its tested 50% setting are unchanged.

This separate experiment is motivated by user feedback that the first
small-speaker mode sounds full but somewhat cluttered on music. A single
broadband nonlinear waveshaper creates not only intended harmonic partials
but also new sum/difference frequencies when multiple bass notes overlap.
An experimentally split 3-band generator (approximately 32–58, 52–88,
78–120 Hz, with overlapping IIR transitions) uses an independent envelope
and nonlinear excitation per band. It places less emphasis on even
harmonics and limits the added wet band to approximately 110–350 Hz.
The original low frequencies are retained. This is a lightweight
multiband *experiment*, not a pitch-tracking or phase-vocoder system.

Synthetic diagnostics in `bridge-clean-bass.log` and
`bridge-clean-bass-spectrum.csv` compare the two existing small-speaker
algorithms on two simultaneous bass tones, reporting target-band harmonic
energy and unwanted intermodulation relative to target harmonic power.
A weaker output does not count as an improvement on its own: tests also
enforce a minimum retained target-band energy and fundamental level.
Additional samples in `clean-bass-ab` are equal-RMS WAVs processed through
the actual C++ implementation. Keep all other sound processing disabled
when playing pre-processed test WAVs.

**Do not assume the new mode sounds better because its numerical IMD metric
is smaller.** Its harmonic balance is different and the perceived bass may
be less full on some recordings. Compare the same track at 50% bass,
0 dB PEQ, unchanged system/output volume and with the compensated
loudness matched as closely as possible. Switch only the clean checkbox.
Listen for kick/bass separation, unwanted ringing or rough midbass,
and retention of punch. If it sounds weaker or worse, switch back.

This experiment changes no APO, driver, installer logic or WASAPI route.

## Experimental small-speaker psychoacoustic mode

The original mode remains the default and is unchanged. Enable Virtual Bass,
then toggle **小扬声器优化** during playback for a 20 ms mode crossfade. Both
banks stay warm and use the existing strength ramp and downstream peak guards.
No route, WASAPI session, PEQ, installer or uninstaller change is needed.

Analysis of the original mode:
- Its cascaded 80 Hz low-pass passes about 29% amplitude at 100 Hz (-10.7 dB).
- Its four harmonic high-pass biquads form an eighth-order filter: an 80 Hz
  second harmonic is reduced to 25% (-12 dB), while 120 Hz passes about 70%.
- The 2/3/4 weighted combination partly cancels its quadratic term:
  0.58*(2*x^2) + 0.13*(8*x^4-8*x^2) = 0.12*x^2 + 1.04*x^4.
- Division by drive and the preceding wet-gain reduction further limit output.
- The replacement mix approaches fully high-passed dry sound at 100% strength,
  reducing the original fundamental even when the speaker can reproduce it.

The new additive bank extracts bass with a 40 Hz second-order high-pass and
110 Hz fourth-order low-pass. These are overlapping synthesis bands, not brick
walls or measurements of the physical speaker. A per-channel envelope (10 ms
attack / 120 ms release) normalizes bounded tanh excitation, with a 0.03 floor
that avoids amplifying near-silence. The 2/3/4 polynomial weights are 0.25/0.55/
0.20. A 120 Hz sixth-order high-pass suppresses residual fundamental leakage;
a 400 Hz fourth-order low-pass reduces higher harmonics. A second tanh stage
bounds the additive wet signal to ±0.25 before the existing Bridge guards.
The original dry bass is not deliberately removed, at any strength. Residual
wet fundamental can still interfere in phase; the tests measure that explicitly.

These filter/gain choices were selected by synthetic spectral comparisons,
including rejecting an earlier candidate that lost too much fundamental. They
are provisional: actual speaker response and preference still require listening.
Tanh saturation also produces orders above 2/3/4; this is harmonic enrichment,
not transparent reproduction. Music can produce intermodulation. If voices or
midrange become rough, reduce strength or return to the original mode.

CI writes `bridge-small-speaker-spectrum.csv` and `bridge-small-speaker.log`.
The CSV contains 40/50/60/80/100 Hz results at four sample rates, three input
levels and four strengths, for both modes: fundamental, second/third/fourth,
120–350 Hz harmonic energy, total RMS, intentional THD (orders 2–10), higher
harmonics above 700 Hz and protection counts. Intentional THD is not interpreted
as unwanted distortion alone. Harmonics above the tenth are not included in
that metric. Synthetic music is additionally compared through the same
120/350 Hz analysis filters with total-RMS normalization; this filtered metric
has transition-band leakage and is not a brick-wall FFT measurement. A separate
per-note projection measures the known bass-note harmonics within 120–350 Hz
after each note settles, divided by total RMS squared; it excludes other
non-harmonic transient energy.

The acceptance checks require stronger absolute target harmonics across the
matrix, fundamental retention, limited measured upper harmonics, and a higher
fraction of target harmonics at equal total RMS for every 50%-strength,
0.5-amplitude test tone. At 100% the original mode removes much of the bass;
its normalized harmonic fraction can exceed the new mode's despite much lower
absolute target energy. Keeping bass and maximizing harmonic fraction are not
the same goal. No claim is made that all normalized settings sound stronger.

### Repeatable A/B listening

1. Live comparison: use the same music, gain, PEQ, output volume and 50% strength.
   Keep Virtual Bass enabled, then alternate only the small-speaker checkbox.
   Try 30% first if 50% sounds rough. This comparison preserves the dry level,
   but does not guarantee equal total loudness.
2. For a loudness-controlled comparison, use the included `speaker-ab` files:
   `tones-A-original-matched.wav` vs `tones-B-small-speaker-matched.wav`, then
   the corresponding `music-...-matched.wav` pair. A is the original algorithm;
   B is the new mode. Each pair is equal total RMS by attenuation only. Separate
   `raw.wav` files preserve the unnormalized levels for comparison.
3. The tones run 40/50/60/80/100 Hz, two seconds each, with short fades. The music
   is a deterministic synthetic bass/kick/chord signal, not a commercial track.
   Disable Bridge Virtual Bass and set PEQ gain to 0 while playing these files
   so their already-processed signal is not processed again. Keep player,
   Windows and physical output volumes identical between A and B.
4. To regenerate all signals and measurements in a writable folder:

   ```powershell
   & "$env:LOCALAPPDATA\Programs\HXAudioBridge\HXAudioBridge.exe" --small-speaker-self-test
   ```

This command is offline and opens no audio client. The test signals, CSV and
logs reproduce the numerical result, not the user's real listening experience.
