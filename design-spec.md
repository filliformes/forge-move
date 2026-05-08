# Forge — Design Spec

8-voice FM/subtractive hybrid drum synthesiser for Ableton Move.

**A "supercharged LXR-02"** — keeps the LXR's specialised voice algorithms, drivable
SVF + drive path, AD-with-repeat envelopes, and cross-voice LFO routing. Adds
Razzmatazz-style per-voice resonator + Digitone-II-style enveloped FM, body
wavefolder, dual-filter routing with Base-Width pre-stage, three concurrent FX
buses, and the Forge signature: **Kit A↔B morph** displayed across Move's 16-pad
Drum Kit template.

## Identity

- **Module ID:** `forge`
- **Display name:** Forge
- **Abbrev:** `FORGE`
- **Author:** Vincent Fillion
- **License:** MIT
- **Language:** C
- **Component type:** sound_generator
- **API:** plugin_api_v2 (`raw_midi: true`)
- **Voices:** 8

## Voice algorithms (5)

| # | Algorithm | DSP topology |
|---|---|---|
| 0 | Drum   | 2-op FM (carrier + sine modulator + FB) + body wavefolder + click + resonator + dual filter |
| 1 | Snare  | Tonal osc + noise + 2-op FM body + per-noise envelope |
| 2 | Cymbal | 3-op serial FM (A→B→C, FB at A) + resonator |
| 3 | Hat    | Same 3-op FM as Cymbal, dual envelope (open/closed), mutual choke when paired |
| 4 | Wild   | Free-form 2-op FM + extra mod flexibility |

Default voice assignment: V1-V4 = Drum, V5 = Snare, V6 = Cymbal, V7-V8 = Hat
(linked, V7 closed / V8 open). User can change any voice's algorithm via the
Setup submenu.

## Per-voice signal flow

```
[Osc/FM (algo-dependent)]──> [Body Wavefolder] ──> [Click stage] ──>
[Resonator (FB up to 200%, cutoff, width)] ──>
[Filter routing: Single | Per-Osc | Serial | Parallel] ──>
   [Base-Width pre-filter (tunable bandpass)] ──>
   [Filt1: SVF or Comb+/− with Drive] ──> [Filt2: same options] ──>
[Bit Crush] ──> [Rate Crush] ──> [Amp (Env1)] ──> [Pan] ──> bus
```

Modulation sources:
- Env1 (amp, hardwired), Env2 (assignable)
- Pitch Env (independent, multi-destination)
- LFO (this voice, cross-voice routable)
- Velocity, Aftertouch, Mod Wheel
- Trigger (any voice) — phase-resets / fires LFO of any voice (LXR-style)

v0.1 modulation depth: 1 mod slot per parameter (single source + depth).
Cross-voice LFO routing kept as the LXR signature feature.

## DSP building blocks (reuse where possible)

- 2-op FM core: `phasma.c` osc/mod path + new feedback term
- 3-op serial FM: new (target Mutable Plaits FM Drum DSP / Digitone topology)
- Body wavefolder: VCV Rack Befaco wavefolder OR Plaits wavefolder
- Per-voice resonator: feedback-comb topology (FB > 100% via tanh saturation), inspired by Razzmatazz Resonator
- Click sample bank: 6 short transients (kick / rim / hat / clap / tom / snap), ~1k samples each, baked into binary
- Filter: Chamberlin SVF (existing pattern from Phasma/Weird Dreams, Q-normalized bandpass), plus comb (Comb+/Comb−) for filter machines
- Base-Width pre-filter: tunable bandpass with width control (centre + bandwidth, no resonance)
- AD envelope with curved exp/log shape and repeat — port from LXR (Sonic Potions LXR is open-source, GPL — port concept not code)
- Drive types: tube tanh, wavefolder, hard clip — existing patterns from KrautDrums Attitude / Phasma
- Reverb: Dattorro figure-8 (port from KrautDrums)
- Delay: ping-pong with band-pass in feedback loop
- Chorus: Panoramic Chorus topology (Digitone-inspired) — multi-tap with stereo width

## Pad layout — Move 16-pad Drum Kit template

```
┌────┬────┬────┬────┐
│ 13 │ 14 │ 15 │ 16 │  Pads 13–16 → Voices 5–8 with Kit B (untouched reference)
├────┼────┼────┼────┤
│  9 │ 10 │ 11 │ 12 │  Pads  9–12 → Voices 1–4 with Kit B (untouched reference)
├────┼────┼────┼────┤
│  5 │  6 │  7 │  8 │  Pads  5–8  → Voices 5–8 with morphed Kit A↔B state (live)
│  1 │  2 │  3 │  4 │  Pads  1–4  → Voices 1–4 with morphed Kit A↔B state (live)
└────┴────┴────┴────┘
```

When `morph = 0`, the lower row plays Kit A and the upper row plays Kit B —
so you have access to both kits simultaneously as a 16-pad performance kit.
At `morph = 1`, both rows play Kit B (cue to load a new variation or snap
morph to 0).

Tap any pad to select that voice for editing. The Voice context follows the
pad row — tapping pad 3 edits Kit A's voice 3, tapping pad 11 edits Kit B's
voice 3. Same submenus, different parameter bank.

Choke groups (None / A / B / C / D) span both rows.

## Page hierarchy (ui_hierarchy)

Standard Schwung ui_hierarchy with **nested levels** — sub-levels declare
drill-in entries via `params: [{"level":...,"label":...}]` just like root.
If the framework doesn't render sub-level drill-ins, fall back to flat siblings
(Voice / V-Osc / V-Filter / V-Env / V-Mod / V-Setup).

```
root: Patch
  Patch (root knobs)
  Voice (drill-in) ────────┐
  Mix                      │
  FX                       │
  General                  │
                           ↓
                       Voice (8-knob algo macro page)
                         Osc (drill-in)
                         Filter
                         Env
                         Mod
                         Setup
```

### Page 0 — Patch (root)

| # | Knob       | Type           | Function |
|---|-----------|----------------|----------|
| 1 | Kit        | int 0..63      | Kit preset jog |
| 2 | Save Kit   | enum [Play,Save] | Phasma-pattern: writes Kit A+B+macros to `forge_kits.dat`, auto-resets to Play |
| 3 | Rnd Kit    | trigger        | Randomize all 8 voices (Kit A side, respecting algorithm) |
| 4 | Rnd Voice  | trigger        | Randomize the currently selected voice |
| 5 | Rnd Pitch  | trigger        | Apply random musical scale across voices |
| 6 | Morph      | float 0..1     | Kit A↔B interpolation — Forge signature |
| 7 | All Decay  | float 1×..4×   | KrautDrums-pattern multiplier on each voice's nominal Env1 decay (snapshot-relative) |
| 8 | Rnd Pan    | trigger        | Randomize panning across voices |

Menu-only (in this order):
1. **All Mono** — reset all pans to centre
2. Init Decay — restore kit decays (undo All Decay)
3. Init Freq — restore kit pitches (undo Same Freq)
4. Same Freq — set master tune across all voices
5. Copy A→B — make Kit B identical to Kit A
6. Copy B→A — make Kit A identical to Kit B
7. Swap A↔B — flip kits
8. Rnd B from A — generate Kit B as randomized variation of Kit A
9. Morph Source — Knob / LFO / Macro / Mod Wheel
10. Morph Curve — Linear / Exp / Log / S-curve

### Page — Mix

| # | Knob | Function |
|---|---|---|
| 1-8 | V1 Lvl … V8 Lvl | 8 voice volumes |

Menu-only: V1-V8 Pan, V1-V8 Mute, V1-V8 FX1 Send, V1-V8 FX2 Send.

### Page — FX

| # | Knob       | Function |
|---|-----------|----------|
| 1 | Rev Mix    | FX2 Reverb wet |
| 2 | Rev Decay  | |
| 3 | Rev Size   | |
| 4 | Dly Mix    | FX1 Delay wet |
| 5 | Dly Rate   | |
| 6 | Dly Fdbk   | |
| 7 | Dly Tone   | |
| 8 | Cho Mix    | FX3 Chorus wet (Panoramic, Digitone-inspired) |

Menu-only: Rev Type (Plate/Spring/Chamber), Rev Pre-delay, Rev Damping, Dly BPF Cutoff, Dly BPF Width, Dly Ping-Pong (on/off), Dly Sync (on/off), Cho Rate, Cho Depth, Cho Width, Cho Voices, Cho Tone, Cho FB.

### Page — General

| # | Knob   | Function |
|---|-------|----------|
| 1 | Comp   | Bus compressor (sub-ms attack, KrautDrums-style EMT 156 macro) |
| 2 | Drive  | Master saturation (tube/fold/clip via menu) |
| 3 | Bit    | Master bit reduction |
| 4 | Rate   | Master rate reduction |
| 5 | EQ Lo  | Low shelf |
| 6 | EQ Mid | Mid peak |
| 7 | EQ Hi  | High shelf |
| 8 | Master | Master volume |

Menu-only: Drive Type (Tube/Fold/Clip), Lo Freq, Mid Freq, Hi Freq, Q Lo, Q Mid, Q Hi, Limiter (on/off), Master Tune, MIDI Channel.

### Page — Voice (drill-in from root)

The 8-knob **Macro page**, mappings depend on the selected voice's algorithm.
This is the root of the per-voice nested submenus.

| Knob | Drum  | Snare | Cymbal | Hat       | Wild |
|------|-------|-------|--------|-----------|------|
| 1    | Pitch | Tune  | Tune   | Tune      | M1   |
| 2    | Decay | Snap  | Decay  | Cls Dec   | M2   |
| 3    | Bend  | Body  | FMIdx  | Opn Dec   | M3   |
| 4    | FM    | Noise | Color  | FM        | M4   |
| 5    | Body  | Tone  | Shape  | Color     | M5   |
| 6    | Click | Decay | Reso   | Shape     | M6   |
| 7    | Cutoff| Cutoff| Cutoff | Cutoff    | M7   |
| 8    | Drive | Drive | Drive  | Drive     | M8   |

Menu drill-ins from Voice level: Osc, Filter, Env, Mod, Setup.

### Page — Voice → Osc (cv_*)

| # | Knob          | Notes |
|---|--------------|-------|
| 1 | Wave          | Carrier waveform (sine/tri/saw/sqr+PWM/noise) |
| 2 | Coarse Ratio  | FM ratio coarse 0.5..16 |
| 3 | Fine Ratio    | FM ratio fine ±1 oct |
| 4 | Detune        | Pitch detune ±50 cents |
| 5 | Level         | Oscillator level |
| 6 | Phase         | Starting phase 0..1 |
| 7 | PWM           | Pulse width (square only) |
| 8 | Feedback      | Modulator feedback |

Menu-only: Op A/B/C selector (3-op algos), Click Type (sample/impulse/phase), Click Sample (kick/rim/hat/clap/tom/snap/none), Click Level, Click Decay, Cross-FM (Wild only).

### Page — Voice → Filter (cv_*)

With v0.2 dual-filter grafts pulled into v0.1.

| # | Knob          | Notes |
|---|--------------|-------|
| 1 | Cutoff (F1)   | Filt1 cutoff |
| 2 | Reso (F1)     | Filt1 resonance |
| 3 | Type (F1)     | LP / HP / BP / BPu / Notch / Peak / Comb+ / Comb− |
| 4 | BW Cutoff     | Base-Width pre-filter centre |
| 5 | BW Width     | Base-Width bandwidth |
| 6 | Routing       | Single / Per-Osc / Serial / Parallel (auto when both filters active) |
| 7 | Drive (F1)    | Filter drive amount |
| 8 | Cutoff (F2)   | Filt2 cutoff |

Menu-only: Reso (F2), Type (F2), Drive (F2), BW On (off/on), Bit Crush, Rate Crush, KeyTrack (F1), KeyTrack (F2), Env1→Cutoff (F1), Env2→Cutoff (F2).

### Page — Voice → Env (cv_*)

| # | Knob          | Notes |
|---|--------------|-------|
| 1 | E1 Atk        | Env1 (amp) attack |
| 2 | E1 Dec        | Env1 decay |
| 3 | E1 Curve      | Exp ↔ Lin ↔ Log |
| 4 | E1 Repeat     | Repeat amount (LXR-style) |
| 5 | E2 Dec        | Env2 decay |
| 6 | E2 Curve      | Env2 curve |
| 7 | PE Amt        | Pitch Env amount |
| 8 | PE Dec        | Pitch Env decay |

Menu-only: E2 Destination (FM Idx / Filter / Reso / Pan / Mod), E1 Repeat Rate, E2 Atk, PE Curve, PE Destination, Vel→E1 Lvl, Vel→E1 Time, Vel→E2 Amt.

### Page — Voice → Mod (cv_*)

| # | Knob          | Notes |
|---|--------------|-------|
| 1 | LFO Wave      | Sine/Tri/Saw/Sqr/S&H/Random |
| 2 | LFO Rate      | 0.01 Hz–audio rate |
| 3 | LFO Sync      | Free / 1/4 / 1/8 / 1/16 etc. |
| 4 | LFO Depth     | LFO output amplitude |
| 5 | Cross LFO Src | Source LFO (any voice) — LXR cross-voice trick |
| 6 | Trig Reset    | Trigger source for LFO phase reset (any voice) |
| 7 | Mod Slot Dest | Destination parameter (1 slot in v0.1) |
| 8 | Mod Slot Dpth | Destination depth |

Menu-only: LFO Phase Init, LFO Bipolar/Unipolar, LFO Restart on Trig (yes/no), Mod Slot Source (Env1/Env2/PEnv/LFO/XLFO/Vel/AT/MW), Mod Slot Curve.

### Page — Voice → Setup (cv_*)

| # | Knob          | Notes |
|---|--------------|-------|
| 1 | Algorithm     | Drum / Snare / Cymbal / Hat / Wild |
| 2 | Choke Group   | None / A / B / C / D |
| 3 | Output Bus    | Main / Aux1 / Aux2 / FX-only |
| 4 | Voice Level   | Pre-mixer voice level |
| 5 | Voice Tune    | Master tune offset for this voice |
| 6 | Polyphony     | One-shot / Legato / Retrig |
| 7 | Glide         | Portamento time |
| 8 | Init Voice    | Trigger — restore this voice to algorithm defaults |

Menu-only: Voice Pan, Voice Mute, FX1 Send, FX2 Send, Velocity Sensitivity, MIDI Note (auto by pad).

## Performance & save

- **Kit A + Kit B + Macros + Morph state + FX state + Master state** all stored
  per kit slot. Binary file at `/data/UserData/schwung/forge_kits.dat`,
  magic `'FRGE' = 0x46524745u`, version 1.
- Phasma-style: `save_kit_state` self-resets after `Save` is committed.
- **64 built-in kits** (Forge ships with curated factory). User kits override
  the built-in slots in-place.
- All synthesis parameters exposed as MIDI CCs for sequencer-side automation.

## v0.1 deferred features (planned for v0.2+)

- 2 parallel FM stacks per voice (currently 1)
- Dedicated FM Index Envs ×2 per voice (currently uses Pitch Env routing)
- 3-slot mod matrix per parameter (currently 1 slot per param)
- User-rebindable Macro page mappings per voice (currently fixed by algorithm)
- "Wild" voice's free-form mod wiring page

## Critical constraints

- 8-field `plugin_api_v2_t` struct (get_error MUST exist as NULL)
- `get_param("ui_hierarchy")` returned from DSP — REQUIRED for menu navigation (Signal lesson)
- `get_param("chain_params")` returned from DSP, listing every key from every page
- module.json mirrors `ui_hierarchy` and includes `chain_params` array
- `dsp.so` filename, install at `modules/sound_generators/forge/`
- `raw_midi: true` for pad MIDI
- Files owned by `ableton:users` after deploy
- Trigger params (rnd_kit, rnd_voice, etc.) need direct `set_param` handlers
  AND `type:"enum"` with `["0","1"]` options for auto-revert display (Signal lesson)
- All Decay must be **multiplier** style (snapshot-relative), not absolute —
  preserves per-voice character. Reset trigger restores 1.0×.
- Save Kit param is **menu-only** (in `params` but NOT in `knobs`), enum [Play,Save].
- Per-voice menus use **cv_*** prefix (current-voice virtual params), same as
  Phasma/Weird Dreams convention.

## CPU budget target

8 voices × (single 2-op FM + wavefolder + resonator + 2-stage filter + Base-Width pre +
4 envelopes + LFO + drive + crush) ≈ within Move A53 budget. Resonator must use
feedback-comb topology (cheap FB > 100%) not full bandpass delay-line.

Per-block mod evaluation; per-sample only for FM index, filter cutoff with
audio-rate LFO. Mark explicitly in destination table (Denis lesson).
