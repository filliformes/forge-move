# Forge

**8-voice FM / subtractive hybrid drum synthesiser** for [Ableton Move](https://www.ableton.com/move/),
built on the [Schwung](https://github.com/charlesvestal/schwung) framework.

> A *supercharged LXR-02* — specialised voice algorithms, a self-oscillating
> analog ladder filter, AD envelopes with curve + repeat, cross-voice LFO
> routing, and enveloped FM. Adds a Razzmatazz-style per-voice resonator, a
> dedicated multi-colour noise oscillator, a Digitone-II dual-filter chain with
> Base-Width pre-stage, Machinedrum-style Ctrl-All performance macros + gated
> reverb, three concurrent FX buses, and the Forge signature: **Kit A↔B morph**
> across Move's 16-pad Drum Kit template.

---

## What's new in v0.2.0 — big update

Everything below has landed since the v0.1 series:

**Synthesis**
- **Dedicated Noise layer** — a true second oscillator per voice: **5 noise
  colours** (White / Pink / Brown / Gaussian / Blue) → a **Base + Width
  band-pass** (narrow tuned ↔ broadband, Digitone-style) → its own AD envelope.
  The real noise source for snares, claps and hat sizzle.
- **ZDF transistor ladder filter** (Moog 4-pole `Ladder` + `Ladder HP`) — a
  self-oscillating analog filter whose resonant "ping" becomes the body/pitch
  of a tom or kick. **The best filter for percussion**, now baked in.
- **LP2 acid filter** — a self-oscillating 2-pole LP (LXR-style) for 303/acid
  perc.
- **Enveloped FM indices** — two dedicated FM-index envelopes per voice so the
  FM amount blooms and collapses per hit (Digitone's soul).
- **LFO dual-shape morph** — blend two LFO waveforms with a morph knob.

**Performance**
- **Ctrl-All macros** (new **Perf** page) — 8 global "nudge every voice at
  once" knobs: Punch / Bright / Decay / Drive / Snap / Bend / Tune / FX. One
  encoder morphs the whole kit live. Each one works on **every** kit — even a
  bone-dry, clickless, or maxed-filter voice (Punch/Bright are a per-voice
  spectral tilt, Snap injects a universal attack tick, FX adds send + wet floor).
  Tuned for fast, snappy sweeps (~1 turn end-to-end).
- **20 ms parameter smoothing** — the Ctrl-All macros, master volume/drive, FX
  wet mix and voice levels ramp instead of jumping, so turning a knob live never
  zippers or clicks.
- **Aftertouch** (poly + channel) as a mod source, plus a **Roll** polyphony
  mode — hold a pad and press harder to speed up a pressure-controlled buzz.
- **Gated reverb** (Machinedrum GATE BOX) — the iconic cut-tail snare verb.
- **50 Voice presets** (10 per algorithm) selectable from the top of the Voice
  menu — swap a voice's whole instrument in one move.
- **Rnd Kit Params** — reroll the current kit's 8 voices *and* its FX buses.

**Kits & content**
- **137 factory kits** (was 64): **64 Razzmatazz ports** (0–63), **64 Forge
  originals** (64–127), **9 LXR TR-808/909/707 ports** (128–136) decoded
  clean-room from the GPL Sonic Potions `.SND` format.
- **Kit A↔B morph is now audible on all 137 kits** (auto-synthesised B
  variation where none was authored).
- **All Decay** range extended to **1 %–400 %** (was 100 %–400 %).
- Normalised exponential **cutoff knob** — musical sweep across the whole range.

**Fixes** — fatter snares (body routed post-filter), reverb dialled to a
sensible level, audible attack **click** (post-filter), every silent/weak voice
eliminated (1096/1096 audible), a proper loudness-boosting **drive**, a
correctly-clamped `fast_tanh` saturator, a now-functional **E1 Rep Rate** knob,
and denormal guards throughout.

---

## Table of contents

- [Features](#features)
- [Pad layout & Kit A↔B morph](#pad-layout--kit-ab-morph)
- [Voice algorithms](#voice-algorithms)
- [Per-voice signal flow](#per-voice-signal-flow)
- [Page hierarchy](#page-hierarchy)
- [Knob assignments per page](#knob-assignments-per-page)
- [Voice presets](#voice-presets)
- [The 137 factory kits](#the-137-factory-kits)
- [Aftertouch & performance](#aftertouch--performance)
- [Save Kit](#save-kit)
- [Installation](#installation)
- [Building from source](#building-from-source)
- [Inspirations & credits](#inspirations--credits)
- [Roadmap](#roadmap)
- [License](#license)

---

## Features

- **8 voices, 5 algorithms** — Drum / Snare / Cymbal / Hat / Wild
  - **Drum**: 2-op FM (carrier + sine modulator with feedback) + body wavefolder + parallel body-sine (kick punch) + click + resonator + dual filter
  - **Snare**: tonal osc + tunable noise mix + 2-op FM body (body summed post-filter for weight)
  - **Cymbal**: 3-op serial FM cascade (A → B → C, feedback at A), enharmonic ratios
  - **Hat**: same 3-op cascade with paired open/closed decays and mutual choke
  - **Wild**: free-form 2-op FM with cross-FM option, wavefolder, broadest mod range
- **Dedicated Noise layer** per voice — 5 colours (White/Pink/Brown/Gaussian/Blue) → Base+Width band-pass → own AD envelope, summed post-filter so it always survives.
- **Enveloped FM** — two dedicated FM-index AD envelopes per voice for per-hit FM bloom.
- **Kit A ↔ Kit B morph** — every parameter of every voice interpolated by one knob (linear / exp / log / S-curve). Both kits playable simultaneously across 16 pads. Audible on all 137 kits.
- **137 factory kits** — 64 Razzmatazz ports, 64 Forge originals, 9 LXR TR-808/909/707 ports.
- **Per-voice resonator** — feedback-comb + tanh saturation (Razzmatazz metallic ring on any voice without burning FM index).
- **Dual filter chain** with Base-Width pre-stage and four routings (Single / Per-Osc / Serial / Parallel); **11 filter types** per slot — LP / HP / BP / BPu / Notch / Peak / Comb+ / Comb− / LP2 (acid) / **Ladder** / **Ladder HP** (self-oscillating Moog 4-pole).
- **Curved AD envelopes with repeat** (LXR signature) — exp/lin/log shape + a repeat that retriggers the attack for synthesised claps and buzz rolls, with a live **Rep Rate** control.
- **Cross-voice LFO routing** (LXR signature) + **dual-shape morph** — any voice's LFO can drive any voice's parameter; any pad-hit can phase-reset any LFO.
- **Ctrl-All performance macros** (Machinedrum) — 8 global nudge knobs on the Perf page, each effective on every kit, with 20 ms smoothing for click-free live sweeps.
- **Aftertouch** + **Roll** polyphony — pressure as a mod source and a pressure-controlled retrigger buzz.
- **3 concurrent FX buses** — Delay (BPF feedback, ping-pong, tanh-saturated), Reverb (Dattorro plate + **gated** mode), Chorus (panoramic multi-engine).
- **Bus compressor** (EMT-156 style) + **3-band EQ** + master saturation (tube/fold/clip) + bit/rate reduction + soft-clip limiter.
- **Click sample bank** — six synthesised transients, no asset loading.
- **50 Voice presets** — 10 named instruments per algorithm.
- **Save Kit** — binary persistence with magic + version check.

---

## Pad layout & Kit A↔B morph

Forge displays across Move's 16-pad Drum Kit template as two interlocked rows:

```
┌────┬────┬────┬────┐
│ 13 │ 14 │ 15 │ 16 │  ← Kit B reference  (Voices 5-8, untouched)
├────┼────┼────┼────┤
│  9 │ 10 │ 11 │ 12 │  ← Kit B reference  (Voices 1-4, untouched)
├────┼────┼────┼────┤
│  5 │  6 │  7 │  8 │  ← Morphed Kit A↔B (Voices 5-8, live)
│  1 │  2 │  3 │  4 │  ← Morphed Kit A↔B (Voices 1-4, live)
└────┴────┴────┴────┘
```

- **At Morph = 0** the lower row plays Kit A and the upper row plays Kit B — both kits simultaneously available as a 16-pad performance kit.
- **As Morph rises**, the lower row gradually transforms into Kit B; the upper row stays put.
- **At Morph = 1**, both rows play Kit B identically (cue to load a new variation or reset Morph).
- Tap any pad to select that voice for editing. The dynamic Voice menu shows a `V<N>` prefix so you always know which voice you're sculpting.
- Choke groups span both rows — a pad-1 hit and a pad-9 hit will choke each other if their voices share a group.

---

## Voice algorithms

Forge's five algorithms are structurally different DSP topologies, not preset
variations. Each voice can run any algorithm; the default assignment is
V1-V4 = Drum, V5 = Snare, V6 = Cymbal, V7-V8 = Hat (mutually choked). Every
voice — whatever its algorithm — also carries the dedicated Noise layer and two
FM-index envelopes.

| # | Algorithm | Topology |
|---|-----------|----------|
| 0 | **Drum**   | 2-op FM (carrier with sine/tri/saw/sqr+PWM/noise pick; sine modulator + feedback) → body wavefolder → click → resonator → dual filter, with a parallel body-sine summed post-filter for kick weight |
| 1 | **Snare**  | Tonal osc + tunable noise mix + 2-op FM body; body summed post-filter for weight |
| 2 | **Cymbal** | 3-op serial FM: op A → op B → op C, self-feedback at A. Enharmonic ratios first-class. |
| 3 | **Hat**    | Same 3-op cascade with paired voices (V7 default closed, V8 default open) + mutual choke |
| 4 | **Wild**   | Free-form 2-op FM + optional cross-FM, broadest macro routing |

---

## Per-voice signal flow

```
Note-on ─► [Pitch env] ──┐
        ─► [FM idx env ×2]├─► FM index / pitch bloom
                          ┘
[Osc / FM (algo-dependent)] ─► [Body wavefolder] ─► [Resonator (FB-comb + tanh)]
       ─► [Filter routing: Single | Per-Osc | Serial | Parallel]
              ├─ [Base-Width pre-filter] (optional)
              ├─ [Filt1: SVF (LP/HP/BP/BPu/Notch/Peak/LP2) · Comb± · Ladder]
              └─ [Filt2: same options]
       ─► + [Body sine]    ← POST-filter (low fundamental always survives)
       ─► + [Click]        ← POST-filter, boosted (attack snap punches through)
       ─► + [Noise layer]  ← POST-filter (colour → Base/Width BP → own AD)
       ─► [Drive (Tube / Fold / Clip)]
       ─► [Bit crush] ─► [Rate crush]
       ─► [Amp (Env1)] ─► [Pan]
       ─► main bus + FX1 / FX2 sends
```

The **body, click and noise are summed after the filter** — so a heavy LP/BP
can shape the FM colour without ever strangling the kick's fundamental, the
attack transient, or the noise crack. This is what keeps every voice audible on
every kit.

Modulation sources: **Env1** (amp), **Env2** (assignable: FM/Filter/Reso/Pan/Mod),
**Pitch Env**, **2 × FM-index envelopes**, **LFO** (cross-voice, dual-shape
morph, audio-rate), **Velocity**, **Aftertouch**, **Mod Wheel**, and **Trigger**
(any pad-hit can phase-reset any LFO).

---

## Page hierarchy

```
root ── Patch        (8 root knobs, always live)
   │
   ├── Perf     (Ctrl-All performance macros — menu #2)
   ├── Voice ──┬── (8 macro knobs, algorithm-dependent labels)
   │          ├── (Preset selector — 50 instruments)
   │          ├── Osc      (oscillator + FM + Noise layer)
   │          ├── Filter   (dual filter + Base-Width + ladder)
   │          ├── Env      (Env1 / Env2 / Pitch Env / FM-idx envs)
   │          ├── Mod      (LFO + dual-shape morph + cross-voice)
   │          └── Setup    (algorithm, choke, polyphony, aftertouch)
   │
   ├── Mix      (8 voice volumes, pans & sends in menu)
   ├── FX       (3 FX buses + gated reverb)
   └── General  (master comp / drive / EQ / crush / volume)
```

---

## Knob assignments per page

### Page 0 — Patch (root)

| K1 | K2 | K3 | K4 | K5 | K6 | K7 | K8 |
|----|----|----|----|----|----|----|----|
| Kit | Rnd Kit | **Rnd Kit Params** | Rnd Voice | **Morph** | **All Decay** | Rnd Pan | Save |

- **Rnd Kit** rerolls the 8 voices (keeps FX); **Rnd Kit Params** rerolls the voices *and* the three FX buses.
- **Menu-only**: Rnd Pitch, All Mono, Init Decay, Init Freq, Same Freq, Copy A→B, Copy B→A, Swap A↔B, Rnd B from A, Morph Source, Morph Curve.

### Perf — Ctrl-All performance macros (menu #2)

| K1 | K2 | K3 | K4 | K5 | K6 | K7 | K8 |
|----|----|----|----|----|----|----|----|
| Punch | Bright | Decay | Drive | Snap | Bend | Tune | FX |

Each knob nudges the corresponding parameter across **all 8 voices at once** (0.5 = neutral). One encoder sweeps the whole kit live — snappy (~1 turn end-to-end) and 20 ms-smoothed so it never zippers. Every macro is effective on any kit, including dry/clickless/maxed-filter voices: Punch (low band) + Bright (high band) are a per-voice spectral tilt, Snap injects a universal attack tick, and FX adds a send + reverb/delay/chorus wet floor.

### Voice (drill-in) — algorithm-aware macro page

| Knob | Drum | Snare | Cymbal | Hat | Wild |
|------|------|-------|--------|-----|------|
| 1 | Pitch | Tune | Tune | Tune | M1 |
| 2 | Decay | Snap | Decay | Cls Dec | M2 |
| 3 | Bend | Body | FMIdx | Opn Dec | M3 |
| 4 | FM | Noise | Color | FM | M4 |
| 5 | Body | Tone | Shape | Color | M5 |
| 6 | Click | Decay | Reso | Shape | M6 |
| 7 | Cutoff | Cutoff | Cutoff | Cutoff | M7 |
| 8 | Drive | Drive | Drive | Drive | M8 |

First menu item is the **Preset** selector (50 named instruments — see below).

### Voice → Osc

| K1 | K2 | K3 | K4 | K5 | K6 | K7 | K8 |
|----|----|----|----|----|----|----|----|
| Wave | Coarse Ratio | Fine Ratio | Detune | Level | Phase | PWM | Feedback |

Menu-only: Op A/B/C, Click Type (Sample/Impulse/Phase), Click Sample, Click Lvl, Click Decay, Cross-FM, **Noise Lvl, Noise Dec, Noise Base, Noise Width, Noise Color** (White/Pink/Brown/Gaussian/Blue).

### Voice → Filter

| K1 | K2 | K3 | K4 | K5 | K6 | K7 | K8 |
|----|----|----|----|----|----|----|----|
| F1 Cutoff | F1 Reso | F1 Type | BW Cutoff | BW Width | Routing | F1 Drive | F2 Cutoff |

F1 / F2 Type: **LP / HP / BP / BPu / Notch / Peak / Comb+ / Comb− / LP2 / Ladder / Ladder HP**.
Routing: **Single / Per-Osc / Serial / Parallel**.
Cutoff is a normalised exponential sweep (20 Hz–20 kHz). For a self-oscillating tom/kick ping, pick **Ladder** and turn up **F1 Reso**.
Menu-only: F2 Reso, F2 Type, F2 Drive, BW On, Bit Crush, Rate Crush, KeyTrack F1/F2, Env1→F1, Env2→F2.

### Voice → Env

| K1 | K2 | K3 | K4 | K5 | K6 | K7 | K8 |
|----|----|----|----|----|----|----|----|
| E1 Atk | E1 Dec | E1 Curve | **E1 Repeat** | E2 Dec | E2 Curve | PE Amt | PE Dec |

Menu-only: E2 Dest, **E1 Rep Rate** (buzz speed), E2 Atk, PE Curve, PE Dest, Vel→E1 Lvl, Vel→E1 Time, Vel→E2 Amt, **FM Env1 Amt/Dec, FM Env2 Amt/Dec** (enveloped FM indices).

### Voice → Mod

| K1 | K2 | K3 | K4 | K5 | K6 | K7 | K8 |
|----|----|----|----|----|----|----|----|
| LFO Wave | LFO Rate | LFO Sync | LFO Depth | XLFO Source | Trig Reset | Mod Dest | Mod Depth |

Menu-only: LFO Phase, Polarity, Restart, Mod Source (incl. AT / Mod-Wheel), Mod Curve, **LFO Wave 2, LFO Morph** (dual-shape blend).

### Voice → Setup

| K1 | K2 | K3 | K4 | K5 | K6 | K7 | K8 |
|----|----|----|----|----|----|----|----|
| Algorithm | Choke | Output Bus | Voice Lvl | Voice Tune | Polyphony | Glide | Init Voice |

Polyphony: **One-Shot / Legato / Retrig / Roll** (Roll = aftertouch buzz).
Menu-only: Voice Pan, Voice Mute, FX1 Send, FX2 Send, Velocity Sens, MIDI Note.

### Mix

K1–K8 = V1–V8 Level. Menu: per-voice pan, FX1 sends, FX2 sends.

### FX — 3 concurrent buses

| K1 | K2 | K3 | K4 | K5 | K6 | K7 | K8 |
|----|----|----|----|----|----|----|----|
| Rev Mix | Rev Decay | Rev Size | Dly Mix | Dly Rate | Dly Fdbk | Dly Tone | Cho Mix |

Menu-only: Rev Type, **Rev Gate** (gated reverb), Rev Predelay, Rev Damping, Dly BPF Cutoff/Width, Ping-Pong, Sync, Cho Rate/Depth/Width/Voices/Tone/Feedback.

### General

| K1 | K2 | K3 | K4 | K5 | K6 | K7 | K8 |
|----|----|----|----|----|----|----|----|
| Comp | Drive | Bit | Rate | EQ Lo | EQ Mid | EQ Hi | Master |

Menu-only: Drive Type (Tube/Fold/Clip), Lo/Mid/Hi Freq, Q Lo/Mid/Hi, Limiter, Master Tune, MIDI Channel.

---

## Voice presets

Fifty full single-voice instruments, 10 per algorithm, selectable from the top
of the Voice menu. Loading one swaps the current voice's whole character while
preserving its mix/routing (pan, level, sends, choke, bus).

- **Drum** — 808 Kick · 909 Kick · Sub Boom · Click Kick · Punch Kick · Dist Kick · Deep Tom · Hi Tom · Zap · Rumble
- **Snare** — 909 Snare · Clap · Rimshot · Noise Snr · FM Snare · Fat Snare · Tight Snr · Brush · Sidestick · Crack
- **Cymbal** — Ride · Crash · Splash · Bell · China · Gong · Ping · Metal Hit · Cluster · Trash
- **Hat** — Closed Hat · Open Hat · Pedal Hat · Tight Hat · Sizzle · Long Hat · Metal Hat · Roll Hat · Shaker · Acid Hat
- **Wild** — Laser Zap · FM Bass · Noise Burst · Metallic · Glitch · Pluck · Drone · Bleep · Acid Blip · Chaos

---

## The 137 factory kits

Three banks. Each kit defines all 8 voices, per-kit FX state (reverb / delay /
chorus) and a Kit B variation, so the Morph knob always produces a gradient.

### Slots 0–63 · Razzmatazz ports

Ported from 1010music Razzmatazz `.nnr` presets — the source of Forge's
per-voice resonator character. Punchy, resonant, electro/IDM-leaning kits:

```
AeroVan · BoomClap · Cannon · DatKick · DNB · ElectroBoom · ElectroPunk ·
HeavyIDM · Krafty · Smolder · SpeakerDama · Transformer · Velocity ·
SergeantKic · BassDrive · RoundHouse · BrokenBot · Thump · DeepTech · Tighty ·
SiM · NewBeat · BitSmash · Hacked · Drip · Flanged · DubTech · GlitchMetal ·
HeadHunter · AmbientPerc · Bounce · BinaryCrush · RadioBuzz · Distorted ·
BoombapSlap · Citadel · BassKick · Classic · TheHorror · EBM · Industry · Acid ·
Deutschland · Chippy · Pulsar · TwoFourTwo · Florida · CYBERDRUNK · Moombahton ·
DubBubbles · SynthWave · DDoS · TechnoPure · Pound · Ritual · BitMetal · Miami ·
Hardcore · Slammer · TheCave · SpaceKit · Marge · SawzAll · StunGun
```

### Slots 64–127 · Forge originals

Original kits spanning the lineage and a spread of genres. (These are Forge's
own interpretations named after the machines/genres — not sample replays.)

| # | Name | Character |
|---|------|-----------|
| 64–73 | Plastic · Anvil · Forge · Cinder · Spark · Dust · Phasma · Static · Glass · Marteau | Forge signatures — resonator, industrial, drone, bell, hammer |
| 74–83 | 808 · 909 · 707 · CR-78 · Linn · DMX · SP-12 · Simmons · Boom · Trap | Classic drum-machine homages |
| 84–93 | LXR · Riga · Schmidt · Indus · Lattice · Caustic · Steam · Iron · Burn · Cathedral | LXR-02 / Sonic Potions / Erica industrial |
| 94–103 | Razz · Bell · Chime · Pluck · Wire · Vinyl · Snap · Bounce · Toy · Crystal | Razzmatazz / per-voice-resonator showcase |
| 104–113 | Digi · Plaits · Cycles · Volca · Wavefold · BaseWidth · Comb · ThreeOp · Index · DX | Digitone II / Plaits / Volca FM |
| 114–123 | Techno · House · Detroit · Acid · Footwork · Jungle · Garage · Dubstep · Bossa · Glitch | Genre kits |
| 124–127 | Frost · Magma · Smoke · Chaos | Experimental |

### Slots 128–136 · LXR TR-808/909/707 ports

Björn Fogelberg's freely-shareable 2017 TR patch set for the Sonic Potions LXR,
decoded **clean-room** from the GPL `.SND` binary format (see
`dsp_refs/lxr_snd_format.md`). The LXR patches lean on loaded WAV samples, and
Forge has no sampler — so these are a **synthesised reinterpretation**: each
patch's pitch, filter, envelope, FM and mix settings drive Forge's own engine.
808/909/707-*flavoured* Forge kits, not sample clones.

```
808_BF · 808_2_BF · 808_3_BF · 808_4_BF · 808_9_BF · 7_8_9_BF · 909_BF ·
707_BF · Tech_BF
```

---

## Aftertouch & performance

Move's pads send channel + polyphonic aftertouch, which Forge turns into
expression:

- **As a mod source** — set *Mod Source = AT* and route to any destination
  (pitch, FM index, filter cutoff, resonator, body, pan, level) via Mod Dest +
  Depth.
- **Roll polyphony** — set a voice to **Poly = Roll**. Holding a pad retriggers
  the voice at a rate that speeds up the harder you press (≈3 Hz → 30 Hz),
  turning any voice into a pressure-controlled buzz/roll. Great on hats,
  snares, and FX (see the Roll Hat / Shaker presets).
- **Ctrl-All** — the Perf page's 8 macros nudge the whole kit at once for live
  builds and drops. Each one bites on every kit (Punch/Bright = a per-voice
  spectral tilt, Snap = a universal attack tick, FX = an added send + wet floor),
  the encoders are fast (~1 turn), and all Ctrl-All/master/FX/level moves are
  20 ms-smoothed so live sweeps never click.

---

## Save Kit

Forge persists all kits to `/data/UserData/schwung/forge_kits.dat` (binary,
magic `'FRGE'`, save version 6). The Save Kit knob is a self-resetting enum:
turn it to *Save* and the full edit state (137 slots × Kit A + Kit B + macros +
per-kit FX) is written; the knob auto-reverts to *Play*.

User kits override the factory in-place. To reset to factory defaults, delete
`forge_kits.dat` and reload the module. A version + slot-count check rejects
stale saves automatically, so factory content loads cleanly after any update.

---

## Installation

### Via the Schwung Manager

1. Open Schwung Manager → Modules.
2. Find **Forge** under Sound Generators.
3. Install. Power-cycle the Move.

### Via the release tarball

1. Download the latest `forge-module.tar.gz` from
   [Releases](https://github.com/filliformes/forge-move/releases).
2. Extract on your Move:
   ```bash
   scp forge-module.tar.gz ableton@move.local:/tmp/
   ssh ableton@move.local
   cd /data/UserData/schwung/modules/sound_generators/
   tar -xzf /tmp/forge-module.tar.gz
   chown -R ableton:users forge
   chmod +x forge/dsp.so
   ```
3. **Power-cycle the Move.** (Removing + re-adding the module from an FX slot
   does *not* reload `module.json` — only a full boot does; the chain host
   caches the manifest at startup.)

---

## Building from source

Requirements: Docker (for the ARM64 cross-compile) + SSH access to a Move.

```bash
git clone https://github.com/filliformes/forge-move
cd forge-move
./scripts/build.sh                            # compile dsp.so via Docker
./scripts/install.sh                          # scp to ableton@move.local
# Then power-cycle the Move.
```

Set `MOVE_HOST=user@host` to install to a different Move:

```bash
MOVE_HOST=ableton@another-move.local ./scripts/install.sh
```

The build cross-compiles with `-O2 -ffast-math` for the Move's ARM Cortex-A53,
and is verified post-compile for the `move_plugin_init_v2` symbol.

---

## Inspirations & credits

Forge is an original design and DSP implementation by **Vincent Fillion**.
Architectural and timbral ideas drawn from:

- **Erica Synths LXR-02** — voice-algorithm philosophy, drivable filter, cross-voice LFO routing, kit morph
- **Sonic Potions LXR** (GPL-2.0, Julian Schmidt) — curved AD-with-repeat envelope concept; its `.SND` file format was reverse-engineered clean-room to port Björn Fogelberg's freely-shareable TR-808/909/707 patch set (kits 128–136)
- **1010music nanobox Razzmatazz** — per-voice resonator; 64 factory kits ported from its `.nnr` presets (kits 0–63)
- **Elektron Digitone II** — enveloped FM indices, body wavefolder, dual-filter chain, Base-Width pre-stage, comb modes
- **Elektron Machinedrum** — Ctrl-All performance macros, gated reverb, LFO dual-shape morph
- **Mutable Instruments Plaits & Peaks** (Émilie Gillet, MIT) — 2-op FM operator pattern
- **DaisySP** (Electrosmith, MIT) — wavefolder + chorus topologies
- **KrautDrums** (Vincent Fillion, MIT — same author) — Dattorro reverb, EMT-156 compressor, multi-mode delay
- **VA filter math** — A. Simper (Cytomic) trapezoidal SVF + V. Zavalishin ZDF transistor ladder (public math) for the analog ladder filter

No code is copied from any proprietary source. Open-source MIT code (Plaits,
Peaks, DaisySP, KrautDrums) is ported with attribution preserved in the source.
The LXR envelope concept and `.SND` file-format decode are clean-room; no LXR
firmware DSP is present.

Framework: [Schwung](https://github.com/charlesvestal/schwung) by **Charles Vestal**.

---

## Roadmap

Shipped in v0.2.0: enveloped FM indices, ladder filter, LP2 acid, dedicated
noise layer, Ctrl-All macros, gated reverb, LFO dual-shape morph, aftertouch +
Roll, 50 voice presets, 137 kits.

Still ahead:

- **Two parallel FM stacks per voice** (currently single stack on Drum/Snare/Wild)
- **3-slot modulation matrix** per parameter (currently 1 slot + fixed routings)
- **User-rebindable macro mappings** per voice (currently fixed by algorithm)
- **Per-osc filter routing** properly split (currently falls back to Parallel)
- **4-op FM with selectable algorithms** for Drum/Wild (engine 2.0)
- **Wavetone machine** — phase-distortion + sync + ring-mod (a new algorithm)

---

## License

MIT — see [LICENSE](LICENSE).
