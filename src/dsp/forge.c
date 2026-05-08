/**
 * Forge — 8-voice FM/subtractive hybrid drum synthesiser for Ableton Move
 * Author: Vincent Fillion
 * License: MIT
 *
 * Architecture: 8 fixed-allocation drum voices with 5 selectable algorithms
 * (Drum / Snare / Cymbal / Hat / Wild). Each voice has a Kit A and Kit B
 * parameter bank; the global Morph control interpolates between them.
 *
 * Pad layout (Move 16-pad Drum Kit template):
 *   Pads  1-8   → voices 1-8 with morphed Kit A↔B state
 *   Pads  9-16  → voices 1-8 with Kit B reference (untouched)
 *
 * API: plugin_api_v2_t
 * Audio: 44100Hz, 128 frames/block, stereo interleaved int16 output
 *
 * v0.1 scope: 5 algorithms × 8 voices, single 2-op FM (3-op for Cymbal/Hat),
 * body wavefolder, per-voice resonator, Base-Width pre-filter + dual SVF
 * (Single/Per-Osc/Serial/Parallel routing) with Comb+/Comb- modes,
 * curved AD envelopes with repeat, cross-voice LFO routing, 3 concurrent
 * FX buses, Phasma-style Save Kit persistence to /data.
 *
 * This file is the v0.1 scaffold: full chain_params + ui_hierarchy strings,
 * Kit A/B parameter banks, save/load skeleton, cv_* current-voice routing.
 * The real algorithmic synthesis is filled in during the Code stage after
 * /dsp-fetch sources the FM/resonator/wavefolder/SVF building blocks.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#define SAMPLE_RATE       44100.0f
#define SR_INV            (1.0f / SAMPLE_RATE)
#define TWO_PI            6.28318530717958647692f
#define NUM_VOICES        8
#define NUM_KITS          64
#define BLOCK_SIZE        128
#define FIRST_PAD_NOTE    36

#define KITS_FILE_PATH    "/data/UserData/schwung/forge_kits.dat"
#define KITS_SAVE_MAGIC   0x46524745u  /* 'FRGE' */
#define KITS_SAVE_VER     1u

/* ────────────────────────────────────────────────────────────────────────────
 * Algorithms
 * ──────────────────────────────────────────────────────────────────────────── */

enum {
    ALGO_DRUM = 0,
    ALGO_SNARE,
    ALGO_CYMBAL,
    ALGO_HAT,
    ALGO_WILD,
    NUM_ALGOS
};

static const char *ALGO_NAMES[NUM_ALGOS] = { "Drum", "Snare", "Cymbal", "Hat", "Wild" };

/* ────────────────────────────────────────────────────────────────────────────
 * Per-voice parameter bank (one Kit A copy + one Kit B copy per voice)
 * ──────────────────────────────────────────────────────────────────────────── */

typedef struct {
    /* Voice setup */
    int   algo;        /* 0=Drum 1=Snare 2=Cymbal 3=Hat 4=Wild */
    int   choke;       /* 0=None 1=A 2=B 3=C 4=D */
    int   bus;         /* 0=Main 1=Aux1 2=Aux2 3=FXOnly */
    int   poly;        /* 0=OneShot 1=Legato 2=Retrig */
    int   mute;
    int   midi_note;
    float voice_lvl;
    float voice_tune;
    float glide;
    float vsens;

    /* Macro page (8 algo-routed values 0..1) */
    float m[8];

    /* Osc / FM */
    int   wave;        /* enum: Sine/Tri/Saw/Sqr/Noise */
    float ratio_c, ratio_f;
    float detune;
    float level;
    float phase;
    float pwm;
    float fbk;
    int   op_select;   /* A/B/C for 3-op algos */
    int   click_type;  /* 0=Sample 1=Impulse 2=Phase */
    int   click_smp;   /* 0..6 (None/Kick/Rim/Hat/Clap/Tom/Snap) */
    float click_lvl;
    float click_dec;
    int   xfm;         /* Cross-FM on/off (Wild only) */

    /* Filter (dual + Base-Width pre) */
    int   f1_cut, f2_cut, bw_cut;     /* Hz */
    float f1_res, f2_res;
    int   f1_type, f2_type;           /* enum: LP/HP/BP/BPu/Notch/Peak/Comb+/Comb- */
    float f1_drv, f2_drv;
    float bw_w;
    int   bw_on;
    int   routing;                     /* 0=Single 1=Per-Osc 2=Serial 3=Parallel */
    float bit, rate;                   /* per-voice crush */
    float kt1, kt2;
    float e1_to_cut, e2_to_cut;

    /* Envelopes */
    float e1_atk, e1_dec, e1_crv, e1_rep, e1_rep_rate;
    float e2_atk, e2_dec, e2_crv;
    int   e2_dest;                     /* 0=FM 1=Filter 2=Reso 3=Pan 4=Mod */
    float pe_amt, pe_dec, pe_crv;
    int   pe_dest;                     /* 0=Pitch 1=FM 2=Filter 3=Reso */
    float v_e1_lvl, v_e1_t, v_e2_amt;

    /* Mod / LFO */
    int   lfo_w, lfo_s;                /* waveform / sync */
    float lfo_r, lfo_d, lfo_p;
    int   lfo_pol, lfo_rt;
    int   xlfo_src;                    /* 0=Self 1..8=V1..V8 */
    int   trig_rst;                    /* 0=None 1..8=V1..V8 */
    int   mod_src, mod_dest;
    float mod_dpth;
    int   mod_crv;

    /* Mixer feeders (the global Mix page also exposes v*_lvl/pan/fx1/fx2 — those
       overlay these for kit-bank ops but the per-voice values above are the
       authoritative ones during morph). */
    float pan;
    float fx1_send, fx2_send;
} voice_bank_t;

/* ────────────────────────────────────────────────────────────────────────────
 * Live voice state (envelopes, oscillator phase, etc.)
 * ──────────────────────────────────────────────────────────────────────────── */

typedef struct {
    int   active;
    float velocity;

    /* Envelopes */
    float e1_v, e2_v, pe_v;            /* current envelope values */
    float e1_t, e2_t, pe_t;            /* time accumulators */
    int   e1_stage, e2_stage, pe_stage;

    /* Oscillator phase */
    float phase_a, phase_b, phase_c;
    float mod_phase_a, mod_phase_b;

    /* Resonator state (feedback-comb, FB > 100% via tanh saturation) */
    float reso_buf[256];
    int   reso_idx;
    float reso_fb_state;

    /* Filter state — Chamberlin SVF, 2 instances + Base-Width pre */
    float svf1_lp, svf1_bp, svf1_hp;
    float svf2_lp, svf2_bp, svf2_hp;
    float bw_lp, bw_bp;

    /* LFO */
    float lfo_v, lfo_phase;

    /* Bit/rate crush state */
    float crush_held;
    float rate_accum;
} voice_state_t;

/* ────────────────────────────────────────────────────────────────────────────
 * Kit slot — Kit A + Kit B + macros + FX state
 * ──────────────────────────────────────────────────────────────────────────── */

typedef struct {
    voice_bank_t kit_a[NUM_VOICES];
    voice_bank_t kit_b[NUM_VOICES];
    /* FX state stored per-kit too so morphing across patterns can rewire FX */
    float rev_mix, rev_decay, rev_size, rev_predelay, rev_damping;
    int   rev_type;
    float dly_mix, dly_rate, dly_fdbk, dly_tone, dly_bpf_w;
    int   dly_bpf_cut, dly_pp, dly_sync;
    float cho_mix, cho_rate, cho_depth, cho_width, cho_tone, cho_fb;
    int   cho_voices;
} kit_slot_t;

/* ────────────────────────────────────────────────────────────────────────────
 * Instance
 * ──────────────────────────────────────────────────────────────────────────── */

typedef struct {
    /* Per-kit data — built-ins overlaid by user saves */
    kit_slot_t kits[NUM_KITS];

    /* Selected kit + morph state */
    int   current_kit;
    float morph;
    int   morph_src, morph_curve;

    /* Currently selected voice (for cv_* routing). Lower row = Kit A context,
       upper row = Kit B context. */
    int   current_voice;
    int   current_kit_context;   /* 0 = Kit A, 1 = Kit B */

    /* Live morphed state per voice (interpolated each block) */
    voice_bank_t live[NUM_VOICES];
    voice_state_t voice[NUM_VOICES];

    /* All-Decay multiplier (1..4×, snapshot-relative), applied on top of live */
    float all_decay_mult;

    /* Save Kit pattern (Phasma-style) */
    int   save_kit_state;        /* 0=Play 1=Save (auto-resets) */

    /* Trigger params (auto-revert display) */
    int   tr_rnd_kit, tr_rnd_voice, tr_rnd_pitch, tr_rnd_pan;
    int   tr_all_mono, tr_init_decay, tr_init_freq;
    int   tr_copy_a_b, tr_copy_b_a, tr_swap_ab, tr_rnd_b_from_a;
    int   tr_cv_init;

    /* Mix page (overlay) */
    float v_lvl[NUM_VOICES];

    /* FX state (live, also persisted in kit_slot_t) */
    float rev_mix, rev_decay, rev_size, rev_predelay, rev_damping;
    int   rev_type;
    float dly_mix, dly_rate, dly_fdbk, dly_tone, dly_bpf_w;
    int   dly_bpf_cut, dly_pp, dly_sync;
    float cho_mix, cho_rate, cho_depth, cho_width, cho_tone, cho_fb;
    int   cho_voices;

    /* General page */
    float comp, drive, bit, rate;
    float eq_lo, eq_mid, eq_hi, master;
    int   drive_type;
    float lo_freq, mid_freq, hi_freq, q_lo, q_mid, q_hi;
    int   limiter;
    float master_tune;
    int   midi_ch;
    int   same_freq;

    /* RNG */
    uint32_t rng;
} forge_instance_t;

/* ────────────────────────────────────────────────────────────────────────────
 * Helpers
 * ──────────────────────────────────────────────────────────────────────────── */

static inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

static inline int clampi(int x, int lo, int hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

static inline float randf(uint32_t *rng) {
    *rng = (*rng) * 1664525u + 1013904223u;
    return ((*rng) >> 8) / 16777216.0f;
}

static inline float fast_tanh(float x) {
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

static inline float note_to_freq(int note) {
    return 440.0f * powf(2.0f, (note - 69) / 12.0f);
}

/* ────────────────────────────────────────────────────────────────────────────
 * Default voice bank (used to init each kit slot)
 * ──────────────────────────────────────────────────────────────────────────── */

static void voice_bank_init_default(voice_bank_t *vb, int voice_index) {
    memset(vb, 0, sizeof(*vb));
    /* Default algorithm mapping: V1-V4=Drum, V5=Snare, V6=Cymbal, V7-V8=Hat */
    if (voice_index < 4)      vb->algo = ALGO_DRUM;
    else if (voice_index == 4) vb->algo = ALGO_SNARE;
    else if (voice_index == 5) vb->algo = ALGO_CYMBAL;
    else                       vb->algo = ALGO_HAT;

    vb->choke      = (voice_index >= 6) ? 1 : 0;  /* hats share group A */
    vb->bus        = 0;
    vb->poly       = 0;
    vb->voice_lvl  = 0.85f;
    vb->voice_tune = 0.0f;
    vb->glide      = 0.0f;
    vb->vsens      = 0.7f;
    vb->midi_note  = FIRST_PAD_NOTE + voice_index;

    /* Macro defaults */
    for (int i = 0; i < 8; i++) vb->m[i] = 0.5f;

    /* Osc */
    vb->wave      = 0;
    vb->ratio_c   = 1.0f;
    vb->ratio_f   = 0.0f;
    vb->detune    = 0.0f;
    vb->level     = 0.85f;
    vb->phase     = 0.0f;
    vb->pwm       = 0.5f;
    vb->fbk       = 0.0f;
    vb->click_type = 0;
    vb->click_smp = (voice_index < 4) ? 1 : (voice_index == 4) ? 4 : 3;
    vb->click_lvl = 0.5f;
    vb->click_dec = 0.005f;

    /* Filter */
    vb->f1_cut    = 8000;
    vb->f2_cut    = 4000;
    vb->bw_cut    = 1000;
    vb->f1_res    = 0.7f;
    vb->f2_res    = 0.7f;
    vb->f1_type   = 0;  /* LP */
    vb->f2_type   = 0;
    vb->f1_drv    = 0.0f;
    vb->f2_drv    = 0.0f;
    vb->bw_w      = 0.5f;
    vb->bw_on     = 0;
    vb->routing   = 0;  /* Single */

    /* Envelopes */
    vb->e1_atk     = 0.001f;
    vb->e1_dec     = 0.25f;
    vb->e1_crv     = -0.5f;  /* exp */
    vb->e1_rep     = 0.0f;
    vb->e1_rep_rate = 0.05f;
    vb->e2_atk     = 0.001f;
    vb->e2_dec     = 0.15f;
    vb->e2_crv     = -0.5f;
    vb->e2_dest    = 1;       /* Filter */
    vb->pe_amt     = 0.0f;
    vb->pe_dec     = 0.05f;
    vb->pe_crv     = -0.5f;
    vb->pe_dest    = 0;       /* Pitch */
    vb->v_e1_lvl   = 0.5f;

    /* LFO */
    vb->lfo_w     = 0;
    vb->lfo_r     = 1.0f;
    vb->lfo_s     = 0;        /* Free */
    vb->lfo_d     = 0.0f;
    vb->lfo_p     = 0.0f;
    vb->lfo_pol   = 0;        /* Bipolar */
    vb->lfo_rt    = 0;
    vb->xlfo_src  = 0;
    vb->trig_rst  = 0;
    vb->mod_src   = 0;
    vb->mod_dest  = 0;
    vb->mod_dpth  = 0.0f;
    vb->mod_crv   = 0;

    /* Mixer */
    vb->pan       = 0.0f;
    vb->fx1_send  = 0.0f;
    vb->fx2_send  = 0.0f;
}

static void kit_slot_init_default(kit_slot_t *k) {
    memset(k, 0, sizeof(*k));
    for (int i = 0; i < NUM_VOICES; i++) {
        voice_bank_init_default(&k->kit_a[i], i);
        voice_bank_init_default(&k->kit_b[i], i);
    }
    k->rev_decay = 0.5f; k->rev_size = 0.5f; k->rev_predelay = 0.05f; k->rev_damping = 0.5f;
    k->dly_rate = 0.3f; k->dly_fdbk = 0.3f; k->dly_tone = 0.5f; k->dly_bpf_cut = 2000; k->dly_bpf_w = 0.5f;
    k->cho_rate = 0.5f; k->cho_depth = 0.3f; k->cho_width = 0.5f; k->cho_voices = 4;
    k->cho_tone = 0.5f; k->cho_fb = 0.0f;
}

/* ────────────────────────────────────────────────────────────────────────────
 * Morph: interpolate Kit A → Kit B into live[] each block
 * Modulation reads from live[], so envelopes/LFOs work continuously across morph.
 * ──────────────────────────────────────────────────────────────────────────── */

static void morph_voices(forge_instance_t *inst) {
    float t = inst->morph;
    /* Curve shaping (linear/exp/log/S-curve) */
    switch (inst->morph_curve) {
        case 1: t = t * t; break;                      /* exp */
        case 2: t = sqrtf(t); break;                   /* log */
        case 3: t = t * t * (3.0f - 2.0f * t); break;  /* S-curve */
        default: break;                                /* linear */
    }
    kit_slot_t *k = &inst->kits[inst->current_kit];
    for (int v = 0; v < NUM_VOICES; v++) {
        voice_bank_t *a = &k->kit_a[v];
        voice_bank_t *b = &k->kit_b[v];
        voice_bank_t *L = &inst->live[v];
        /* Continuous params interpolate; enums/ints snap to A below 0.5, B above */
        L->algo       = (t < 0.5f) ? a->algo : b->algo;
        L->choke      = (t < 0.5f) ? a->choke : b->choke;
        L->bus        = (t < 0.5f) ? a->bus : b->bus;
        L->poly       = (t < 0.5f) ? a->poly : b->poly;
        L->mute       = (t < 0.5f) ? a->mute : b->mute;
        L->midi_note  = a->midi_note;
        L->voice_lvl  = a->voice_lvl + t * (b->voice_lvl - a->voice_lvl);
        L->voice_tune = a->voice_tune + t * (b->voice_tune - a->voice_tune);
        L->glide      = a->glide + t * (b->glide - a->glide);
        L->vsens      = a->vsens + t * (b->vsens - a->vsens);
        for (int i = 0; i < 8; i++) L->m[i] = a->m[i] + t * (b->m[i] - a->m[i]);
        L->wave       = (t < 0.5f) ? a->wave : b->wave;
        L->ratio_c    = a->ratio_c + t * (b->ratio_c - a->ratio_c);
        L->ratio_f    = a->ratio_f + t * (b->ratio_f - a->ratio_f);
        L->detune     = a->detune + t * (b->detune - a->detune);
        L->level      = a->level + t * (b->level - a->level);
        L->phase      = a->phase + t * (b->phase - a->phase);
        L->pwm        = a->pwm + t * (b->pwm - a->pwm);
        L->fbk        = a->fbk + t * (b->fbk - a->fbk);
        L->click_type = (t < 0.5f) ? a->click_type : b->click_type;
        L->click_smp  = (t < 0.5f) ? a->click_smp : b->click_smp;
        L->click_lvl  = a->click_lvl + t * (b->click_lvl - a->click_lvl);
        L->click_dec  = a->click_dec + t * (b->click_dec - a->click_dec);
        L->f1_cut     = (int)(a->f1_cut + t * (b->f1_cut - a->f1_cut));
        L->f2_cut     = (int)(a->f2_cut + t * (b->f2_cut - a->f2_cut));
        L->bw_cut     = (int)(a->bw_cut + t * (b->bw_cut - a->bw_cut));
        L->f1_res     = a->f1_res + t * (b->f1_res - a->f1_res);
        L->f2_res     = a->f2_res + t * (b->f2_res - a->f2_res);
        L->f1_type    = (t < 0.5f) ? a->f1_type : b->f1_type;
        L->f2_type    = (t < 0.5f) ? a->f2_type : b->f2_type;
        L->f1_drv     = a->f1_drv + t * (b->f1_drv - a->f1_drv);
        L->f2_drv     = a->f2_drv + t * (b->f2_drv - a->f2_drv);
        L->bw_w       = a->bw_w + t * (b->bw_w - a->bw_w);
        L->bw_on      = (t < 0.5f) ? a->bw_on : b->bw_on;
        L->routing    = (t < 0.5f) ? a->routing : b->routing;
        L->e1_atk     = a->e1_atk + t * (b->e1_atk - a->e1_atk);
        L->e1_dec     = a->e1_dec + t * (b->e1_dec - a->e1_dec);
        L->e1_crv     = a->e1_crv + t * (b->e1_crv - a->e1_crv);
        L->e1_rep     = a->e1_rep + t * (b->e1_rep - a->e1_rep);
        L->e2_atk     = a->e2_atk + t * (b->e2_atk - a->e2_atk);
        L->e2_dec     = a->e2_dec + t * (b->e2_dec - a->e2_dec);
        L->e2_crv     = a->e2_crv + t * (b->e2_crv - a->e2_crv);
        L->pe_amt     = a->pe_amt + t * (b->pe_amt - a->pe_amt);
        L->pe_dec     = a->pe_dec + t * (b->pe_dec - a->pe_dec);
        L->lfo_r      = a->lfo_r + t * (b->lfo_r - a->lfo_r);
        L->lfo_d      = a->lfo_d + t * (b->lfo_d - a->lfo_d);
        L->mod_dpth   = a->mod_dpth + t * (b->mod_dpth - a->mod_dpth);
        L->pan        = a->pan + t * (b->pan - a->pan);
        L->fx1_send   = a->fx1_send + t * (b->fx1_send - a->fx1_send);
        L->fx2_send   = a->fx2_send + t * (b->fx2_send - a->fx2_send);
    }
}

/* ────────────────────────────────────────────────────────────────────────────
 * Helpers — get bank pointer for cv_* routing (selects A or B based on context)
 * ──────────────────────────────────────────────────────────────────────────── */

static voice_bank_t *cv_bank(forge_instance_t *inst) {
    kit_slot_t *k = &inst->kits[inst->current_kit];
    return inst->current_kit_context ? &k->kit_b[inst->current_voice]
                                     : &k->kit_a[inst->current_voice];
}

/* ────────────────────────────────────────────────────────────────────────────
 * Save/Load — Phasma-style binary persistence
 * ──────────────────────────────────────────────────────────────────────────── */

typedef struct {
    uint32_t magic, version, count, reserved;
    kit_slot_t kits[NUM_KITS];
} kits_file_t;

static void forge_save_kits(forge_instance_t *inst) {
    FILE *f = fopen(KITS_FILE_PATH, "wb");
    if (!f) return;
    kits_file_t file;
    file.magic = KITS_SAVE_MAGIC;
    file.version = KITS_SAVE_VER;
    file.count = NUM_KITS;
    file.reserved = 0;
    memcpy(file.kits, inst->kits, sizeof(inst->kits));
    fwrite(&file, sizeof(file), 1, f);
    fclose(f);
}

static void forge_load_kits(forge_instance_t *inst) {
    FILE *f = fopen(KITS_FILE_PATH, "rb");
    if (!f) return;
    kits_file_t file;
    int ok = (fread(&file, sizeof(file), 1, f) == 1);
    fclose(f);
    if (!ok || file.magic != KITS_SAVE_MAGIC || file.version != KITS_SAVE_VER || file.count != NUM_KITS) return;
    memcpy(inst->kits, file.kits, sizeof(inst->kits));
}

/* ────────────────────────────────────────────────────────────────────────────
 * Lifecycle
 * ──────────────────────────────────────────────────────────────────────────── */

static void *create_instance(const char *module_dir, const char *json_defaults) {
    (void)module_dir; (void)json_defaults;
    forge_instance_t *inst = calloc(1, sizeof(forge_instance_t));
    if (!inst) return NULL;

    /* Init all 64 kit slots with defaults, then overlay user saves */
    for (int i = 0; i < NUM_KITS; i++) kit_slot_init_default(&inst->kits[i]);
    forge_load_kits(inst);

    inst->current_kit = 0;
    inst->morph = 0.0f;
    inst->morph_src = 0;
    inst->morph_curve = 0;
    inst->current_voice = 0;
    inst->current_kit_context = 0;
    inst->all_decay_mult = 1.0f;
    inst->save_kit_state = 0;
    inst->rng = 0xC0FFEEu;

    /* Mix overlay defaults */
    for (int i = 0; i < NUM_VOICES; i++) inst->v_lvl[i] = 0.85f;

    /* FX defaults */
    inst->rev_decay = 0.5f; inst->rev_size = 0.5f; inst->rev_predelay = 0.05f; inst->rev_damping = 0.5f;
    inst->dly_rate = 0.3f; inst->dly_fdbk = 0.3f; inst->dly_tone = 0.5f;
    inst->dly_bpf_cut = 2000; inst->dly_bpf_w = 0.5f;
    inst->cho_rate = 0.5f; inst->cho_depth = 0.3f; inst->cho_width = 0.5f;
    inst->cho_voices = 4; inst->cho_tone = 0.5f; inst->cho_fb = 0.0f;

    /* General defaults */
    inst->master = 0.85f;
    inst->lo_freq = 200.0f; inst->mid_freq = 1000.0f; inst->hi_freq = 5000.0f;
    inst->q_lo = 0.7f; inst->q_mid = 0.7f; inst->q_hi = 0.7f;
    inst->limiter = 1;
    inst->midi_ch = 1;
    inst->same_freq = 440;

    morph_voices(inst);
    return inst;
}

static void destroy_instance(void *instance) {
    free(instance);
}

/* ────────────────────────────────────────────────────────────────────────────
 * MIDI
 * ──────────────────────────────────────────────────────────────────────────── */

static void trigger_voice(forge_instance_t *inst, int idx, float vel) {
    if (idx < 0 || idx >= NUM_VOICES) return;
    voice_state_t *vs = &inst->voice[idx];
    voice_bank_t *vb = &inst->live[idx];

    /* Choke groups: kill voices in the same non-zero group */
    if (vb->choke > 0) {
        for (int j = 0; j < NUM_VOICES; j++) {
            if (j == idx) continue;
            if (inst->live[j].choke == vb->choke) {
                inst->voice[j].active = 0;
                inst->voice[j].e1_v = 0.0f;
            }
        }
    }

    vs->active = 1;
    vs->velocity = vel;
    vs->e1_v = 1.0f;       /* Start at peak — exp decay */
    vs->e2_v = 1.0f;
    vs->pe_v = 1.0f;
    vs->e1_t = 0.0f;
    vs->e2_t = 0.0f;
    vs->pe_t = 0.0f;
    vs->phase_a = vb->phase;
    vs->phase_b = 0.0f;
    vs->phase_c = 0.0f;
    vs->lfo_phase = vb->lfo_p;
}

static void on_midi(void *instance, const uint8_t *msg, int len, int source) {
    (void)source;
    forge_instance_t *inst = (forge_instance_t *)instance;
    if (!inst || len < 1) return;

    uint8_t status = msg[0] & 0xF0;
    uint8_t note   = (len >= 2) ? msg[1] : 0;
    uint8_t vel    = (len >= 3) ? msg[2] : 0;

    if (status == 0x90 && vel > 0) {
        /* Move pads send MIDI from FIRST_PAD_NOTE upward. The first 8 pads are
           the morphed Kit A row, the next 8 are the Kit B reference row. */
        int pad = (int)note - FIRST_PAD_NOTE;
        if (pad >= 0 && pad < 16) {
            int voice_idx = pad % NUM_VOICES;
            inst->current_voice = voice_idx;
            inst->current_kit_context = (pad >= NUM_VOICES) ? 1 : 0;
            trigger_voice(inst, voice_idx, vel / 127.0f);
        }
    }
    /* Pitch bend / aftertouch handled in v0.2 */
}

/* ────────────────────────────────────────────────────────────────────────────
 * Parameters
 * ──────────────────────────────────────────────────────────────────────────── */

/* Map cv_* keys to a function that reads/writes the active voice bank */
static int handle_cv_set(forge_instance_t *inst, const char *key, const char *val) {
    voice_bank_t *vb = cv_bank(inst);
    float f = (float)atof(val);
    int   n = atoi(val);

    /* Macros */
    if (key[0] == 'c' && key[1] == 'v' && key[2] == '_' && key[3] == 'm' && key[4] >= '1' && key[4] <= '8' && key[5] == 0) {
        int idx = key[4] - '1'; vb->m[idx] = clampf(f, 0.0f, 1.0f); return 1;
    }
    if (strcmp(key, "cv_wave") == 0)       { vb->wave = clampi(n, 0, 4); return 1; }
    if (strcmp(key, "cv_ratio_c") == 0)    { vb->ratio_c = clampf(f, 0.5f, 16.0f); return 1; }
    if (strcmp(key, "cv_ratio_f") == 0)    { vb->ratio_f = clampf(f, -12.0f, 12.0f); return 1; }
    if (strcmp(key, "cv_detune") == 0)     { vb->detune = clampf(f, -50.0f, 50.0f); return 1; }
    if (strcmp(key, "cv_level") == 0)      { vb->level = clampf(f, 0.0f, 1.0f); return 1; }
    if (strcmp(key, "cv_phase") == 0)      { vb->phase = clampf(f, 0.0f, 1.0f); return 1; }
    if (strcmp(key, "cv_pwm") == 0)        { vb->pwm = clampf(f, 0.0f, 1.0f); return 1; }
    if (strcmp(key, "cv_fbk") == 0)        { vb->fbk = clampf(f, 0.0f, 1.0f); return 1; }
    if (strcmp(key, "cv_op") == 0)         { vb->op_select = clampi(n, 0, 2); return 1; }
    if (strcmp(key, "cv_click_type") == 0) { vb->click_type = clampi(n, 0, 2); return 1; }
    if (strcmp(key, "cv_click_smp") == 0)  { vb->click_smp = clampi(n, 0, 6); return 1; }
    if (strcmp(key, "cv_click_lvl") == 0)  { vb->click_lvl = clampf(f, 0.0f, 1.0f); return 1; }
    if (strcmp(key, "cv_click_dec") == 0)  { vb->click_dec = clampf(f, 0.001f, 0.5f); return 1; }
    if (strcmp(key, "cv_xfm") == 0)        { vb->xfm = (n != 0); return 1; }

    if (strcmp(key, "cv_f1_cut") == 0)     { vb->f1_cut = clampi(n, 20, 20000); return 1; }
    if (strcmp(key, "cv_f1_res") == 0)     { vb->f1_res = clampf(f, 0.5f, 20.0f); return 1; }
    if (strcmp(key, "cv_f1_type") == 0)    { vb->f1_type = clampi(n, 0, 7); return 1; }
    if (strcmp(key, "cv_bw_cut") == 0)     { vb->bw_cut = clampi(n, 20, 20000); return 1; }
    if (strcmp(key, "cv_bw_w") == 0)       { vb->bw_w = clampf(f, 0.0f, 1.0f); return 1; }
    if (strcmp(key, "cv_routing") == 0)    { vb->routing = clampi(n, 0, 3); return 1; }
    if (strcmp(key, "cv_f1_drv") == 0)     { vb->f1_drv = clampf(f, 0.0f, 1.0f); return 1; }
    if (strcmp(key, "cv_f2_cut") == 0)     { vb->f2_cut = clampi(n, 20, 20000); return 1; }
    if (strcmp(key, "cv_f2_res") == 0)     { vb->f2_res = clampf(f, 0.5f, 20.0f); return 1; }
    if (strcmp(key, "cv_f2_type") == 0)    { vb->f2_type = clampi(n, 0, 7); return 1; }
    if (strcmp(key, "cv_f2_drv") == 0)     { vb->f2_drv = clampf(f, 0.0f, 1.0f); return 1; }
    if (strcmp(key, "cv_bw_on") == 0)      { vb->bw_on = (n != 0); return 1; }
    if (strcmp(key, "cv_bit") == 0)        { vb->bit = clampf(f, 0.0f, 1.0f); return 1; }
    if (strcmp(key, "cv_rate") == 0)       { vb->rate = clampf(f, 0.0f, 1.0f); return 1; }
    if (strcmp(key, "cv_kt1") == 0)        { vb->kt1 = clampf(f, 0.0f, 1.0f); return 1; }
    if (strcmp(key, "cv_kt2") == 0)        { vb->kt2 = clampf(f, 0.0f, 1.0f); return 1; }
    if (strcmp(key, "cv_e1_to_cut") == 0)  { vb->e1_to_cut = clampf(f, -1.0f, 1.0f); return 1; }
    if (strcmp(key, "cv_e2_to_cut") == 0)  { vb->e2_to_cut = clampf(f, -1.0f, 1.0f); return 1; }

    if (strcmp(key, "cv_e1_atk") == 0)     { vb->e1_atk = clampf(f, 0.0001f, 1.0f); return 1; }
    if (strcmp(key, "cv_e1_dec") == 0)     { vb->e1_dec = clampf(f, 0.001f, 4.0f); return 1; }
    if (strcmp(key, "cv_e1_crv") == 0)     { vb->e1_crv = clampf(f, -1.0f, 1.0f); return 1; }
    if (strcmp(key, "cv_e1_rep") == 0)     { vb->e1_rep = clampf(f, 0.0f, 1.0f); return 1; }
    if (strcmp(key, "cv_e2_dec") == 0)     { vb->e2_dec = clampf(f, 0.001f, 4.0f); return 1; }
    if (strcmp(key, "cv_e2_crv") == 0)     { vb->e2_crv = clampf(f, -1.0f, 1.0f); return 1; }
    if (strcmp(key, "cv_pe_amt") == 0)     { vb->pe_amt = clampf(f, -1.0f, 1.0f); return 1; }
    if (strcmp(key, "cv_pe_dec") == 0)     { vb->pe_dec = clampf(f, 0.001f, 4.0f); return 1; }
    if (strcmp(key, "cv_e2_dest") == 0)    { vb->e2_dest = clampi(n, 0, 4); return 1; }
    if (strcmp(key, "cv_e1_rep_rate") == 0) { vb->e1_rep_rate = clampf(f, 0.01f, 1.0f); return 1; }
    if (strcmp(key, "cv_e2_atk") == 0)     { vb->e2_atk = clampf(f, 0.0001f, 1.0f); return 1; }
    if (strcmp(key, "cv_pe_crv") == 0)     { vb->pe_crv = clampf(f, -1.0f, 1.0f); return 1; }
    if (strcmp(key, "cv_pe_dest") == 0)    { vb->pe_dest = clampi(n, 0, 3); return 1; }
    if (strcmp(key, "cv_v_e1_lvl") == 0)   { vb->v_e1_lvl = clampf(f, 0.0f, 1.0f); return 1; }
    if (strcmp(key, "cv_v_e1_t") == 0)     { vb->v_e1_t = clampf(f, -1.0f, 1.0f); return 1; }
    if (strcmp(key, "cv_v_e2_amt") == 0)   { vb->v_e2_amt = clampf(f, 0.0f, 1.0f); return 1; }

    if (strcmp(key, "cv_lfo_w") == 0)      { vb->lfo_w = clampi(n, 0, 5); return 1; }
    if (strcmp(key, "cv_lfo_r") == 0)      { vb->lfo_r = clampf(f, 0.01f, 200.0f); return 1; }
    if (strcmp(key, "cv_lfo_s") == 0)      { vb->lfo_s = clampi(n, 0, 6); return 1; }
    if (strcmp(key, "cv_lfo_d") == 0)      { vb->lfo_d = clampf(f, 0.0f, 1.0f); return 1; }
    if (strcmp(key, "cv_xlfo_src") == 0)   { vb->xlfo_src = clampi(n, 0, 8); return 1; }
    if (strcmp(key, "cv_trig_rst") == 0)   { vb->trig_rst = clampi(n, 0, 8); return 1; }
    if (strcmp(key, "cv_mod_dest") == 0)   { vb->mod_dest = clampi(n, 0, 7); return 1; }
    if (strcmp(key, "cv_mod_dpth") == 0)   { vb->mod_dpth = clampf(f, -1.0f, 1.0f); return 1; }
    if (strcmp(key, "cv_lfo_p") == 0)      { vb->lfo_p = clampf(f, 0.0f, 1.0f); return 1; }
    if (strcmp(key, "cv_lfo_pol") == 0)    { vb->lfo_pol = clampi(n, 0, 1); return 1; }
    if (strcmp(key, "cv_lfo_rt") == 0)     { vb->lfo_rt = clampi(n, 0, 1); return 1; }
    if (strcmp(key, "cv_mod_src") == 0)    { vb->mod_src = clampi(n, 0, 7); return 1; }
    if (strcmp(key, "cv_mod_crv") == 0)    { vb->mod_crv = clampi(n, 0, 3); return 1; }

    if (strcmp(key, "cv_algo") == 0)       { vb->algo = clampi(n, 0, NUM_ALGOS - 1); return 1; }
    if (strcmp(key, "cv_choke") == 0)      { vb->choke = clampi(n, 0, 4); return 1; }
    if (strcmp(key, "cv_bus") == 0)        { vb->bus = clampi(n, 0, 3); return 1; }
    if (strcmp(key, "cv_lvl") == 0)        { vb->voice_lvl = clampf(f, 0.0f, 1.0f); return 1; }
    if (strcmp(key, "cv_tune") == 0)       { vb->voice_tune = clampf(f, -24.0f, 24.0f); return 1; }
    if (strcmp(key, "cv_poly") == 0)       { vb->poly = clampi(n, 0, 2); return 1; }
    if (strcmp(key, "cv_glide") == 0)      { vb->glide = clampf(f, 0.0f, 1.0f); return 1; }
    if (strcmp(key, "cv_pan") == 0)        { vb->pan = clampf(f, -1.0f, 1.0f); return 1; }
    if (strcmp(key, "cv_mute") == 0)       { vb->mute = (n != 0); return 1; }
    if (strcmp(key, "cv_fx1") == 0)        { vb->fx1_send = clampf(f, 0.0f, 1.0f); return 1; }
    if (strcmp(key, "cv_fx2") == 0)        { vb->fx2_send = clampf(f, 0.0f, 1.0f); return 1; }
    if (strcmp(key, "cv_vsens") == 0)      { vb->vsens = clampf(f, 0.0f, 1.0f); return 1; }
    if (strcmp(key, "cv_note") == 0)       { vb->midi_note = clampi(n, 0, 127); return 1; }
    return 0;
}

/* ─── Helpers for triggers ─── */
static void do_rnd_voice(forge_instance_t *inst, int v) {
    voice_bank_t *vb = &inst->kits[inst->current_kit].kit_a[v];
    vb->ratio_c   = 0.5f + randf(&inst->rng) * 7.5f;
    vb->ratio_f   = (randf(&inst->rng) - 0.5f) * 2.0f;
    vb->fbk       = randf(&inst->rng) * 0.7f;
    vb->f1_cut    = (int)(200.0f + randf(&inst->rng) * 8000.0f);
    vb->e1_dec    = 0.05f + randf(&inst->rng) * 1.5f;
    vb->pe_amt    = (randf(&inst->rng) - 0.3f) * 1.0f;
    vb->lfo_d     = randf(&inst->rng) * 0.4f;
}
static void do_rnd_kit(forge_instance_t *inst) {
    for (int v = 0; v < NUM_VOICES; v++) do_rnd_voice(inst, v);
}
static void do_rnd_pan(forge_instance_t *inst) {
    for (int v = 0; v < NUM_VOICES; v++) {
        inst->kits[inst->current_kit].kit_a[v].pan = (randf(&inst->rng) - 0.5f) * 1.5f;
    }
}
static void do_all_mono(forge_instance_t *inst) {
    for (int v = 0; v < NUM_VOICES; v++) inst->kits[inst->current_kit].kit_a[v].pan = 0.0f;
}
static void do_init_decay(forge_instance_t *inst) { inst->all_decay_mult = 1.0f; }
static void do_copy_a_b(forge_instance_t *inst) {
    kit_slot_t *k = &inst->kits[inst->current_kit];
    memcpy(k->kit_b, k->kit_a, sizeof(k->kit_a));
}
static void do_copy_b_a(forge_instance_t *inst) {
    kit_slot_t *k = &inst->kits[inst->current_kit];
    memcpy(k->kit_a, k->kit_b, sizeof(k->kit_b));
}
static void do_swap_ab(forge_instance_t *inst) {
    kit_slot_t *k = &inst->kits[inst->current_kit];
    voice_bank_t tmp[NUM_VOICES];
    memcpy(tmp, k->kit_a, sizeof(tmp));
    memcpy(k->kit_a, k->kit_b, sizeof(tmp));
    memcpy(k->kit_b, tmp, sizeof(tmp));
}
static void do_rnd_b_from_a(forge_instance_t *inst) {
    kit_slot_t *k = &inst->kits[inst->current_kit];
    memcpy(k->kit_b, k->kit_a, sizeof(k->kit_a));
    for (int v = 0; v < NUM_VOICES; v++) {
        voice_bank_t *vb = &k->kit_b[v];
        vb->ratio_c *= 0.7f + randf(&inst->rng) * 0.6f;
        vb->fbk     = clampf(vb->fbk + (randf(&inst->rng) - 0.5f) * 0.3f, 0.0f, 1.0f);
        vb->f1_cut  = clampi((int)(vb->f1_cut * (0.5f + randf(&inst->rng) * 1.5f)), 100, 18000);
        vb->e1_dec  = clampf(vb->e1_dec * (0.5f + randf(&inst->rng) * 1.5f), 0.005f, 4.0f);
    }
}
static void do_init_voice(forge_instance_t *inst) {
    voice_bank_t *vb = cv_bank(inst);
    voice_bank_init_default(vb, inst->current_voice);
}

static void set_param(void *instance, const char *key, const char *val) {
    forge_instance_t *inst = (forge_instance_t *)instance;
    if (!inst || !key || !val) return;

    /* cv_* params route to selected voice's active kit bank */
    if (key[0] == 'c' && key[1] == 'v' && key[2] == '_') {
        if (handle_cv_set(inst, key, val)) return;
    }

    /* Level-navigation hint (Schwung sets this as user navigates pages) */
    if (strcmp(key, "_level") == 0 || strcmp(key, "current_level") == 0) {
        return;  /* page tracking deferred to v0.2 (per-page knob overlay) */
    }

    float f = (float)atof(val);
    int   n = atoi(val);

    /* Patch page */
    if (strcmp(key, "kit") == 0)       { inst->current_kit = clampi(n, 0, NUM_KITS - 1); return; }
    if (strcmp(key, "morph") == 0)     { inst->morph = clampf(f, 0.0f, 1.0f); return; }
    if (strcmp(key, "all_decay") == 0) { inst->all_decay_mult = clampf(f, 1.0f, 4.0f); return; }
    if (strcmp(key, "morph_src") == 0)  { inst->morph_src = clampi(n, 0, 3); return; }
    if (strcmp(key, "morph_curve") == 0){ inst->morph_curve = clampi(n, 0, 3); return; }
    if (strcmp(key, "same_freq") == 0)  { inst->same_freq = clampi(n, 20, 20000); return; }

    /* Save Kit (Phasma pattern) */
    if (strcmp(key, "save_kit") == 0) {
        if (strcmp(val, "Save") == 0) {
            forge_save_kits(inst);
            inst->save_kit_state = 0;  /* auto-revert to Play */
        }
        return;
    }

    /* Triggers (auto-revert int state, fire action when val == "1") */
    if (strcmp(key, "rnd_kit") == 0) {
        if (atoi(val) == 1 || strcmp(val, "1") == 0) { do_rnd_kit(inst); }
        inst->tr_rnd_kit = 0; return;
    }
    if (strcmp(key, "rnd_voice") == 0) {
        if (atoi(val) == 1 || strcmp(val, "1") == 0) { do_rnd_voice(inst, inst->current_voice); }
        inst->tr_rnd_voice = 0; return;
    }
    if (strcmp(key, "rnd_pitch") == 0) {
        if (atoi(val) == 1 || strcmp(val, "1") == 0) { /* TODO: musical scale randomize */ }
        inst->tr_rnd_pitch = 0; return;
    }
    if (strcmp(key, "rnd_pan") == 0) {
        if (atoi(val) == 1 || strcmp(val, "1") == 0) { do_rnd_pan(inst); }
        inst->tr_rnd_pan = 0; return;
    }
    if (strcmp(key, "all_mono") == 0) {
        if (atoi(val) == 1 || strcmp(val, "1") == 0) { do_all_mono(inst); }
        inst->tr_all_mono = 0; return;
    }
    if (strcmp(key, "init_decay") == 0) {
        if (atoi(val) == 1 || strcmp(val, "1") == 0) { do_init_decay(inst); }
        inst->tr_init_decay = 0; return;
    }
    if (strcmp(key, "init_freq") == 0) {
        if (atoi(val) == 1 || strcmp(val, "1") == 0) { /* TODO */ }
        inst->tr_init_freq = 0; return;
    }
    if (strcmp(key, "copy_a_b") == 0) {
        if (atoi(val) == 1 || strcmp(val, "1") == 0) { do_copy_a_b(inst); }
        inst->tr_copy_a_b = 0; return;
    }
    if (strcmp(key, "copy_b_a") == 0) {
        if (atoi(val) == 1 || strcmp(val, "1") == 0) { do_copy_b_a(inst); }
        inst->tr_copy_b_a = 0; return;
    }
    if (strcmp(key, "swap_ab") == 0) {
        if (atoi(val) == 1 || strcmp(val, "1") == 0) { do_swap_ab(inst); }
        inst->tr_swap_ab = 0; return;
    }
    if (strcmp(key, "rnd_b_from_a") == 0) {
        if (atoi(val) == 1 || strcmp(val, "1") == 0) { do_rnd_b_from_a(inst); }
        inst->tr_rnd_b_from_a = 0; return;
    }
    if (strcmp(key, "cv_init") == 0) {
        if (atoi(val) == 1 || strcmp(val, "1") == 0) { do_init_voice(inst); }
        inst->tr_cv_init = 0; return;
    }

    /* Mix (per-voice level overlays) */
    if (key[0] == 'v' && key[1] >= '1' && key[1] <= '8' && key[2] == '_') {
        int vi = key[1] - '1';
        if (strcmp(key + 2, "_lvl") == 0) { inst->v_lvl[vi] = clampf(f, 0.0f, 1.0f); return; }
        if (strcmp(key + 2, "_pan") == 0) { inst->kits[inst->current_kit].kit_a[vi].pan = clampf(f, -1.0f, 1.0f); return; }
        if (strcmp(key + 2, "_fx1") == 0) { inst->kits[inst->current_kit].kit_a[vi].fx1_send = clampf(f, 0.0f, 1.0f); return; }
        if (strcmp(key + 2, "_fx2") == 0) { inst->kits[inst->current_kit].kit_a[vi].fx2_send = clampf(f, 0.0f, 1.0f); return; }
    }

    /* FX */
    if (strcmp(key, "rev_mix") == 0)       { inst->rev_mix = clampf(f, 0, 1); return; }
    if (strcmp(key, "rev_decay") == 0)     { inst->rev_decay = clampf(f, 0, 1); return; }
    if (strcmp(key, "rev_size") == 0)      { inst->rev_size = clampf(f, 0, 1); return; }
    if (strcmp(key, "dly_mix") == 0)       { inst->dly_mix = clampf(f, 0, 1); return; }
    if (strcmp(key, "dly_rate") == 0)      { inst->dly_rate = clampf(f, 0, 1); return; }
    if (strcmp(key, "dly_fdbk") == 0)      { inst->dly_fdbk = clampf(f, 0, 0.95f); return; }
    if (strcmp(key, "dly_tone") == 0)      { inst->dly_tone = clampf(f, 0, 1); return; }
    if (strcmp(key, "cho_mix") == 0)       { inst->cho_mix = clampf(f, 0, 1); return; }
    if (strcmp(key, "rev_type") == 0)      { inst->rev_type = clampi(n, 0, 2); return; }
    if (strcmp(key, "rev_predelay") == 0)  { inst->rev_predelay = clampf(f, 0, 1); return; }
    if (strcmp(key, "rev_damping") == 0)   { inst->rev_damping = clampf(f, 0, 1); return; }
    if (strcmp(key, "dly_bpf_cut") == 0)   { inst->dly_bpf_cut = clampi(n, 100, 18000); return; }
    if (strcmp(key, "dly_bpf_w") == 0)     { inst->dly_bpf_w = clampf(f, 0, 1); return; }
    if (strcmp(key, "dly_pp") == 0)        { inst->dly_pp = clampi(n, 0, 1); return; }
    if (strcmp(key, "dly_sync") == 0)      { inst->dly_sync = clampi(n, 0, 4); return; }
    if (strcmp(key, "cho_rate") == 0)      { inst->cho_rate = clampf(f, 0.01f, 5.0f); return; }
    if (strcmp(key, "cho_depth") == 0)     { inst->cho_depth = clampf(f, 0, 1); return; }
    if (strcmp(key, "cho_width") == 0)     { inst->cho_width = clampf(f, 0, 1); return; }
    if (strcmp(key, "cho_voices") == 0)    { inst->cho_voices = clampi(n, 2, 8); return; }
    if (strcmp(key, "cho_tone") == 0)      { inst->cho_tone = clampf(f, 0, 1); return; }
    if (strcmp(key, "cho_fb") == 0)        { inst->cho_fb = clampf(f, 0, 0.9f); return; }

    /* General */
    if (strcmp(key, "comp") == 0)          { inst->comp = clampf(f, 0, 1); return; }
    if (strcmp(key, "drive") == 0)         { inst->drive = clampf(f, 0, 1); return; }
    if (strcmp(key, "bit") == 0)           { inst->bit = clampf(f, 0, 1); return; }
    if (strcmp(key, "rate") == 0)          { inst->rate = clampf(f, 0, 1); return; }
    if (strcmp(key, "eq_lo") == 0)         { inst->eq_lo = clampf(f, -12, 12); return; }
    if (strcmp(key, "eq_mid") == 0)        { inst->eq_mid = clampf(f, -12, 12); return; }
    if (strcmp(key, "eq_hi") == 0)         { inst->eq_hi = clampf(f, -12, 12); return; }
    if (strcmp(key, "master") == 0)        { inst->master = clampf(f, 0, 1); return; }
    if (strcmp(key, "drive_type") == 0)    { inst->drive_type = clampi(n, 0, 2); return; }
    if (strcmp(key, "lo_freq") == 0)       { inst->lo_freq = clampf(f, 20, 500); return; }
    if (strcmp(key, "mid_freq") == 0)      { inst->mid_freq = clampf(f, 200, 8000); return; }
    if (strcmp(key, "hi_freq") == 0)       { inst->hi_freq = clampf(f, 2000, 18000); return; }
    if (strcmp(key, "q_lo") == 0)          { inst->q_lo = clampf(f, 0.3f, 8); return; }
    if (strcmp(key, "q_mid") == 0)         { inst->q_mid = clampf(f, 0.3f, 8); return; }
    if (strcmp(key, "q_hi") == 0)          { inst->q_hi = clampf(f, 0.3f, 8); return; }
    if (strcmp(key, "limiter") == 0)       { inst->limiter = clampi(n, 0, 1); return; }
    if (strcmp(key, "master_tune") == 0)   { inst->master_tune = clampf(f, -100, 100); return; }
    if (strcmp(key, "midi_ch") == 0)       { inst->midi_ch = clampi(n, 1, 16); return; }
}

/* ────────────────────────────────────────────────────────────────────────────
 * get_param — full chain_params + ui_hierarchy strings, all queries
 * ──────────────────────────────────────────────────────────────────────────── */

/* The big static strings for chain_params and ui_hierarchy live in module.json
 * (which Schwung reads directly) but the DSP MUST also return them via
 * get_param("chain_params") and get_param("ui_hierarchy") for the Shadow UI.
 * To keep the .c file readable, we forward those queries to module.json data
 * loaded at runtime — but we don't have a JSON parser here. Instead we read
 * module.json from the install directory once and cache the string. */

static char *g_chain_params_cache = NULL;
static char *g_ui_hierarchy_cache = NULL;
static char  g_module_dir[512] = {0};

/* Crude JSON field extractor: find "key": and copy until end of object/array
 * value (matches braces/brackets). Good enough for chain_params and
 * ui_hierarchy when the file is well-formed. */
static char *extract_json_value(const char *json, const char *key) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return NULL;
    p = strchr(p + strlen(search), ':');
    if (!p) return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    char open = *p;
    if (open != '{' && open != '[') return NULL;
    char close = (open == '{') ? '}' : ']';
    int depth = 0;
    const char *start = p;
    while (*p) {
        if (*p == open) depth++;
        else if (*p == close) {
            depth--;
            if (depth == 0) {
                int len = (int)(p - start) + 1;
                char *out = (char *)malloc(len + 1);
                if (!out) return NULL;
                memcpy(out, start, len);
                out[len] = '\0';
                return out;
            }
        }
        p++;
    }
    return NULL;
}

static void load_module_json(const char *dir) {
    if (!dir || !*dir) return;
    char path[768];
    snprintf(path, sizeof(path), "%s/module.json", dir);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 256 * 1024) { fclose(f); return; }
    char *buf = (char *)malloc(size + 1);
    if (!buf) { fclose(f); return; }
    fread(buf, 1, size, f);
    buf[size] = '\0';
    fclose(f);
    g_chain_params_cache = extract_json_value(buf, "chain_params");
    g_ui_hierarchy_cache = extract_json_value(buf, "ui_hierarchy");
    free(buf);
}

static int get_param(void *instance, const char *key, char *buf, int buf_len) {
    forge_instance_t *inst = (forge_instance_t *)instance;
    if (!inst || !key || !buf || buf_len <= 0) return -1;

    if (strcmp(key, "name") == 0) return snprintf(buf, buf_len, "Forge");

    if (strcmp(key, "chain_params") == 0) {
        if (!g_chain_params_cache && g_module_dir[0]) load_module_json(g_module_dir);
        if (g_chain_params_cache) return snprintf(buf, buf_len, "%s", g_chain_params_cache);
        return snprintf(buf, buf_len, "[]");
    }
    if (strcmp(key, "ui_hierarchy") == 0) {
        if (!g_ui_hierarchy_cache && g_module_dir[0]) load_module_json(g_module_dir);
        if (g_ui_hierarchy_cache) return snprintf(buf, buf_len, "%s", g_ui_hierarchy_cache);
        return snprintf(buf, buf_len, "{}");
    }

    /* Patch */
    if (strcmp(key, "kit") == 0)       return snprintf(buf, buf_len, "%d", inst->current_kit);
    if (strcmp(key, "save_kit") == 0)  return snprintf(buf, buf_len, "%s", inst->save_kit_state ? "Save" : "Play");
    if (strcmp(key, "rnd_kit") == 0    || strcmp(key, "rnd_voice") == 0 ||
        strcmp(key, "rnd_pitch") == 0  || strcmp(key, "rnd_pan") == 0 ||
        strcmp(key, "all_mono") == 0   || strcmp(key, "init_decay") == 0 ||
        strcmp(key, "init_freq") == 0  || strcmp(key, "copy_a_b") == 0 ||
        strcmp(key, "copy_b_a") == 0   || strcmp(key, "swap_ab") == 0 ||
        strcmp(key, "rnd_b_from_a") == 0 || strcmp(key, "cv_init") == 0)
        return snprintf(buf, buf_len, "0");
    if (strcmp(key, "morph") == 0)     return snprintf(buf, buf_len, "%.4f", inst->morph);
    if (strcmp(key, "all_decay") == 0) return snprintf(buf, buf_len, "%.4f", inst->all_decay_mult);
    if (strcmp(key, "morph_src") == 0) {
        static const char *N[] = {"Knob","LFO","Macro","Wheel"};
        return snprintf(buf, buf_len, "%s", N[clampi(inst->morph_src, 0, 3)]);
    }
    if (strcmp(key, "morph_curve") == 0) {
        static const char *N[] = {"Linear","Exp","Log","S-Curve"};
        return snprintf(buf, buf_len, "%s", N[clampi(inst->morph_curve, 0, 3)]);
    }
    if (strcmp(key, "same_freq") == 0) return snprintf(buf, buf_len, "%d", inst->same_freq);

    /* Mix */
    if (key[0] == 'v' && key[1] >= '1' && key[1] <= '8' && key[2] == '_') {
        int vi = key[1] - '1';
        if (strcmp(key + 2, "_lvl") == 0) return snprintf(buf, buf_len, "%.4f", inst->v_lvl[vi]);
        voice_bank_t *vb = &inst->kits[inst->current_kit].kit_a[vi];
        if (strcmp(key + 2, "_pan") == 0) return snprintf(buf, buf_len, "%.4f", vb->pan);
        if (strcmp(key + 2, "_fx1") == 0) return snprintf(buf, buf_len, "%.4f", vb->fx1_send);
        if (strcmp(key + 2, "_fx2") == 0) return snprintf(buf, buf_len, "%.4f", vb->fx2_send);
    }

    /* cv_* — read from active bank */
    if (key[0] == 'c' && key[1] == 'v' && key[2] == '_') {
        voice_bank_t *vb = cv_bank(inst);
        if (key[3] == 'm' && key[4] >= '1' && key[4] <= '8' && key[5] == 0)
            return snprintf(buf, buf_len, "%.4f", vb->m[key[4] - '1']);

        if (strcmp(key, "cv_wave") == 0) {
            static const char *W[] = {"Sine","Tri","Saw","Square","Noise"};
            return snprintf(buf, buf_len, "%s", W[clampi(vb->wave, 0, 4)]);
        }
        if (strcmp(key, "cv_ratio_c") == 0)    return snprintf(buf, buf_len, "%.4f", vb->ratio_c);
        if (strcmp(key, "cv_ratio_f") == 0)    return snprintf(buf, buf_len, "%.4f", vb->ratio_f);
        if (strcmp(key, "cv_detune") == 0)     return snprintf(buf, buf_len, "%.4f", vb->detune);
        if (strcmp(key, "cv_level") == 0)      return snprintf(buf, buf_len, "%.4f", vb->level);
        if (strcmp(key, "cv_phase") == 0)      return snprintf(buf, buf_len, "%.4f", vb->phase);
        if (strcmp(key, "cv_pwm") == 0)        return snprintf(buf, buf_len, "%.4f", vb->pwm);
        if (strcmp(key, "cv_fbk") == 0)        return snprintf(buf, buf_len, "%.4f", vb->fbk);
        if (strcmp(key, "cv_op") == 0) {
            static const char *N[] = {"A","B","C"};
            return snprintf(buf, buf_len, "%s", N[clampi(vb->op_select, 0, 2)]);
        }
        if (strcmp(key, "cv_click_type") == 0) {
            static const char *N[] = {"Sample","Impulse","Phase"};
            return snprintf(buf, buf_len, "%s", N[clampi(vb->click_type, 0, 2)]);
        }
        if (strcmp(key, "cv_click_smp") == 0) {
            static const char *N[] = {"None","Kick","Rim","Hat","Clap","Tom","Snap"};
            return snprintf(buf, buf_len, "%s", N[clampi(vb->click_smp, 0, 6)]);
        }
        if (strcmp(key, "cv_click_lvl") == 0)  return snprintf(buf, buf_len, "%.4f", vb->click_lvl);
        if (strcmp(key, "cv_click_dec") == 0)  return snprintf(buf, buf_len, "%.4f", vb->click_dec);
        if (strcmp(key, "cv_xfm") == 0)        return snprintf(buf, buf_len, "%s", vb->xfm ? "On" : "Off");

        if (strcmp(key, "cv_f1_cut") == 0)     return snprintf(buf, buf_len, "%d", vb->f1_cut);
        if (strcmp(key, "cv_f1_res") == 0)     return snprintf(buf, buf_len, "%.4f", vb->f1_res);
        if (strcmp(key, "cv_f1_type") == 0) {
            static const char *N[] = {"LP","HP","BP","BPu","Notch","Peak","Comb+","Comb-"};
            return snprintf(buf, buf_len, "%s", N[clampi(vb->f1_type, 0, 7)]);
        }
        if (strcmp(key, "cv_bw_cut") == 0)     return snprintf(buf, buf_len, "%d", vb->bw_cut);
        if (strcmp(key, "cv_bw_w") == 0)       return snprintf(buf, buf_len, "%.4f", vb->bw_w);
        if (strcmp(key, "cv_routing") == 0) {
            static const char *N[] = {"Single","Per-Osc","Serial","Parallel"};
            return snprintf(buf, buf_len, "%s", N[clampi(vb->routing, 0, 3)]);
        }
        if (strcmp(key, "cv_f1_drv") == 0)     return snprintf(buf, buf_len, "%.4f", vb->f1_drv);
        if (strcmp(key, "cv_f2_cut") == 0)     return snprintf(buf, buf_len, "%d", vb->f2_cut);
        if (strcmp(key, "cv_f2_res") == 0)     return snprintf(buf, buf_len, "%.4f", vb->f2_res);
        if (strcmp(key, "cv_f2_type") == 0) {
            static const char *N[] = {"LP","HP","BP","BPu","Notch","Peak","Comb+","Comb-"};
            return snprintf(buf, buf_len, "%s", N[clampi(vb->f2_type, 0, 7)]);
        }
        if (strcmp(key, "cv_f2_drv") == 0)     return snprintf(buf, buf_len, "%.4f", vb->f2_drv);
        if (strcmp(key, "cv_bw_on") == 0)      return snprintf(buf, buf_len, "%s", vb->bw_on ? "On" : "Off");
        if (strcmp(key, "cv_bit") == 0)        return snprintf(buf, buf_len, "%.4f", vb->bit);
        if (strcmp(key, "cv_rate") == 0)       return snprintf(buf, buf_len, "%.4f", vb->rate);
        if (strcmp(key, "cv_kt1") == 0)        return snprintf(buf, buf_len, "%.4f", vb->kt1);
        if (strcmp(key, "cv_kt2") == 0)        return snprintf(buf, buf_len, "%.4f", vb->kt2);
        if (strcmp(key, "cv_e1_to_cut") == 0)  return snprintf(buf, buf_len, "%.4f", vb->e1_to_cut);
        if (strcmp(key, "cv_e2_to_cut") == 0)  return snprintf(buf, buf_len, "%.4f", vb->e2_to_cut);

        if (strcmp(key, "cv_e1_atk") == 0)     return snprintf(buf, buf_len, "%.4f", vb->e1_atk);
        if (strcmp(key, "cv_e1_dec") == 0)     return snprintf(buf, buf_len, "%.4f", vb->e1_dec);
        if (strcmp(key, "cv_e1_crv") == 0)     return snprintf(buf, buf_len, "%.4f", vb->e1_crv);
        if (strcmp(key, "cv_e1_rep") == 0)     return snprintf(buf, buf_len, "%.4f", vb->e1_rep);
        if (strcmp(key, "cv_e2_dec") == 0)     return snprintf(buf, buf_len, "%.4f", vb->e2_dec);
        if (strcmp(key, "cv_e2_crv") == 0)     return snprintf(buf, buf_len, "%.4f", vb->e2_crv);
        if (strcmp(key, "cv_pe_amt") == 0)     return snprintf(buf, buf_len, "%.4f", vb->pe_amt);
        if (strcmp(key, "cv_pe_dec") == 0)     return snprintf(buf, buf_len, "%.4f", vb->pe_dec);
        if (strcmp(key, "cv_e2_dest") == 0) {
            static const char *N[] = {"FM","Filter","Reso","Pan","Mod"};
            return snprintf(buf, buf_len, "%s", N[clampi(vb->e2_dest, 0, 4)]);
        }
        if (strcmp(key, "cv_e1_rep_rate") == 0) return snprintf(buf, buf_len, "%.4f", vb->e1_rep_rate);
        if (strcmp(key, "cv_e2_atk") == 0)     return snprintf(buf, buf_len, "%.4f", vb->e2_atk);
        if (strcmp(key, "cv_pe_crv") == 0)     return snprintf(buf, buf_len, "%.4f", vb->pe_crv);
        if (strcmp(key, "cv_pe_dest") == 0) {
            static const char *N[] = {"Pitch","FM","Filter","Reso"};
            return snprintf(buf, buf_len, "%s", N[clampi(vb->pe_dest, 0, 3)]);
        }
        if (strcmp(key, "cv_v_e1_lvl") == 0)   return snprintf(buf, buf_len, "%.4f", vb->v_e1_lvl);
        if (strcmp(key, "cv_v_e1_t") == 0)     return snprintf(buf, buf_len, "%.4f", vb->v_e1_t);
        if (strcmp(key, "cv_v_e2_amt") == 0)   return snprintf(buf, buf_len, "%.4f", vb->v_e2_amt);

        if (strcmp(key, "cv_lfo_w") == 0) {
            static const char *N[] = {"Sine","Tri","Saw","Square","S&H","Random"};
            return snprintf(buf, buf_len, "%s", N[clampi(vb->lfo_w, 0, 5)]);
        }
        if (strcmp(key, "cv_lfo_r") == 0)      return snprintf(buf, buf_len, "%.4f", vb->lfo_r);
        if (strcmp(key, "cv_lfo_s") == 0) {
            static const char *N[] = {"Free","1/1","1/2","1/4","1/8","1/16","1/32"};
            return snprintf(buf, buf_len, "%s", N[clampi(vb->lfo_s, 0, 6)]);
        }
        if (strcmp(key, "cv_lfo_d") == 0)      return snprintf(buf, buf_len, "%.4f", vb->lfo_d);
        if (strcmp(key, "cv_xlfo_src") == 0) {
            static const char *N[] = {"Self","V1","V2","V3","V4","V5","V6","V7","V8"};
            return snprintf(buf, buf_len, "%s", N[clampi(vb->xlfo_src, 0, 8)]);
        }
        if (strcmp(key, "cv_trig_rst") == 0) {
            static const char *N[] = {"None","V1","V2","V3","V4","V5","V6","V7","V8"};
            return snprintf(buf, buf_len, "%s", N[clampi(vb->trig_rst, 0, 8)]);
        }
        if (strcmp(key, "cv_mod_dest") == 0) {
            static const char *N[] = {"None","Pitch","FM Idx","Filt Cut","Reso FB","Body","Pan","Level"};
            return snprintf(buf, buf_len, "%s", N[clampi(vb->mod_dest, 0, 7)]);
        }
        if (strcmp(key, "cv_mod_dpth") == 0)   return snprintf(buf, buf_len, "%.4f", vb->mod_dpth);
        if (strcmp(key, "cv_lfo_p") == 0)      return snprintf(buf, buf_len, "%.4f", vb->lfo_p);
        if (strcmp(key, "cv_lfo_pol") == 0)    return snprintf(buf, buf_len, "%s", vb->lfo_pol ? "Unipolar" : "Bipolar");
        if (strcmp(key, "cv_lfo_rt") == 0)     return snprintf(buf, buf_len, "%s", vb->lfo_rt ? "On" : "Off");
        if (strcmp(key, "cv_mod_src") == 0) {
            static const char *N[] = {"LFO","XLFO","E1","E2","PE","Vel","AT","MW"};
            return snprintf(buf, buf_len, "%s", N[clampi(vb->mod_src, 0, 7)]);
        }
        if (strcmp(key, "cv_mod_crv") == 0) {
            static const char *N[] = {"Linear","Exp","Log","S-Curve"};
            return snprintf(buf, buf_len, "%s", N[clampi(vb->mod_crv, 0, 3)]);
        }

        if (strcmp(key, "cv_algo") == 0)       return snprintf(buf, buf_len, "%s", ALGO_NAMES[clampi(vb->algo, 0, NUM_ALGOS - 1)]);
        if (strcmp(key, "cv_choke") == 0) {
            static const char *N[] = {"None","A","B","C","D"};
            return snprintf(buf, buf_len, "%s", N[clampi(vb->choke, 0, 4)]);
        }
        if (strcmp(key, "cv_bus") == 0) {
            static const char *N[] = {"Main","Aux1","Aux2","FX-Only"};
            return snprintf(buf, buf_len, "%s", N[clampi(vb->bus, 0, 3)]);
        }
        if (strcmp(key, "cv_lvl") == 0)        return snprintf(buf, buf_len, "%.4f", vb->voice_lvl);
        if (strcmp(key, "cv_tune") == 0)       return snprintf(buf, buf_len, "%.4f", vb->voice_tune);
        if (strcmp(key, "cv_poly") == 0) {
            static const char *N[] = {"One-Shot","Legato","Retrig"};
            return snprintf(buf, buf_len, "%s", N[clampi(vb->poly, 0, 2)]);
        }
        if (strcmp(key, "cv_glide") == 0)      return snprintf(buf, buf_len, "%.4f", vb->glide);
        if (strcmp(key, "cv_pan") == 0)        return snprintf(buf, buf_len, "%.4f", vb->pan);
        if (strcmp(key, "cv_mute") == 0)       return snprintf(buf, buf_len, "%s", vb->mute ? "On" : "Off");
        if (strcmp(key, "cv_fx1") == 0)        return snprintf(buf, buf_len, "%.4f", vb->fx1_send);
        if (strcmp(key, "cv_fx2") == 0)        return snprintf(buf, buf_len, "%.4f", vb->fx2_send);
        if (strcmp(key, "cv_vsens") == 0)      return snprintf(buf, buf_len, "%.4f", vb->vsens);
        if (strcmp(key, "cv_note") == 0)       return snprintf(buf, buf_len, "%d", vb->midi_note);
    }

    /* FX */
    if (strcmp(key, "rev_mix") == 0)       return snprintf(buf, buf_len, "%.4f", inst->rev_mix);
    if (strcmp(key, "rev_decay") == 0)     return snprintf(buf, buf_len, "%.4f", inst->rev_decay);
    if (strcmp(key, "rev_size") == 0)      return snprintf(buf, buf_len, "%.4f", inst->rev_size);
    if (strcmp(key, "dly_mix") == 0)       return snprintf(buf, buf_len, "%.4f", inst->dly_mix);
    if (strcmp(key, "dly_rate") == 0)      return snprintf(buf, buf_len, "%.4f", inst->dly_rate);
    if (strcmp(key, "dly_fdbk") == 0)      return snprintf(buf, buf_len, "%.4f", inst->dly_fdbk);
    if (strcmp(key, "dly_tone") == 0)      return snprintf(buf, buf_len, "%.4f", inst->dly_tone);
    if (strcmp(key, "cho_mix") == 0)       return snprintf(buf, buf_len, "%.4f", inst->cho_mix);
    if (strcmp(key, "rev_type") == 0) {
        static const char *N[] = {"Plate","Spring","Chamber"};
        return snprintf(buf, buf_len, "%s", N[clampi(inst->rev_type, 0, 2)]);
    }
    if (strcmp(key, "rev_predelay") == 0)  return snprintf(buf, buf_len, "%.4f", inst->rev_predelay);
    if (strcmp(key, "rev_damping") == 0)   return snprintf(buf, buf_len, "%.4f", inst->rev_damping);
    if (strcmp(key, "dly_bpf_cut") == 0)   return snprintf(buf, buf_len, "%d", inst->dly_bpf_cut);
    if (strcmp(key, "dly_bpf_w") == 0)     return snprintf(buf, buf_len, "%.4f", inst->dly_bpf_w);
    if (strcmp(key, "dly_pp") == 0)        return snprintf(buf, buf_len, "%s", inst->dly_pp ? "On" : "Off");
    if (strcmp(key, "dly_sync") == 0) {
        static const char *N[] = {"Free","1/4","1/8","1/16","1/32"};
        return snprintf(buf, buf_len, "%s", N[clampi(inst->dly_sync, 0, 4)]);
    }
    if (strcmp(key, "cho_rate") == 0)      return snprintf(buf, buf_len, "%.4f", inst->cho_rate);
    if (strcmp(key, "cho_depth") == 0)     return snprintf(buf, buf_len, "%.4f", inst->cho_depth);
    if (strcmp(key, "cho_width") == 0)     return snprintf(buf, buf_len, "%.4f", inst->cho_width);
    if (strcmp(key, "cho_voices") == 0)    return snprintf(buf, buf_len, "%d", inst->cho_voices);
    if (strcmp(key, "cho_tone") == 0)      return snprintf(buf, buf_len, "%.4f", inst->cho_tone);
    if (strcmp(key, "cho_fb") == 0)        return snprintf(buf, buf_len, "%.4f", inst->cho_fb);

    /* General */
    if (strcmp(key, "comp") == 0)          return snprintf(buf, buf_len, "%.4f", inst->comp);
    if (strcmp(key, "drive") == 0)         return snprintf(buf, buf_len, "%.4f", inst->drive);
    if (strcmp(key, "bit") == 0)           return snprintf(buf, buf_len, "%.4f", inst->bit);
    if (strcmp(key, "rate") == 0)          return snprintf(buf, buf_len, "%.4f", inst->rate);
    if (strcmp(key, "eq_lo") == 0)         return snprintf(buf, buf_len, "%.4f", inst->eq_lo);
    if (strcmp(key, "eq_mid") == 0)        return snprintf(buf, buf_len, "%.4f", inst->eq_mid);
    if (strcmp(key, "eq_hi") == 0)         return snprintf(buf, buf_len, "%.4f", inst->eq_hi);
    if (strcmp(key, "master") == 0)        return snprintf(buf, buf_len, "%.4f", inst->master);
    if (strcmp(key, "drive_type") == 0) {
        static const char *N[] = {"Tube","Fold","Clip"};
        return snprintf(buf, buf_len, "%s", N[clampi(inst->drive_type, 0, 2)]);
    }
    if (strcmp(key, "lo_freq") == 0)       return snprintf(buf, buf_len, "%.4f", inst->lo_freq);
    if (strcmp(key, "mid_freq") == 0)      return snprintf(buf, buf_len, "%.4f", inst->mid_freq);
    if (strcmp(key, "hi_freq") == 0)       return snprintf(buf, buf_len, "%.4f", inst->hi_freq);
    if (strcmp(key, "q_lo") == 0)          return snprintf(buf, buf_len, "%.4f", inst->q_lo);
    if (strcmp(key, "q_mid") == 0)         return snprintf(buf, buf_len, "%.4f", inst->q_mid);
    if (strcmp(key, "q_hi") == 0)          return snprintf(buf, buf_len, "%.4f", inst->q_hi);
    if (strcmp(key, "limiter") == 0)       return snprintf(buf, buf_len, "%s", inst->limiter ? "On" : "Off");
    if (strcmp(key, "master_tune") == 0)   return snprintf(buf, buf_len, "%.4f", inst->master_tune);
    if (strcmp(key, "midi_ch") == 0)       return snprintf(buf, buf_len, "%d", inst->midi_ch);

    return -1;
}

/* ────────────────────────────────────────────────────────────────────────────
 * Audio render — v0.1 stub: simple click + decaying sine per voice.
 * Real algorithmic synthesis is implemented in the Code stage after dsp-fetch.
 * ──────────────────────────────────────────────────────────────────────────── */

static void render_block(void *instance, int16_t *out_lr, int frames) {
    forge_instance_t *inst = (forge_instance_t *)instance;
    if (!inst) {
        memset(out_lr, 0, frames * 2 * sizeof(int16_t));
        return;
    }

    morph_voices(inst);

    for (int i = 0; i < frames; i++) {
        float l = 0.0f, r = 0.0f;

        for (int v = 0; v < NUM_VOICES; v++) {
            voice_state_t *vs = &inst->voice[v];
            if (!vs->active) continue;

            voice_bank_t *vb = &inst->live[v];
            if (vb->mute) continue;

            /* Effective decay: live × all_decay multiplier */
            float dec = vb->e1_dec * inst->all_decay_mult;
            float dec_coef = expf(-1.0f / (dec * SAMPLE_RATE));
            vs->e1_v *= dec_coef;
            if (vs->e1_v < 1e-5f) { vs->active = 0; vs->e1_v = 0.0f; continue; }

            /* Naive sine at note freq * ratio (placeholder — real algo in v0.1 code stage) */
            float freq = note_to_freq(vb->midi_note) * vb->ratio_c;
            vs->phase_a += freq * SR_INV;
            if (vs->phase_a >= 1.0f) vs->phase_a -= 1.0f;
            float osc = sinf(vs->phase_a * TWO_PI);

            /* Add a click on the first ~5ms */
            float click = 0.0f;
            if (vs->e1_v > 0.95f) click = (randf(&inst->rng) - 0.5f) * 0.5f;

            float s = (osc + click * vb->click_lvl) * vs->e1_v * vs->velocity * vb->voice_lvl * inst->v_lvl[v];

            float p = clampf(vb->pan * 0.5f + 0.5f, 0.0f, 1.0f);
            l += s * (1.0f - p);
            r += s * p;
        }

        l *= inst->master;
        r *= inst->master;
        l = clampf(l, -1.0f, 1.0f);
        r = clampf(r, -1.0f, 1.0f);

        out_lr[i * 2]     = (int16_t)(l * 32767.0f);
        out_lr[i * 2 + 1] = (int16_t)(r * 32767.0f);
    }
}

/* ────────────────────────────────────────────────────────────────────────────
 * API v2 export
 * ──────────────────────────────────────────────────────────────────────────── */

typedef struct host_api_v1 {
    uint32_t api_version;
    uint8_t *mapped_memory;
    uint32_t audio_in_offset;
    uint32_t audio_out_offset;
} host_api_v1_t;

typedef struct {
    uint32_t api_version;
    void* (*create_instance)(const char *, const char *);
    void  (*destroy_instance)(void *);
    void  (*on_midi)(void *, const uint8_t *, int, int);
    void  (*set_param)(void *, const char *, const char *);
    int   (*get_param)(void *, const char *, char *, int);
    int   (*get_error)(void *, char *, int);
    void  (*render_block)(void *, int16_t *, int);
} plugin_api_v2_t;

/* Wrapper to capture module_dir at load time so we can lazy-load module.json */
static void *create_instance_with_dir(const char *module_dir, const char *json_defaults) {
    if (module_dir && *module_dir) {
        strncpy(g_module_dir, module_dir, sizeof(g_module_dir) - 1);
        g_module_dir[sizeof(g_module_dir) - 1] = '\0';
    }
    return create_instance(module_dir, json_defaults);
}

plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t *host) {
    (void)host;
    static plugin_api_v2_t api = {
        .api_version      = 2,
        .create_instance  = create_instance_with_dir,
        .destroy_instance = destroy_instance,
        .on_midi          = on_midi,
        .set_param        = set_param,
        .get_param        = get_param,
        .get_error        = NULL,
        .render_block     = render_block,
    };
    return &api;
}
