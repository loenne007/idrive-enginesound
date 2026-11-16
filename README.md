# Engine Sound for iDrive 7

A synthesized engine sound on your BMW's speakers, driven live by the car's **real rpm, gear and engine load**.
Pick an **inline-6** (B58 / S58 style), a cross-plane **V8**, a high-revving **V10** or a **V12**, or let the
**Enhance** sound engine give the car's own diesel the depth of a big six or V8 TDI. The sound is generated in real time
on the head unit, not played back from recordings.

Made for the BMW iDrive 7 head unit (MGU), installed through the [MGU Store](https://github.com/robertnisipeanu/mgu-store).
Developed for a G20 320d (B47 diesel, ZF 8HP).

> **Use at your own risk.** An unofficial hobby project, not affiliated with BMW. It changes nothing in the car's
> control units; it only plays audio. Keep the volume sensible: you still need to hear the road, sirens and the
> car's warnings.

## Install

1. In the MGU Store: **Settings → Add source**, type `loenne007/idrive-enginesound`, choose the branch **main**.
2. Install **Engine Sound**. The Store installs its service (**Engine Sound service**, `lfb-enginesound`) with it.
3. Restart the head unit when the Store asks (the service starts at the next start).
4. Start the engine. The app starts with the head unit; open its tile to pick the engine, the sound engine and the
   audio route.

Updating keeps your settings: the sound engine chosen in an earlier version (`a` / `b` / `c` / `d`, `pulse` / `bass`)
becomes Waveguide / Replica / Simulate / Enhance, Simulate / Enhance.

## How it works

```
 car (SOME/IP)                    app lfb_enginesound (Node 8, autostart)           service lfb-enginesound
 rpm, gear, torque, speed,  ──▶  maps the car's rpm onto the engine's rev range, ──▶ esd.js ──▶ es-synth-<id> (C)
 driving mode                     load from torque, on/off logic, audio routing      Unix socket    │
                                  (GENIVI AudioManager: a source → the speakers)                    ▼
                                                                                ALSA → SmartX → car speakers
```

- **rpm**: the diesel's idle-to-max range (about 800–4800 rpm) is stretched over the chosen engine's idle-to-redline
  range, so a V10 really screams at the top. *Settings → Rev range → The car's real rpm* turns that off; the Bass
  sound engine always gets the real rpm.
- **load**: crankshaft torque against the car's max torque (*Car's max torque*, 400 Nm for a 320d). Pulling hard
  makes it louder and harder-edged; negative torque (engine braking) gives the overrun character.
- **gear**: a gear change of the automatic (ZF 8HP) is sent to the synth (shift character, no ignition cut).
- It plays while it is switched on and the engine runs (with *Only in Sport mode*: only in Sport). Otherwise it
  fades out and, a few seconds later, the audio route is released.

## Sound engines

Four synthesizers ("sound engines") are installed side by side; each plays all four presets. Pick one on the
Engine page or in *Settings → Sound engine*. **Simulate** and **Enhance** are the two to use; the older **Waveguide**
and **Replica** are offered only with *Settings → Legacy sound engines*.

| Id | Title | |
|---|---|---|
| `simulate` | **Simulate** (default) | Light and quick. Per cylinder an exhaust and an intake pressure pulse at its crank angle, with the load's rasp riding on each pulse, through quarter-wave pipes, a collector and muffler chambers; an explicit level map (off the gas about 4 dB under full load). Follows a step in rpm within ~26 ms, runs at 300x+ realtime. [synth/simulate](synth/simulate/README.md) |
| `waveguide` | **Waveguide** (legacy) | Engine_Sim's sound path rewritten in C: combustion events into delay-line waveguide pipes, muffler, Helmholtz airbox, a sub layer, a turbo layer, cabin filter, small room and three-band dynamics. Fuller and darker. [synth/waveguide](synth/waveguide/README.md) |
| `replica` | **Replica** (legacy) | The Engine_Sim website's sound engine ported 1:1 to C (its Web Audio nodes rebuilt as Chrome runs them), driven by the car's data, with automatic-gearbox shifts and a quiet turbo. The heaviest on the CPU (~35x realtime on a Mac). [synth/replica](synth/replica/README.md) |
| `enhance` | **Enhance** | An Active Sound Design emulation: it does not play a foreign engine but adds rpm-locked low engine orders (mostly 30–150 Hz) under the car's own diesel, from the **real** rpm. Its presets are diesel characters: **B57 straight-6** (i6), **V8 TDI** (v8), **V6 TDI** (v10), **V6 TDI Sport** (v12). [synth/enhance](synth/enhance/README.md) |

Each is a folder `synth/<id>/` with its own C source, tests and sample renders (`samples/*.wav`, 40 s demo drives),
built into `tool/lfb-enginesound/es-synth-<id>`. All share one command line and control protocol
([synth/PROTOCOL.md](synth/PROTOCOL.md)).

## Pages

- **Engine**: playing / standby and why, the sound engine, the audio mode (tap: over the music / instead of it),
  on / off and **Test sound** (10 s of beeps through the route the engine plays on, also with the engine off); live
  car rpm, the engine's rpm, load, gear and speed; the preset.
- **Sound**: sliders (0–200 %, 100 % = the sound engine's own default) for **Off the gas** (idle, coasting, light
  load), **Turbo**, **Blow-off**, **Pops & bangs** and **Bass**; **Reset all**. Also in Settings. Whatever they are
  set to, the speaker protection (below) keeps the output safe.
- **Audio**: the audio route (below), its state, and the mix route candidates with a test each.

## Audio: exclusive or mix

**Exclusive** (default): the engine plays as its own source on the main speakers, the AudioManager source
`CarLifeMedia` (287, China-only and free elsewhere) into SmartX `CarLifeMedia_CF_48_STEREO`, the route
[Moonride](https://github.com/JustAnotherPU/moonride) uses on iDrive 7. The music pauses while the engine plays and
comes back a few seconds after it stops.

**Mix** (*Play over music (mix)*, on the Audio page or in Settings): the engine plays over the music, as a
navigation prompt does (the car decides whether it mixes or ducks the music). The default mix route is the
AudioManager source `OAPGenericOverlay` (1048) to the main speakers (1024); its SmartX endpoint is found by name in
the service's list of SmartX shared-memory files (`/dev/shm`), falling back to
`plug:smartx:OAPGenericOverlay_CF_48_STEREO`.

Mix is new and not yet confirmed in the car. To find a route that works:

1. Open the **Audio** page. It reads the AudioManager's sources and the SmartX endpoints (**Refresh** reads them
   again) and lists the **mix candidates**: the default, and every source whose name sounds like a prompt
   (Overlay, Interrupt, Navi, TTS, Announcement, Speech, Prompt, Chime) and that has a SmartX endpoint.
2. Play some music, then press **Test** on a candidate: 10 s of beeps through that route. If you hear the beeps
   with the music still playing (perhaps quieter), that route mixes.
3. Press **Use for mix** on it (kept in the app's data folder, `mix-route.json`) and switch to **Over the music**.

If the AudioManager or the SmartX list is unavailable, the default is used and the page says so. A mix route that
the AudioManager refuses, or that does not reach `CS_CONNECTED` within 6 s, is released and shown as failed (also in
the app's log). No connection and no "available" mark is ever left behind: only the China-only CarLifeMedia is marked
available, and only while it plays; leftovers of a crash or a power-off are removed at the next start.

## Speaker protection

The last stage of every sound engine (`synth/common/protect.h`). In the car it first raises the engine by 10 dB to
the loudness of radio and music, so the volume knob can stay where the radio is comfortable (and the radio isn't
deafening when the engine sound stops). Then: no DC or infrasound (30 Hz high-pass), nothing above 16 kHz, the bass
below ~150 Hz held under -3 dBFS, the long-term power held under -9 dBFS RMS (about mastered music), peaks under
-1 dBFS, and a fade-in at start. `tools/add_protect.py` puts it into a new engine; `make -C synth test` tests it.

## Settings

| Setting | |
|---|---|
| Engine sound | on / off (also the **On / Off** button) |
| Engine | inline-6, V8, V10, V12 (with Enhance: B57 straight-6, V8 TDI, V6 TDI, V6 TDI Sport) |
| Sound engine | Simulate, Enhance (the legacy Waveguide, Replica with *Legacy sound engines*) |
| Legacy sound engines | offers Waveguide and Replica too |
| Volume | 10–100 %, default 50 %; the car's own volume knob applies on top. 50 % is about 90 % of versions before 1.1, 100 % about 6 dB more |
| Only in Sport mode | plays only while the driving mode is Sport |
| Play over music (mix) | the mix route instead of the exclusive one (above) |
| Turbo sound | the turbo whistle and blow-off of the turbo presets |
| Sound: … | the Sound page's sliders |
| Rev range, Car's max rpm, Car's max torque | the mapping above |
| Colour theme, Screen layout, Renderer, Demo data | the app kit's display settings |

## Repository

| Path | |
|---|---|
| `apps/lfb_enginesound/` | the app: `main.js` (pages, background), `core.js` (mapping, on/off, sound engines), `route.js` (AudioManager routes), `daemon.js` (service socket client), `app.json` |
| `tool/lfb-enginesound/` | the service: `esd.js`, the prebuilt `es-synth-<id>` (x86-64, glibc ≤ 2.27, loader `/lib/ld-linux-x86-64.so.2`), `check.js` (the Store's health check), its systemd unit and `component.json` |
| `synth/<id>/` | each sound engine: C source, Makefile, tests, sample renders |
| `synth/common/` | the shared speaker protection and its test |
| `synth/PROTOCOL.md` | the shared command line and control lines |
| `tools/` | `import_engine.sh` (a sound engine from a branch into `synth/<id>/`), `add_protect.py`, `check_glibc.py`, `make_es_icon.js` (the app icon) |
| `test/core.test.js` | the app's logic and routes, with a fake service and AudioManager |
| `kit/` | the MGU app kit from [idrive7-apps](https://github.com/cauliflowermossgrape/idrive7-apps) (MIT): runtime, `tools/build.py`, previews |
| `mgu-repo.json` | the Store's repo-tree manifest: the head unit builds the app from source (`kit/tools/build.py`), the service's files are prebuilt |

## Development

```sh
node test/core.test.js                       # the app's logic and audio routes
make -C synth test                           # every sound engine (renders, checks, bench) and the protection
make -C synth car                            # the head unit's binaries (zig 0.17), checked by tools/check_glibc.py
synth/simulate/build/es-synth-host --out wav:v8.wav --demo 40 --preset v8   # listen to a demo drive

python3 kit/tools/build.py --preview-only lfb_enginesound        # stage the app without BMW's SDK
node kit/tools/preview.js dist/lfb_enginesound preview wait=8 render --set demo=on   # its page as PNGs
```

The service runs locally too: `ES_RUN_DIR=<dir> ES_SYNTH_DIR=<dir with es-synth-<id>> ES_SHM=<dir> node
tool/lfb-enginesound/esd.js`, and the app's `ES_CTL_SOCK` points at its socket. An installable package needs BMW's
`oap-sdk` from your own head unit (`kit/tools/extract_sdk.js`; never commit it).

A new sound engine: `tools/import_engine.sh <id> <git ref>` (same command line and control lines), then its binary
in `component.json` `files`, its entry in `SOUND_ENGINES` (`core.js`) and in the `soundEngine` setting (`app.json`).

## Credits

- [Engine_Sim](https://github.com/YigitSalihEmecen/Engine_Sim) by Yigit Salih Emecen (MIT): the Waveguide engine
  re-implements its sound path; Simulate uses its pipe, muffler and airbox values; Replica ports it 1:1; firing orders and crank angles.
- [DasEtwas/enginesound](https://github.com/DasEtwas/enginesound) (MIT) and
  [Antonio-R1/engine-sound-generator](https://github.com/Antonio-R1/engine-sound-generator) (MIT): ideas (valve and
  waveguide engine model after Baldan et al. 2015).
- [ange-yaghi/engine-sim](https://github.com/ange-yaghi/engine-sim) (MIT): ideas (noise multiplied onto the pressure
  signal).
  No code was copied from these three.
- App kit: [idrive7-apps](https://github.com/cauliflowermossgrape/idrive7-apps) by cauliflowermossgrape (MIT).
- The audio route: [Moonride](https://github.com/JustAnotherPU/moonride) and [RideTube](https://github.com/JustAnotherPU/ridetube)
  by JustAnotherPU showed the SmartX / AudioManager path; [NamelsMissing/mgu-apps](https://github.com/NamelsMissing/mgu-apps)
  published the AudioManager's source list.

## License

MIT (`LICENSE`). The kit keeps its own notice (`LICENSE.idrive7-apps`); the Engine_Sim licence text is in the
sound engines' READMEs.
