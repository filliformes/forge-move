#!/usr/bin/env python3
"""Port Sonic Potions LXR .SND patches into Forge fk_compact_t C literals.

Reads the 9 freely-shareable Björn Fogelberg TR-707/808/909 patches
(P001-P009.SND) and emits ../../src/dsp/lxr_kits.h — FACTORY_KITS_LXR[9].

The .SND byte layout is documented in ../lxr_snd_format.md (offsets 8-222,
fully verified). Each file = 8-byte name + flat 1-byte/param dump. We read the
6 LXR voices and map them onto Forge's 8-voice native layout:

    Forge V1-4 = Drum, V5 = Snare, V6 = Cymbal, V7-8 = Hat

    Forge0 (Drum)   <- LXR1 (kick)
    Forge1 (Drum)   <- LXR2
    Forge2 (Drum)   <- LXR3
    Forge3 (Drum)   <- LXR2  (dup: Forge has 4 drum slots, LXR has 3)
    Forge4 (Snare)  <- LXR4
    Forge5 (Cymbal) <- LXR5
    Forge6 (Hat)    <- LXR6 (closed decay, off 68)
    Forge7 (Hat)    <- LXR6 (open decay, off 69)

The TR patches lean on loaded WAV SAMPLES for many voices (Forge has no
sampler), so this is a SYNTHESISED reinterpretation: the LXR patch's pitch,
filter (type/cutoff/reso/drive), envelope, FM and mix settings drive Forge's
own FM/subtractive engine. Result = 808/909/707-flavoured Forge kits.
"""
import os, struct, math

SND_DIR = "/e/HARDWARE SYNTHS & DRUM MACHINE PATCHES & FILES/LXR-02/TR-patches-for-LXR"
# Fallback Windows path if the /e mount isn't present:
if not os.path.isdir(SND_DIR):
    SND_DIR = r"E:\HARDWARE SYNTHS & DRUM MACHINE PATCHES & FILES\LXR-02\TR-patches-for-LXR"

FILES = [f"P00{i}" for i in range(1, 10)]

# Forge algo / click / filter enum values (must match forge.c)
DRUM, SNARE, CYMBAL, HAT, WILD = 0, 1, 2, 3, 4
CSMP_NONE, CSMP_KICK, CSMP_RIM, CSMP_HAT, CSMP_CLAP, CSMP_TOM, CSMP_SNAP = 0, 1, 2, 3, 4, 5, 6
FILT_LP, FILT_HP, FILT_BP, FILT_BPU, FILT_NOTCH, FILT_PEAK, FILT_COMBP, FILT_COMBN, FILT_LP2 = range(9)

ALGO_C  = {DRUM:"ALGO_DRUM", SNARE:"ALGO_SNARE", CYMBAL:"ALGO_CYMBAL", HAT:"ALGO_HAT", WILD:"ALGO_WILD"}
CSMP_C  = {CSMP_NONE:"CSMP_NONE", CSMP_KICK:"CSMP_KICK", CSMP_RIM:"CSMP_RIM", CSMP_HAT:"CSMP_HAT",
           CSMP_CLAP:"CSMP_CLAP", CSMP_TOM:"CSMP_TOM", CSMP_SNAP:"CSMP_SNAP"}
FILT_C  = {FILT_LP:"FILT_LP", FILT_HP:"FILT_HP", FILT_BP:"FILT_BP", FILT_BPU:"FILT_BPU",
           FILT_NOTCH:"FILT_NOTCH", FILT_PEAK:"FILT_PEAK", FILT_LP2:"FILT_LP2"}

# LXR filter type (0-7) -> Forge filter type; 7 = "off" -> LP wide-open
LXR_FILT = {0:FILT_LP, 1:FILT_HP, 2:FILT_BP, 3:FILT_BPU, 4:FILT_NOTCH, 5:FILT_PEAK, 6:FILT_LP2, 7:FILT_LP}

def clamp(x, lo, hi): return lo if x < lo else hi if x > hi else x

# ---- .SND field readers (file_offset = 8 + enum_index) ----
def rd(buf, off): return buf[off]

def voice_bytes(buf, n):
    """Return a dict of the sound-relevant raw bytes for LXR voice n (1..6)."""
    wave_off = {1:9, 2:10, 3:11, 4:12, 5:14, 6:15}[n]
    d = dict(
        wave   = rd(buf, wave_off),
        coarse = rd(buf, 16 + 2*(n-1)),
        fine   = rd(buf, 17 + 2*(n-1)),
        filtF  = rd(buf, 45 + (n-1)),
        reso   = rd(buf, 51 + (n-1)),
        ftype  = rd(buf, 199 + (n-1)),
        fdrv   = rd(buf, 136 + (n-1)),
        velA   = rd(buf, 57 + 2*(n-1)),
        vslope = rd(buf, 70 + (n-1)),
        vol    = rd(buf, 96 + (n-1)),
        pan    = rd(buf, [102,103,104,107,108,109][n-1]),
        decim  = rd(buf, 116 + (n-1)),
        transV = rd(buf, 205 + (n-1)),
    )
    # decay: v1-5 at 58,60,62,64,66 ; v6 has closed(68)+open(69)
    if n < 6:
        d['velD'] = rd(buf, 58 + 2*(n-1))
    else:
        d['velD_closed'] = rd(buf, 68)
        d['velD_open']   = rd(buf, 69)
    # FM only voices 1-3
    if n <= 3:
        d['fmAmt']  = rd(buf, 90 + 2*(n-1))
        d['fmFreq'] = rd(buf, 91 + 2*(n-1))
    else:
        d['fmAmt'] = d['fmFreq'] = 0
    # pitch env only voices 1-4
    if n <= 4:
        d['pSlope'] = rd(buf, 86 + (n-1))
        d['modEG']  = rd(buf, 78 + (n-1))
        d['modAmt'] = rd(buf, 82 + (n-1))
    else:
        d['pSlope'] = d['modEG'] = d['modAmt'] = 0
    return d

# ---- value conversions LXR (0-127) -> Forge ----
BASE_NOTE = {DRUM:36, SNARE:52, CYMBAL:64, HAT:62}

def to_note(coarse, algo):
    return int(clamp(round(BASE_NOTE[algo] + (coarse - 30) * 0.5), 20, 96))

def to_hz(c):                    # exp 30..16000 Hz
    return int(round(30.0 * (16000.0/30.0) ** (c/127.0)))

def to_res(r):                   # 0.7 .. 7.0
    return round(0.7 + (r/127.0) * 6.3, 3)

def to_dec(v):                   # exp 0.02 .. ~3.0 s
    return round(0.02 * (150.0 ** (v/127.0)), 3)

def to_unit(v):                  # 0..1
    return round(v/127.0, 3)

def to_ratio(fmfreq):            # 0.5 .. 8.0
    return round(0.5 + (fmfreq/127.0) * 7.5, 3)

def drum_pe(v):
    """Pitch-env amount for a drum voice from its LXR pitch slope."""
    if v['pSlope'] > 4:
        return round(clamp(0.4 + (v['pSlope']/127.0) * 0.5, 0.0, 0.9), 3)
    return 0.30

# Forge voice map: (lxr_voice, forge_algo, decay_key)
VMAP = [
    (1, DRUM,   'velD'),
    (2, DRUM,   'velD'),
    (3, DRUM,   'velD'),
    (2, DRUM,   'velD'),          # dup
    (4, SNARE,  'velD'),
    (5, CYMBAL, 'velD'),
    (6, HAT,    'velD_closed'),
    (6, HAT,    'velD_open'),
]

def click_smp_for(algo, is_kick):
    if algo == DRUM:   return CSMP_KICK if is_kick else CSMP_TOM
    if algo == SNARE:  return CSMP_SNAP
    if algo == HAT:    return CSMP_HAT
    return CSMP_NONE   # cymbal

def build_kit(name, buf):
    """Return a dict of 8-length Forge arrays for one kit."""
    vv = {n: voice_bytes(buf, n) for n in range(1, 7)}
    K = dict(algo=[], note=[], click=[], ratio=[], fbk=[], dec=[], pe=[], fm=[],
             body=[], reso=[], drive=[], f1_cut=[], f1_type=[], f1_res=[], f1_drv=[],
             click_lvl=[], lvl=[], rate=[])
    for fi, (ln, algo, deckey) in enumerate(VMAP):
        v = vv[ln]
        is_kick = (fi == 0)
        off = (v['ftype'] == 7)                       # LXR "off" filter
        # decay
        dec = to_dec(v[deckey])
        # note
        note = to_note(v['coarse'], algo)
        # FM (drums 1-3 carry it; snare gets a default osc/noise mix)
        if algo == DRUM and v['fmAmt'] is not None and v['fmFreq'] > 0:
            fm    = round(to_unit(v['fmAmt']), 3)
            ratio = to_ratio(v['fmFreq'])
        elif algo == SNARE:
            fm, ratio = 0.5, 1.0
        else:
            fm, ratio = (0.3 if algo in (CYMBAL, HAT) else 0.0), 1.0
        # pitch env
        pe = drum_pe(v) if algo == DRUM else (0.30 if algo == SNARE else 0.05)
        # filter
        ftype = LXR_FILT[v['ftype']]
        cut   = 16000 if off else to_hz(v['filtF'])
        res   = 0.0 if off else to_res(v['reso'])
        fdrv  = to_unit(v['fdrv'])
        # transient / click
        tv = v['transV']
        if tv < 4:
            click = CSMP_NONE; clvl = 0.0
        else:
            click = click_smp_for(algo, is_kick); clvl = round(to_unit(tv), 3)
        K['algo'].append(algo)
        K['note'].append(note)
        K['click'].append(click)
        K['ratio'].append(ratio)
        K['fbk'].append(0.0)
        K['dec'].append(clamp(dec, 0.02, 3.5))
        K['pe'].append(pe)
        K['fm'].append(fm)
        K['body'].append(0.30)
        K['reso'].append(0.05)
        K['drive'].append(0.0)
        K['f1_cut'].append(cut)
        K['f1_type'].append(ftype)
        K['f1_res'].append(res)
        K['f1_drv'].append(fdrv)
        K['click_lvl'].append(clvl)
        # Map LXR vol (0-127) into an AUDIBLE range 0.40..1.00 rather than raw
        # 0..1 — the TR patches set some voices as near-silent ghost hits, which
        # read as "broken/weak" in a factory kit. This keeps relative balance
        # while guaranteeing every ported voice is audible.
        K['lvl'].append(round(0.40 + 0.60 * (v['vol'] / 127.0), 3))
        # NOTE: LXR decimation reads 127 = "no decimation" (clean) across these
        # kits, so we deliberately do NOT map it to Forge rate-crush (127 would
        # otherwise crush everything). Left clean.
        K['rate'].append(0.0)
    return K

def fa(vals, fmt="{:.3f}f"):
    return "{" + ",".join(fmt.format(x) if isinstance(x, float) else str(x) for x in vals) + "}"

def emit(name, disp, K):
    L = [f'{{ .name="{disp}",']
    L.append("  .algo={%s}, .note={%s}," % (",".join(ALGO_C[a] for a in K['algo']),
                                            ",".join(str(n) for n in K['note'])))
    L.append("  .click={%s}," % ",".join(CSMP_C[c] for c in K['click']))
    L.append("  .ratio=%s," % fa(K['ratio']))
    L.append("  .dec  =%s," % fa(K['dec']))
    L.append("  .pe   =%s," % fa(K['pe']))
    L.append("  .fm   =%s," % fa(K['fm']))
    L.append("  .body =%s," % fa(K['body']))
    L.append("  .reso =%s," % fa(K['reso']))
    L.append("  .filter_mask=0xFF, .f1_cut={%s}, .f1_type={%s}," %
             (",".join(str(c) for c in K['f1_cut']), ",".join(FILT_C[t] for t in K['f1_type'])))
    L.append("  .f1_res=%s, .f1_drv=%s," % (fa(K['f1_res']), fa(K['f1_drv'])))
    L.append("  .click_lvl=%s," % fa(K['click_lvl']))
    L.append("  .lvl=%s," % fa(K['lvl']))
    if any(r > 0 for r in K['rate']):
        L.append("  .rate=%s," % fa(K['rate']))
    L.append("  .b_pitch_off=5, .b_dec_scale=1.4f, .b_drive_add=0.10f },")
    return "\n".join(L)

def main():
    names, entries = [], []
    for f in FILES:
        path = os.path.join(SND_DIR, f + ".SND")
        with open(path, "rb") as fh:
            buf = fh.read()
        disp = buf[0:8].decode("latin-1").rstrip().replace("/", "_")
        K = build_kit(disp, buf)
        names.append(disp)
        entries.append(emit(f, disp, K))
        print(f"  {f}: {disp!r} -> 8 voices")
    out = os.path.join(os.path.dirname(__file__), "..", "..", "src", "dsp", "lxr_kits.h")
    out = os.path.normpath(out)
    with open(out, "w", newline="\n") as fh:
        fh.write("/* AUTO-GENERATED by dsp_refs/lxr_presets/port_lxr.py\n")
        fh.write(" * LXR (Sonic Potions) TR-707/808/909 patches ported to Forge kit slots 128+.\n")
        fh.write(" * Free patch set by Bjorn Fogelberg (2017); .SND format reverse-engineered\n")
        fh.write(" * clean-room from the GPL LXR firmware (see dsp_refs/lxr_snd_format.md).\n")
        fh.write(" * Synthesised reinterpretation — Forge has no sampler, so the LXR patch's\n")
        fh.write(" * pitch/filter/envelope/FM settings drive Forge's own engine.\n")
        fh.write(" * Do not edit by hand — re-run the porter to regenerate. */\n\n")
        fh.write("#define NUM_LXR_KITS %d\n\n" % len(entries))
        fh.write("static const char *LXR_KIT_NAMES[NUM_LXR_KITS] __attribute__((unused)) = {\n    ")
        fh.write(", ".join('"%s"' % n for n in names))
        fh.write("\n};\n\n")
        fh.write("static const fk_compact_t FACTORY_KITS_LXR[NUM_LXR_KITS] = {\n")
        fh.write("\n".join(entries))
        fh.write("\n};\n")
    print(f"wrote {out} ({len(entries)} kits)")

if __name__ == "__main__":
    main()
