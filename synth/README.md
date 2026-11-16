# es-synth

Small C99 procedural petrol-engine audio source for the 320d / iDrive 7 overlay.
No recorded samples, ALSA build dependency, runtime heap allocation in the DSP,
convolution, or extra thread. Output: 48,000 Hz, signed 16-bit little-endian,
interleaved stereo (identical L/R for reliable cabin mono compatibility).

## Build and verify

```
make host
./test.sh
make car
./render_all.sh
```

`make host` produces `build/es-synth-host`. `make car` uses
`$(HOME)/.local/zig/zig-aarch64-macos-0.17.1/zig` (override `ZIG`) with
`-target x86_64-linux-gnu.2.27 -O2 -s`, then copies the stripped ELF to
`../tool/lfb-enginesound/es-synth`. The car target runs `file` and a dynamic
symbol version check; details are saved in `build/glibc-symbols.txt`.
Both targets use only libc and libm; Linux also links libdl. Python 3, without
third-party packages, supports offline verification and sample packaging.

`test.sh` checks internal floating-point DSP states on extreme RPM/load sweeps,
limiter bounds, all four demo levels, relative sample-slope outliers, firing
pitch at 1,800 and 3,600 RPM, spectral brightening under load, V8/V10 overrun
crest factors, rapid engine switching, RPM jumps, shifts, malformed commands,
mute, quit and stale-input fading. It then prints a 60-second benchmark for
all presets. FFT tests use Hann windows and are objective regressions, not a
substitute for listening. `build/` contains additional stereo test renders.

The four delivered `samples/*.wav` are 40-second **mono** PCM preview files
(3,840,044 bytes each); live and normal WAV output remain stereo. A 40-second
48 kHz stereo PCM file cannot fit the requested 4 MB ceiling (7,680,044 bytes).
`render_all.sh` first renders the normal stereo file, then losslessly removes
the identical right channel to satisfy the size ceiling.

## Use

```
./es-synth --out alsa:plug:smartx:OAPGenericOverlay_CF_48_STEREO --preset v8 --volume 0.8
./build/es-synth-host --out wav:build/demo.wav --demo 40 --preset v10
./build/es-synth-host --out wav:build/events.wav --script build/drive.txt
./es-synth --out raw --preset i6
./es-synth --tone alsa:plug:smartx:OAPGenericOverlay_CF_48_STEREO
./es-synth --info
./build/es-synth-host --bench 60
```

`--info` emits JSON: `sample_rate`, and `presets`, each with `id`, `idle`,
`redline`, `cylinders`, and `turbo_default`. Logs use stderr; raw PCM uses stdout.
Raw mode runs at wall-clock speed, ALSA at the device's pace; WAV renders offline.
`--tone` plays three short, softly gated 880 Hz beeps per second for ten seconds
at a conservative level. SIGINT/SIGTERM trigger the normal quit fade.

Timed scripts contain `<seconds> <control line>`, in chronological order:

```
0 E v8
0 V 0.8
0 P 650 0.05 0.03 0 0
0.05 P 660 0.05 0.03 0 0
```

Scripts use the **same stale-data watchdog**, so keep sending P updates at least
every 1.5 seconds. Script scheduling resolves to a 256-frame boundary (5.33 ms).
The offline duration is the last timestamp plus one second; Q fades out and can
end it earlier. Demo duration scales a 40-second sequence: three seconds idle,
two free revs, acceleration through gears 1–5 to 85% redline per gear, overrun
through 2nd, light cruise in 6th, then stop/idle. The final half second fades.

## Control protocol

Newline-terminated ASCII on stdin, typically 20–100 Hz:

| Command | Effect |
| --- | --- |
| `P rpm load throttle gear speed_kmh` | Virtual RPM; signed load -1..1; throttle 0..1; gear -1..10; speed km/h |
| `E i6\|v8\|v10\|v12` | 300 ms settling crossfade; selects that engine's turbo default |
| `T 0\|1` | Disable/enable subtle turbo, including on normally NA presets if explicitly requested |
| `V 0..1` | Smooth master level |
| `M 0\|1` | Unmute/mute, approximately 300 ms to 95% of destination |
| `S fromGear toGear` | Smooth modest upshift torque dip or downshift blip |
| `Q` | Fade out, exit after 800 ms |

Malformed/nonfinite values and unknown engine names are ignored; finite P
parameters are clamped to safe ranges. Overlong input lines are discarded.
Only one bounded nonblocking read occurs per block; stdin never waits on a line.
No P for 1.5 seconds fades out, including on initial startup. New data fades back
in. RPM/load/throttle/speed and volume are smoothed every sample; shift envelopes
and source phase never reset on a parameter change. Speed/gear are retained for
the controller, not used to infer RPM: the external daemon owns that mapping.

ALSA is loaded using `dlopen("libasound.so.2")` and manually declared opaque
function pointers, including open/set_params/writei/recover/delay/drop/close.
Open is nonblocking. `ES_ALSA_US` requests the buffer (default 64000; accepted
16000-200000); the device's real buffer comes from `snd_pcm_get_params` (SmartX:
3 x 1024 frames). `snd_pcm_set_params` starts playback once the buffer is full, so
the synth keeps it full: it renders a 256-frame block only while `snd_pcm_delay`
leaves room for it, and otherwise sleeps 2 ms. The device's own clock paces the
synth (no drift against the wall clock); the wall clock paces only while the device
is not open. EAGAIN (nobody reads: the AudioManager route not connected) drops the
block. Underruns are recovered; other errors reopen the device, retrying every two
seconds.

## Sound design

Each cylinder has a true 720-degree crank firing angle and a recurrent
fast-rise/slow-decay pressure envelope. Its radiated pressure difference excites
the exhaust, with load/RPM-dependent rise, decay and brightness. A deterministic
PRNG varies each burn's energy and next-cycle ignition timing. Timing is anchored
to integer cycles, so jitter cannot accumulate into pitch drift. Each preset has
a persistent voice; weighted audio-rate transitions allow rapid switches without
resetting an audible state. Inactive voices stop processing after fading.

Two separate short damped feedback waveguides preserve bank firing structure.
A shorter collector, muffler lowpass and cabin lowpass follow. Pipe tuning uses
an effective 510 m/s hot-gas velocity. Modest unequal bank weights and unequal
pipe lengths preserve V8 half orders in the mono mix. A 35 Hz highpass removes
DC and sub-bass, keeping a 6-cylinder's idle firing (35 Hz); there is no bass boost.
The output has +6 dB of make-up gain before the soft limiter (peaks under -1 dBFS). Intake is turbulent, load-gated and pulse
modulated; mechanical highpassed noise remains low in level. V8/V10 negative
load adds randomized decaying exhaust crackles/pops into the same pipe path.
Turbo has independent spool inertia, a quiet 850–2050 Hz whine and a filtered
noise blow-off on falling spool. Final rational soft limiting asymptotes to
0.89 (below -1 dBFS). DSP feedback is bounded, tiny states are flushed, and x86
FTZ/DAZ is enabled. Coefficients update every 32 samples; controls smooth at
audio rate. There is no arbitrary oscillator standing in for firing frequency.

| Preset | Cylinders / firing order | Angles, degrees | Idle/redline | Bank lengths / collector, m | Character |
| --- | --- | --- | --- | --- | --- |
| i6 | 6 / 1-5-3-6-2-4 | 0:120:600 | 700/7000 | .71,.74 / .42 | smooth, harder metallic upper range, subtle turbo |
| v8 | 8 / 1-8-7-3-6-5-4-2 | 0:90:630 | 650/7200 | 1.03,1.17 / .59 | cross-plane uneven bank burble, turbo, overrun |
| v10 | 10 / 1-6-5-10-2-7-3-8-4-9 | 0:72:648 | 900/8250 | .49,.55 / .29 | shorter/brighter pipes, high firing pitch, NA, overrun |
| v12 | 12 / 1-7-5-11-3-9-6-12-2-8-4-10 | 0:60:660 | 700/7500 | .83,.87 / .47 | softer dense smooth pulses, low variation, NA |

V8 bank membership follows the reference convention 1–4 / 5–8, producing bank
angles 0/270/540/630 and 90/180/360/450. V10/V12 use contiguous cylinder banks;
i6 uses a single bank. These are evocative synthetic presets, not measured BMW
engine/exhaust models or exact recordings of the named cars.

The build and automated spectral checks do **not** establish subjective sound
quality, actual Apollo Lake CPU usage, hardware routing, or SmartX latency.
Those require on-car audition and measurement. Start with low vehicle volume.

## Credits and license notice

Sound-design reference: [Engine_Sim](https://github.com/yigitsalihemecen/Engine_Sim),
by Yigit Salih Emecen, MIT licensed. Studied README.md, AGENT_CONTEXT.md,
src/CONTRACT.md, profiles.js, pulse.js, combustion-worklet.js, resonators.js,
character.js, layers.js, and fx.js. Borrowed constants: the four firing orders,
even 720-degree firing geometry and contiguous bank numbering. Pipe lengths,
levels, pulse coefficients and cabin tuning here are original. The event-based
pressure variation and waveguide architecture are adapted ideas, independently
implemented in lean C, not a 1:1 port.

Copyright (c) Yigit Salih Emecen

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
