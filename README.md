# Forge

**8-voice FM / subtractive hybrid drum synthesiser** for [Ableton Move](https://www.ableton.com/move/),
built on the [Schwung](https://github.com/charlesvestal/schwung) framework.

> A *supercharged LXR-02* — specialised voice algorithms, drivable filter
> with peak mode, AD envelopes with curve + repeat, cross-voice LFO routing.
> Adds Razzmatazz-style per-voice resonator, Digitone-II-inspired body
> wavefolder + dual-filter chain with Base-Width pre-stage, three concurrent
> FX buses, and the Forge signature: **Kit A↔B morph** across Move's 16-pad
> Drum Kit template.

---

## Table of contents

- [Features](#features)
- [Pad layout & Kit A↔B morph](#pad-layout--kit-ab-morph)
- [Voice algorithms](#voice-algorithms)
- [Per-voice signal flow](#per-voice-signal-flow)
- [Page hierarchy](#page-hierarchy)
- [Knob assignments per page](#knob-assignments-per-page)
- [The 64 factory kits](#the-64-factory-kits)
- [Save Kit](#save-kit)
- [Installation](#installation)
- [Building from source](#building-from-source)
- [Inspirations & credits](#inspirations--credits)
- [Roadmap](#roadmap-v02)
- [License](#license)

---

## Features

- **8 voices, 5 algorithms** — Drum / Snare / Cymbal / Hat / Wild
  - **Drum**: 2-op FM (carrier + sine modulator with feedback) + body wavefolder + per-voice resonator + dual filter
  - **Snare**: 2-op FM body + LP-filtered noise mix + click stage
  - **Cymbal**: 3-op serial FM cascade (A → B → C with feedback at A), enharmonic ratios
  - **Hat**: same 3-op cascade with paired open/closed decays and mutual choke
  - **Wild**: free-form 2-op FM with cross-FM option, wavefolder, broadest mod range
- **Kit A ↔ Kit B morph** — every parameter of every voice interpolated by a single knob, with linear / exp / log / S-curve morph shapes. Both kits playable simultaneously across 16 pads — lower row plays the morphed live state, upper row plays the untouched Kit B reference.
- **64 factory kits** spanning the inspirations: LXR-02, Razzmatazz, Digitone II, TR-808/909/707/CR-78, LinnDrum, DMX, SP-12, Simmons, Plaits, Volca Drum, Acid, Jungle, Dubstep, Cathedral reverbs and more.
- **Per-voice resonator** with feedback comb topology and tanh saturation (FB up to 1.6× — Razzmatazz-style metallic ring on any voice without burning FM index).
- **Dual filter chain** with Base-Width pre-stage and four routing modes (Single / Per-Osc / Serial / Parallel); 8 filter types per slot — LP / HP / BP / BPu / Notch / Peak / Comb+ / Comb−.
- **Curved AD envelopes with repeat** (LXR signature) — exp / lin / log shape via a single curve knob, plus a repeat parameter that retriggers the attack for synthesised claps and buzz rolls.
- **Cross-voice LFO routing** (LXR signature) — any voice's LFO can drive any voice's parameter, and any voice's pad-hit can phase-reset any LFO.
- **3 concurrent FX buses**:
  - **Delay** with BPF in the feedback path, ping-pong, tanh-saturated FB
  - **Reverb** — Dattorro figure-8 plate
  - **Chorus** — DaisySP-inspired panoramic multi-engine
- **Bus compressor** — EMT-156 / Neumann broadcast-limiter style, sub-millisecond attack for drums.
- **3-band EQ + master saturation** (tube / fold / clip) + bit / rate reduction + soft-clip limiter.
- **Click sample bank** — six procedurally synthesised transients (kick / rim / hat / clap / tom / snap), no asset loading.
- **Phasma-style Save Kit** — binary persistence to `/data/UserData/schwung/forge_kits.dat` with magic + version check.
- **All Decay** — KrautDrums-pattern snapshot-relative multiplier (1× to 4×) preserving per-voice character.

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
- Tap any pad to select that voice for editing. Lower row edits Kit A's voice; upper row edits Kit B's voice. The dynamic Voice menu (knob popup shows `V<N>` prefix on voice-related pages) confirms which voice you've selected.
- Choke groups span both rows. A pad-1 hit and a pad-9 hit will choke each other if their voices share a choke group.

---

## Voice algorithms

Forge's five algorithms are not preset variations — they are structurally
different DSP topologies. Each voice can run any algorithm; the default
assignment is V1-V4 = Drum, V5 = Snare, V6 = Cymbal, V7-V8 = Hat (mutually
choked).

| # | Algorithm | Topology |
|---|-----------|----------|
| 0 | **Drum**   | 2-op FM (carrier with multi-waveform pick: sine/tri/saw/sqr+PWM/noise; modulator is sine with feedback) → body wavefolder → click stage → resonator → dual filter chain |
| 1 | **Snare**  | Tonal osc + LP-filtered noise mix + 2-op FM body for harmonic content |
| 2 | **Cymbal** | 3-op serial FM: op A modulates op B modulates op C, with self-feedback at op A. Enharmonic ratios first-class. |
| 3 | **Hat**    | Same 3-op cascade as Cymbal but with paired voices (V7 default closed, V8 default open) and mutual choke |
| 4 | **Wild**   | Free-form: 2-op FM + optional cross-FM (op B's output also phase-modulates op A's carrier), broadest macro routing |

---

## Per-voice signal flow

```
Note-on  ──► [Pitch env]      ──┐
                                 ├──► pitch destinations
       ──► [FM index env]    ──┘
                                                      
[Osc / FM (algo-dependent)] ──► [Body Wavefolder] ──► [Click stage] 
       ──► [Resonator (FB-comb + tanh, up to ~160% FB)]
       ──► [Filter routing: Single | Per-Osc | Serial | Parallel]
              ├─► [Base-Width pre-filter] (optional)
              ├─► [Filt1: SVF (LP/HP/BP/BPu/Notch/Peak) or Comb±]
              └─► [Filt2: same options]
       ──► [Drive (Tube / Fold / Clip)]
       ──► [Bit crush] ──► [Rate crush]
       ──► [Amp (Env1)] ──► [Pan]
       ──► main bus + FX1 / FX2 sends
```

Modulation sources:

- **Env1** (amp, hard-wired) — curved AD with repeat
- **Env2** (assignable) — curved AD with repeat, destinations: FM / Filter / Reso / Pan / Mod
- **Pitch Env** (separate) — destinations: Pitch / FM / Filter / Reso
- **LFO** (per voice, cross-voice routable) — 6 waveforms, audio-rate capable
- **Velocity**, **Aftertouch**, **Mod Wheel**
- **Trigger** (any voice's pad-hit can phase-reset any LFO)

v0.1 has **1 mod slot per voice** (source × depth → destination) plus the always-on pitch-env and Env2 destination routing. v0.2 will expand to 3 mod slots and dedicated FM-index envelopes.

---

## Page hierarchy

```
root ── Patch        (8 root knobs, always live)
   │
   ├── Voice ──┬── (8 macro knobs, algorithm-dependent labels)
   │          ├── Osc      (oscillator + FM detail)
   │          ├── Filter   (dual filter + Base-Width)
   │          ├── Env      (Env1 / Env2 / Pitch Env)
   │          ├── Mod      (LFO + cross-voice + mod slot)
   │          └── Setup    (algorithm, choke, polyphony)
   │
   ├── Mix      (8 voice volumes, pans & sends in menu)
   ├── FX       (3 FX buses)
   └── General  (master comp / drive / EQ / crush / volume)
```

The Voice menu and its 5 sub-pages are dynamic — they always edit whichever
voice you most recently triggered via a pad. The knob popup label shows
`V<N> <Param>` so you always know which voice you're sculpting.

---

## Knob assignments per page

### Page 0 — Patch (root)

| K1 | K2 | K3 | K4 | K5 | K6 | K7 | K8 |
|----|----|----|----|----|----|----|----|
| Kit | Save | Rnd Kit | Rnd Voice | Rnd Pitch | **Morph** | **All Decay** | Rnd Pan |

**Menu-only**: All Mono, Init Decay, Init Freq, Same Freq, Copy A→B, Copy B→A, Swap A↔B, Rnd B from A, Morph Source, Morph Curve.

### Voice (drill-in from root) — algorithm-aware macro page

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

### Voice → Osc

| K1 | K2 | K3 | K4 | K5 | K6 | K7 | K8 |
|----|----|----|----|----|----|----|----|
| Wave | Coarse Ratio | Fine Ratio | Detune | Level | Phase | PWM | Feedback |

Menu-only: Op A/B/C selector, Click Type (Sample / Impulse / Phase), Click Sample (Kick / Rim / Hat / Clap / Tom / Snap / None), Click Lvl, Click Decay, Cross-FM.

### Voice → Filter

| K1 | K2 | K3 | K4 | K5 | K6 | K7 | K8 |
|----|----|----|----|----|----|----|----|
| F1 Cutoff | F1 Reso | F1 Type | BW Cutoff | BW Width | Routing | F1 Drive | F2 Cutoff |

F1 / F2 Type: **LP / HP / BP / BPu / Notch / Peak / Comb+ / Comb−**.
Routing: **Single / Per-Osc / Serial / Parallel**.
Menu-only: F2 Reso, F2 Type, F2 Drive, BW On, Bit Crush, Rate Crush, KeyTrack F1, KeyTrack F2, Env1→F1, Env2→F2.

### Voice → Env

| K1 | K2 | K3 | K4 | K5 | K6 | K7 | K8 |
|----|----|----|----|----|----|----|----|
| E1 Atk | E1 Dec | E1 Curve | **E1 Repeat** | E2 Dec | E2 Curve | PE Amt | PE Dec |

Menu-only: E2 Destination, Repeat Rate, E2 Atk, PE Curve, PE Destination, Vel→E1 Lvl, Vel→E1 Time, Vel→E2 Amt.

### Voice → Mod

| K1 | K2 | K3 | K4 | K5 | K6 | K7 | K8 |
|----|----|----|----|----|----|----|----|
| LFO Wave | LFO Rate | LFO Sync | LFO Depth | XLFO Source | Trig Reset | Mod Dest | Mod Depth |

Menu-only: LFO Phase, Polarity, Restart, Mod Source, Mod Curve.

### Voice → Setup

| K1 | K2 | K3 | K4 | K5 | K6 | K7 | K8 |
|----|----|----|----|----|----|----|----|
| Algorithm | Choke | Output Bus | Voice Lvl | Voice Tune | Polyphony | Glide | Init Voice |

Menu-only: Voice Pan, Voice Mute, FX1 Send, FX2 Send, Velocity Sens, MIDI Note.

### Mix

K1–K8 = V1–V8 Level. Menu: per-voice pan, FX1 sends, FX2 sends.

### FX — 3 concurrent buses

| K1 | K2 | K3 | K4 | K5 | K6 | K7 | K8 |
|----|----|----|----|----|----|----|----|
| Rev Mix | Rev Decay | Rev Size | Dly Mix | Dly Rate | Dly Fdbk | Dly Tone | Cho Mix |

Menu-only: Rev Type, Rev Predelay, Rev Damping, Dly BPF Cutoff, Dly BPF Width, Ping-Pong, Sync, Cho Rate, Depth, Width, Voices, Tone, Feedback.

### General

| K1 | K2 | K3 | K4 | K5 | K6 | K7 | K8 |
|----|----|----|----|----|----|----|----|
| Comp | Drive | Bit | Rate | EQ Lo | EQ Mid | EQ Hi | Master |

Menu-only: Drive Type (Tube/Fold/Clip), Lo Freq, Mid Freq, Hi Freq, Q Lo/Mid/Hi, Limiter, Master Tune, MIDI Channel.

---

## The 64 factory kits

| # | Name | Character |
|---|------|-----------|
| **Slots 0–9 · Forge originals** | | |
| 0 | Plastic | Razzmatazz-like, high resonator, snappy impulse clicks |
| 1 | Anvil | Heavy industrial, drive + ringing reso |
| 2 | Forge | Canonical 808-adjacent, balanced |
| 3 | Cinder | Dark, burned, deep LP filter cascade |
| 4 | Spark | Fast, bright, snappy with ping-pong delay |
| 5 | Dust | Lo-fi crushed, narrow filters, bit/rate reduced |
| 6 | Phasma | Drone homage — long decays + reverb wash + hat repeat-env |
| 7 | Static | Noise / wavefolder / unstable feedback |
| 8 | Glass | Bell-forward, all-Cymbal algo, ringing |
| 9 | Marteau | Mixed-algo showcase, hammer-like attacks |
| **Slots 10–19 · Classic drum machines** | | |
| 10 | 808 | TR-808 — long ringy kick, snappy claves, sizzling hat |
| 11 | 909 | TR-909 — punchy, white-noise snare, crispy hats |
| 12 | 707 | TR-707 — sample-flavoured, less tail |
| 13 | CR-78 | Roland CR-78 — vintage warm analog |
| 14 | Linn | LinnDrum — 80s sample, mid-bright |
| 15 | DMX | Oberheim DMX — heavy, dramatic |
| 16 | SP-12 | E-mu sampler grit, bit-crushed |
| 17 | Simmons | Electronic drum kit, pitched toms |
| 18 | Boom | Boom-bap — fat kick, dusty snare |
| 19 | Trap | Modern trap, thick 808 sub |
| **Slots 20–29 · LXR-02 / Sonic Potions / Erica** | | |
| 20 | LXR | Erica LXR-02 direct vibe — drivable peak filter |
| 21 | Riga | Erica Latvian dark moody |
| 22 | Schmidt | Sonic Potions LXR homage — repeat env claps |
| 23 | Indus | Heavy industrial, drive maxed |
| 24 | Lattice | LFO matrix showcase |
| 25 | Caustic | Peak-mode filter heavy |
| 26 | Steam | Pressurised, long sustained body |
| 27 | Iron | Clean industrial metallic lines |
| 28 | Burn | Ring-mod intense distorted FM |
| 29 | Cathedral | Long sacred-space reverb |
| **Slots 30–39 · Razzmatazz / per-voice resonator** | | |
| 30 | Razz | Razzmatazz direct — snap + high reso everywhere |
| 31 | Bell | Pitched bell array (all Cymbal algo) |
| 32 | Chime | High bright pitches |
| 33 | Pluck | Karplus-Strong-like short pluck |
| 34 | Wire | Taut ringing, high FB resonator |
| 35 | Vinyl | Crackled plastic, lo-fi chorus |
| 36 | Snap | Snap-forward, very short transients |
| 37 | Bounce | Rubber bouncy, repeat envelopes |
| 38 | Toy | Tinny high-pitch FM |
| 39 | Crystal | Sparkly bell + reverb |
| **Slots 40–49 · Digitone II / Plaits / Volca FM** | | |
| 40 | Digi | Digitone II vibe — body wavefolder + complex transient |
| 41 | Plaits | Mutable Plaits synthetic kick + snare |
| 42 | Cycles | Elektron Model:Cycles FM groovebox |
| 43 | Volca | Korg Volca Drum FM-based, lo-fi |
| 44 | Wavefold | Body wavefolder showcase |
| 45 | BaseWidth | Base-Width pre-filter showcase |
| 46 | Comb | Comb+/− filter showcase, pitched comb resonance |
| 47 | ThreeOp | 3-op cascade showcase (all Cymbal/Hat) |
| 48 | Index | FM index envelope focus, snappy attacks |
| 49 | DX | Yamaha DX-flavoured FM |
| **Slots 50–59 · Genres** | | |
| 50 | Techno | Straight 4-on-floor Berlin |
| 51 | House | Chicago house, warm + bouncy |
| 52 | Detroit | Detroit techno, raw + driving |
| 53 | Acid | TB-303-flavoured, resonant squelch |
| 54 | Footwork | Chicago footwork, fast aggressive |
| 55 | Jungle | Drum'n'bass break-friendly |
| 56 | Garage | UK garage shuffly |
| 57 | Dubstep | Wobble drop, sub-heavy |
| 58 | Bossa | Latin percussion |
| 59 | Glitch | Broken / error sounds, randomness |
| **Slots 60–63 · Experimental** | | |
| 60 | Frost | Icy cold high-frequency |
| 61 | Magma | Molten heavy, deep bass + drive |
| 62 | Smoke | Wispy atmospheric, soft transients |
| 63 | Chaos | Randomised aggressive, all-Wild + max FM |

Each kit defines all 8 voices plus per-kit FX state (reverb / delay / chorus) and a Kit B variation so the Morph knob produces an actual gradient — pitch shift, decay scale, drive bump, resonator bump, or algorithm swap depending on the kit.

---

## Save Kit

Forge persists user kits to `/data/UserData/schwung/forge_kits.dat` (binary
format with magic `'FRGE'`, version 2). The Save Kit knob is a self-resetting
enum: turn it to *Save* and the current edit state (all 64 slots × Kit A +
Kit B + macros + FX) is written; the knob auto-reverts to *Play*.

User kits override the factory in-place. To reset to factory defaults, delete
or rename `forge_kits.dat` and re-load the module.

### Migrating from older builds

If you used a pre-0.1.0 build, you may have a v1 save file. v1 → v2 saves are
automatically rejected by the version check, so the new 64-kit factory loads
cleanly. The old file is preserved on disk; rename or delete it at your
leisure.

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
   does NOT reload `module.json` — only a full boot does. Chain host caches the
   manifest at startup.)

---

## Building from source

Requirements: Docker (for the ARM64 cross-compile) + SSH access to a Move on
your network.

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

The build uses the `docker create + docker cp` pattern (Windows MSYS-safe);
the binary is verified post-compile via `aarch64-nm` for the
`move_plugin_init_v2` symbol.

---

## Inspirations & credits

Forge is an original design and DSP implementation by **Vincent Fillion**.
Architectural and timbral ideas drawn from:

- **Erica Synths LXR-02** — voice algorithm philosophy, drivable filter with peak mode, cross-voice LFO routing, kit morph
- **Sonic Potions LXR** (open-source by Julian Schmidt) — concept of curved AD envelope with repeat parameter
- **1010music nanobox Razzmatazz** — per-voice resonator with high feedback
- **Elektron Digitone II** — body wavefolder, dual filter machines, Base-Width pre-stage, comb modes
- **Mutable Instruments Plaits & Peaks** (Émilie Gillet, MIT) — 2-op FM operator pattern
- **DaisySP** (Electrosmith, MIT) — wavefolder and chorus topologies
- **KrautDrums** (Vincent Fillion, MIT — same author) — Dattorro reverb, EMT-156 compressor, multi-mode delay patterns

No code is copied from any proprietary source. Open-source code from MIT
projects (Plaits, Peaks, DaisySP, KrautDrums) is ported with attribution
preserved in the source comments. LXR's curve+repeat envelope was
re-implemented from the algorithmic concept; no code from the LXR firmware is
present.

Framework: [Schwung](https://github.com/charlesvestal/schwung) by **Charles Vestal**.

---

## Roadmap (v0.2)

Deferred from v0.1 to keep the first release shippable:

- **Two parallel FM stacks per voice** (currently single stack on Drum/Snare/Wild; Cymbal/Hat use a 3-op cascade)
- **Dedicated FM index envelopes** × 2 per voice (Digitone-style enveloped FM indices)
- **3-slot modulation matrix** per parameter (currently 1 slot)
- **User-rebindable macro mappings** per voice (currently fixed by algorithm)
- **Wild voice's free-form mod-wiring page**
- **Per-osc filter routing** properly split (currently falls back to Parallel)
- **Tone knob in chorus** wired (currently structural placeholder)

---

## License

MIT — see [LICENSE](LICENSE).
