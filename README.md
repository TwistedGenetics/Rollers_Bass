# Rollers

A jungle / drum & bass bass oscillator for the **Korg NTS-1 digital kit mkII** and the **Korg microKORG 2**, built with the logue SDK.

Rollers handles the bassline: a heavy, clean **sub** with a moving **Reese** (or a **Hoover** stack) on top. The two are split inside the oscillator. The upper band is high-passed just above the note, so the sub owns the fundamental, and **drive and crunch only touch the upper band**. However dirty the top gets, the low end stays solid on a big system. A built-in **pitch drop** on every note gives you 808-style thuds and glides without spending the synth's envelopes, and **CRUNCH** adds S950/S1000-style resampled grit.

## Download & install

Go to [**Releases**](https://github.com/TwistedGenetics/Rollers_Bass/releases/latest) and download the file for your synth.

### NTS-1 mkII: `Rollers.nts1mkiiunit`

1. Open **KORG KONTROL Editor** with the NTS-1 mkII connected (firmware 1.3 or later recommended).
2. Drag the file onto the **oscillator** list, then press **SEND**.
3. Select Rollers with the OSC button.

### microKORG 2: `Rollers.mk2unit`

Needs microKORG 2 firmware **2.0.0 or later** (the version that added logue SDK support).

- **KORG Sound Librarian** (Korg's official app for v2.0): connect the microKORG 2 by USB, add `Rollers.mk2unit` as a user oscillator and send it to the synth.
- **Or USB mass storage:** hold the first option button while powering on, then copy the file into `Units/Oscs/` (into a `SlotN` folder) on the drive that appears.

Rollers then appears at the end of the oscillator list. The microKORG 2 version is polyphonic: every voice has its own pitch drop and its own Reese drift.

The microKORG 2 runs a **lean version of the engine** (`core/rollers_lean4.h`, since v1.1.0), because the full engine run once per voice was too heavy for the synth and crackled. It renders four voices at a time with NEON, works out the slow-moving maths once per audio buffer instead of every sample, and uses cheaper saws (DPW, as in Korg's own `vox`). Silent voices cost nothing. The controls and sound are the same; levels match the NTS-1 version within about 1.5 dB.

**`RLmeter.mk2unit`** (optional, also in each Release) is the same oscillator with its CPU load shown next to MODE, e.g. `S+RS 37%` (peak over the last half second; 100 % = out of time, crackles). Load it alongside Rollers if you hear crackles and want to know how close to the limit a patch is.

## Controls

The parameters are the same on both synths, in the same order.

| NTS-1 mkII | microKORG 2 | Parameter | Range | What it does |
|---|---|---|---|---|
| Knob A | OSC page 1 | BLEND | 0–1023 | **Sub ↔ upper balance.** Both are at full level around the middle (512). In SUB mode it adds "body" (a clean 2nd harmonic); in REESE mode it's a tone control (dark → open) |
| Knob B | OSC page 1 | DETUNE | 0–1023 | Reese detune, from slow phasing (0.5 cent) to full wobble (±45 cents); also sets how far DRIFT wanders |
| Edit 1 | OSC page 1 | SUB SHAPE | SINE, TRI, FOLD | Sub waveform. FOLD is a lightly folded sine (adds a 3rd harmonic), so the sub still reads on laptop speakers and phones |
| Edit 2 | OSC page 2 | PITCH DROP | 0–100 % | How far above the note each note starts, falling back to the note (0 to 2 octaves). Affects the whole oscillator |
| Edit 3 | OSC page 2 | DROP TIME | 0–100 % | How fast it falls: 5 ms (punchy click/thud) to ~0.8 s (long 808 glide) |
| Edit 4 | OSC page 2 | VOICES | 2–4 | Number of detuned saws in the Reese / Hoover stack. Level is compensated |
| Edit 5 | OSC page 2 | DRIFT RATE | 0–100 % | Speed of the slow random wander of each saw's tuning (0.05–4 Hz; 0 = static). Stops the Reese sounding like a fixed chorus. In HOOVER it also speeds up the PWM |
| Edit 6 | OSC page 2 | DRIVE | 0–100 % | tanh saturation on the **upper band only**, level compensated |
| Edit 7 | OSC page 3 | CRUNCH | 0–100 % | Sample-rate reduction (48 kHz → ~4 kHz) + bit reduction (16 → 5 bits) on the **upper band only**. Aliasing is deliberate here. 0 = clean |
| Edit 8 | OSC page 3 | MODE | SUB, REESE, SUB+RS, HOOVER | What's playing (below) |

**NTS-1 mkII:** hold **OSC** and turn the TYPE encoder to choose an Edit parameter, then use Knob B to set the value. The NTS-1's **shape LFO** modulates BLEND, which is good for sub/Reese swells.

**microKORG 2:** the parameters are on OSC pages 1–3. In the **virtual patch**, Rollers adds four modulation destinations: **Blend**, **Detune**, **Drive** and **Crunch**. Modulation is per voice. Try velocity → Drive, or an envelope → Detune for a Reese that widens as the note opens.

### Modes

- **SUB**: just the sub. Knob A adds body (2nd harmonic).
- **REESE**: just the detuned saw stack, full range (no sub, no split). Knob A is a low-pass tone control. Classic Reese on its own, for layering with something else.
- **SUB+RS** (default): clean sub plus the high-passed Reese on top. Knob A balances them. The main jungle/DnB bass.
- **HOOVER**: a detuned stack of PWM-modulated saws with a short pitch dip on attack (starts a fifth low and swoops up over ~70 ms), the "Mentasm" family of sounds. Sub underneath as in SUB+RS.

Changing MODE or VOICES while a note is playing does a quick (one block) fade-out/fade-in instead of a hard switch, so it doesn't click.

## Starter settings

Hit the notes with short gates for rollers, longer for 808-ish glides. Knob values are the NTS-1 readout (0–1023).

| Name | Mode | Blend | Detune | Sub shape | Drop | Drop time | Voices | Drift | Drive | Crunch |
|---|---|---|---|---|---|---|---|---|---|---|
| Rolling sub | SUB | 0 | 0 | SINE | 25 % | 25 % | 2 | 0 % | 0 % | 0 % |
| Classic Reese | SUB+RS | 560 | 450 | SINE | 0 % | 30 % | 3 | 35 % | 30 % | 0 % |
| Dark Reese | REESE | 350 | 650 | SINE | 0 % | 30 % | 4 | 55 % | 60 % | 0 % |
| Hoover stab | HOOVER | 700 | 520 | SINE | 15 % | 15 % | 4 | 40 % | 20 % | 0 % |
| Crunchy 94 | SUB+RS | 600 | 400 | FOLD | 10 % | 20 % | 3 | 30 % | 45 % | 55 % |
| 808 glide | SUB+RS | 150 | 300 | TRI | 70 % | 70 % | 2 | 20 % | 20 % | 0 % |

The desktop tests render a short 170 BPM riff with each of these to `test/out/preset_*.wav`. They're also in each CI run's artifact, so you can hear them before loading anything.

## Test checklist (by ear on the hardware)

Start from the defaults (SUB+RS, Blend 560, Detune 410, 3 voices, Drop 20 %, Drive 25 %).

- [ ] **Tuning**: MODE SUB, SINE, PITCH DROP 0: A4 reads 440 Hz on a tuner.
- [ ] **Pitch drop**: DROP 100 %, DROP TIME low → a thud; DROP TIME high → a long falling glide that lands in tune.
- [ ] **Blend**: Knob A from 0 to 1023 goes sub → sub+Reese → Reese with no volume jumps or clicks.
- [ ] **Sub stays clean**: SUB+RS, Blend ~512, turn DRIVE and CRUNCH to 100 %: the top gets filthy, the sub doesn't get weaker or distorted.
- [ ] **Detune / drift**: DETUNE 0 → slow phasing; DETUNE up → wide wobble; DRIFT 0 = steady, DRIFT up = the movement wanders.
- [ ] **Voices**: 2/3/4 sound progressively thicker at about the same level.
- [ ] **Crunch**: 0 clean; small amounts add grit; 100 % = very lo-fi, aliasing top end.
- [ ] **Hoover**: swoops up into each note; the stack shimmers (PWM).
- [ ] **Switching**: change MODE and VOICES while holding a note: a brief dip, no click.
- [ ] **High notes**: top octave with DRIVE and CRUNCH at 0 sounds clean, no whistling aliases.
- [ ] **NTS-1 shape LFO**: route to shape → BLEND wobbles between sub and Reese.
- [ ] **microKORG 2 virtual patch**: route an LFO or envelope to Blend / Detune / Drive / Crunch and hear each move.
- [ ] **microKORG 2 chords / full polyphony**: HOOVER, 4 voices, DRIVE and CRUNCH up, hold 8 notes: no dropouts or crackles (this is the heaviest case for the CPU). With RLmeter loaded, note the % shown next to MODE.
- [ ] **Everything at max**: loud and nasty, but no dropouts, crackles, or the unit stopping.

## CPU

Rollers uses no tables and allocates no memory. Expensive maths (detune, drift, drive/crunch settings, PWM widths, filter coefficients) runs once per block. The per-sample work is a handful of PolyBLEP saws and polynomial sines, plus one exp2 while a pitch drop is still falling.

- **Desktop** (test harness, worst case: HOOVER, 4 voices, drop + drive + crunch): ~50 ns per sample, about 0.25 % of real time on the build machine.
- **NTS-1 mkII** (Cortex-M7): this is an estimate, not a measurement. At a few hundred cycles per sample, it should sit well under 10 % of the CPU.
- **microKORG 2** (Cortex-A7, up to 8 voices): the lean renderer measures about 275 instructions per sample for a group of four voices in the typical case (SUB+RS, 3 saws, drive on) and about 450 in the worst case (HOOVER, 4 saws, drive and crunch at max). That's desktop SSE2 counts; NEON needs fewer. Groups with no sounding voice are skipped. Korg doesn't publish the per-oscillator budget, so check with RLmeter. If a big chord still crackles, use fewer VOICES, a lighter mode, or fewer voices in the patch.

The NTS-1 unit is ~8.5 KB (limit 48 KB) and links no library code beyond what Korg's examples use.

## Building

GitHub Actions builds both units with Korg's own toolchains and runs the DSP tests on every push. Each run's files are under the **Actions** tab.

Releases are automatic: when `.version` in `nts1mkii/header.c` and `microkorg2/header.c` changes (e.g. `0x00010001U` = 1.0.1) and is pushed to main, Actions tags it and publishes a new Release with both unit files. Pushes that keep the same version refresh that Release's files.

To build locally (no Docker needed):

```sh
./build.sh          # both units into dist/
./build.sh nts1     # NTS-1 mkII only
./build.sh mk2      # microKORG 2 only (Linux: Korg only publishes its toolchain for Linux)
```

This downloads the toolchains and the logue SDK into `.deps/` on first run.

Desktop DSP tests check NaN/clipping/DC across every mode, shape, voice count and note range, plus tuning, pitch-drop landing, aliasing, zipper noise, level consistency, and that drive and crunch don't touch the sub. They also write demo WAVs to `test/out/`:

```sh
./test/run.sh
```

## Layout

```
core/         rollers.h (full DSP: NTS-1 mkII, desktop tests), rollers_lean4.h (4-voice NEON renderer: microKORG 2)
nts1mkii/     NTS-1 mkII unit: header.c (parameters), unit.cc (SDK callbacks), Makefile, config.mk
microkorg2/   microKORG 2 unit: header.c, unit.cc (voice groups, voice events, virtual patch, optional load meter), Makefile, config.mk
tools/mk2-meter/  RLmeter: the microKORG 2 unit built with the CPU load meter
test/         render.cc, run.sh
build.sh
.github/workflows/build.yml   CI build + release
```

## Notes

- Developer ID `0x5477476E` ('TwGn'), unit ID `3` (VectorFM is 1, Formant Mutator is 2). If you register an ID with Korg (pull request to the logue SDK's `developer_ids.md`), set it in both `header.c` files.
- Built against logue SDK commit `e78b16c`.
- BSD 3-Clause licence (see `LICENSE`).
