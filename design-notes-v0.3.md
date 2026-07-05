# Forge vs the greats — what to borrow to be exceptional

Comparison of Forge (v0.2.0) against the four instruments in its lineage,
after reading their manuals, plus the Elektron Machinedrum as a fresh
reference. Forge is a **Move sound generator** — it has no internal sequencer
(Move's grid + companion MIDI FX do that), so sequencer-only features are
scoped out; synthesis, modulation, FX, and performance are in scope.

## Capability matrix

| | Forge v0.2 | Digitone II | LXR-02 | Razzmatazz | Machinedrum |
|---|---|---|---|---|---|
| Voices | 8 fixed | 16 poly-alloc | 6 fixed | 8 fixed | 16 fixed |
| Engine model | 5 algos, runtime per voice | 4 machines, runtime swap | 4 fixed topologies | 1 unified voice | many machines, runtime swap |
| FM depth | 2-op + parallel body / 3-op cascade | **4-op, 8 algorithms** | 2-op | 2×2-op parallel + WAV | EFM machines (per-drum) |
| Enveloped FM | pitch-env + 1 mod slot | **2 dedicated FM-index envelopes** | pitch env → index | HD env → index | env → index |
| Osc alt-modes | wave select + PWM | **Wavetone: phase-distortion, sync, RM** | rect/saw/noise | wave select | analog-model / sample |
| Wavefolder | ✅ (via drive) | ✅ (FM Drum body) | — | — | distortion |
| Resonator | ✅ per-voice | — | — | ✅ per-voice | — |
| Filter | dual SVF + Base-Width + Comb± | multimode + EQ + Comb± + **Base-Width** + Legacy | SVF + **LP2 acid** + drive | dual, 3 routings | per-track LP/BP |
| Env | AD + curve + **repeat** | multi-stage + 2 index envs | AD + slope + **repeat** | HD only | AD |
| LFO | 1/voice, **cross-voice** routing | 3/voice, **dual-shape morph (SHMIX)** | 1/voice, cross-voice | 1/voice local | **dual-shape morph** |
| Per-voice FX | drive, bit, rate | OD, bit, SRR, Base-Width | SRR | dist, bit, rate, snap | **AM, EQ, dist, SRR** |
| Global FX | delay, reverb, chorus, comp, EQ | 3 sends + master comp/OD | 1-at-a-time + comp | 3 concurrent | **Rhythm Echo + Gate Box** |
| Kit morph | ✅ A↔B across 16 pads | scenes | ✅ kit morph | — | kit swap |
| Performance macro | All Decay only | scenes/macros | — | 2 macros (X/Y) | **Ctrl-All (all tracks)** |
| Kits shipped | **128** | 16 patterns | — | 120 | — |

## What Forge already nails (parity or better)

Forge is genuinely already a synthesis of all four:

- **Cross-voice LFO routing** (LXR signature) — have it.
- **AD envelope with curve + repeat** (LXR signature) — have it.
- **Dual filter + Base-Width pre-stage + Comb± modes** (Digitone) — have it.
- **Per-voice resonator** (Razzmatazz signature) — have it.
- **3 concurrent FX buses** (Razz/Digitone) — have it.
- **Kit A↔B morph across the 16-pad row split** — more immediate than any
  of them; the untouched-B reference row is unique to Forge.
- **128 factory kits** — more than any ship with.
- **Parallel body sine kick** (Razzmatazz's two-osc-stack insight) — have it
  (v0.2 fix).

## What to borrow — ranked by impact × fit

### Tier 1 — signature upgrades that would make Forge exceptional

1. **Enveloped FM indices (Digitone II's soul).** Two dedicated FM-index
   envelopes per voice, each an AD with its own depth, targeting the FM
   amount. This is *the* thing that makes Digitone drums breathe — the FM
   index blooms and collapses per hit. Forge fakes a little of this through
   the pitch env → FM routing, but a dedicated pair is a different league.
   **High sonic impact, moderate effort** (2 more AD envs per voice + wire to
   `fm_idx`). This is the biggest missing piece.

2. **Ctrl-All performance macros (Machinedrum's live ethos).** Generalise
   All Decay into a small bank of global "nudge every voice at once" knobs:
   Punch (body), Bright (cutoff), Decay, Drive, Pitch, Snap. On Move's 8
   encoders these turn Forge into a live-morphable instrument — one knob
   sweeps the whole kit. **High performance impact, low effort** (snapshot-
   relative multipliers, exactly the All-Decay pattern extended).

3. **Gated reverb (Machinedrum GATE BOX).** The single most iconic drum
   effect (Phil Collins snare). Add a gate mode to the existing reverb: the
   tail is cut by a fast envelope. **Medium impact, low effort** (envelope
   the reverb output). Very "drum machine".

### Tier 2 — character options

4. **LP2 acid self-oscillating filter mode (LXR).** Add a screaming
   self-resonant LP as a filter type. Instant 303/acid-perc character.
   **Medium impact, low effort** (one more mode in the SVF switch, allow Q to
   self-oscillate).

5. **LFO dual-shape morph (Machinedrum SHMIX / Digitone).** Blend two LFO
   waveforms with a mix knob instead of picking one. Evolving, less static
   modulation. **Medium impact, low effort.**

6. **Per-voice AM / ring-mod (Machinedrum AM + Razz).** A per-voice amplitude
   modulator that ranges from slow tremolo to audio-rate ring-mod (metallic).
   Great on hats, snares, FX. **Medium impact, moderate effort.**

7. **Parametric Snap generator (Razzmatazz).** Replace/augment the click
   bank with a fully synthesised snap: 1–64 sample impulse width, optional
   ×2 double-pulse, signed level (polarity), velocity scaling. More flexible
   and "electronic" than the baked click bank. **Medium impact, moderate.**

### Tier 3 — engine expansion (bigger builds)

8. **4-op FM with selectable algorithms (Digitone) for Drum/Wild.** Move from
   2-op to a 4-op core with a handful of algorithms + X/Y carrier mix. Much
   wider FM palette. **High impact, high effort.**

9. **Wavetone machine (Digitone).** A new algorithm: phase-distortion osc +
   oscillator sync + ring-mod + defined noise. A whole new sonic territory.
   **High impact, high effort.**

10. **Finish Per-Osc filter routing.** Currently falls back to Parallel; wire
    Filt1→Osc1, Filt2→Osc2 properly. **Low impact, low effort — cleanup.**

## Recommended v0.3 scope

The three Tier-1 items are the sweet spot — each is a signature feature of one
of the greats, each is high-impact, and together they'd make Forge feel
alive (enveloped FM), playable (Ctrl-All), and iconic (gated verb):

- **Enveloped FM indices** ×2 per voice
- **Ctrl-All performance macros** (6 global nudge knobs)
- **Gated reverb** mode

Plus 1–2 cheap Tier-2 wins (LP2 acid filter, LFO morph) if there's room.

Tier 3 (4-op FM, Wavetone) is a v0.4 "engine 2.0" project.
