# Forge

8-voice FM/subtractive hybrid drum synthesiser for [Ableton Move](https://www.ableton.com/move/),
built for the [Schwung](https://github.com/charlesvestal/schwung) framework.

A "supercharged LXR-02": specialised voice algorithms, drivable filter+drive,
AD-with-repeat envelopes, cross-voice LFO routing — plus Razzmatazz-style
per-voice resonator, Digitone-II-inspired body wavefolder, dual-filter
routing with Base-Width pre-stage, and three concurrent FX buses.

**Forge signature:** Kit A↔B morph displayed across Move's 16-pad Drum Kit
template. Lower row plays the morphed state, upper row plays Kit B as an
untouched reference — both kits simultaneously available as a 16-pad
performance kit, with a single Morph knob smoothly interpolating every
parameter of every voice between them.

## Features

- **8 voices, 5 algorithms:** Drum / Snare / Cymbal / Hat / Wild
  - Drum: 2-op FM + body wavefolder + per-voice resonator + dual filter
  - Snare: tonal osc + noise + 2-op FM body
  - Cymbal: 3-op serial FM (A→B→C with feedback)
  - Hat: 3-op FM with open/closed envelopes and mutual choke
  - Wild: free-form 2-op FM with extra modulation flexibility
- **Kit A ↔ Kit B morph** — every parameter of every voice interpolated
  by a single knob; both kits playable simultaneously across 16 pads
- **Per-voice resonator** with FB up to 200% (Razzmatazz-style metallic ring
  on any voice, no FM-index pushing required)
- **Dual filter** with Base-Width pre-stage and four routing modes
  (Single / Per-Osc / Serial / Parallel); Filt1+Filt2 modes include
  LP, HP, BP, BPu, Notch, Peak, Comb+, Comb−
- **Curved AD envelopes with repeat** (LXR signature) — exp/lin/log shape
  + repeat parameter for synthesised claps and buzz rolls
- **Cross-voice LFO routing** — any voice's LFO can target any voice's
  parameter; any voice's trigger can phase-reset any LFO
- **3 concurrent FX buses** — Delay (with BPF in feedback), Reverb
  (Plate / Spring / Chamber), Panoramic Chorus (Digitone-inspired)
- **64 kit presets** with full Phasma-style Save Kit (writes to `/data`)
- **All Decay** multiplier (1×–4×) preserves per-voice character

## Pad layout

```
┌────┬────┬────┬────┐
│ 13 │ 14 │ 15 │ 16 │  ← Kit B reference (untouched)
├────┼────┼────┼────┤
│  9 │ 10 │ 11 │ 12 │
├────┼────┼────┼────┤
│  5 │  6 │  7 │  8 │  ← Morphed Kit A↔B state (live)
│  1 │  2 │  3 │  4 │
└────┴────┴────┴────┘
```

Tap any pad to select that voice for editing. Pads 1-8 edit Kit A's voices,
pads 9-16 edit Kit B's voices.

## Pages

| Page | Content |
|------|---------|
| **Patch** | Kit, Save Kit, Rnd Kit/Voice/Pitch/Pan, Morph, All Decay |
| **Voice** | 8 algo-dependent macro knobs; drills into Osc / Filter / Env / Mod / Setup |
| ↳ Osc | Wave, Ratio, Detune, Level, Phase, PWM, Feedback (+ click stage in menu) |
| ↳ Filter | Filt1 + Filt2 + Base-Width pre + Routing + Drive (+ Bit/Rate crush in menu) |
| ↳ Env | E1 + E2 + Pitch Env (curved AD with repeat) |
| ↳ Mod | LFO + cross-voice routing + 1 mod slot |
| ↳ Setup | Algorithm, Choke, Output Bus, Polyphony, Glide |
| **Mix** | 8 voice volumes (pans, sends, mutes in menu) |
| **FX** | Rev Mix/Decay/Size, Dly Mix/Rate/Fdbk/Tone, Cho Mix |
| **General** | Comp, Drive, Bit, Rate, 3-band EQ, Master |

## Building

```bash
./scripts/build.sh
```

Requires Docker (cross-compiles for ARM64 via `aarch64-linux-gnu-gcc`).

## Installation

```bash
./scripts/install.sh
```

Or grab the latest tarball from
[Releases](https://github.com/filliformes/forge-move/releases) and install
via the Schwung Manager.

After installing, **power-cycle the Move** (or remove and re-add the
module from an FX slot) so chain_host picks up the new `module.json`.

## Roadmap (v0.2+)

- 2 parallel FM stacks per voice
- Dedicated FM index envelopes (Digitone II-style)
- 3-slot modulation matrix per parameter
- User-rebindable Macro page mappings per voice
- Wild voice's free-form modulation wiring page

## Inspirations

Forge is an original work by Vincent Fillion. Architectural ideas borrowed
from:

- **Erica Synths LXR-02** — voice algorithm philosophy, drivable filter,
  cross-voice LFO routing
- **Sonic Potions LXR** (open-source, GPL — Julian Schmidt) — curved AD
  envelopes with repeat
- **1010music Razzmatazz** — per-voice resonator
- **Elektron Digitone II** — body wavefolder, dual filter machines

No code is copied from any of these — the DSP is original, sourced from
open-source building blocks (VCV Rack, Mutable Instruments, Jatin
Chowdhury, etc.) where applicable.

## Credits

- Design + DSP: Vincent Fillion
- Framework: [Schwung](https://github.com/charlesvestal/schwung) (Charles Vestal)

## License

MIT — see [LICENSE](LICENSE)
