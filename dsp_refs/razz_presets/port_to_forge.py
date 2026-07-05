#!/usr/bin/env python3
"""Port Razzmatazz .nnr presets into Forge fk_compact_t C struct literals.

Reads _all_presets.json (produced by extract_razz.py) and emits
../../src/dsp/factory_kits_razz.h — an array FACTORY_KITS_RAZZ[N] that Forge's
init_factory_kits() applies to kit slots 64..64+N-1.

Mapping calibrated from the archetype analysis of 1032 Razz pads:
  algo     Razz drummodeltype / pad-name keyword → Forge ALGO_*
  note     Razz oscFixedPitch (MIDI note)        → Forge midi_note
  ratio    Razz fmratio (0-16)                   → Forge ratio_c (0.5-16)
  fm       Razz fmamt (0-1000+)                  → Forge fm macro (0-1)
  fbk      Razz fmfeedback (0-1000)              → Forge fbk (0-1)
  dec      Razz envdecay (0-1000)  → e1_dec seconds via exp curve
  f1_type  Razz filtertype {0LP,1HP,2BP,3Notch}  → Forge {0,1,2,4}
  f1_cut   Razz cutoff (0-1000)    → Hz via exp curve
  reso     Razz dfx.reso_fb (0-2000)             → reso macro (0-1)
  drive    Razz dfx.dist (0-1000)                → drive macro (0-1)
  bit/rate Razz dfx.bit/rate (0-1000)            → 0-1
  fx sends Razz dfx.fx1/fx2 send (0-1000)        → 0-1
  pan      Razz panpos (-1000..1000)             → -1..1 (informs Kit B / mix)
Global FX derived from Razz delay / reverb cells.

Pure-sample presets (all 8 pads = Sample) are skipped — Forge is synthetic.
"""

from __future__ import annotations
import json
import math
import re
from pathlib import Path

HERE = Path(__file__).parent
OUT = HERE / '..' / '..' / 'src' / 'dsp' / 'factory_kits_razz.h'

# ── conversions ──────────────────────────────────────────────────────────────

def razz_decay_to_sec(v: int) -> float:
    """Razz envdecay 0-1000 → Forge e1_dec seconds (exp, drum-musical)."""
    t = 0.08 * math.exp((v / 1000.0) * 3.5)
    return max(0.02, min(3.0, t))

def razz_cut_to_hz(v: int) -> int:
    """Razz cutoff 0-1000 → Forge f1_cut Hz (exp)."""
    f = 30.0 * (600.0 ** (v / 1000.0))
    return int(max(30, min(18000, f)))

def razz_res_to_forge(v: int) -> float:
    """Razz res 0-1000 → Forge f1_res 0.5-20."""
    return round(0.5 + (v / 1000.0) ** 1.5 * 12.0, 2)

FILT_MAP = {0: 0, 1: 1, 2: 2, 3: 4}   # LP, HP, BP, Notch(=Forge idx 4)

# Forge algo enum
DRUM, SNARE, CYMBAL, HAT, WILD = 0, 1, 2, 3, 4
ALGO_C = {DRUM: 'ALGO_DRUM', SNARE: 'ALGO_SNARE', CYMBAL: 'ALGO_CYMBAL',
          HAT: 'ALGO_HAT', WILD: 'ALGO_WILD'}
# Forge click sample enum
CSMP = {'none': 0, 'kick': 1, 'rim': 2, 'hat': 3, 'clap': 4, 'tom': 5, 'snap': 6}
CSMP_C = {0: 'CSMP_NONE', 1: 'CSMP_KICK', 2: 'CSMP_RIM', 3: 'CSMP_HAT',
          4: 'CSMP_CLAP', 5: 'CSMP_TOM', 6: 'CSMP_SNAP'}

RAZZ_MODEL_TO_ALGO = {
    'Kick': DRUM, 'Snare': SNARE, 'HHatC': HAT, 'Tom': DRUM,
    'User': WILD, 'Sample': DRUM,
}

def role_from_name(name: str) -> str | None:
    n = name.lower()
    if any(k in n for k in ('kick', 'bd', 'bass d', 'boom', 'sub', 'thump')):
        return 'kick'
    if any(k in n for k in ('snare', 'sd', 'clap', 'rim', 'snap', 'clp')):
        return 'snare'
    if any(k in n for k in ('ohat', 'open h', 'openhat', 'oh')):
        return 'ohat'
    if any(k in n for k in ('chat', 'closd', 'closed', 'hat', 'hh', 'ch')):
        return 'chat'
    if any(k in n for k in ('ride', 'crash', 'cym', 'bell', 'ping', 'metal')):
        return 'cym'
    if any(k in n for k in ('tom', 'conga', 'bongo', 'perc', 'clave', 'block',
                            'cow', 'tabla', 'click', 'tick')):
        return 'perc'
    return None

def choose_algo(pad: dict) -> tuple[int, int]:
    """Return (Forge algo, click sample) from a Razz pad."""
    role = role_from_name(pad.get('name', ''))
    razz_algo = RAZZ_MODEL_TO_ALGO.get(pad.get('algo', ''), None)
    # Name role wins where it clearly maps to a Forge algo/click.
    if role == 'kick':
        return DRUM, CSMP['kick']
    if role == 'snare':
        return SNARE, CSMP['snap']
    if role == 'chat':
        return HAT, CSMP['hat']
    if role == 'ohat':
        return HAT, CSMP['hat']
    if role == 'cym':
        return CYMBAL, CSMP['none']
    if role == 'perc':
        return DRUM, CSMP['tom']
    if razz_algo is not None:
        click = {DRUM: CSMP['kick'], SNARE: CSMP['snap'], HAT: CSMP['hat'],
                 WILD: CSMP['rim']}.get(razz_algo, CSMP['none'])
        return razz_algo, click
    return WILD, CSMP['rim']

def loud_osc(pad: dict) -> dict:
    """Return the louder of the two FM oscillators (the tonal driver)."""
    a, b = pad['fm1'], pad['fm2']
    return a if a.get('level', 0) >= b.get('level', 0) else b

def port_pad(pad: dict) -> dict:
    algo, click = choose_algo(pad)
    osc = loud_osc(pad)
    fm1, fm2, f1 = pad['fm1'], pad['fm2'], pad['f1']
    dfx = pad['dfx']

    # Note (fundamental). The louder Razz osc on snares/hats is often the
    # high-pitch noise operator, so clamp to a role-appropriate register
    # instead of inheriting an out-of-range fixed pitch.
    raw_note = int(osc.get('pitch', 48))
    if algo == DRUM and raw_note < 52:            # kick
        note = max(28, min(52, raw_note))
    elif algo == DRUM:                             # tom / low perc
        note = max(40, min(70, raw_note))
    elif algo == SNARE:
        note = raw_note if 40 <= raw_note <= 62 else 52
    elif algo == HAT:
        note = raw_note if 56 <= raw_note <= 74 else 62
    elif algo == CYMBAL:
        note = raw_note if 54 <= raw_note <= 80 else 66
    else:                                          # WILD
        note = max(36, min(84, raw_note))
    ratio = float(max(fm1.get('ratio', 1), 1))
    ratio = max(0.5, min(16.0, ratio))
    fm = max(fm1.get('fmamt', 0), fm2.get('fmamt', 0)) / 1000.0
    fm = max(0.0, min(1.0, fm))
    # Snare m[3] controls both FM index AND body/noise mix — floor it so every
    # snare has audible broadband crack that survives its band-pass filter
    # (fixes near-silent tonal snares like the ported AeroVan pad 2).
    if algo == SNARE:
        fm = max(fm, 0.4)
    fbk = max(fm1.get('fbk', 0), fm2.get('fbk', 0)) / 1000.0
    fbk = max(0.0, min(1.0, fbk))
    dec = razz_decay_to_sec(pad['e1'].get('decay', 200))
    # Pitch-env amount: kicks/toms bend, others little
    if algo == DRUM and note < 52:
        pe = 0.68
    elif algo == DRUM:
        pe = 0.4
    elif algo == SNARE:
        pe = 0.3
    else:
        pe = 0.05
    reso = max(0.0, min(0.95, dfx.get('reso_fb', 0) / 1200.0))
    drive = max(0.0, min(1.0, dfx.get('dist', 0) / 1000.0))
    bit = max(0.0, min(1.0, dfx.get('bit', 0) / 1000.0))
    rate = max(0.0, min(1.0, dfx.get('rate', 0) / 1000.0))
    fx1 = max(0.0, min(1.0, dfx.get('fx1_send', 0) / 1000.0))
    fx2 = max(0.0, min(1.0, dfx.get('fx2_send', 0) / 1000.0))

    # Filter override only when the Razz filter is enabled and not a plain
    # wide-open LP (keeps the table lean).
    f_on = f1.get('on', 0)
    f_cut = razz_cut_to_hz(f1.get('cutoff', 600))
    f_type = FILT_MAP.get(f1.get('type', 0), 0)

    # 'body' field: for Drum it's overridden by register in apply_fk_compact;
    # for Wild it drives the wavefolder, so derive from FM feedback intensity.
    body = 0.25 + 0.4 * fbk if algo == WILD else 0.3

    return {
        'algo': algo, 'note': note, 'click': click, 'ratio': round(ratio, 2),
        'fbk': round(fbk, 3), 'dec': round(dec, 3), 'pe': pe,
        'fm': round(fm, 3), 'body': round(body, 3), 'reso': round(reso, 3),
        'drive': round(drive, 3), 'bit': round(bit, 3), 'rate': round(rate, 3),
        'f_on': f_on, 'f_cut': f_cut, 'f_type': f_type,
        'fx1': round(fx1, 3), 'fx2': round(fx2, 3),
    }

# ── C emission ───────────────────────────────────────────────────────────────

def fvec(vals):
    return '{' + ','.join(f'{v:.3f}f' for v in vals) + '}'

def ivec(vals):
    return '{' + ','.join(str(int(v)) for v in vals) + '}'

def clean_name(fname: str) -> str:
    n = fname.replace('.nnr', '')
    n = re.sub(r'^\d+', '', n)          # drop leading number
    n = n if n else fname.replace('.nnr', '')
    return n[:11]                        # keep short for Move display

def emit_kit(preset: dict) -> str:
    pads = preset['pads'][:8]
    while len(pads) < 8:
        # pad out with a copy of the last (or a default kick)
        pads.append(pads[-1] if pads else {'name': '', 'algo': 'Kick',
                    'fm1': {}, 'fm2': {}, 'f1': {}, 'e1': {}, 'dfx': {}})
    ported = [port_pad(p) for p in pads]

    name = clean_name(preset['file'])
    algo = [ALGO_C[p['algo']] for p in ported]
    note = [p['note'] for p in ported]
    click = [CSMP_C[p['click']] for p in ported]

    # Filter override — content-aware so a filter can never annihilate a
    # voice. An HP/BP/Notch whose cutoff sits above the voice's actual spectral
    # content silences it (a 65 Hz sine through BP@4000, a low kick through
    # HP@9000). For every voice we estimate the top of its content from the
    # fundamental, FM ratio/index, feedback noise, and algo brightness; if the
    # HP/BP/Notch cutoff is above that, we collapse it to a safe LP so the body
    # survives. Genuinely bright voices (hats/cymbals, high FM/feedback) keep
    # their HP/BP.
    def note_hz(n):
        return 440.0 * (2.0 ** ((n - 69) / 12.0))

    fmask = 0
    fcut = []
    ftype = []
    for i, p in enumerate(ported):
        ft = p['f_type']
        fc = max(p['f_cut'], 200)
        want = p['f_on'] and (ft != 0 or fc < 4000)
        if ft != 0:                                    # HP / BP / Notch
            fund = note_hz(p['note'])
            content_top = fund * p['ratio'] * (1.0 + p['fm'] * 8.0)
            if p['fbk'] > 0.3:
                content_top = max(content_top, 3000.0 + p['fbk'] * 6000.0)
            if p['algo'] in (CYMBAL, HAT):
                content_top = max(content_top, 8000.0)
            if fc > content_top * 0.8:                 # filter would kill it
                ft = 0                                  # → safe LP
                fc = max(fc, fund * 4.0, 1200)
                want = fc < 13000
        if want:
            fmask |= (1 << i)
            fcut.append(int(fc))
            ftype.append(ft)
        else:
            fcut.append(0)
            ftype.append(-1)

    # Global FX from Razz cells
    fx = preset.get('fx', {})
    rev = fx.get('reverb', {})
    dly = fx.get('delay', {})
    rev_decay = max(0.0, min(1.0, rev.get('decay', 500) / 1000.0)) if rev else 0.5
    rev_damp = max(0.0, min(1.0, rev.get('damping', 500) / 1000.0)) if rev else 0.5
    dly_fdbk = max(0.0, min(0.9, dly.get('feedback', 300) / 1000.0)) if dly else 0.3
    dly_pp = int(dly.get('delaypingpong', 0)) if dly else 0

    # Kit-wide sends: use the median of per-pad sends so returns are audible
    fx1s = sorted(p['fx1'] for p in ported)[4]
    fx2s = sorted(p['fx2'] for p in ported)[4]
    rev_mix = round(min(0.35, fx2s * 1.3), 3)
    dly_mix = round(min(0.30, fx1s * 1.3), 3)

    lines = []
    lines.append(f'{{ .name="{name}",')
    lines.append(f'  .algo={{{",".join(algo)}}}, .note={ivec(note)},')
    lines.append(f'  .click={{{",".join(click)}}},')
    lines.append(f'  .ratio={fvec(p["ratio"] for p in ported)},')
    lines.append(f'  .fbk  ={fvec(p["fbk"] for p in ported)},')
    lines.append(f'  .dec  ={fvec(p["dec"] for p in ported)},')
    lines.append(f'  .pe   ={fvec(p["pe"] for p in ported)},')
    lines.append(f'  .fm   ={fvec(p["fm"] for p in ported)},')
    lines.append(f'  .body ={fvec(p["body"] for p in ported)},')
    lines.append(f'  .reso ={fvec(p["reso"] for p in ported)},')
    lines.append(f'  .drive={fvec(p["drive"] for p in ported)},')
    if fmask:
        lines.append(f'  .filter_mask=0x{fmask:02X}, .f1_cut={ivec(fcut)}, .f1_type={ivec(ftype)},')
    # Per-voice bit/rate only if any nonzero
    if any(p['bit'] > 0.02 for p in ported):
        lines.append(f'  .bit={fvec(p["bit"] for p in ported)},')
    if any(p['rate'] > 0.02 for p in ported):
        lines.append(f'  .rate={fvec(p["rate"] for p in ported)},')
    if fx1s > 0.02:
        lines.append(f'  .fx1_send={fx1s:.3f}f,')
    if fx2s > 0.02:
        lines.append(f'  .fx2_send={fx2s:.3f}f,')
    # Kit B variation — mild pitch/decay shift for a usable morph gradient
    lines.append(f'  .b_pitch_off=3, .b_dec_scale=0.7f, .b_drive_add=0.10f,')
    fxparts = []
    if rev_mix > 0.01:
        fxparts.append(f'.rev_mix={rev_mix:.3f}f')
        fxparts.append(f'.rev_decay={rev_decay:.3f}f')
    if dly_mix > 0.01:
        fxparts.append(f'.dly_mix={dly_mix:.3f}f')
        fxparts.append(f'.dly_fdbk={dly_fdbk:.3f}f')
        if dly_pp:
            fxparts.append('.dly_pp=1')
    if fxparts:
        lines.append('  ' + ', '.join(fxparts) + ' },')
    else:
        # ensure the entry closes
        lines[-1] = lines[-1].rstrip(',') + ' },'
    return '\n'.join(lines)


def is_pure_sample(preset: dict) -> bool:
    pads = preset.get('pads', [])
    if not pads:
        return True
    return all(p.get('algo') == 'Sample' for p in pads)


def main():
    presets = json.load(open(HERE / '_all_presets.json', encoding='utf-8'))
    # Skip pure-sample presets; Forge is synthetic.
    usable = [p for p in presets if 'error' not in p and not is_pure_sample(p)]
    # Take up to 64, preferring the factory-numbered ones (already curated).
    selected = usable[:64]
    print(f'Usable non-sample presets: {len(usable)}; selecting {len(selected)}')

    body = []
    body.append('/* AUTO-GENERATED by dsp_refs/razz_presets/port_to_forge.py')
    body.append(' * Razzmatazz factory presets ported to Forge kit slots 64+.')
    body.append(' * Do not edit by hand — re-run the porter to regenerate. */')
    body.append('')
    body.append(f'#define NUM_RAZZ_KITS {len(selected)}')
    body.append('')
    body.append('static const char *RAZZ_KIT_NAMES[NUM_RAZZ_KITS] __attribute__((unused)) = {')
    names = [clean_name(p['file']) for p in selected]
    for i in range(0, len(names), 5):
        row = ', '.join(f'"{n}"' for n in names[i:i+5])
        body.append(f'    {row},')
    body.append('};')
    body.append('')
    body.append('static const fk_compact_t FACTORY_KITS_RAZZ[NUM_RAZZ_KITS] = {')
    for p in selected:
        body.append(emit_kit(p))
    body.append('};')
    body.append('')

    OUT.write_text('\n'.join(body), encoding='utf-8')
    print(f'Wrote {OUT} ({len(selected)} kits)')
    # Sanity: role distribution of ported voices
    from collections import Counter
    roles = Counter()
    for p in selected:
        for pad in p['pads'][:8]:
            a, _ = choose_algo(pad)
            roles[ALGO_C[a]] += 1
    print('Ported voice algo distribution:', dict(roles))


if __name__ == '__main__':
    main()
