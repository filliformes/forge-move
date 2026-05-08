# Forge — Claude Code context

## What this is

8-voice FM/subtractive hybrid drum synthesiser for Ableton Move. A
"supercharged LXR-02" — keeps the LXR's specialised voice algorithms,
drivable filter+drive, AD-with-repeat envelopes, and cross-voice LFO
routing. Adds Razzmatazz-style per-voice resonator + Digitone-II
inspirations: body wavefolder, dual-filter routing with Base-Width
pre-stage, three concurrent FX buses. Forge signature: **Kit A↔B morph**
displayed across Move's 16-pad Drum Kit template.

Schwung sound generator. API: plugin_api_v2_t. Language: C.
Voice architecture: 8 fixed-allocation drum voices (no stealing, no
polyphony per slot — drum machine semantics). `raw_midi: true` for pads.

See `design-spec.md` for the complete instrument spec.

## Repo structure
- `src/dsp/forge.c` — all DSP (voice state, MIDI, envelopes, FM/filter, render_block)
- `src/module.json` — module metadata, ui_hierarchy (nested), chain_params
- `scripts/build.sh` — Docker ARM64 cross-compile (docker create + cp pattern, Windows-safe)
- `scripts/install.sh` — deploys to Move via scp, fixes ownership
- `.github/workflows/release.yml` — CI: verifies version, builds, releases, updates release.json
- `design-spec.md` — full instrument spec (algorithms, signal flow, page hierarchy, pad layout)

## Algorithms (5)

| # | Algo   | Topology |
|---|-------|----------|
| 0 | Drum   | 2-op FM + body wavefolder + click + resonator + dual filter |
| 1 | Snare  | Tonal osc + noise + 2-op FM body |
| 2 | Cymbal | 3-op serial FM (A→B→C, FB at A) |
| 3 | Hat    | 3-op FM with open/closed envelopes, mutual choke |
| 4 | Wild   | Free-form 2-op FM + extra mod flexibility |

Default voice mapping: V1-V4=Drum, V5=Snare, V6=Cymbal, V7-V8=Hat.

## Per-voice signal flow

```
[Osc/FM] → [Body Wavefolder] → [Click] → [Resonator] →
[Filter routing] → [Base-Width pre] → [Filt1 SVF/Comb] → [Filt2] →
[Bit/Rate Crush] → [Amp (Env1)] → [Pan] → bus
```

## Page hierarchy (10 levels, nested)

```
root: Patch (8 knobs always live)
├── Patch (kit/save_kit/rnd_*/morph/all_decay/rnd_pan + menu-only ops)
├── Voice (drill-in)
│   ├── Macro page (8 algo-routed knobs)
│   ├── Osc / Filter / Env / Mod / Setup (sub-pages)
├── Mix (8 voice levels)
├── FX (Rev / Dly / Cho on 3 concurrent buses)
└── General (Comp / Drive / Bit / Rate / EQ / Master)
```

cv_* params are virtual current-voice parameters — they read/write the
selected pad's voice bank in the active kit (A or B based on pad row).

## Pad layout (Move 16-pad Drum Kit)

```
Pads  9-16 → Voices 1-8 with Kit B (untouched reference)
Pads  1-8  → Voices 1-8 with morphed Kit A↔B state
```

When morph=0, lower row plays Kit A and upper row plays Kit B — both kits
simultaneously available as a 16-pad performance kit. As morph increases,
the lower row morphs toward Kit B; upper row stays put. At morph=1, both
rows play identically (cue to load a new variation).

Choke groups span both rows (a pad-1 hit and pad-9 hit can choke each
other if they share a group).

## Save Kit (Phasma pattern)

Binary file at `/data/UserData/schwung/forge_kits.dat`, magic
`'FRGE' = 0x46524745u`, version 1. Contents: full 64 kit slots
(each slot stores Kit A + Kit B + per-kit FX state). On Save,
auto-resets `save_kit` enum to "Play". On init, defaults overlay
loaded from disk.

## All Decay (snapshot-relative multiplier — KrautDrums pattern)

`all_decay_mult` is 1.0×–4.0× applied on top of each voice's nominal
`e1_dec`. Init Decay (menu-only trigger) restores 1.0×. Per-voice
character is preserved across the multiplier.

## Critical constraints

- 8-field `plugin_api_v2_t` struct (`get_error` field MUST exist as NULL)
- `get_param("ui_hierarchy")` MUST be implemented (Signal lesson) — Schwung
  reads it from DSP at runtime, falls back to "No presets" if missing
- `get_param("chain_params")` MUST be implemented — Shadow UI metadata source
- Both ui_hierarchy and chain_params come from `module.json` at module load
  time (lazy-cached) — keep DSP and module.json in sync
- `dsp.so` filename, install at `modules/sound_generators/forge/`
- `raw_midi: true` for pads
- Files owned by `ableton:users` after deploy — `scripts/install.sh` does this
- Trigger params (rnd_kit, rnd_voice, save_kit, etc.) need direct `set_param`
  handlers AND `type:"enum"` with `["0","1"]` (or `["Play","Save"]`) for
  auto-revert display (Signal lesson)
- `get_param` MUST return -1 for unknown keys (NOT 0) — returning 0 breaks
  Master FX menu editing for float/int params (Signal lesson)
- get_param must return RAW values for state round-trip ("0.5000" not "50%") —
  display formatting belongs in knob_N_value handlers only (Phasma lesson)
- Power-cycle Move after module.json changes (chain host caches at startup)
- C declaration order: helpers must precede callers (Signal lesson)

## Build & deploy

```bash
./scripts/build.sh    # Docker ARM64 cross-compile (Windows-safe)
./scripts/install.sh  # SCP to move.local, fix ownership
# Then power-cycle the Move or remove + re-add the module
```

## v0.1 → v0.2 plan

Deferred to v0.2:
- 2 parallel FM stacks per voice (currently single)
- Dedicated FM Index Envs ×2 per voice
- 3-slot mod matrix per parameter (currently 1 slot)
- User-rebindable Macro page mappings per voice (currently fixed)
- Wild voice's free-form mod wiring page

## DSP sources to fetch (next: /dsp-fetch)

- 2-op FM core: existing pattern from phasma.c, add proper feedback term
- Body wavefolder: VCV Rack Befaco / Mutable Plaits
- Resonator: feedback-comb topology (Jatin Chowdhury BBD or simple comb-with-tanh)
- Chamberlin SVF: existing pattern from KrautDrums/Phasma
- Comb filter (Comb+/Comb-): Schroeder allpass cascade
- AD envelope with curve+repeat: Sonic Potions LXR (GPL — port concept only)
- Click samples: existing pattern (baked transient arrays)

## Source / license notes

Original design + DSP: Vincent Fillion (2026). MIT.
Inspirations (no code copied):
- Erica Synths LXR-02 (proprietary firmware) — voice algorithm philosophy
- Sonic Potions LXR (GPL-2.0 — Julian Schmidt) — env curve+repeat concept
- 1010music nanobox Razzmatazz (proprietary) — per-voice resonator
- Elektron Digitone II (proprietary) — enveloped FM index, FM Drum machine
