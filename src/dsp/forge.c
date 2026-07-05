/**
 * Forge — 8-voice FM/subtractive hybrid drum synthesiser for Ableton Move
 *
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
 * Audio: 44100 Hz, 128 frames/block, stereo interleaved int16 output
 *
 * v0.1 includes:
 *   - 5 algorithms (Drum/Snare/Cymbal/Hat/Wild)
 *   - 2-op FM with feedback (Drum/Snare/Wild)
 *   - 3-op serial FM cascade with FB at A (Cymbal/Hat)
 *   - Body wavefolder (post-osc nonlinearity)
 *   - Per-voice resonator (feedback-comb with tanh saturation)
 *   - Base-Width pre-filter + dual SVF (Single/Per-Osc/Serial/Parallel)
 *     with LP/HP/BP/BPu/Notch/Peak/Comb+/Comb- modes
 *   - AD envelopes with curve + repeat (LXR-style)
 *   - Pitch envelope with destination routing
 *   - LFO with cross-voice routing & trigger phase reset
 *   - Per-voice bit/rate crush
 *   - Click stage (synthesised impulse + procedurally-generated transient bank)
 *   - 3 concurrent FX buses: Delay (BPF in feedback, ping-pong),
 *     Reverb (Dattorro Plate), Panoramic Chorus (multi-engine stereo)
 *   - Bus compressor (EMT-156 style), 3-band EQ, master drive/bit/rate/limiter
 *   - Kit A↔B morph engine with linear/exp/log/S-curve curves
 *   - Phasma-style Save Kit binary persistence (forge_kits.dat, magic 'FRGE')
 *   - All-Decay multiplier (snapshot-relative, KrautDrums-style)
 *
 * Credits (all MIT):
 *   - 2-op FM concept inspired by Plaits operator (Émilie Gillet, MIT)
 *   - Wavefolder topology from DaisySP (Electrosmith, MIT)
 *   - Chorus topology inspired by DaisySP (Electrosmith, MIT)
 *   - Reverb / delay / compressor patterns ported from KrautDrums
 *     (Vincent Fillion, MIT — same author)
 *   - Schwung framework: Charles Vestal, MIT
 *   - Sonic Potions LXR (custom non-commercial license) — concept of
 *     AD-with-curve-and-repeat envelope state machine; no code copied.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#define SAMPLE_RATE       44100.0f
#define SR_INV            (1.0f / SAMPLE_RATE)
#define TWO_PI            6.28318530717958647692f
#define PI                3.14159265358979323846f
#define NUM_VOICES        8
#define NUM_KITS          128   /* 64 Forge originals + 64 Razzmatazz ports */
#define BLOCK_SIZE        128
#define FIRST_PAD_NOTE    36
#define DENORM_EPS        1e-25f

#define KITS_FILE_PATH    "/data/UserData/schwung/forge_kits.dat"
#define KITS_SAVE_MAGIC   0x46524745u  /* 'FRGE' */
/* Bumped 1→2 (10→64 kits), 2→3 (64→128 kits + parallel body sine).
 * The version + slot-count check rejects stale saves so a fresh factory
 * loads cleanly after a structural change. */
#define KITS_SAVE_VER     3u

#define RESO_MAX_SAMPS    1024         /* max delay length for the per-voice resonator (~23 ms @ 44.1k → 43 Hz) */
#define COMB_MAX_SAMPS    1024
#define DELAY_BUF_SAMPS   65536        /* ~1.49 s @ 44.1k for global delay */

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

/* Filter types (matches cv_f1_type / cv_f2_type chain_param enum order) */
enum {
    FILT_LP = 0, FILT_HP, FILT_BP, FILT_BPU, FILT_NOTCH, FILT_PEAK,
    FILT_COMBP, FILT_COMBN,
    FILT_LP2   /* LXR-style self-oscillating acid LP */
};

enum { ROUTING_SINGLE = 0, ROUTING_PER_OSC, ROUTING_SERIAL, ROUTING_PARALLEL };
enum { CLICK_SAMPLE = 0, CLICK_IMPULSE, CLICK_PHASE };

/* Page tracker for dynamic knob labels (Weird Dreams pattern).
 * Schwung sends set_param("_level", "<level-name>") on navigation. */
enum {
    PAGE_PATCH = 0,
    PAGE_VOICE,    /* macro page (8 algo-routed labels) */
    PAGE_OSC,
    PAGE_FILTER,
    PAGE_ENV,
    PAGE_MOD,
    PAGE_SETUP,
    PAGE_MIX,
    PAGE_FX,
    PAGE_PERF,
    PAGE_GENERAL
};

/* Knob labels per page. Voice pages get a "V<N> " prefix at runtime. */
static const char *PATCH_KNOB_NAMES [8] = {"Kit","Save","Rnd Kit","Rnd Vox","Rnd Pch","Morph","All Dec","Rnd Pan"};
static const char *MIX_KNOB_NAMES   [8] = {"V1 Lvl","V2 Lvl","V3 Lvl","V4 Lvl","V5 Lvl","V6 Lvl","V7 Lvl","V8 Lvl"};
static const char *FX_KNOB_NAMES    [8] = {"Rev Mix","Rev Dec","Rev Size","Dly Mix","Dly Rate","Dly Fdbk","Dly Tone","Cho Mix"};
static const char *PERF_KNOB_NAMES  [8] = {"Punch","Bright","Decay","Drive","Snap","Bend","Tune","FX"};
static const char *GEN_KNOB_NAMES   [8] = {"Comp","Drive","Bit","Rate","EQ Lo","EQ Mid","EQ Hi","Master"};

/* Voice macro page — algo-dependent labels (per design-spec table). */
static const char *VOICE_MACRO_NAMES[5][8] = {
    /* ALGO_DRUM   */ {"Pitch","Decay","Bend","FM","Body","Click","Cutoff","Drive"},
    /* ALGO_SNARE  */ {"Tune","Snap","Body","Noise","Tone","Decay","Cutoff","Drive"},
    /* ALGO_CYMBAL */ {"Tune","Decay","FMIdx","Color","Shape","Reso","Cutoff","Drive"},
    /* ALGO_HAT    */ {"Tune","Cls Dec","Opn Dec","FM","Color","Shape","Cutoff","Drive"},
    /* ALGO_WILD   */ {"M1","M2","M3","M4","M5","M6","M7","M8"}
};

/* Sub-pages — same labels regardless of algo (drill-in detail editors). */
static const char *OSC_KNOB_NAMES   [8] = {"Wave","Ratio","Fine","Detune","Level","Phase","PWM","Fbk"};
static const char *FILTER_KNOB_NAMES[8] = {"F1 Cut","F1 Res","F1 Type","BW Cut","BW W","Rout","F1 Drv","F2 Cut"};
static const char *ENV_KNOB_NAMES   [8] = {"E1 Atk","E1 Dec","E1 Crv","E1 Rep","E2 Dec","E2 Crv","PE Amt","PE Dec"};
static const char *MOD_KNOB_NAMES   [8] = {"LFO W","LFO R","LFO S","LFO D","XLFO","Trig","Dest","Depth"};
static const char *SETUP_KNOB_NAMES [8] = {"Algo","Choke","Bus","Lvl","Tune","Poly","Glide","Init"};

/* Click sample bank slots */
enum { CSMP_NONE = 0, CSMP_KICK, CSMP_RIM, CSMP_HAT, CSMP_CLAP, CSMP_TOM, CSMP_SNAP, NUM_CSMP };
#define CSMP_LEN 1024  /* ~23 ms @ 44.1k per click — plenty for transient */

/* ────────────────────────────────────────────────────────────────────────────
 * Voice bank — one Kit A copy + one Kit B copy per voice
 * ──────────────────────────────────────────────────────────────────────────── */

typedef struct {
    /* Voice setup */
    int   algo;
    int   choke;
    int   bus;
    int   poly;
    int   mute;
    int   midi_note;
    float voice_lvl;
    float voice_tune;
    float glide;
    float vsens;

    /* Macro page */
    float m[8];

    /* Osc / FM */
    int   wave;
    float ratio_c, ratio_f;
    float detune;
    float level;
    float phase;
    float pwm;
    float fbk;
    int   op_select;
    int   click_type;
    int   click_smp;
    float click_lvl;
    float click_dec;
    int   xfm;

    /* Filter (dual + Base-Width pre) */
    int   f1_cut, f2_cut, bw_cut;
    float f1_res, f2_res;
    int   f1_type, f2_type;
    float f1_drv, f2_drv;
    float bw_w;
    int   bw_on;
    int   routing;
    float bit, rate;
    float kt1, kt2;
    float e1_to_cut, e2_to_cut;

    /* Envelopes */
    float e1_atk, e1_dec, e1_crv, e1_rep, e1_rep_rate;
    float e2_atk, e2_dec, e2_crv;
    int   e2_dest;
    float pe_amt, pe_dec, pe_crv;
    int   pe_dest;
    float v_e1_lvl, v_e1_t, v_e2_amt;
    /* FM index envelopes (Digitone II-style enveloped FM) — bloom the FM
     * amount at attack then collapse to the base index. Two per voice. */
    float fie1_amt, fie1_dec, fie1_crv;
    float fie2_amt, fie2_dec, fie2_crv;

    /* Mod / LFO */
    int   lfo_w, lfo_w2;                        /* two shapes to morph between */
    float lfo_morph;                           /* 0 = shape1, 1 = shape2 */
    int   lfo_s;
    float lfo_r, lfo_d, lfo_p;
    int   lfo_pol, lfo_rt;
    int   xlfo_src;
    int   trig_rst;
    int   mod_src, mod_dest;
    float mod_dpth;
    int   mod_crv;

    /* Mixer */
    float pan;
    float fx1_send, fx2_send;
} voice_bank_t;

/* ────────────────────────────────────────────────────────────────────────────
 * Live voice state
 * ──────────────────────────────────────────────────────────────────────────── */

typedef struct {
    int   active;
    float velocity;

    /* Envelopes (state-machine A→D→REPEAT) */
    /* Env1: amp; Env2: assignable; PEnv: pitch destination */
    int   e1_state, e2_state, pe_state;        /* 0=idle 1=A 2=D 3=REPEAT */
    float e1_v, e2_v, pe_v;
    float e1_t, e2_t, pe_t;                    /* time accumulator (sec) */
    int   e1_rep_cnt;
    /* FM index envelopes */
    int   fie1_state, fie2_state;
    float fie1_v, fie2_v, fie1_t, fie2_t;

    /* Oscillator phase (0..1) */
    float ph_a, ph_b, ph_c;                    /* carrier (3-op cascade) */
    float ph_mod;                              /* modulator phase for 2-op */
    float ph_body;                             /* parallel body-sine phase (Drum) */
    float fb_state[2];                         /* feedback averaging (Plaits-style) */

    /* Noise state for snare body */
    float noise_lp;
    uint32_t rng;

    /* Click stage state */
    int   click_idx;
    int   click_active;

    /* Resonator */
    float reso_buf[RESO_MAX_SAMPS];
    int   reso_idx;

    /* Comb filters (per filter slot — used when type == COMB+/-) */
    float comb1_buf[COMB_MAX_SAMPS];
    float comb2_buf[COMB_MAX_SAMPS];
    int   comb1_idx, comb2_idx;

    /* SVF Chamberlin state */
    float svf1_lp, svf1_bp;
    float svf2_lp, svf2_bp;
    /* Base-Width pre-filter (1-pole LP + 1-pole HP) */
    float bw_lp_st, bw_hp_st;

    /* LFO */
    float lfo_v, lfo_phase;
    float lfo_sah_v;
    float lfo_phase_prev;

    /* Crush */
    float crush_held_l, crush_held_r;
    float crush_accum;

    /* Filter coefficient cache (recomputed per block) */
    float f1_coef, f1_q;
    float f2_coef, f2_q;
    float bw_lp_coef, bw_hp_coef;
    int   f1_comb_len, f2_comb_len;
    int   reso_len;
} voice_state_t;

/* ────────────────────────────────────────────────────────────────────────────
 * Kit slot (Kit A + Kit B + per-kit FX state)
 * ──────────────────────────────────────────────────────────────────────────── */

typedef struct {
    voice_bank_t kit_a[NUM_VOICES];
    voice_bank_t kit_b[NUM_VOICES];
    float rev_mix, rev_decay, rev_size, rev_predelay, rev_damping;
    int   rev_type;
    float rev_gate;
    float dly_mix, dly_rate, dly_fdbk, dly_tone, dly_bpf_w;
    int   dly_bpf_cut, dly_pp, dly_sync;
    float cho_mix, cho_rate, cho_depth, cho_width, cho_tone, cho_fb;
    int   cho_voices;
} kit_slot_t;

/* ────────────────────────────────────────────────────────────────────────────
 * FX state structures
 * ──────────────────────────────────────────────────────────────────────────── */

typedef struct {
    float buf_l[DELAY_BUF_SAMPS];
    float buf_r[DELAY_BUF_SAMPS];
    int   write_idx;
    /* BPF in feedback path: 1-pole HP + 1-pole LP cascade */
    float fb_hp_l, fb_hp_r;
    float fb_lp_l, fb_lp_r;
} delay_state_t;

#define REV_PRE_LEN     256   /* ~5.4 ms */
#define REV_DA_LEN      512
#define REV_DB_LEN      512
#define REV_DC_LEN      1024
#define REV_DD_LEN      1024
#define REV_TANK_LEN    8192
#define REV_AP_LEN      512

typedef struct {
    /* Pre-delay */
    float pre[REV_PRE_LEN]; int pre_idx;
    /* Input diffuser allpasses (4) */
    float ap_a[REV_DA_LEN]; int ap_a_idx;
    float ap_b[REV_DB_LEN]; int ap_b_idx;
    float ap_c[REV_DC_LEN]; int ap_c_idx;
    float ap_d[REV_DD_LEN]; int ap_d_idx;
    /* Cross-coupled tank (Dattorro figure-8): 2 delays per side, with damping */
    float tank_a1[REV_TANK_LEN]; int tank_a1_idx; int tank_a1_len;
    float tank_a2[REV_TANK_LEN]; int tank_a2_idx; int tank_a2_len;
    float tank_b1[REV_TANK_LEN]; int tank_b1_idx; int tank_b1_len;
    float tank_b2[REV_TANK_LEN]; int tank_b2_idx; int tank_b2_len;
    /* Modulated input allpasses on each tank */
    float tank_a_ap[REV_AP_LEN]; int tank_a_ap_idx; int tank_a_ap_len;
    float tank_b_ap[REV_AP_LEN]; int tank_b_ap_idx; int tank_b_ap_len;
    /* Damping LPF state */
    float damp_a, damp_b;
    /* Cross-coupled output state */
    float out_a, out_b;
    /* Modulation phase */
    float mod_phase;
    /* Gate (Machinedrum Gate Box): envelope follower on input + gate level */
    float gate_env;    /* follows input transients */
    float gate_level;  /* current gate opening 0..1 */
} reverb_state_t;

#define CHO_TAPS        4
#define CHO_BUF_SIZE    4096

typedef struct {
    float buf[CHO_BUF_SIZE];
    int   write_idx;
    float lfo_phase[CHO_TAPS];   /* per-tap LFO phase */
    float fb_state;              /* feedback memory */
} chorus_state_t;

typedef struct {
    float env;
    float gain;
} compressor_state_t;

typedef struct {
    /* 1-pole shelf states */
    float lo_st, mid_st1, mid_st2, hi_st;
} eq_state_t;

/* ────────────────────────────────────────────────────────────────────────────
 * Instance
 * ──────────────────────────────────────────────────────────────────────────── */

typedef struct {
    kit_slot_t kits[NUM_KITS];

    int   current_kit;
    float morph;
    int   morph_src, morph_curve;

    int   current_voice;
    int   current_kit_context;     /* 0 = Kit A, 1 = Kit B */
    int   current_page;            /* one of PAGE_* for knob_N_name routing */

    voice_bank_t  live[NUM_VOICES];
    voice_state_t voice[NUM_VOICES];

    float all_decay_mult;
    /* Ctrl-All performance macros (Machinedrum-style) — global nudges applied
     * across every voice at render time. 0.5 = neutral for the bipolar ones. */
    float all_punch;    /* body-sine weight    (0..1, 0.5 neutral) */
    float all_bright;   /* filter cutoff       (0..1, 0.5 neutral) */
    float all_drive;    /* per-voice drive     (0..1, 0.5 neutral) */
    float all_snap;     /* click/transient lvl (0..1, 0.5 neutral) */
    float all_bend;     /* pitch-env amount    (0..1, 0.5 neutral) */
    float all_tune;     /* global pitch offset (0..1, 0.5 neutral) */
    float all_fx;       /* FX send scale       (0..1, 0.5 neutral) */
    int   save_kit_state;

    /* Trigger params (auto-revert) */
    int   tr_rnd_kit, tr_rnd_voice, tr_rnd_pitch, tr_rnd_pan;
    int   tr_all_mono, tr_init_decay, tr_init_freq;
    int   tr_copy_a_b, tr_copy_b_a, tr_swap_ab, tr_rnd_b_from_a;
    int   tr_cv_init;

    /* Mix overlay */
    float v_lvl[NUM_VOICES];

    /* FX state (live) */
    float rev_mix, rev_decay, rev_size, rev_predelay, rev_damping;
    int   rev_type;
    float rev_gate;   /* 0 = normal tail, >0 = gated reverb (Machinedrum Gate Box) */
    float dly_mix, dly_rate, dly_fdbk, dly_tone, dly_bpf_w;
    int   dly_bpf_cut, dly_pp, dly_sync;
    float cho_mix, cho_rate, cho_depth, cho_width, cho_tone, cho_fb;
    int   cho_voices;

    /* Master/General */
    float comp, drive, bit, rate;
    float eq_lo, eq_mid, eq_hi, master;
    int   drive_type;
    float lo_freq, mid_freq, hi_freq, q_lo, q_mid, q_hi;
    int   limiter;
    float master_tune;
    int   midi_ch;
    int   same_freq;

    /* FX state structs */
    delay_state_t      delay_st;
    reverb_state_t     reverb_st;
    chorus_state_t     chorus_st;
    compressor_state_t comp_st;
    eq_state_t         eq_st;

    /* Master crush state */
    float master_crush_held_l, master_crush_held_r;
    float master_crush_accum;

    /* Click sample bank — generated procedurally on init */
    float click_bank[NUM_CSMP][CSMP_LEN];

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
/* Bipolar [-1, +1] white noise sample */
static inline float wnoise(uint32_t *rng) {
    return randf(rng) * 2.0f - 1.0f;
}
/* Fast tanh approximation — Padé (good to ~1% over [-3, 3]) */
static inline float fast_tanh(float x) {
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}
/* Cubic soft-clip — softer than tanh, no transcendentals */
static inline float soft_clip(float x) {
    if (x >  1.5f) return  1.0f;
    if (x < -1.5f) return -1.0f;
    return x - (x * x * x) * (1.0f / 6.75f);
}
static inline float note_to_freq(int note) {
    return 440.0f * powf(2.0f, ((float)note - 69.0f) / 12.0f);
}

/* Sine via small lookup + linear interpolation (avoids per-sample sinf) */
#define SINE_TABLE_SIZE 1024
static float SINE_TABLE[SINE_TABLE_SIZE + 1];
static int sine_table_initialized = 0;
static void init_sine_table(void) {
    if (sine_table_initialized) return;
    for (int i = 0; i <= SINE_TABLE_SIZE; i++) {
        SINE_TABLE[i] = sinf((float)i * TWO_PI / (float)SINE_TABLE_SIZE);
    }
    sine_table_initialized = 1;
}
/* phase is [0, 1) */
static inline float lookup_sine(float phase) {
    while (phase < 0.0f) phase += 1.0f;
    while (phase >= 1.0f) phase -= 1.0f;
    float fi = phase * (float)SINE_TABLE_SIZE;
    int   i  = (int)fi;
    float f  = fi - (float)i;
    return SINE_TABLE[i] * (1.0f - f) + SINE_TABLE[i + 1] * f;
}

/* ────────────────────────────────────────────────────────────────────────────
 * DSP — Multi-waveform oscillator (carrier)
 * Returns a sample in [-1, +1] for the chosen carrier waveform.
 * `phase` ∈ [0, 1). PWM only affects square (0..1, 0.5 = 50%).
 * ──────────────────────────────────────────────────────────────────────────── */

enum { OSC_SINE = 0, OSC_TRI, OSC_SAW, OSC_SQUARE, OSC_NOISE };

static inline float carrier_sample(int wave, float phase, float pwm, uint32_t *rng) {
    switch (wave) {
        case OSC_SINE:   return lookup_sine(phase);
        case OSC_TRI:    {
            /* Triangle from phase: 0 → +1 → 0 → -1 → 0 */
            float p = phase;
            if (p < 0.5f) return -1.0f + 4.0f * p;
            else          return  3.0f - 4.0f * p;
        }
        case OSC_SAW:    return 2.0f * phase - 1.0f;
        case OSC_SQUARE: return phase < pwm ? 1.0f : -1.0f;
        case OSC_NOISE:  return wnoise(rng);
        default:         return 0.0f;
    }
}

/* ────────────────────────────────────────────────────────────────────────────
 * DSP — 2-op FM operator (carrier + sine modulator with feedback)
 *
 * Concept ported from Plaits operator.h (Émilie Gillet, MIT). Adapted to
 * float-phase arithmetic and Forge's per-sample loop. The feedback path uses
 * a 2-sample average (Plaits trick) to suppress DC at high feedback levels.
 *
 * Per call (one sample):
 *   - Modulator: sine at carrier_freq * ratio, with feedback PM
 *   - Carrier: chosen waveform PM'd by modulator output × index
 * ──────────────────────────────────────────────────────────────────────────── */

static inline float fm_op_2op(
    float carrier_freq,                /* Hz */
    float ratio,                       /* mod / carrier ratio */
    float fm_index,                    /* modulation depth (0..~12) */
    float feedback,                    /* 0..1, applied to modulator self-PM */
    int   carrier_wave,
    float pwm,
    float *carrier_phase,              /* persistent state */
    float *mod_phase,                  /* persistent state */
    float *fb_state,                   /* fb_state[2] persistent */
    uint32_t *rng)
{
    /* Modulator phase advance */
    float mod_inc = carrier_freq * ratio * SR_INV;
    *mod_phase += mod_inc;
    if (*mod_phase >= 1.0f) *mod_phase -= floorf(*mod_phase);

    /* Modulator output: sine with PM from feedback (averaged) */
    float fb_pm = (fb_state[0] + fb_state[1]) * feedback * 0.5f;
    float mod   = lookup_sine(*mod_phase + fb_pm);
    fb_state[1] = fb_state[0];
    fb_state[0] = mod;

    /* Carrier phase advance */
    float car_inc = carrier_freq * SR_INV;
    *carrier_phase += car_inc;
    if (*carrier_phase >= 1.0f) *carrier_phase -= floorf(*carrier_phase);

    /* Carrier modulated by modulator output × index */
    float car_phase_mod = *carrier_phase + mod * fm_index * (1.0f / TWO_PI);
    return carrier_sample(carrier_wave, car_phase_mod - floorf(car_phase_mod), pwm, rng);
}

/* ────────────────────────────────────────────────────────────────────────────
 * DSP — 3-op serial FM (A → B → C, FB at A)
 *
 * Cymbal/Hat algorithm. All three operators are sine PM operators. A has its
 * own self-feedback (2-sample average), modulates B's phase. B modulates C's
 * phase. C's output is the voice signal.
 * ──────────────────────────────────────────────────────────────────────────── */

static inline float fm_op_3op_serial(
    float base_freq,
    float ratio_a, float ratio_b, float ratio_c,
    float idx_ab, float idx_bc,         /* modulation indices */
    float feedback_a,
    float *ph_a, float *ph_b, float *ph_c,
    float *fb_state)
{
    /* A: self-feedback PM */
    float a_inc = base_freq * ratio_a * SR_INV;
    *ph_a += a_inc;
    if (*ph_a >= 1.0f) *ph_a -= floorf(*ph_a);
    float fb_pm = (fb_state[0] + fb_state[1]) * feedback_a * 0.5f;
    float a_out = lookup_sine(*ph_a + fb_pm);
    fb_state[1] = fb_state[0];
    fb_state[0] = a_out;

    /* B modulated by A */
    float b_inc = base_freq * ratio_b * SR_INV;
    *ph_b += b_inc;
    if (*ph_b >= 1.0f) *ph_b -= floorf(*ph_b);
    float b_out = lookup_sine(*ph_b + a_out * idx_ab * (1.0f / TWO_PI));

    /* C modulated by B */
    float c_inc = base_freq * ratio_c * SR_INV;
    *ph_c += c_inc;
    if (*ph_c >= 1.0f) *ph_c -= floorf(*ph_c);
    float c_out = lookup_sine(*ph_c + b_out * idx_bc * (1.0f / TWO_PI));

    return c_out;
}

/* ────────────────────────────────────────────────────────────────────────────
 * DSP — Wavefolder (DaisySP topology, MIT — Electrosmith)
 *
 * Triangle-wave folding. Amplitude > 1 begins folding. Soft, low CPU.
 * Concept from daisysp/wavefolder.cpp.
 * ──────────────────────────────────────────────────────────────────────────── */

static inline float wavefold(float x, float gain) {
    if (gain < 0.001f) return x;
    float in = x * gain;
    float ft = floorf((in + 1.0f) * 0.5f);
    float sgn = ((int)ft & 1) == 0 ? 1.0f : -1.0f;
    return sgn * (in - 2.0f * ft);
}

/* ────────────────────────────────────────────────────────────────────────────
 * DSP — Per-voice resonator (feedback comb + tanh saturation)
 *
 * Razzmatazz-style ringing. With g > 1 the tanh provides limit-cycle stability,
 * giving a tunable metallic ring per drum without burning FM index. Cheap:
 * one delay-line read + one tanh per sample.
 * ──────────────────────────────────────────────────────────────────────────── */

static inline float resonator_process(
    float in,
    float fb_amount,                   /* 0..2 (>1 saturates via tanh) */
    int   delay_len,                   /* samples */
    float *buf,                        /* state */
    int   *idx)
{
    if (fb_amount < 0.001f) return in;
    if (delay_len < 2) delay_len = 2;
    if (delay_len >= RESO_MAX_SAMPS) delay_len = RESO_MAX_SAMPS - 1;
    int read_idx = *idx - delay_len;
    while (read_idx < 0) read_idx += RESO_MAX_SAMPS;
    float delayed = buf[read_idx % RESO_MAX_SAMPS];
    /* Tanh-saturated feedback: high-FB ring without runaway */
    float fb = fast_tanh(delayed * fb_amount);
    float out = in + fb;
    buf[*idx] = out + DENORM_EPS;
    *idx = (*idx + 1) % RESO_MAX_SAMPS;
    return out;
}

/* ────────────────────────────────────────────────────────────────────────────
 * DSP — Chamberlin SVF (LP/HP/BP/BPu/Notch/Peak)
 *
 * Standard 2-pole state-variable. Q-normalized bandpass (bp * fb to keep
 * peak level constant — Spectra/KrautDrums lesson).
 *
 * coef = 2 * sin(π * f / Fs)  (per-block)
 * fb   = 1 / Q                (per-block)
 *
 * In:  one sample
 * Out: chosen mode mixed
 * ──────────────────────────────────────────────────────────────────────────── */

static inline float svf_process(
    float in, int mode, float coef, float fb,
    float *lp_st, float *bp_st)
{
    /* LP2 — LXR-style self-oscillating acid low-pass. Pushes the resonance
     * far past the normal range (tiny fb = huge Q) and tanh-limits the
     * feedback so it screams and self-oscillates without blowing up. */
    if (mode == FILT_LP2) {
        float q_fb = fb * 0.22f + 0.008f;   /* ~4.5× the resonance */
        float lp = *lp_st + coef * (*bp_st);
        float hp = in - lp - q_fb * fast_tanh(*bp_st * 3.0f);
        float bp = coef * hp + (*bp_st);
        *lp_st = lp + DENORM_EPS;
        *bp_st = bp + DENORM_EPS;
        return lp;
    }
    /* Two passes for stability at high Q */
    float lp = *lp_st + coef * (*bp_st);
    float hp = in - lp - fb * (*bp_st);
    float bp = coef * hp + (*bp_st);
    /* Second pass */
    lp += coef * bp;
    hp = in - lp - fb * bp;
    bp = coef * hp + bp;
    /* Denormal flush — drum tails decay state toward 0; ARM A53 stalls on
     * denormals without FTZ. -ffast-math handles this but the explicit add
     * is the pattern used everywhere else in the file. */
    *lp_st = lp + DENORM_EPS;
    *bp_st = bp + DENORM_EPS;
    float bp_norm = bp * fb;            /* Q-normalized */
    switch (mode) {
        case FILT_LP:    return lp;
        case FILT_HP:    return hp;
        case FILT_BP:    return bp_norm;
        case FILT_BPU:   return bp;     /* unity-gain bandpass */
        case FILT_NOTCH: return in - bp_norm;
        case FILT_PEAK:  return lp + bp_norm * 1.5f;   /* lp + emphasized bp */
        default:         return lp;
    }
}

/* ────────────────────────────────────────────────────────────────────────────
 * DSP — Comb filter (Comb+ / Comb-)
 *
 * Comb+:  y[n] = x[n] + g·y[n-N]
 * Comb-:  y[n] = x[n] − g·y[n-N]
 * Used as filter modes 7 and 8 in the same per-voice filter slot.
 * ──────────────────────────────────────────────────────────────────────────── */

static inline float comb_process(
    float in, int sign,                /* +1 or -1 */
    int   delay_len, float g,
    float *buf, int *idx)
{
    if (delay_len < 2) delay_len = 2;
    if (delay_len >= COMB_MAX_SAMPS) delay_len = COMB_MAX_SAMPS - 1;
    int read_idx = *idx - delay_len;
    while (read_idx < 0) read_idx += COMB_MAX_SAMPS;
    float delayed = buf[read_idx % COMB_MAX_SAMPS];
    float out = in + (sign > 0 ? g : -g) * delayed;
    /* Soft saturate to keep stable at g near 1 */
    out = soft_clip(out * 0.7f) * 1.4f;
    buf[*idx] = out + DENORM_EPS;
    *idx = (*idx + 1) % COMB_MAX_SAMPS;
    return out;
}

/* ────────────────────────────────────────────────────────────────────────────
 * DSP — 1-pole LP / HP (used for Base-Width pre-filter and feedback HP/LP)
 * ──────────────────────────────────────────────────────────────────────────── */

static inline float onepole_lp(float in, float coef, float *st) {
    *st += coef * (in - *st) + DENORM_EPS;
    return *st;
}
static inline float onepole_hp(float in, float coef, float *st) {
    *st += coef * (in - *st) + DENORM_EPS;
    return in - *st;
}
/* coef = 1 - exp(-2π * f / Fs) */
static inline float onepole_coef(float freq) {
    if (freq < 1.0f) freq = 1.0f;
    if (freq > 20000.0f) freq = 20000.0f;
    return 1.0f - expf(-TWO_PI * freq * SR_INV);
}

/* ────────────────────────────────────────────────────────────────────────────
 * DSP — AD envelope with curve + repeat (LXR-style state machine)
 *
 * State: 0=idle, 1=attack, 2=decay, 3=repeat (fast retrigger of attack)
 * Curve: -1 = log-like, 0 = linear, +1 = exp-like
 *
 * Returns the next envelope value 0..1. Caller advances state by passing
 * the appropriate atk/dec times in seconds.
 * ──────────────────────────────────────────────────────────────────────────── */

static inline float curve_shape(float t, float curve) {
    /* t in [0,1]. curve in [-1,+1]. */
    if (curve > 0.001f) {
        float k = 1.0f + curve * 4.0f;
        return powf(t, k);                 /* expo (slow start, fast end) */
    } else if (curve < -0.001f) {
        float k = 1.0f - curve * 4.0f;     /* curve negative → k > 1 */
        return 1.0f - powf(1.0f - t, k);   /* log (fast start, slow end) */
    }
    return t;
}

/* Advance an AD env one sample. State machine matches LXR SlopeEg2 (concept).
 *
 * Args:
 *   state  : pointer to int state (0..3)
 *   v      : pointer to current value
 *   t      : pointer to time accumulator (seconds since stage start)
 *   atk    : attack time in seconds
 *   dec    : decay time in seconds
 *   curve  : -1..+1 (applied to decay phase only — attack is always linear)
 *   repeat : 0..1 (probability/intensity of triggering REPEAT)
 *   rep_rate : 0..1 (repeat decay duration: 0=fast, 1=slow)
 *   rep_cnt: pointer to current repeat counter
 *
 * Returns the new envelope value.
 */
static inline float ad_env_advance(
    int   *state,
    float *v,
    float *t,
    float atk, float dec,
    float curve, float repeat, float rep_rate,
    int   *rep_cnt)
{
    if (*state == 0) return 0.0f;
    *t += SR_INV;
    if (*state == 1) {
        /* Attack: linear ramp to 1.0 */
        float a = atk > 0.0001f ? atk : 0.0001f;
        if (*t >= a) {
            *v = 1.0f;
            *state = 2;
            *t = 0.0f;
        } else {
            *v = *t / a;
        }
    } else if (*state == 2) {
        /* Decay: shaped curve from 1 → 0 */
        float d = dec > 0.001f ? dec : 0.001f;
        if (*t >= d) {
            *v = 0.0f;
            if (repeat > 0.05f && (*rep_cnt) < 16) {
                *state = 1;
                *t = 0.0f;
                (*rep_cnt)++;
                /* Slightly shorten attack when repeating to give "buzz" feel */
                /* (handled by caller via rep_rate-modulated atk passed in) */
            } else {
                *state = 0;
                *rep_cnt = 0;
            }
        } else {
            float u = *t / d;
            *v = 1.0f - curve_shape(u, curve);
        }
    }
    return *v;
}

/* ────────────────────────────────────────────────────────────────────────────
 * DSP — Pitch envelope (simpler — single shaped decay)
 *
 * Returns 0..1 envelope. Used to modulate pitch / FM index / filter cut.
 * ──────────────────────────────────────────────────────────────────────────── */

static inline float pitch_env_advance(
    int *state, float *v, float *t, float dec, float curve)
{
    if (*state == 0) return 0.0f;
    *t += SR_INV;
    float d = dec > 0.001f ? dec : 0.001f;
    if (*t >= d) {
        *v = 0.0f;
        *state = 0;
    } else {
        float u = *t / d;
        *v = 1.0f - curve_shape(u, curve);
    }
    return *v;
}

/* ────────────────────────────────────────────────────────────────────────────
 * DSP — LFO (6 waveforms)
 * ──────────────────────────────────────────────────────────────────────────── */

enum { LFO_SINE = 0, LFO_TRI, LFO_SAW, LFO_SQUARE, LFO_SH, LFO_RANDOM };

/* Stateless LFO shape — S&H value is sampled once per cycle by the caller and
 * passed in as sah_v, so morphing between two shapes doesn't double-advance it. */
static inline float lfo_shape(int wave, float phase, float sah_v) {
    switch (wave) {
        case LFO_SINE:   return lookup_sine(phase);
        case LFO_TRI: {
            float p = phase;
            if (p < 0.5f) return -1.0f + 4.0f * p;
            else          return  3.0f - 4.0f * p;
        }
        case LFO_SAW:    return 2.0f * phase - 1.0f;
        case LFO_SQUARE: return phase < 0.5f ? 1.0f : -1.0f;
        case LFO_SH:     return sah_v;
        case LFO_RANDOM: return sah_v * (0.5f + 0.5f * lookup_sine(phase));
        default:         return 0.0f;
    }
}

/* Morph between two LFO shapes (Machinedrum SHMIX / Digitone dual-shape). */
static inline float lfo_morph_sample(int w1, int w2, float morph, float phase, float sah_v) {
    float a = lfo_shape(w1, phase, sah_v);
    if (morph < 0.001f) return a;
    float b = lfo_shape(w2, phase, sah_v);
    return a * (1.0f - morph) + b * morph;
}

/* ────────────────────────────────────────────────────────────────────────────
 * DSP — Drive (tube / fold / clip)
 * ──────────────────────────────────────────────────────────────────────────── */

enum { DRIVE_TUBE = 0, DRIVE_FOLD, DRIVE_CLIP };

static inline float drive_sample(int type, float in, float amount) {
    if (amount < 0.001f) return in;
    /* Drive gain 1..11. tanh/fold/clip all naturally limit to ±1, so we do
     * NOT divide by drive (that was the old bug that made drive quieter). The
     * saturator raises RMS/harmonics = louder & fatter. A gentle makeup keeps
     * peaks near unity so the sound gets thicker without dropping level. */
    float drive = 1.0f + amount * amount * 10.0f;
    float y;
    switch (type) {
        case DRIVE_TUBE:
            /* Asymmetric tanh, DC-corrected for 2nd-harmonic warmth */
            y = fast_tanh(in * drive + 0.08f) - fast_tanh(0.08f);
            break;
        case DRIVE_FOLD:
            y = wavefold(in, drive);
            break;
        case DRIVE_CLIP:
            y = clampf(in * drive, -1.0f, 1.0f);
            break;
        default:
            y = in;
    }
    /* Makeup: at high drive the saturator already peaks near ±1, so trim a
     * touch for headroom; at low drive keep close to unity. Net effect is
     * always ≥ input loudness — a real, fatter saturation. */
    float makeup = 1.0f / (1.0f + amount * 0.4f);
    return y * makeup;
}

/* ────────────────────────────────────────────────────────────────────────────
 * DSP — Bit / rate crush
 *
 * bit:  0=12 bits (transparent), 1=2 bits (very crushed)
 * rate: 0=full SR, 1=down to ~441 Hz
 * ──────────────────────────────────────────────────────────────────────────── */

static inline float bit_crush(float in, float amount) {
    if (amount < 0.001f) return in;
    int bits = 12 - (int)(amount * 10.0f);
    if (bits < 2) bits = 2;
    float steps = (float)(1 << bits);
    return floorf(in * steps + 0.5f) / steps;
}

/* Stateful rate-reducer: holds last sample and emits new one only every N. */
static inline float rate_crush_l(float in, float amount, float *held, float *accum) {
    if (amount < 0.001f) return in;
    float ratio = 1.0f + amount * 99.0f;     /* 1..100 */
    *accum += 1.0f;
    if (*accum >= ratio) {
        *accum -= ratio;
        *held = in;
    }
    return *held;
}

/* ────────────────────────────────────────────────────────────────────────────
 * Click-bank generator — runs once on init.
 *
 * Produces 6 short procedural transients in `inst->click_bank[]`.
 * Each is CSMP_LEN = 1024 samples (~23 ms @ 44.1k). Synthesised, not sampled,
 * so binary stays small and licensing is trivial.
 * ──────────────────────────────────────────────────────────────────────────── */

static void synth_click_kick(float *out, uint32_t *rng) {
    /* Short click: pitched sine sweep + noise burst */
    for (int i = 0; i < CSMP_LEN; i++) {
        float t = (float)i / (float)CSMP_LEN;
        float env = expf(-t * 25.0f);
        float pitch = 80.0f * expf(-t * 20.0f) + 40.0f;
        float ph = pitch * (float)i * SR_INV;
        ph -= floorf(ph);
        float sine = lookup_sine(ph);
        float noise = wnoise(rng) * expf(-t * 60.0f) * 0.4f;
        out[i] = (sine * 0.7f + noise) * env;
    }
}

static void synth_click_rim(float *out, uint32_t *rng) {
    /* Sharp click: short noise burst + 2 kHz sine ping */
    for (int i = 0; i < CSMP_LEN; i++) {
        float t = (float)i / (float)CSMP_LEN;
        float env = expf(-t * 80.0f);
        float ph = 2000.0f * (float)i * SR_INV;
        ph -= floorf(ph);
        float sine = lookup_sine(ph);
        float noise = wnoise(rng);
        out[i] = (sine * 0.5f + noise * 0.5f) * env;
    }
}

static void synth_click_hat(float *out, uint32_t *rng) {
    /* Bright noise click, short */
    static float lp_st = 0.0f;
    lp_st = 0.0f;
    for (int i = 0; i < CSMP_LEN; i++) {
        float t = (float)i / (float)CSMP_LEN;
        float env = expf(-t * 40.0f);
        float n = wnoise(rng);
        /* HPF the noise via subtractive 1-pole */
        lp_st += 0.4f * (n - lp_st);
        float hp = n - lp_st;
        out[i] = hp * env;
    }
}

static void synth_click_clap(float *out, uint32_t *rng) {
    /* Series of 3 short noise bursts (clap/buzz) */
    for (int i = 0; i < CSMP_LEN; i++) {
        float t = (float)i / (float)CSMP_LEN;
        float env = expf(-t * 30.0f);
        /* Modulate noise amplitude with a short multi-burst pattern */
        float pulse = 0.0f;
        if (t < 0.05f) pulse = 1.0f;
        else if (t < 0.10f) pulse = 0.0f;
        else if (t < 0.15f) pulse = 1.0f;
        else if (t < 0.20f) pulse = 0.0f;
        else if (t < 0.25f) pulse = 1.0f;
        else pulse = 0.7f;
        out[i] = wnoise(rng) * pulse * env;
    }
}

static void synth_click_tom(float *out, uint32_t *rng) {
    /* Pitched sweep, slower than kick, with subtle noise */
    for (int i = 0; i < CSMP_LEN; i++) {
        float t = (float)i / (float)CSMP_LEN;
        float env = expf(-t * 12.0f);
        float pitch = 220.0f * expf(-t * 10.0f) + 100.0f;
        float ph = pitch * (float)i * SR_INV;
        ph -= floorf(ph);
        float sine = lookup_sine(ph);
        float noise = wnoise(rng) * expf(-t * 40.0f) * 0.2f;
        out[i] = (sine * 0.85f + noise) * env;
    }
}

static void synth_click_snap(float *out, uint32_t *rng) {
    /* Very short noise burst — 1 ms burst, sharp */
    for (int i = 0; i < CSMP_LEN; i++) {
        float t = (float)i / (float)CSMP_LEN;
        float env = (t < 0.04f) ? expf(-t * 50.0f) : expf(-t * 200.0f);
        out[i] = wnoise(rng) * env;
    }
}

static void init_click_bank(forge_instance_t *inst) {
    uint32_t rng = 0xC11CB006u;  /* "click bank" seed */
    memset(inst->click_bank[CSMP_NONE], 0, sizeof(float) * CSMP_LEN);
    synth_click_kick(inst->click_bank[CSMP_KICK], &rng);
    synth_click_rim (inst->click_bank[CSMP_RIM],  &rng);
    synth_click_hat (inst->click_bank[CSMP_HAT],  &rng);
    synth_click_clap(inst->click_bank[CSMP_CLAP], &rng);
    synth_click_tom (inst->click_bank[CSMP_TOM],  &rng);
    synth_click_snap(inst->click_bank[CSMP_SNAP], &rng);
}

/* ────────────────────────────────────────────────────────────────────────────
 * Default voice bank initialiser (per algorithm)
 * ──────────────────────────────────────────────────────────────────────────── */

static void voice_bank_init_default(voice_bank_t *vb, int voice_index) {
    memset(vb, 0, sizeof(*vb));
    if (voice_index < 4)        vb->algo = ALGO_DRUM;
    else if (voice_index == 4)  vb->algo = ALGO_SNARE;
    else if (voice_index == 5)  vb->algo = ALGO_CYMBAL;
    else                        vb->algo = ALGO_HAT;

    vb->choke      = (voice_index >= 6) ? 1 : 0;
    vb->bus        = 0;
    vb->poly       = 0;
    vb->voice_lvl  = 0.85f;
    vb->voice_tune = 0.0f;
    vb->glide      = 0.0f;
    vb->vsens      = 0.7f;
    vb->midi_note  = FIRST_PAD_NOTE + voice_index;
    for (int i = 0; i < 8; i++) vb->m[i] = 0.5f;

    vb->wave      = OSC_SINE;
    vb->ratio_c   = 1.0f;
    vb->ratio_f   = 0.0f;
    vb->detune    = 0.0f;
    vb->level     = 0.85f;
    vb->phase     = 0.0f;
    vb->pwm       = 0.5f;
    vb->fbk       = 0.0f;
    vb->click_type = CLICK_SAMPLE;
    vb->click_smp = (voice_index < 4) ? CSMP_KICK :
                    (voice_index == 4) ? CSMP_CLAP :
                    (voice_index == 5) ? CSMP_HAT  : CSMP_HAT;
    vb->click_lvl = 0.5f;
    vb->click_dec = 0.005f;

    vb->f1_cut    = 8000;
    vb->f2_cut    = 4000;
    vb->bw_cut    = 1000;
    vb->f1_res    = 0.7f;
    vb->f2_res    = 0.7f;
    vb->f1_type   = FILT_LP;
    vb->f2_type   = FILT_LP;
    vb->f1_drv    = 0.0f;
    vb->f2_drv    = 0.0f;
    vb->bw_w      = 0.5f;
    vb->bw_on     = 0;
    vb->routing   = ROUTING_SINGLE;

    vb->e1_atk     = 0.001f;
    vb->e1_dec     = 0.25f;
    vb->e1_crv     = -0.5f;
    vb->e1_rep     = 0.0f;
    vb->e1_rep_rate = 0.05f;
    vb->e2_atk     = 0.001f;
    vb->e2_dec     = 0.15f;
    vb->e2_crv     = -0.5f;
    vb->e2_dest    = 1;
    vb->pe_amt     = 0.0f;
    vb->pe_dec     = 0.05f;
    vb->pe_crv     = -0.5f;
    vb->pe_dest    = 0;
    vb->v_e1_lvl   = 0.5f;

    /* FM index envelope defaults — a gentle attack bloom on the FM amount so
     * every voice "breathes" out of the box (Digitone-style). Per-algo tuning
     * below overrides. */
    vb->fie1_amt   = 0.25f; vb->fie1_dec = 0.03f;  vb->fie1_crv = -0.5f;
    vb->fie2_amt   = 0.0f;  vb->fie2_dec = 0.05f;  vb->fie2_crv = -0.5f;

    vb->lfo_w     = LFO_SINE;
    vb->lfo_w2    = LFO_TRI;
    vb->lfo_morph = 0.0f;
    vb->lfo_r     = 1.0f;
    vb->lfo_s     = 0;
    vb->lfo_d     = 0.0f;
    vb->lfo_p     = 0.0f;
    vb->lfo_pol   = 0;
    vb->lfo_rt    = 0;
    vb->xlfo_src  = 0;
    vb->trig_rst  = 0;
    vb->mod_src   = 0;
    vb->mod_dest  = 0;
    vb->mod_dpth  = 0.0f;
    vb->mod_crv   = 0;

    vb->pan       = 0.0f;
    vb->fx1_send  = 0.0f;
    vb->fx2_send  = 0.0f;

    /* Algorithm-specific tuning */
    switch (vb->algo) {
        case ALGO_DRUM:
            /* Tuned for kick: low base pitch, exp decay, mild FM, strong body */
            vb->ratio_c  = 1.0f;
            vb->fbk      = 0.2f;
            vb->f1_cut   = 6000;
            vb->e1_dec   = 0.3f;
            vb->pe_amt   = 0.6f;    /* base pitch sweep (Bend macro adds more) */
            vb->pe_dec   = 0.055f;  /* ~55ms sweep — audible bouncy "dooow" */
            vb->pe_crv   = -0.6f;   /* exp drop for a natural bend shape */
            vb->m[2]     = 0.5f;    /* Bend macro live by default */
            vb->m[4]     = 0.78f;   /* Body macro → strong parallel body sine */
            vb->fie1_amt = 0.30f; vb->fie1_dec = 0.025f;  /* attack FM click */
            break;
        case ALGO_SNARE:
            vb->ratio_c  = 1.5f;
            vb->fbk      = 0.4f;
            vb->f1_cut   = 4000;
            vb->f1_type  = FILT_BP;
            vb->e1_dec   = 0.2f;
            vb->pe_amt   = 0.3f;
            vb->fie1_amt = 0.40f; vb->fie1_dec = 0.02f;
            break;
        case ALGO_CYMBAL:
            vb->ratio_c  = 4.7f;          /* enharmonic */
            vb->fbk      = 0.7f;
            vb->f1_cut   = 8000;
            vb->f1_type  = FILT_HP;
            vb->e1_dec   = 0.6f;
            vb->fie1_amt = 0.50f; vb->fie1_dec = 0.08f;   /* metallic bloom A→B */
            vb->fie2_amt = 0.40f; vb->fie2_dec = 0.14f;   /* and B→C */
            break;
        case ALGO_HAT:
            vb->ratio_c  = 4.7f;
            vb->fbk      = 0.6f;
            vb->f1_cut   = 9000;
            vb->f1_type  = FILT_HP;
            vb->e1_dec   = (voice_index == 6) ? 0.05f : 0.3f;
            vb->fie1_amt = 0.40f; vb->fie1_dec = 0.02f;
            vb->fie2_amt = 0.30f; vb->fie2_dec = 0.03f;
            break;
        case ALGO_WILD:
            vb->ratio_c  = 2.0f;
            vb->fbk      = 0.0f;
            vb->fie1_amt = 0.30f; vb->fie1_dec = 0.05f;
            vb->fie2_amt = 0.20f; vb->fie2_dec = 0.05f;
            break;
    }
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
 * Factory kits — 64 kits inspired by the drum machines that inspired Forge
 *
 * Each kit defines all 8 voices' (algo, note, ratio, fbk, dec, pe, macros),
 * per-kit FX state (rev / dly / cho), per-voice FX sends, and Kit B variation
 * mods.  Data-driven layout — entries are static const struct literals in
 * FACTORY_KITS[NUM_KITS], iterated by init_factory_kits().
 *
 * Inspiration map:
 *   Slots  0- 9 : Forge originals  (Plastic / Anvil / Forge / Cinder / Spark
 *                                   / Dust / Phasma / Static / Glass / Marteau)
 *   Slots 10-19 : Classic drum machines  (808 / 909 / 707 / CR-78 / Linn /
 *                                          DMX / SP-12 / Simmons / Boom / Trap)
 *   Slots 20-29 : LXR-02 / Sonic Potions / Erica industrial  (LXR / Riga /
 *                  Schmidt / Indus / Lattice / Caustic / Steam / Iron / Burn
 *                  / Cathedral)
 *   Slots 30-39 : Razzmatazz / per-voice resonator showcase  (Razz / Bell /
 *                  Chime / Pluck / Wire / Vinyl / Snap / Bounce / Toy / Crystal)
 *   Slots 40-49 : Digitone II / Plaits / Volca Drum FM  (Digi / Plaits /
 *                  Cycles / Volca / Wavefold / BaseWidth / Comb / ThreeOp /
 *                  Index / DX)
 *   Slots 50-59 : Genres  (Techno / House / Detroit / Acid / Footwork /
 *                  Jungle / Garage / Dubstep / Bossa / Glitch)
 *   Slots 60-63 : Experimental  (Frost / Magma / Smoke / Chaos)
 *
 * Each Kit B is a meaningful variation: pitch shift + decay scale + drive bump
 * + reso bump — so the Morph knob produces an actual gradient.
 * ──────────────────────────────────────────────────────────────────────────── */

typedef struct {
    const char *name;
    /* Required per-voice arrays (8 entries each) */
    uint8_t algo[NUM_VOICES];
    uint8_t note[NUM_VOICES];
    uint8_t click[NUM_VOICES];
    float   ratio[NUM_VOICES];
    float   fbk[NUM_VOICES];
    float   dec[NUM_VOICES];
    float   pe[NUM_VOICES];
    float   fm[NUM_VOICES];     /* macro m[3] = FM amount */
    float   body[NUM_VOICES];   /* macro m[4] = body wavefolder */
    float   reso[NUM_VOICES];   /* macro m[5] = resonator amount */
    float   drive[NUM_VOICES];  /* macro m[7] = drive amount */
    /* Optional filter override (bitmask: bit v set = override f1_cut/f1_type for voice v) */
    uint8_t filter_mask;
    int16_t f1_cut[NUM_VOICES];
    int8_t  f1_type[NUM_VOICES];
    /* Optional per-voice extras (0 = use default) */
    float   e1_rep[NUM_VOICES];
    float   bit[NUM_VOICES];
    float   rate[NUM_VOICES];
    uint8_t xfm[NUM_VOICES];
    int8_t  click_type[NUM_VOICES];     /* 0=Sample 1=Impulse 2=Phase */
    /* Kit-wide FX sends (applied to every voice if > 0) */
    float   fx1_send;
    float   fx2_send;
    /* Kit B variation modifiers */
    int8_t  b_pitch_off;
    float   b_dec_scale;
    float   b_drive_add;
    float   b_reso_add;
    /* FX state — note: 0 = use system default rather than 0 mix */
    float   rev_mix, rev_decay, rev_size, rev_gate;
    int8_t  rev_type;
    float   dly_mix, dly_rate, dly_fdbk, dly_tone;
    int8_t  dly_pp;
    float   cho_mix, cho_rate, cho_depth;
} fk_compact_t;

static const char *KIT_NAMES[NUM_KITS] = {
    /* 00 */ "Plastic",  "Anvil",   "Forge",   "Cinder",   "Spark",
    /* 05 */ "Dust",     "Phasma",  "Static",  "Glass",    "Marteau",
    /* 10 */ "808",      "909",     "707",     "CR-78",    "Linn",
    /* 15 */ "DMX",      "SP-12",   "Simmons", "Boom",     "Trap",
    /* 20 */ "LXR",      "Riga",    "Schmidt", "Indus",    "Lattice",
    /* 25 */ "Caustic",  "Steam",   "Iron",    "Burn",     "Cathedral",
    /* 30 */ "Razz",     "Bell",    "Chime",   "Pluck",    "Wire",
    /* 35 */ "Vinyl",    "Snap",    "Bounce",  "Toy",      "Crystal",
    /* 40 */ "Digi",     "Plaits",  "Cycles",  "Volca",    "Wavefold",
    /* 45 */ "BaseWidth","Comb",    "ThreeOp", "Index",    "DX",
    /* 50 */ "Techno",   "House",   "Detroit", "Acid",     "Footwork",
    /* 55 */ "Jungle",   "Garage",  "Dubstep", "Bossa",    "Glitch",
    /* 60 */ "Frost",    "Magma",   "Smoke",   "Chaos",
    /* 64+: filled out to NUM_KITS so KIT_NAMES is safely indexable.
       Slots 0-63 are the factory roster above. */
};

/* Re-apply algo-specific filter/ratio defaults after an algo change. Mirrors
 * the switch in voice_bank_init_default so a kit can put any algo on any voice
 * without inheriting the wrong filter type/cutoff. */
static void apply_algo_defaults(voice_bank_t *vb) {
    switch (vb->algo) {
        case ALGO_DRUM:
            vb->f1_cut = 6000; vb->f1_type = FILT_LP; vb->pe_dec = 0.04f; break;
        case ALGO_SNARE:
            vb->f1_cut = 4000; vb->f1_type = FILT_BP; vb->pe_dec = 0.04f; break;
        case ALGO_CYMBAL:
            vb->f1_cut = 8000; vb->f1_type = FILT_HP; break;
        case ALGO_HAT:
            vb->f1_cut = 9000; vb->f1_type = FILT_HP; break;
        case ALGO_WILD:
            /* Neutral wide-open LP so a Wild voice never inherits a leftover
             * band-pass from whatever algo the slot defaulted to (a low Wild
             * sine through a leaked Snare BP@4000 went silent). */
            vb->f1_cut = 12000; vb->f1_type = FILT_LP; break;
    }
}

/* Apply a compact kit spec to a kit_slot_t. Generates Kit A from the spec
 * and Kit B as a parametric variation. */
static void apply_fk_compact(kit_slot_t *k, const fk_compact_t *fk) {
    /* Reset both banks to defaults */
    for (int v = 0; v < NUM_VOICES; v++) {
        voice_bank_init_default(&k->kit_a[v], v);
    }
    /* Reset FX */
    k->rev_mix = 0; k->rev_decay = 0.5f; k->rev_size = 0.5f;
    k->rev_predelay = 0.05f; k->rev_damping = 0.5f; k->rev_type = 0;
    k->dly_mix = 0; k->dly_rate = 0.3f; k->dly_fdbk = 0.3f;
    k->dly_tone = 0.5f; k->dly_bpf_cut = 2000; k->dly_bpf_w = 0.5f;
    k->dly_pp = 0; k->dly_sync = 0;
    k->cho_mix = 0; k->cho_rate = 0.5f; k->cho_depth = 0.3f;
    k->cho_width = 0.5f; k->cho_voices = 4; k->cho_tone = 0.5f; k->cho_fb = 0.0f;

    voice_bank_t *A = k->kit_a, *B = k->kit_b;

    for (int v = 0; v < NUM_VOICES; v++) {
        /* Set the kit's algo and ALWAYS re-apply that algo's filter defaults,
         * even when the algo is unchanged — this guarantees no filter/pitch
         * setting leaks from the slot's init-default algo (e.g. slot 5 inits
         * as Snare BP@4000; a Wild/Drum kit voice there must not keep that BP).
         * The kit's own filter_mask override, if any, then applies on top. */
        A[v].algo = fk->algo[v];
        apply_algo_defaults(&A[v]);
        A[v].midi_note  = fk->note[v];
        A[v].click_smp  = fk->click[v];
        A[v].ratio_c    = fk->ratio[v];
        A[v].fbk        = fk->fbk[v];
        A[v].e1_dec     = fk->dec[v];
        A[v].pe_amt     = fk->pe[v];
        A[v].m[3]       = fk->fm[v];
        A[v].m[4]       = fk->body[v];
        A[v].m[5]       = fk->reso[v];
        A[v].m[7]       = fk->drive[v];
        /* For Drum voices the "Body" macro (m[4]) now drives the parallel
         * body-sine weight (kick punch), not the old wavefolder. Kicks sit
         * low; give them a strong register-based body-sine floor so every
         * kit's kick has weight, nudged by the kit's authored body value. */
        if (A[v].algo == ALGO_DRUM) {
            float reg = (A[v].midi_note < 42) ? 0.80f :   /* kick */
                        (A[v].midi_note < 52) ? 0.64f :   /* low tom / perc */
                                                0.50f;    /* higher perc */
            A[v].m[4] = clampf(reg + (fk->body[v] - 0.5f) * 0.4f, 0.35f, 0.95f);
        }
        /* Optional filter override */
        if ((fk->filter_mask >> v) & 1) {
            if (fk->f1_cut[v] > 0)  A[v].f1_cut  = fk->f1_cut[v];
            if (fk->f1_type[v] >= 0) A[v].f1_type = fk->f1_type[v];
        }
        /* Optional per-voice extras (zero-init = no effect) */
        A[v].e1_rep   = fk->e1_rep[v];
        A[v].bit      = fk->bit[v];
        A[v].rate     = fk->rate[v];
        A[v].xfm      = fk->xfm[v];
        if (fk->click_type[v] >= 0 && fk->click_type[v] <= 2)
            A[v].click_type = fk->click_type[v];
        /* Kit-wide FX sends */
        A[v].fx1_send = fk->fx1_send;
        A[v].fx2_send = fk->fx2_send;
    }

    /* Kit B = Kit A + variation */
    float dec_scale = (fk->b_dec_scale > 0.001f) ? fk->b_dec_scale : 1.0f;
    for (int v = 0; v < NUM_VOICES; v++) {
        B[v] = A[v];
        int b_note = (int)A[v].midi_note + (int)fk->b_pitch_off;
        if (b_note < 0)   b_note = 0;
        if (b_note > 127) b_note = 127;
        B[v].midi_note = (uint8_t)b_note;
        B[v].e1_dec    = clampf(A[v].e1_dec * dec_scale, 0.001f, 4.0f);
        B[v].m[7]      = clampf(A[v].m[7] + fk->b_drive_add, 0.0f, 1.0f);
        B[v].m[5]      = clampf(A[v].m[5] + fk->b_reso_add, 0.0f, 1.0f);
    }

    /* FX state */
    if (fk->rev_mix > 0.0f)   k->rev_mix   = fk->rev_mix;
    if (fk->rev_decay > 0.0f) k->rev_decay = fk->rev_decay;
    if (fk->rev_size > 0.0f)  k->rev_size  = fk->rev_size;
    k->rev_type = fk->rev_type;
    if (fk->dly_mix > 0.0f)   k->dly_mix   = fk->dly_mix;
    if (fk->dly_rate > 0.0f)  k->dly_rate  = fk->dly_rate;
    if (fk->dly_fdbk > 0.0f)  k->dly_fdbk  = fk->dly_fdbk;
    if (fk->dly_tone > 0.0f)  k->dly_tone  = fk->dly_tone;
    k->dly_pp = fk->dly_pp;
    if (fk->cho_mix > 0.0f)   k->cho_mix   = fk->cho_mix;
    if (fk->cho_rate > 0.0f)  k->cho_rate  = fk->cho_rate;
    if (fk->cho_depth > 0.0f) k->cho_depth = fk->cho_depth;
    k->rev_gate = fk->rev_gate;
}

/* Load a kit's stored FX state into the live instance FX. Called when a kit is
 * selected (and at init) so each kit's authored reverb/delay/chorus actually
 * takes effect — previously the per-kit FX were stored but never applied. */
static void load_kit_fx(forge_instance_t *inst, int kit) {
    if (kit < 0 || kit >= NUM_KITS) return;
    kit_slot_t *k = &inst->kits[kit];
    inst->rev_mix = k->rev_mix; inst->rev_decay = k->rev_decay;
    inst->rev_size = k->rev_size; inst->rev_predelay = k->rev_predelay;
    inst->rev_damping = k->rev_damping; inst->rev_type = k->rev_type;
    inst->rev_gate = k->rev_gate;
    inst->dly_mix = k->dly_mix; inst->dly_rate = k->dly_rate;
    inst->dly_fdbk = k->dly_fdbk; inst->dly_tone = k->dly_tone;
    inst->dly_bpf_w = k->dly_bpf_w; inst->dly_bpf_cut = k->dly_bpf_cut;
    inst->dly_pp = k->dly_pp; inst->dly_sync = k->dly_sync;
    inst->cho_mix = k->cho_mix; inst->cho_rate = k->cho_rate;
    inst->cho_depth = k->cho_depth; inst->cho_width = k->cho_width;
    inst->cho_tone = k->cho_tone; inst->cho_fb = k->cho_fb;
    inst->cho_voices = k->cho_voices;
}

/* Macro shortcuts for compactness in the table */
#define D ALGO_DRUM
#define S ALGO_SNARE
#define C ALGO_CYMBAL
#define H ALGO_HAT
#define W ALGO_WILD
#define K  CSMP_KICK
#define RM CSMP_RIM
#define HT CSMP_HAT
#define CL CSMP_CLAP
#define TM CSMP_TOM
#define SN CSMP_SNAP
#define NN CSMP_NONE
#define V(a) {a, a, a, a, a, a, a, a}   /* same value for all 8 voices */

static const fk_compact_t FACTORY_KITS[64] = {
/* ===================== Slots 0-9 : Forge originals ===================== */
/* 0 Plastic — Razzmatazz-like, high resonator everywhere, snappy clicks */
{ .name="Plastic",
  .algo={D,D,D,D,S,C,H,H}, .note={38,43,47,50,53,64,64,64},
  .click={K,RM,RM,SN,SN,HT,HT,HT},
  .ratio={1.2f,1.7f,2.3f,3.5f,2.0f,5.3f,5.3f,5.3f},
  .fbk  ={0.30f,0.35f,0.40f,0.55f,0.55f,0.60f,0.55f,0.55f},
  .dec  ={0.18f,0.16f,0.13f,0.10f,0.18f,0.40f,0.07f,0.30f},
  .pe   ={0.40f,0.35f,0.30f,0.20f,0.25f,0.05f,0.05f,0.05f},
  .fm   ={0.50f,0.55f,0.60f,0.60f,0.45f,0.40f,0.40f,0.40f},
  .body ={0.25f,0.30f,0.25f,0.20f,0.30f,0.15f,0.15f,0.15f},
  .reso ={0.65f,0.70f,0.75f,0.80f,0.60f,0.55f,0.50f,0.50f},
  .drive={0.15f,0.15f,0.15f,0.15f,0.25f,0.20f,0.15f,0.15f},
  .click_type=V(CLICK_IMPULSE), .fx2_send=0.25f,
  .b_dec_scale=1.6f, .b_reso_add=0.20f,
  .rev_mix=0.10f, .cho_mix=0.15f, .cho_rate=0.6f, .cho_depth=0.4f },

/* 1 Anvil — heavy industrial, drive maxed, ringing resonator */
{ .name="Anvil",
  .algo={D,D,D,D,S,C,H,H}, .note={33,38,42,46,50,65,65,65},
  .click={K,K,TM,RM,RM,HT,HT,HT},
  .ratio={0.75f,0.85f,1.5f,2.5f,1.7f,5.7f,5.7f,5.7f},
  .fbk  ={0.55f,0.55f,0.50f,0.65f,0.70f,0.85f,0.80f,0.80f},
  .dec  ={0.45f,0.32f,0.30f,0.18f,0.25f,0.50f,0.06f,0.45f},
  .pe   ={0.60f,0.55f,0.40f,0.30f,0.35f,0.05f,0.05f,0.05f},
  .fm   ={0.40f,0.50f,0.60f,0.65f,0.45f,0.55f,0.50f,0.50f},
  .body ={0.55f,0.50f,0.45f,0.45f,0.40f,0.35f,0.30f,0.30f},
  .reso ={0.30f,0.30f,0.25f,0.35f,0.20f,0.10f,0.10f,0.10f},
  .drive={0.65f,0.70f,0.65f,0.70f,0.70f,0.55f,0.50f,0.50f},
  .fx2_send=0.20f,
  .b_drive_add=0.15f, .b_reso_add=0.40f,
  .rev_mix=0.18f, .rev_decay=0.70f, .rev_size=0.55f, .rev_gate=0.55f },

/* 2 Forge — canonical 808-adjacent, balanced */
{ .name="Forge",
  .algo={D,D,D,D,S,C,H,H}, .note={36,41,45,48,52,60,60,60},
  .click={K,TM,TM,SN,CL,HT,HT,HT},
  .ratio={1.0f,1.5f,1.5f,3.0f,1.5f,4.7f,4.7f,4.7f},
  .fbk  ={0.20f,0.30f,0.30f,0.50f,0.50f,0.70f,0.60f,0.60f},
  .dec  ={0.40f,0.30f,0.28f,0.15f,0.22f,0.55f,0.05f,0.40f},
  .pe   ={0.70f,0.45f,0.40f,0.20f,0.30f,0.05f,0.05f,0.05f},
  .fm   ={0.30f,0.40f,0.40f,0.55f,0.50f,0.50f,0.50f,0.50f},
  .body ={0.25f,0.20f,0.20f,0.15f,0.20f,0.10f,0.10f,0.10f},
  .reso ={0.10f,0.10f,0.10f,0.20f,0.10f,0.05f,0.05f,0.05f},
  .drive={0.20f,0.15f,0.15f,0.10f,0.25f,0.20f,0.15f,0.15f},
  .fx2_send=0.30f,
  .b_pitch_off=5, .b_dec_scale=0.7f, .b_drive_add=0.10f,
  .rev_mix=0.12f, .rev_size=0.5f, .rev_decay=0.55f },

/* 3 Cinder — dark, burned, deep LP filter, long decays */
{ .name="Cinder",
  .algo={D,D,D,D,S,C,H,H}, .note={30,35,38,42,46,56,56,56},
  .click={K,TM,TM,RM,CL,HT,HT,HT},
  .ratio={0.50f,0.75f,1.0f,1.5f,1.2f,4.0f,4.0f,4.0f},
  .fbk  ={0.40f,0.40f,0.45f,0.50f,0.45f,0.55f,0.50f,0.50f},
  .dec  ={0.60f,0.55f,0.50f,0.40f,0.45f,0.80f,0.10f,0.60f},
  .pe   ={0.80f,0.55f,0.45f,0.30f,0.30f,0.05f,0.05f,0.05f},
  .fm   ={0.35f,0.40f,0.45f,0.50f,0.45f,0.40f,0.40f,0.40f},
  .body ={0.40f,0.35f,0.30f,0.30f,0.25f,0.15f,0.15f,0.15f},
  .reso ={0.20f,0.20f,0.20f,0.25f,0.20f,0.10f,0.10f,0.10f},
  .drive={0.40f,0.35f,0.30f,0.30f,0.40f,0.30f,0.25f,0.25f},
  .filter_mask=0xFF, .f1_cut={1500,1700,1900,2100,2300,2500,2700,2900},
  .f1_type=V(FILT_LP),
  .fx2_send=0.40f,
  .b_dec_scale=0.6f, .b_drive_add=-0.10f,
  .rev_mix=0.30f, .rev_size=0.75f, .rev_decay=0.70f },

/* 4 Spark — fast, bright, snappy, ping-pong delay */
{ .name="Spark",
  .algo={D,D,D,D,S,C,H,H}, .note={48,53,57,60,62,72,72,72},
  .click={K,RM,RM,SN,SN,HT,HT,HT},
  .ratio={1.5f,2.0f,2.5f,3.5f,2.5f,5.7f,5.7f,5.7f},
  .fbk  ={0.20f,0.30f,0.35f,0.45f,0.45f,0.60f,0.55f,0.55f},
  .dec  ={0.10f,0.08f,0.07f,0.05f,0.10f,0.20f,0.04f,0.18f},
  .pe   ={0.50f,0.40f,0.35f,0.25f,0.30f,0.05f,0.05f,0.05f},
  .fm   ={0.45f,0.50f,0.55f,0.60f,0.55f,0.45f,0.45f,0.45f},
  .body ={0.15f,0.10f,0.10f,0.10f,0.15f,0.10f,0.10f,0.10f},
  .reso ={0.10f,0.10f,0.10f,0.15f,0.10f,0.05f,0.05f,0.05f},
  .drive={0.10f,0.10f,0.10f,0.10f,0.20f,0.15f,0.10f,0.10f},
  .fx1_send=0.35f,
  .b_dec_scale=1.3f, .b_reso_add=0.15f,
  .dly_mix=0.22f, .dly_rate=0.18f, .dly_fdbk=0.45f, .dly_pp=1, .dly_tone=0.65f },

/* 5 Dust — lo-fi crushed, narrow filters */
{ .name="Dust",
  .algo={D,D,D,D,S,C,H,H}, .note={36,41,45,48,52,58,58,58},
  .click={K,TM,TM,RM,CL,HT,HT,HT},
  .ratio={1.0f,1.5f,1.5f,2.5f,1.5f,4.3f,4.3f,4.3f},
  .fbk  ={0.25f,0.30f,0.30f,0.45f,0.45f,0.65f,0.55f,0.55f},
  .dec  ={0.30f,0.25f,0.22f,0.15f,0.20f,0.45f,0.05f,0.30f},
  .pe   ={0.50f,0.40f,0.35f,0.25f,0.30f,0.05f,0.05f,0.05f},
  .fm   ={0.40f,0.45f,0.45f,0.55f,0.50f,0.45f,0.45f,0.45f},
  .body ={0.30f,0.25f,0.25f,0.20f,0.25f,0.15f,0.15f,0.15f},
  .reso ={0.10f,0.10f,0.10f,0.15f,0.10f,0.05f,0.05f,0.05f},
  .drive={0.20f,0.20f,0.20f,0.20f,0.30f,0.20f,0.15f,0.15f},
  .filter_mask=0xFF, .f1_cut=V(4000), .f1_type=V(FILT_LP),
  .bit=V(0.55f), .rate=V(0.45f),
  .fx2_send=0.15f,
  .b_drive_add=0.20f,
  .rev_mix=0.06f, .rev_size=0.4f },

/* 6 Phasma — drone homage, long decays + reverb wash */
{ .name="Phasma",
  .algo={D,D,D,D,S,C,H,H}, .note={33,37,42,47,50,60,60,60},
  .click={K,TM,TM,RM,CL,HT,HT,HT},
  .ratio={0.9f,1.3f,1.7f,2.3f,1.8f,4.1f,4.1f,4.1f},
  .fbk  ={0.50f,0.55f,0.60f,0.65f,0.55f,0.75f,0.65f,0.65f},
  .dec  ={1.80f,1.50f,1.30f,1.10f,1.00f,2.50f,0.40f,1.80f},
  .pe   ={0.30f,0.25f,0.25f,0.20f,0.20f,0.05f,0.05f,0.05f},
  .fm   ={0.50f,0.55f,0.55f,0.60f,0.55f,0.55f,0.50f,0.50f},
  .body ={0.30f,0.25f,0.25f,0.20f,0.25f,0.20f,0.20f,0.20f},
  .reso ={0.50f,0.55f,0.60f,0.65f,0.55f,0.55f,0.45f,0.45f},
  .drive={0.30f,0.25f,0.25f,0.20f,0.30f,0.25f,0.20f,0.20f},
  .e1_rep={0,0,0,0,0,0,0.6f,0.4f},
  .fx2_send=0.55f,
  .b_dec_scale=0.25f,
  .rev_mix=0.40f, .rev_decay=0.85f, .rev_size=0.85f },

/* 7 Static — noise / wavefolder / unstable feedback */
{ .name="Static",
  .algo={D,D,D,W,S,C,H,H}, .note={38,42,46,50,53,64,64,64},
  .click={K,RM,RM,SN,SN,HT,HT,HT},
  .ratio={1.3f,1.9f,2.7f,3.5f,2.5f,5.7f,5.7f,5.7f},
  .fbk  ={0.85f,0.85f,0.90f,0.85f,0.85f,0.95f,0.85f,0.85f},
  .dec  ={0.30f,0.25f,0.20f,0.15f,0.20f,0.40f,0.06f,0.30f},
  .pe   ={0.40f,0.35f,0.30f,0.30f,0.30f,0.05f,0.05f,0.05f},
  .fm   ={0.85f,0.85f,0.90f,0.90f,0.80f,0.85f,0.80f,0.80f},
  .body ={0.70f,0.65f,0.65f,0.60f,0.60f,0.55f,0.50f,0.50f},
  .reso ={0.20f,0.20f,0.25f,0.30f,0.20f,0.10f,0.10f,0.10f},
  .drive={0.40f,0.40f,0.40f,0.45f,0.45f,0.40f,0.35f,0.35f},
  .xfm={0,0,0,1,0,0,0,0},
  .fx1_send=0.20f,
  .b_drive_add=-0.20f, .b_reso_add=-0.10f,
  .cho_mix=0.18f, .cho_rate=0.8f, .cho_depth=0.5f },

/* 8 Glass — all-Cymbal bell array, ringing */
{ .name="Glass",
  .algo=V(C), .note={48,52,55,60,63,67,72,76},
  .click=V(NN),
  .ratio={3.7f,5.3f,7.1f,11.0f,4.7f,9.0f,13.0f,17.0f},
  .fbk  ={0.55f,0.55f,0.55f,0.55f,0.55f,0.55f,0.55f,0.55f},
  .dec  ={0.80f,0.75f,0.70f,0.65f,0.60f,0.55f,0.50f,0.45f},
  .pe   ={0.05f,0.05f,0.05f,0.05f,0.05f,0.05f,0.05f,0.05f},
  .fm   ={0.45f,0.48f,0.51f,0.54f,0.57f,0.60f,0.63f,0.66f},
  .body ={0.10f,0.10f,0.10f,0.10f,0.10f,0.10f,0.10f,0.10f},
  .reso ={0.10f,0.10f,0.10f,0.10f,0.10f,0.10f,0.10f,0.10f},
  .drive=V(0.15f),
  .click_type=V(CLICK_IMPULSE),
  .fx2_send=0.40f,
  .b_pitch_off=-12, .b_dec_scale=0.15f, .b_reso_add=-0.10f,
  .rev_mix=0.25f, .rev_size=0.65f, .rev_decay=0.55f,
  .cho_mix=0.10f },

/* 9 Marteau — mixed-algo showcase, hammer-like attacks */
{ .name="Marteau",
  .algo={D,W,S,D,C,W,H,H}, .note={34,40,44,48,55,60,63,63},
  .click={K,RM,CL,TM,HT,RM,HT,HT},
  .ratio={0.9f,2.1f,1.5f,1.8f,5.3f,3.7f,4.7f,4.7f},
  .fbk  ={0.45f,0.55f,0.50f,0.40f,0.65f,0.50f,0.60f,0.60f},
  .dec  ={0.55f,0.25f,0.20f,0.18f,0.40f,0.30f,0.06f,0.40f},
  .pe   ={0.65f,0.40f,0.35f,0.30f,0.05f,0.20f,0.05f,0.05f},
  .fm   ={0.45f,0.65f,0.55f,0.50f,0.55f,0.60f,0.45f,0.45f},
  .body ={0.50f,0.45f,0.30f,0.35f,0.25f,0.40f,0.20f,0.20f},
  .reso ={0.25f,0.40f,0.25f,0.30f,0.20f,0.35f,0.10f,0.10f},
  .drive={0.55f,0.45f,0.50f,0.45f,0.40f,0.45f,0.30f,0.30f},
  .xfm={0,1,0,0,0,1,0,0},
  .fx1_send=0.20f, .fx2_send=0.25f,
  .b_dec_scale=1.8f, .b_drive_add=0.10f,
  .dly_mix=0.10f, .dly_rate=0.35f, .dly_pp=1,
  .rev_mix=0.18f, .rev_size=0.6f },

/* ===================== Slots 10-19 : Classic drum machines ===================== */
/* 10 808 — TR-808 classic, long ringy kick, snappy claves, sizzling hat */
{ .name="808",
  .algo={D,D,D,D,S,C,H,H}, .note={36,40,43,48,50,58,58,58},
  .click={K,TM,TM,RM,CL,HT,HT,HT},
  .ratio={1.0f,1.2f,1.5f,3.0f,1.8f,4.7f,4.7f,4.7f},
  .fbk  ={0.10f,0.20f,0.20f,0.45f,0.35f,0.65f,0.55f,0.55f},
  .dec  ={0.90f,0.40f,0.35f,0.18f,0.25f,0.35f,0.05f,0.40f},
  .pe   ={0.75f,0.50f,0.45f,0.20f,0.30f,0.05f,0.05f,0.05f},
  .fm   ={0.25f,0.35f,0.35f,0.50f,0.45f,0.45f,0.45f,0.45f},
  .body ={0.15f,0.10f,0.10f,0.10f,0.15f,0.05f,0.05f,0.05f},
  .reso ={0.05f,0.05f,0.05f,0.10f,0.10f,0.05f,0.05f,0.05f},
  .drive={0.10f,0.08f,0.08f,0.08f,0.15f,0.10f,0.08f,0.08f},
  .fx2_send=0.20f,
  .b_pitch_off=5,
  .rev_mix=0.08f, .rev_size=0.4f },

/* 11 909 — TR-909 punchy, white-noise snare, crispy hats */
{ .name="909",
  .algo={D,D,D,D,S,C,H,H}, .note={36,42,46,48,52,62,62,62},
  .click={K,TM,TM,SN,CL,HT,HT,HT},
  .ratio={1.0f,1.3f,1.5f,3.0f,1.6f,5.1f,5.1f,5.1f},
  .fbk  ={0.25f,0.30f,0.30f,0.50f,0.55f,0.75f,0.65f,0.65f},
  .dec  ={0.35f,0.30f,0.25f,0.12f,0.18f,0.30f,0.04f,0.30f},
  .pe   ={0.50f,0.45f,0.40f,0.20f,0.25f,0.05f,0.05f,0.05f},
  .fm   ={0.40f,0.45f,0.45f,0.55f,0.55f,0.55f,0.50f,0.50f},
  .body ={0.20f,0.15f,0.15f,0.10f,0.15f,0.10f,0.10f,0.10f},
  .reso ={0.05f,0.05f,0.05f,0.10f,0.10f,0.05f,0.05f,0.05f},
  .drive={0.30f,0.25f,0.25f,0.20f,0.30f,0.20f,0.15f,0.15f},
  .fx2_send=0.30f,
  .b_dec_scale=0.8f, .b_drive_add=0.10f,
  .rev_mix=0.16f, .rev_size=0.55f, .rev_gate=0.60f },

/* 12 707 — TR-707 sample-flavoured, bright, less tail */
{ .name="707",
  .algo={D,D,D,D,S,C,H,H}, .note={40,45,48,52,55,64,64,64},
  .click={K,TM,TM,RM,SN,HT,HT,HT},
  .ratio={1.4f,1.8f,2.1f,3.5f,1.9f,5.5f,5.5f,5.5f},
  .fbk  ={0.20f,0.25f,0.30f,0.45f,0.40f,0.60f,0.50f,0.50f},
  .dec  ={0.25f,0.18f,0.15f,0.10f,0.16f,0.25f,0.04f,0.22f},
  .pe   ={0.40f,0.30f,0.30f,0.20f,0.25f,0.05f,0.05f,0.05f},
  .fm   ={0.35f,0.40f,0.45f,0.55f,0.50f,0.50f,0.45f,0.45f},
  .body ={0.10f,0.10f,0.10f,0.10f,0.10f,0.05f,0.05f,0.05f},
  .reso ={0.05f,0.05f,0.05f,0.10f,0.10f,0.05f,0.05f,0.05f},
  .drive={0.05f,0.05f,0.05f,0.05f,0.10f,0.10f,0.05f,0.05f},
  .filter_mask=0xFF, .f1_cut=V(6000), .f1_type=V(FILT_LP),
  .b_pitch_off=3, .b_dec_scale=1.2f,
  .rev_mix=0.06f },

/* 13 CR-78 — Roland CR-78 vintage analog, soft warm */
{ .name="CR-78",
  .algo={D,D,D,D,S,C,H,H}, .note={37,42,46,50,52,59,59,59},
  .click={K,TM,TM,RM,CL,HT,HT,HT},
  .ratio={1.0f,1.4f,1.6f,2.5f,1.5f,4.5f,4.5f,4.5f},
  .fbk  ={0.15f,0.20f,0.20f,0.40f,0.35f,0.55f,0.45f,0.45f},
  .dec  ={0.50f,0.35f,0.30f,0.15f,0.20f,0.45f,0.06f,0.35f},
  .pe   ={0.55f,0.40f,0.35f,0.20f,0.25f,0.05f,0.05f,0.05f},
  .fm   ={0.30f,0.35f,0.35f,0.50f,0.45f,0.40f,0.40f,0.40f},
  .body ={0.20f,0.15f,0.15f,0.15f,0.20f,0.10f,0.10f,0.10f},
  .reso ={0.05f,0.05f,0.05f,0.10f,0.10f,0.05f,0.05f,0.05f},
  .drive={0.15f,0.10f,0.10f,0.10f,0.20f,0.15f,0.10f,0.10f},
  .filter_mask=0xFF, .f1_cut=V(5000), .f1_type=V(FILT_LP),
  .fx2_send=0.25f,
  .b_dec_scale=1.5f,
  .rev_mix=0.15f, .rev_size=0.5f },

/* 14 Linn — LinnDrum 80s sample-flavoured, mid-bright */
{ .name="Linn",
  .algo={D,D,D,D,S,C,H,H}, .note={38,43,47,52,54,62,62,62},
  .click={K,TM,TM,RM,CL,HT,HT,HT},
  .ratio={1.2f,1.6f,1.8f,3.0f,1.7f,5.0f,5.0f,5.0f},
  .fbk  ={0.20f,0.25f,0.25f,0.45f,0.40f,0.65f,0.55f,0.55f},
  .dec  ={0.30f,0.22f,0.20f,0.12f,0.18f,0.30f,0.05f,0.25f},
  .pe   ={0.45f,0.35f,0.30f,0.20f,0.25f,0.05f,0.05f,0.05f},
  .fm   ={0.40f,0.45f,0.45f,0.55f,0.50f,0.50f,0.45f,0.45f},
  .body ={0.15f,0.10f,0.10f,0.10f,0.15f,0.10f,0.10f,0.10f},
  .reso ={0.05f,0.05f,0.05f,0.10f,0.10f,0.05f,0.05f,0.05f},
  .drive={0.10f,0.08f,0.08f,0.08f,0.15f,0.10f,0.08f,0.08f},
  .b_pitch_off=2,
  .rev_mix=0.10f },

/* 15 DMX — Oberheim DMX, heavy, dramatic */
{ .name="DMX",
  .algo={D,D,D,D,S,C,H,H}, .note={34,39,43,48,50,60,60,60},
  .click={K,K,TM,RM,CL,HT,HT,HT},
  .ratio={0.85f,1.0f,1.4f,2.5f,1.6f,4.7f,4.7f,4.7f},
  .fbk  ={0.35f,0.35f,0.30f,0.50f,0.50f,0.70f,0.60f,0.60f},
  .dec  ={0.60f,0.45f,0.35f,0.18f,0.25f,0.45f,0.06f,0.45f},
  .pe   ={0.70f,0.55f,0.45f,0.25f,0.30f,0.05f,0.05f,0.05f},
  .fm   ={0.35f,0.45f,0.50f,0.55f,0.50f,0.50f,0.45f,0.45f},
  .body ={0.30f,0.25f,0.20f,0.15f,0.20f,0.10f,0.10f,0.10f},
  .reso ={0.10f,0.10f,0.10f,0.15f,0.10f,0.05f,0.05f,0.05f},
  .drive={0.30f,0.25f,0.20f,0.20f,0.30f,0.20f,0.15f,0.15f},
  .fx2_send=0.30f,
  .b_drive_add=0.10f,
  .rev_mix=0.18f, .rev_decay=0.65f },

/* 16 SP-12 — E-mu SP-12 sampler grit */
{ .name="SP-12",
  .algo={D,D,D,D,S,C,H,H}, .note={36,41,45,49,52,60,60,60},
  .click={K,TM,TM,RM,CL,HT,HT,HT},
  .ratio={1.0f,1.4f,1.6f,2.8f,1.6f,4.7f,4.7f,4.7f},
  .fbk  ={0.20f,0.25f,0.30f,0.45f,0.45f,0.65f,0.55f,0.55f},
  .dec  ={0.40f,0.28f,0.25f,0.14f,0.22f,0.35f,0.05f,0.30f},
  .pe   ={0.50f,0.40f,0.35f,0.20f,0.30f,0.05f,0.05f,0.05f},
  .fm   ={0.40f,0.45f,0.45f,0.55f,0.50f,0.50f,0.45f,0.45f},
  .body ={0.20f,0.15f,0.15f,0.15f,0.20f,0.10f,0.10f,0.10f},
  .reso ={0.05f,0.05f,0.05f,0.10f,0.10f,0.05f,0.05f,0.05f},
  .drive={0.20f,0.15f,0.15f,0.15f,0.25f,0.20f,0.15f,0.15f},
  .filter_mask=0xFF, .f1_cut=V(8000), .f1_type=V(FILT_LP),
  .bit=V(0.30f), .rate=V(0.20f),
  .b_drive_add=0.05f,
  .rev_mix=0.08f },

/* 17 Simmons — electronic drum kit, pitched toms */
{ .name="Simmons",
  .algo={D,D,D,D,S,C,H,H}, .note={36,43,48,55,52,64,64,64},
  .click={K,TM,TM,TM,CL,HT,HT,HT},
  .ratio={1.0f,1.6f,2.0f,2.8f,1.8f,5.0f,5.0f,5.0f},
  .fbk  ={0.25f,0.30f,0.30f,0.30f,0.50f,0.65f,0.55f,0.55f},
  .dec  ={0.45f,0.55f,0.50f,0.45f,0.30f,0.35f,0.06f,0.30f},
  .pe   ={0.80f,0.75f,0.70f,0.65f,0.35f,0.05f,0.05f,0.05f},
  .fm   ={0.45f,0.50f,0.55f,0.60f,0.55f,0.50f,0.45f,0.45f},
  .body ={0.25f,0.20f,0.20f,0.20f,0.25f,0.10f,0.10f,0.10f},
  .reso ={0.10f,0.10f,0.10f,0.15f,0.10f,0.05f,0.05f,0.05f},
  .drive={0.15f,0.15f,0.15f,0.15f,0.25f,0.15f,0.10f,0.10f},
  .fx2_send=0.25f,
  .b_pitch_off=-3, .b_dec_scale=0.8f,
  .rev_mix=0.15f, .rev_decay=0.55f },

/* 18 Boom — boom bap fat kick, dusty snare */
{ .name="Boom",
  .algo={D,D,D,D,S,C,H,H}, .note={32,38,42,46,52,60,60,60},
  .click={K,K,TM,RM,CL,HT,HT,HT},
  .ratio={0.7f,0.9f,1.3f,2.0f,1.5f,4.5f,4.5f,4.5f},
  .fbk  ={0.20f,0.25f,0.30f,0.40f,0.45f,0.65f,0.55f,0.55f},
  .dec  ={0.65f,0.40f,0.30f,0.18f,0.25f,0.40f,0.05f,0.30f},
  .pe   ={0.85f,0.55f,0.40f,0.25f,0.30f,0.05f,0.05f,0.05f},
  .fm   ={0.30f,0.40f,0.45f,0.55f,0.50f,0.45f,0.45f,0.45f},
  .body ={0.30f,0.25f,0.20f,0.20f,0.25f,0.10f,0.10f,0.10f},
  .reso ={0.05f,0.05f,0.05f,0.10f,0.10f,0.05f,0.05f,0.05f},
  .drive={0.30f,0.20f,0.20f,0.15f,0.30f,0.20f,0.15f,0.15f},
  .filter_mask=0xFF, .f1_cut=V(6000), .f1_type=V(FILT_LP),
  .bit=V(0.20f),
  .fx2_send=0.20f,
  .rev_mix=0.10f },

/* 19 Trap — modern trap, thick 808 sub */
{ .name="Trap",
  .algo={D,D,D,D,S,C,H,H}, .note={28,33,38,42,52,62,62,62},
  .click={K,K,K,TM,CL,HT,HT,HT},
  .ratio={0.5f,0.6f,0.75f,1.2f,1.6f,5.1f,5.1f,5.1f},
  .fbk  ={0.10f,0.15f,0.20f,0.30f,0.50f,0.70f,0.60f,0.60f},
  .dec  ={1.20f,0.80f,0.50f,0.30f,0.20f,0.30f,0.03f,0.20f},
  .pe   ={0.70f,0.60f,0.45f,0.30f,0.30f,0.05f,0.05f,0.05f},
  .fm   ={0.30f,0.35f,0.40f,0.45f,0.55f,0.55f,0.50f,0.50f},
  .body ={0.20f,0.15f,0.15f,0.15f,0.20f,0.10f,0.10f,0.10f},
  .reso ={0.05f,0.05f,0.05f,0.10f,0.10f,0.05f,0.05f,0.05f},
  .drive={0.15f,0.15f,0.15f,0.10f,0.25f,0.15f,0.10f,0.10f},
  .fx2_send=0.25f,
  .b_pitch_off=-5,
  .rev_mix=0.18f, .rev_size=0.7f, .rev_decay=0.7f },

/* ===================== Slots 20-29 : LXR-02 / Sonic Potions / Erica ===================== */
/* 20 LXR — Erica LXR-02 direct vibe, drivable, peak filter */
{ .name="LXR",
  .algo={D,D,D,D,S,C,H,H}, .note={36,40,45,50,52,62,62,62},
  .click={K,TM,RM,SN,CL,HT,HT,HT},
  .ratio={1.0f,1.5f,2.0f,3.0f,1.7f,5.1f,5.1f,5.1f},
  .fbk  ={0.30f,0.35f,0.40f,0.55f,0.55f,0.75f,0.65f,0.65f},
  .dec  ={0.40f,0.30f,0.20f,0.13f,0.20f,0.45f,0.05f,0.35f},
  .pe   ={0.55f,0.45f,0.35f,0.25f,0.30f,0.05f,0.05f,0.05f},
  .fm   ={0.50f,0.55f,0.60f,0.65f,0.55f,0.60f,0.55f,0.55f},
  .body ={0.30f,0.25f,0.25f,0.20f,0.25f,0.20f,0.20f,0.20f},
  .reso ={0.20f,0.20f,0.20f,0.25f,0.20f,0.15f,0.15f,0.15f},
  .drive={0.45f,0.40f,0.40f,0.35f,0.45f,0.40f,0.35f,0.35f},
  .filter_mask=0xFF, .f1_cut=V(5000), .f1_type=V(FILT_PEAK),
  .fx2_send=0.30f,
  .b_drive_add=0.15f,
  .rev_mix=0.15f, .rev_decay=0.6f },

/* 21 Riga — Erica Latvian dark moody */
{ .name="Riga",
  .algo={D,D,D,D,S,C,H,H}, .note={32,36,40,44,48,58,58,58},
  .click={K,K,TM,RM,CL,HT,HT,HT},
  .ratio={0.7f,0.85f,1.2f,2.0f,1.4f,4.5f,4.5f,4.5f},
  .fbk  ={0.40f,0.45f,0.50f,0.55f,0.50f,0.65f,0.55f,0.55f},
  .dec  ={0.55f,0.45f,0.40f,0.30f,0.35f,0.60f,0.08f,0.50f},
  .pe   ={0.70f,0.55f,0.45f,0.30f,0.30f,0.05f,0.05f,0.05f},
  .fm   ={0.40f,0.45f,0.50f,0.55f,0.50f,0.45f,0.45f,0.45f},
  .body ={0.35f,0.30f,0.30f,0.25f,0.25f,0.15f,0.15f,0.15f},
  .reso ={0.25f,0.25f,0.25f,0.30f,0.25f,0.10f,0.10f,0.10f},
  .drive={0.35f,0.30f,0.30f,0.30f,0.40f,0.30f,0.25f,0.25f},
  .filter_mask=0xFF, .f1_cut=V(3000), .f1_type=V(FILT_LP),
  .fx2_send=0.40f,
  .b_drive_add=0.10f,
  .rev_mix=0.25f, .rev_size=0.7f, .rev_decay=0.7f },

/* 22 Schmidt — Sonic Potions LXR homage, repeat env claps */
{ .name="Schmidt",
  .algo={D,D,D,D,S,C,H,H}, .note={36,41,46,50,52,60,60,60},
  .click={K,TM,RM,SN,CL,HT,HT,HT},
  .ratio={1.0f,1.5f,2.0f,3.0f,1.6f,4.8f,4.8f,4.8f},
  .fbk  ={0.25f,0.30f,0.35f,0.50f,0.55f,0.70f,0.60f,0.60f},
  .dec  ={0.45f,0.30f,0.25f,0.15f,0.30f,0.45f,0.05f,0.40f},
  .pe   ={0.60f,0.45f,0.35f,0.25f,0.30f,0.05f,0.05f,0.05f},
  .fm   ={0.40f,0.45f,0.55f,0.60f,0.50f,0.50f,0.50f,0.50f},
  .body ={0.25f,0.20f,0.20f,0.20f,0.25f,0.15f,0.15f,0.15f},
  .reso ={0.15f,0.15f,0.15f,0.25f,0.15f,0.10f,0.10f,0.10f},
  .drive={0.30f,0.25f,0.25f,0.20f,0.35f,0.25f,0.20f,0.20f},
  .e1_rep={0,0,0,0,0.55f,0,0,0.30f},
  .fx2_send=0.30f,
  .b_drive_add=0.10f,
  .rev_mix=0.15f },

/* 23 Indus — heavy industrial, drive maxed everywhere */
{ .name="Indus",
  .algo={D,D,D,W,S,C,H,H}, .note={30,34,38,44,48,65,65,65},
  .click={K,K,RM,RM,RM,HT,HT,HT},
  .ratio={0.5f,0.7f,1.2f,2.5f,1.5f,5.7f,5.7f,5.7f},
  .fbk  ={0.65f,0.70f,0.70f,0.75f,0.75f,0.90f,0.80f,0.80f},
  .dec  ={0.55f,0.40f,0.35f,0.22f,0.30f,0.55f,0.06f,0.50f},
  .pe   ={0.65f,0.55f,0.45f,0.35f,0.35f,0.05f,0.05f,0.05f},
  .fm   ={0.55f,0.60f,0.65f,0.70f,0.60f,0.65f,0.60f,0.60f},
  .body ={0.60f,0.55f,0.50f,0.50f,0.45f,0.40f,0.35f,0.35f},
  .reso ={0.30f,0.30f,0.30f,0.40f,0.25f,0.15f,0.15f,0.15f},
  .drive={0.80f,0.80f,0.75f,0.80f,0.75f,0.65f,0.60f,0.60f},
  .xfm={0,0,0,1,0,0,0,0},
  .fx2_send=0.30f,
  .b_drive_add=0.15f, .b_reso_add=0.20f,
  .rev_mix=0.20f, .rev_decay=0.75f },

/* 24 Lattice — LFO-driven cross-voice routing showcase */
{ .name="Lattice",
  .algo={D,D,D,D,S,C,H,H}, .note={37,42,46,50,53,60,60,60},
  .click={K,TM,TM,RM,CL,HT,HT,HT},
  .ratio={1.1f,1.5f,1.8f,2.5f,1.6f,4.8f,4.8f,4.8f},
  .fbk  ={0.25f,0.30f,0.35f,0.45f,0.50f,0.65f,0.55f,0.55f},
  .dec  ={0.35f,0.28f,0.25f,0.15f,0.22f,0.40f,0.05f,0.30f},
  .pe   ={0.50f,0.40f,0.35f,0.25f,0.30f,0.05f,0.05f,0.05f},
  .fm   ={0.45f,0.50f,0.55f,0.60f,0.55f,0.55f,0.50f,0.50f},
  .body ={0.25f,0.20f,0.20f,0.20f,0.25f,0.15f,0.15f,0.15f},
  .reso ={0.30f,0.30f,0.30f,0.35f,0.25f,0.15f,0.15f,0.15f},
  .drive={0.25f,0.20f,0.20f,0.20f,0.30f,0.20f,0.15f,0.15f},
  .fx2_send=0.30f,
  .b_reso_add=0.20f,
  .rev_mix=0.18f },

/* 25 Caustic — drivable filter peak mode heavy */
{ .name="Caustic",
  .algo={D,D,D,D,S,C,H,H}, .note={35,40,44,48,52,62,62,62},
  .click={K,TM,RM,RM,CL,HT,HT,HT},
  .ratio={0.9f,1.3f,1.7f,2.5f,1.6f,5.0f,5.0f,5.0f},
  .fbk  ={0.40f,0.45f,0.50f,0.55f,0.50f,0.70f,0.60f,0.60f},
  .dec  ={0.40f,0.30f,0.25f,0.15f,0.20f,0.40f,0.05f,0.35f},
  .pe   ={0.55f,0.45f,0.35f,0.25f,0.30f,0.05f,0.05f,0.05f},
  .fm   ={0.45f,0.50f,0.55f,0.60f,0.55f,0.55f,0.50f,0.50f},
  .body ={0.30f,0.25f,0.25f,0.25f,0.30f,0.20f,0.20f,0.20f},
  .reso ={0.25f,0.25f,0.25f,0.30f,0.25f,0.15f,0.15f,0.15f},
  .drive={0.50f,0.45f,0.45f,0.40f,0.50f,0.40f,0.35f,0.35f},
  .filter_mask=0xFF, .f1_cut=V(2500), .f1_type=V(FILT_PEAK),
  .fx2_send=0.35f,
  .b_drive_add=0.10f,
  .rev_mix=0.22f, .rev_decay=0.65f },

/* 26 Steam — pressurised drum, long sustained body */
{ .name="Steam",
  .algo={D,D,D,W,S,C,H,H}, .note={34,38,42,48,50,60,60,60},
  .click={K,TM,TM,RM,SN,HT,HT,HT},
  .ratio={0.85f,1.2f,1.5f,2.5f,1.8f,4.7f,4.7f,4.7f},
  .fbk  ={0.45f,0.50f,0.55f,0.60f,0.55f,0.70f,0.60f,0.60f},
  .dec  ={1.00f,0.70f,0.60f,0.40f,0.50f,1.20f,0.20f,0.90f},
  .pe   ={0.40f,0.35f,0.30f,0.25f,0.25f,0.05f,0.05f,0.05f},
  .fm   ={0.50f,0.55f,0.55f,0.60f,0.55f,0.55f,0.50f,0.50f},
  .body ={0.40f,0.35f,0.35f,0.35f,0.30f,0.25f,0.20f,0.20f},
  .reso ={0.40f,0.45f,0.45f,0.45f,0.35f,0.25f,0.20f,0.20f},
  .drive={0.35f,0.30f,0.30f,0.30f,0.40f,0.30f,0.25f,0.25f},
  .fx2_send=0.50f,
  .b_dec_scale=0.5f,
  .rev_mix=0.30f, .rev_size=0.75f, .rev_decay=0.80f },

/* 27 Iron — clean industrial, neat metallic lines */
{ .name="Iron",
  .algo={D,D,D,D,S,C,H,H}, .note={36,40,44,50,52,64,64,64},
  .click={K,RM,RM,RM,CL,HT,HT,HT},
  .ratio={1.0f,1.6f,2.1f,2.8f,1.7f,5.3f,5.3f,5.3f},
  .fbk  ={0.30f,0.40f,0.45f,0.55f,0.55f,0.70f,0.60f,0.60f},
  .dec  ={0.30f,0.22f,0.18f,0.12f,0.20f,0.35f,0.05f,0.30f},
  .pe   ={0.50f,0.40f,0.35f,0.25f,0.30f,0.05f,0.05f,0.05f},
  .fm   ={0.45f,0.50f,0.55f,0.60f,0.55f,0.55f,0.50f,0.50f},
  .body ={0.20f,0.20f,0.20f,0.20f,0.20f,0.15f,0.15f,0.15f},
  .reso ={0.30f,0.35f,0.35f,0.40f,0.30f,0.20f,0.15f,0.15f},
  .drive={0.30f,0.30f,0.30f,0.30f,0.35f,0.25f,0.20f,0.20f},
  .fx2_send=0.25f,
  .b_reso_add=0.15f,
  .rev_mix=0.15f, .rev_size=0.5f },

/* 28 Burn — ring-mod intense distorted FM */
{ .name="Burn",
  .algo={D,W,W,W,S,W,H,H}, .note={36,40,44,48,50,60,62,62},
  .click={K,RM,RM,SN,SN,HT,HT,HT},
  .ratio={1.0f,2.5f,3.7f,5.1f,1.9f,7.3f,5.7f,5.7f},
  .fbk  ={0.55f,0.65f,0.70f,0.80f,0.65f,0.85f,0.70f,0.70f},
  .dec  ={0.40f,0.30f,0.25f,0.18f,0.22f,0.40f,0.06f,0.35f},
  .pe   ={0.45f,0.35f,0.30f,0.25f,0.30f,0.05f,0.05f,0.05f},
  .fm   ={0.65f,0.75f,0.80f,0.85f,0.70f,0.80f,0.65f,0.65f},
  .body ={0.40f,0.45f,0.45f,0.50f,0.40f,0.40f,0.30f,0.30f},
  .reso ={0.30f,0.35f,0.40f,0.45f,0.30f,0.20f,0.15f,0.15f},
  .drive={0.50f,0.55f,0.55f,0.55f,0.50f,0.40f,0.35f,0.35f},
  .xfm={0,1,1,1,0,1,0,0},
  .fx2_send=0.30f,
  .b_drive_add=0.20f, .b_reso_add=0.10f,
  .rev_mix=0.20f },

/* 29 Cathedral — long sacred reverb, slow attacks */
{ .name="Cathedral",
  .algo={D,D,D,D,S,C,H,H}, .note={36,40,45,50,52,62,62,62},
  .click={K,TM,TM,RM,CL,HT,HT,HT},
  .ratio={1.0f,1.4f,1.7f,2.4f,1.6f,4.9f,4.9f,4.9f},
  .fbk  ={0.25f,0.30f,0.30f,0.45f,0.45f,0.65f,0.55f,0.55f},
  .dec  ={0.80f,0.60f,0.50f,0.30f,0.40f,1.20f,0.15f,0.70f},
  .pe   ={0.40f,0.35f,0.30f,0.20f,0.25f,0.05f,0.05f,0.05f},
  .fm   ={0.40f,0.45f,0.50f,0.55f,0.50f,0.50f,0.45f,0.45f},
  .body ={0.20f,0.15f,0.15f,0.15f,0.20f,0.10f,0.10f,0.10f},
  .reso ={0.15f,0.15f,0.15f,0.20f,0.15f,0.10f,0.10f,0.10f},
  .drive={0.15f,0.15f,0.15f,0.15f,0.20f,0.15f,0.10f,0.10f},
  .fx2_send=0.65f,
  .b_dec_scale=0.6f,
  .rev_mix=0.50f, .rev_size=0.95f, .rev_decay=0.90f },

/* ===================== Slots 30-39 : Razzmatazz / per-voice resonator ===================== */
/* 30 Razz — Razzmatazz direct vibe, snap + high resonator everywhere */
{ .name="Razz",
  .algo={D,D,D,D,S,C,H,H}, .note={40,45,49,52,55,64,64,64},
  .click={K,RM,RM,SN,SN,HT,HT,HT},
  .ratio={1.3f,1.8f,2.4f,3.7f,2.1f,5.5f,5.5f,5.5f},
  .fbk  ={0.30f,0.35f,0.40f,0.55f,0.55f,0.65f,0.55f,0.55f},
  .dec  ={0.20f,0.15f,0.12f,0.08f,0.15f,0.35f,0.06f,0.25f},
  .pe   ={0.35f,0.30f,0.25f,0.20f,0.25f,0.05f,0.05f,0.05f},
  .fm   ={0.55f,0.60f,0.65f,0.65f,0.50f,0.45f,0.45f,0.45f},
  .body ={0.25f,0.25f,0.20f,0.20f,0.30f,0.15f,0.15f,0.15f},
  .reso ={0.75f,0.80f,0.85f,0.90f,0.70f,0.60f,0.55f,0.55f},
  .drive={0.20f,0.20f,0.20f,0.20f,0.30f,0.20f,0.15f,0.15f},
  .click_type=V(CLICK_IMPULSE),
  .fx2_send=0.20f,
  .b_reso_add=0.10f,
  .cho_mix=0.20f, .cho_rate=0.7f, .cho_depth=0.5f },

/* 31 Bell — pitched bell array, melodic */
{ .name="Bell",
  .algo=V(C), .note={48,52,55,60,64,67,72,76},
  .click=V(NN),
  .ratio={2.1f,3.0f,4.0f,5.3f,2.5f,3.7f,7.1f,11.0f},
  .fbk  =V(0.50f),
  .dec  ={0.90f,0.85f,0.80f,0.75f,0.70f,0.65f,0.60f,0.55f},
  .pe   =V(0.05f),
  .fm   ={0.40f,0.45f,0.50f,0.55f,0.50f,0.55f,0.60f,0.65f},
  .body =V(0.10f),
  .reso =V(0.30f),
  .drive=V(0.20f),
  .click_type=V(CLICK_IMPULSE),
  .fx2_send=0.40f,
  .b_pitch_off=12, .b_dec_scale=0.4f,
  .rev_mix=0.30f, .rev_size=0.7f, .rev_decay=0.6f },

/* 32 Chime — high bright pitches */
{ .name="Chime",
  .algo=V(C), .note={60,64,67,72,76,79,84,88},
  .click=V(NN),
  .ratio={2.5f,3.5f,4.5f,5.5f,6.5f,7.5f,8.5f,9.5f},
  .fbk  =V(0.45f),
  .dec  ={0.60f,0.55f,0.50f,0.45f,0.40f,0.35f,0.30f,0.25f},
  .pe   =V(0.05f),
  .fm   ={0.40f,0.45f,0.50f,0.55f,0.60f,0.55f,0.50f,0.45f},
  .body =V(0.10f),
  .reso =V(0.40f),
  .drive=V(0.15f),
  .click_type=V(CLICK_IMPULSE),
  .fx2_send=0.40f,
  .b_pitch_off=-7, .b_dec_scale=1.5f,
  .rev_mix=0.25f, .rev_size=0.6f, .cho_mix=0.10f },

/* 33 Pluck — Karplus-Strong-like, short pluck */
{ .name="Pluck",
  .algo={D,D,D,D,W,W,H,H}, .note={40,45,50,55,52,62,64,64},
  .click={NN,NN,NN,NN,SN,RM,HT,HT},
  .ratio={1.0f,1.3f,1.7f,2.2f,1.5f,3.7f,4.5f,4.5f},
  .fbk  ={0.40f,0.45f,0.50f,0.55f,0.55f,0.65f,0.55f,0.55f},
  .dec  ={0.30f,0.25f,0.20f,0.18f,0.20f,0.30f,0.06f,0.25f},
  .pe   ={0.20f,0.20f,0.20f,0.20f,0.25f,0.20f,0.05f,0.05f},
  .fm   ={0.50f,0.55f,0.60f,0.65f,0.55f,0.60f,0.45f,0.45f},
  .body ={0.20f,0.20f,0.20f,0.20f,0.25f,0.20f,0.15f,0.15f},
  .reso ={0.85f,0.90f,0.95f,1.00f,0.80f,0.85f,0.55f,0.55f},
  .drive={0.15f,0.15f,0.15f,0.15f,0.20f,0.15f,0.10f,0.10f},
  .click_type={2,2,2,2,1,1,0,0},  /* mostly phase init for plucks */
  .fx2_send=0.30f,
  .b_reso_add=0.05f,
  .rev_mix=0.15f, .cho_mix=0.10f },

/* 34 Wire — taut ringing, high FB resonator */
{ .name="Wire",
  .algo={D,D,W,W,W,C,H,H}, .note={40,45,50,55,60,65,68,68},
  .click={K,RM,RM,RM,RM,HT,HT,HT},
  .ratio={1.5f,2.3f,3.1f,4.0f,5.3f,5.7f,5.7f,5.7f},
  .fbk  ={0.55f,0.60f,0.65f,0.70f,0.75f,0.80f,0.70f,0.70f},
  .dec  ={0.45f,0.40f,0.35f,0.30f,0.25f,0.50f,0.06f,0.40f},
  .pe   ={0.25f,0.25f,0.25f,0.25f,0.25f,0.05f,0.05f,0.05f},
  .fm   ={0.55f,0.60f,0.65f,0.70f,0.65f,0.55f,0.50f,0.50f},
  .body ={0.30f,0.30f,0.30f,0.30f,0.30f,0.20f,0.20f,0.20f},
  .reso ={0.95f,1.00f,1.05f,1.10f,1.10f,0.70f,0.65f,0.65f},
  .drive={0.30f,0.30f,0.30f,0.30f,0.30f,0.25f,0.20f,0.20f},
  .xfm={0,0,1,1,1,0,0,0},
  .fx2_send=0.30f,
  .b_reso_add=0.15f,
  .rev_mix=0.20f },

/* 35 Vinyl — crackled plastic, lo-fi chorus */
{ .name="Vinyl",
  .algo={D,D,D,D,S,C,H,H}, .note={36,40,44,48,52,60,60,60},
  .click={K,TM,TM,RM,CL,HT,HT,HT},
  .ratio={1.0f,1.4f,1.6f,2.5f,1.5f,4.5f,4.5f,4.5f},
  .fbk  ={0.25f,0.30f,0.35f,0.45f,0.45f,0.65f,0.55f,0.55f},
  .dec  ={0.35f,0.25f,0.22f,0.13f,0.22f,0.40f,0.05f,0.30f},
  .pe   ={0.45f,0.35f,0.30f,0.20f,0.25f,0.05f,0.05f,0.05f},
  .fm   ={0.40f,0.45f,0.50f,0.55f,0.50f,0.50f,0.45f,0.45f},
  .body ={0.20f,0.20f,0.20f,0.20f,0.25f,0.15f,0.15f,0.15f},
  .reso ={0.30f,0.30f,0.30f,0.35f,0.25f,0.15f,0.15f,0.15f},
  .drive={0.20f,0.20f,0.20f,0.20f,0.30f,0.20f,0.15f,0.15f},
  .bit=V(0.25f), .rate=V(0.15f),
  .fx2_send=0.30f,
  .cho_mix=0.30f, .cho_rate=0.4f, .cho_depth=0.6f, .rev_mix=0.12f },

/* 36 Snap — snap-forward, very short transients */
{ .name="Snap",
  .algo={D,D,D,D,S,C,H,H}, .note={42,48,52,55,58,68,68,68},
  .click={SN,SN,SN,SN,SN,HT,HT,HT},
  .ratio={1.5f,2.0f,2.5f,3.5f,2.5f,5.5f,5.5f,5.5f},
  .fbk  ={0.30f,0.35f,0.40f,0.50f,0.50f,0.65f,0.55f,0.55f},
  .dec  ={0.08f,0.06f,0.05f,0.04f,0.07f,0.15f,0.03f,0.10f},
  .pe   ={0.35f,0.30f,0.25f,0.20f,0.25f,0.05f,0.05f,0.05f},
  .fm   ={0.50f,0.55f,0.60f,0.65f,0.55f,0.50f,0.45f,0.45f},
  .body ={0.20f,0.20f,0.20f,0.20f,0.25f,0.15f,0.15f,0.15f},
  .reso ={0.40f,0.40f,0.40f,0.45f,0.35f,0.20f,0.15f,0.15f},
  .drive={0.20f,0.20f,0.20f,0.20f,0.25f,0.15f,0.10f,0.10f},
  .click_type=V(CLICK_IMPULSE),
  .b_dec_scale=2.0f,
  .rev_mix=0.10f, .cho_mix=0.10f },

/* 37 Bounce — rubber bouncy, repeat envelopes */
{ .name="Bounce",
  .algo={D,D,D,D,S,C,H,H}, .note={42,46,50,54,52,60,60,60},
  .click={K,TM,RM,SN,CL,HT,HT,HT},
  .ratio={1.3f,1.7f,2.2f,3.0f,1.8f,4.7f,4.7f,4.7f},
  .fbk  ={0.30f,0.35f,0.40f,0.50f,0.50f,0.65f,0.55f,0.55f},
  .dec  ={0.20f,0.18f,0.15f,0.10f,0.20f,0.35f,0.05f,0.20f},
  .pe   ={0.45f,0.40f,0.35f,0.25f,0.30f,0.05f,0.05f,0.05f},
  .fm   ={0.45f,0.50f,0.55f,0.60f,0.55f,0.50f,0.45f,0.45f},
  .body ={0.20f,0.20f,0.20f,0.20f,0.25f,0.15f,0.15f,0.15f},
  .reso ={0.45f,0.50f,0.55f,0.55f,0.40f,0.25f,0.20f,0.20f},
  .drive={0.20f,0.20f,0.20f,0.20f,0.30f,0.20f,0.15f,0.15f},
  .e1_rep={0.40f,0.40f,0.40f,0.30f,0.40f,0,0.30f,0},
  .fx2_send=0.25f,
  .b_reso_add=0.15f,
  .rev_mix=0.15f },

/* 38 Toy — tinny high-pitch FM */
{ .name="Toy",
  .algo={D,D,D,W,S,C,H,H}, .note={55,60,64,68,62,72,76,76},
  .click={RM,RM,SN,SN,SN,HT,HT,HT},
  .ratio={2.0f,2.8f,3.5f,4.5f,2.5f,5.7f,5.7f,5.7f},
  .fbk  ={0.40f,0.50f,0.55f,0.65f,0.55f,0.70f,0.60f,0.60f},
  .dec  ={0.15f,0.12f,0.10f,0.08f,0.15f,0.30f,0.05f,0.20f},
  .pe   ={0.40f,0.35f,0.30f,0.25f,0.25f,0.05f,0.05f,0.05f},
  .fm   ={0.60f,0.65f,0.70f,0.75f,0.60f,0.60f,0.55f,0.55f},
  .body ={0.30f,0.30f,0.30f,0.35f,0.30f,0.20f,0.20f,0.20f},
  .reso ={0.55f,0.60f,0.65f,0.70f,0.50f,0.30f,0.25f,0.25f},
  .drive={0.25f,0.25f,0.25f,0.25f,0.30f,0.20f,0.15f,0.15f},
  .xfm={0,0,0,1,0,0,0,0},
  .click_type=V(CLICK_IMPULSE),
  .fx2_send=0.25f,
  .b_pitch_off=-12,
  .cho_mix=0.20f, .cho_depth=0.6f, .rev_mix=0.10f },

/* 39 Crystal — sparkly bell + reverb */
{ .name="Crystal",
  .algo=V(C), .note={55,60,64,67,72,76,79,84},
  .click=V(NN),
  .ratio={2.5f,3.5f,4.5f,5.5f,3.7f,5.3f,7.1f,8.3f},
  .fbk  =V(0.45f),
  .dec  ={0.75f,0.70f,0.65f,0.60f,0.55f,0.50f,0.45f,0.40f},
  .pe   =V(0.05f),
  .fm   ={0.35f,0.40f,0.45f,0.50f,0.45f,0.50f,0.55f,0.60f},
  .body =V(0.10f),
  .reso =V(0.55f),
  .drive=V(0.10f),
  .click_type=V(CLICK_IMPULSE),
  .fx2_send=0.55f,
  .b_pitch_off=12, .b_dec_scale=0.5f,
  .rev_mix=0.40f, .rev_size=0.8f, .rev_decay=0.65f,
  .cho_mix=0.20f },

/* ===================== Slots 40-49 : Digitone II / Plaits / Volca FM ===================== */
/* 40 Digi — Digitone II vibe, body wavefolder + complex transient */
{ .name="Digi",
  .algo={D,D,D,D,S,C,H,H}, .note={36,41,45,50,52,62,62,62},
  .click={K,RM,RM,SN,SN,HT,HT,HT},
  .ratio={1.0f,1.5f,2.0f,3.0f,1.6f,5.0f,5.0f,5.0f},
  .fbk  ={0.30f,0.40f,0.45f,0.55f,0.50f,0.70f,0.60f,0.60f},
  .dec  ={0.40f,0.30f,0.25f,0.15f,0.22f,0.40f,0.05f,0.30f},
  .pe   ={0.55f,0.45f,0.35f,0.25f,0.30f,0.05f,0.05f,0.05f},
  .fm   ={0.55f,0.60f,0.65f,0.70f,0.60f,0.60f,0.55f,0.55f},
  .body ={0.50f,0.45f,0.40f,0.40f,0.40f,0.30f,0.25f,0.25f},
  .reso ={0.20f,0.20f,0.20f,0.25f,0.20f,0.10f,0.10f,0.10f},
  .drive={0.35f,0.30f,0.30f,0.30f,0.35f,0.25f,0.20f,0.20f},
  .fx2_send=0.30f,
  .b_drive_add=0.10f,
  .rev_mix=0.15f, .rev_size=0.55f },

/* 41 Plaits — Mutable Plaits synthetic kick + snare */
{ .name="Plaits",
  .algo={D,D,D,D,S,C,H,H}, .note={36,40,44,48,50,60,60,60},
  .click={K,TM,TM,RM,CL,HT,HT,HT},
  .ratio={0.9f,1.3f,1.6f,2.5f,1.4f,4.7f,4.7f,4.7f},
  .fbk  ={0.40f,0.45f,0.50f,0.55f,0.50f,0.70f,0.60f,0.60f},
  .dec  ={0.50f,0.35f,0.30f,0.15f,0.25f,0.45f,0.06f,0.40f},
  .pe   ={0.65f,0.50f,0.40f,0.25f,0.30f,0.05f,0.05f,0.05f},
  .fm   ={0.50f,0.55f,0.60f,0.65f,0.55f,0.55f,0.50f,0.50f},
  .body ={0.30f,0.25f,0.25f,0.25f,0.30f,0.20f,0.15f,0.15f},
  .reso ={0.15f,0.15f,0.15f,0.20f,0.15f,0.10f,0.10f,0.10f},
  .drive={0.25f,0.20f,0.20f,0.20f,0.30f,0.20f,0.15f,0.15f},
  .fx2_send=0.30f,
  .b_drive_add=0.10f,
  .rev_mix=0.18f },

/* 42 Cycles — Elektron Model:Cycles FM groovebox */
{ .name="Cycles",
  .algo={D,D,D,D,S,C,H,H}, .note={36,42,46,50,52,62,62,62},
  .click={K,TM,RM,SN,CL,HT,HT,HT},
  .ratio={1.0f,1.5f,2.0f,3.0f,1.7f,5.0f,5.0f,5.0f},
  .fbk  ={0.35f,0.40f,0.45f,0.55f,0.55f,0.70f,0.60f,0.60f},
  .dec  ={0.40f,0.28f,0.22f,0.13f,0.20f,0.35f,0.05f,0.30f},
  .pe   ={0.55f,0.45f,0.35f,0.25f,0.30f,0.05f,0.05f,0.05f},
  .fm   ={0.60f,0.65f,0.70f,0.75f,0.60f,0.60f,0.55f,0.55f},
  .body ={0.30f,0.25f,0.25f,0.25f,0.30f,0.20f,0.20f,0.20f},
  .reso ={0.20f,0.20f,0.20f,0.25f,0.20f,0.10f,0.10f,0.10f},
  .drive={0.35f,0.30f,0.30f,0.30f,0.40f,0.25f,0.20f,0.20f},
  .fx2_send=0.25f,
  .b_drive_add=0.10f,
  .rev_mix=0.16f },

/* 43 Volca — Korg Volca Drum FM-based, lo-fi */
{ .name="Volca",
  .algo={D,D,D,D,S,C,H,H}, .note={38,42,46,50,53,60,60,60},
  .click={K,TM,RM,SN,CL,HT,HT,HT},
  .ratio={1.1f,1.5f,1.9f,2.7f,1.7f,4.8f,4.8f,4.8f},
  .fbk  ={0.35f,0.40f,0.45f,0.55f,0.50f,0.70f,0.60f,0.60f},
  .dec  ={0.30f,0.22f,0.18f,0.12f,0.20f,0.35f,0.05f,0.25f},
  .pe   ={0.50f,0.40f,0.35f,0.25f,0.30f,0.05f,0.05f,0.05f},
  .fm   ={0.55f,0.60f,0.65f,0.70f,0.55f,0.55f,0.50f,0.50f},
  .body ={0.25f,0.20f,0.20f,0.25f,0.25f,0.15f,0.15f,0.15f},
  .reso ={0.30f,0.35f,0.35f,0.40f,0.30f,0.15f,0.15f,0.15f},
  .drive={0.30f,0.25f,0.25f,0.25f,0.35f,0.20f,0.15f,0.15f},
  .bit=V(0.20f),
  .fx2_send=0.20f,
  .rev_mix=0.12f },

/* 44 Wavefold — body wavefolder showcase */
{ .name="Wavefold",
  .algo={D,D,D,W,S,C,H,H}, .note={36,40,44,48,52,62,62,62},
  .click={K,TM,RM,SN,SN,HT,HT,HT},
  .ratio={1.0f,1.5f,2.0f,2.8f,1.6f,5.0f,5.0f,5.0f},
  .fbk  ={0.35f,0.45f,0.50f,0.60f,0.55f,0.70f,0.60f,0.60f},
  .dec  ={0.40f,0.30f,0.25f,0.18f,0.22f,0.40f,0.05f,0.30f},
  .pe   ={0.50f,0.40f,0.35f,0.25f,0.30f,0.05f,0.05f,0.05f},
  .fm   ={0.45f,0.50f,0.55f,0.60f,0.55f,0.55f,0.50f,0.50f},
  .body ={0.75f,0.80f,0.85f,0.90f,0.70f,0.55f,0.45f,0.45f},
  .reso ={0.20f,0.20f,0.20f,0.25f,0.20f,0.10f,0.10f,0.10f},
  .drive={0.20f,0.20f,0.20f,0.25f,0.25f,0.20f,0.15f,0.15f},
  .fx2_send=0.25f,
  .b_drive_add=0.10f,
  .rev_mix=0.15f, .cho_mix=0.10f },

/* 45 BaseWidth — Base-Width pre-filter showcase (would need bw_on per-voice;
    in v0.1 we use macros and routing instead) — emphasises mid frequency bandpass */
{ .name="BaseWidth",
  .algo={D,D,D,D,S,C,H,H}, .note={36,41,45,50,52,62,62,62},
  .click={K,TM,RM,SN,CL,HT,HT,HT},
  .ratio={1.0f,1.4f,1.7f,2.5f,1.6f,4.8f,4.8f,4.8f},
  .fbk  ={0.25f,0.30f,0.35f,0.50f,0.50f,0.65f,0.55f,0.55f},
  .dec  ={0.35f,0.28f,0.25f,0.15f,0.22f,0.35f,0.05f,0.30f},
  .pe   ={0.45f,0.40f,0.35f,0.25f,0.30f,0.05f,0.05f,0.05f},
  .fm   ={0.45f,0.50f,0.55f,0.60f,0.55f,0.55f,0.50f,0.50f},
  .body ={0.25f,0.20f,0.20f,0.20f,0.25f,0.15f,0.15f,0.15f},
  .reso ={0.10f,0.10f,0.10f,0.15f,0.10f,0.05f,0.05f,0.05f},
  .drive={0.20f,0.20f,0.20f,0.20f,0.25f,0.20f,0.15f,0.15f},
  .filter_mask=0xFF, .f1_cut=V(1500), .f1_type=V(FILT_BP),
  .fx2_send=0.30f,
  .rev_mix=0.18f },

/* 46 Comb — Comb+/− filter showcase, pitched comb resonance */
{ .name="Comb",
  .algo={D,D,D,D,W,W,W,W}, .note={36,42,48,52,55,60,64,68},
  .click={K,RM,RM,SN,RM,RM,HT,HT},
  .ratio={1.0f,1.5f,2.0f,2.5f,3.0f,3.7f,4.5f,5.3f},
  .fbk  ={0.35f,0.40f,0.45f,0.50f,0.55f,0.60f,0.55f,0.55f},
  .dec  ={0.40f,0.30f,0.25f,0.20f,0.18f,0.15f,0.12f,0.10f},
  .pe   ={0.35f,0.30f,0.25f,0.20f,0.20f,0.15f,0.05f,0.05f},
  .fm   ={0.45f,0.50f,0.55f,0.60f,0.55f,0.60f,0.50f,0.50f},
  .body ={0.20f,0.20f,0.20f,0.20f,0.20f,0.20f,0.15f,0.15f},
  .reso ={0.15f,0.15f,0.15f,0.15f,0.15f,0.15f,0.10f,0.10f},
  .drive={0.20f,0.20f,0.20f,0.20f,0.20f,0.20f,0.15f,0.15f},
  .filter_mask=0xFF, .f1_cut={200,300,400,500,700,900,1200,1500},
  .f1_type=V(FILT_COMBP),
  .fx2_send=0.30f,
  .rev_mix=0.18f },

/* 47 ThreeOp — 3-op cascade showcase (all Cymbal/Hat) */
{ .name="ThreeOp",
  .algo={C,C,C,C,C,C,H,H}, .note={36,42,48,54,60,66,72,72},
  .click=V(NN),
  .ratio={3.7f,4.3f,5.1f,5.7f,7.1f,9.0f,5.7f,5.7f},
  .fbk  ={0.55f,0.60f,0.65f,0.70f,0.75f,0.80f,0.65f,0.65f},
  .dec  ={0.70f,0.60f,0.55f,0.50f,0.45f,0.40f,0.06f,0.45f},
  .pe   =V(0.05f),
  .fm   ={0.55f,0.60f,0.65f,0.70f,0.65f,0.60f,0.55f,0.55f},
  .body =V(0.15f),
  .reso ={0.40f,0.45f,0.50f,0.55f,0.45f,0.35f,0.20f,0.20f},
  .drive=V(0.25f),
  .click_type=V(CLICK_IMPULSE),
  .fx2_send=0.40f,
  .b_pitch_off=12, .b_dec_scale=0.5f,
  .rev_mix=0.20f, .rev_decay=0.65f, .cho_mix=0.10f },

/* 48 Index — FM index envelope focus, snappy attacks */
{ .name="Index",
  .algo={D,D,D,D,S,C,H,H}, .note={36,41,45,50,52,62,62,62},
  .click={K,RM,RM,SN,SN,HT,HT,HT},
  .ratio={1.0f,1.5f,2.0f,3.0f,1.6f,4.8f,4.8f,4.8f},
  .fbk  ={0.40f,0.50f,0.55f,0.65f,0.60f,0.75f,0.65f,0.65f},
  .dec  ={0.35f,0.25f,0.20f,0.15f,0.20f,0.35f,0.05f,0.30f},
  .pe   ={0.85f,0.75f,0.65f,0.50f,0.55f,0.05f,0.05f,0.05f},  /* heavy pitch env */
  .fm   ={0.70f,0.75f,0.80f,0.85f,0.70f,0.65f,0.55f,0.55f},  /* high FM */
  .body ={0.20f,0.20f,0.20f,0.20f,0.25f,0.15f,0.15f,0.15f},
  .reso ={0.15f,0.15f,0.15f,0.20f,0.15f,0.10f,0.10f,0.10f},
  .drive={0.20f,0.20f,0.20f,0.20f,0.30f,0.20f,0.15f,0.15f},
  .fx2_send=0.30f,
  .b_drive_add=0.10f,
  .rev_mix=0.18f },

/* 49 DX — Yamaha DX-flavored FM */
{ .name="DX",
  .algo={D,D,D,C,S,C,H,H}, .note={36,42,48,54,52,62,64,64},
  .click={K,RM,RM,RM,SN,HT,HT,HT},
  .ratio={1.0f,2.0f,3.0f,4.7f,1.5f,5.3f,4.7f,4.7f},
  .fbk  ={0.45f,0.55f,0.65f,0.75f,0.60f,0.80f,0.70f,0.70f},
  .dec  ={0.40f,0.30f,0.25f,0.40f,0.20f,0.45f,0.05f,0.35f},
  .pe   ={0.55f,0.45f,0.35f,0.10f,0.30f,0.05f,0.05f,0.05f},
  .fm   ={0.65f,0.70f,0.75f,0.70f,0.65f,0.65f,0.55f,0.55f},
  .body ={0.20f,0.20f,0.20f,0.20f,0.25f,0.20f,0.15f,0.15f},
  .reso ={0.20f,0.25f,0.30f,0.35f,0.20f,0.20f,0.10f,0.10f},
  .drive={0.20f,0.20f,0.20f,0.25f,0.30f,0.25f,0.20f,0.20f},
  .fx2_send=0.30f,
  .b_drive_add=0.10f, .b_reso_add=0.10f,
  .rev_mix=0.20f, .cho_mix=0.10f },

/* ===================== Slots 50-59 : Genres ===================== */
/* 50 Techno — straight 4-on-floor Berlin */
{ .name="Techno",
  .algo={D,D,D,D,S,C,H,H}, .note={36,42,46,48,50,62,62,62},
  .click={K,K,TM,RM,CL,HT,HT,HT},
  .ratio={1.0f,1.0f,1.5f,2.5f,1.6f,5.0f,5.0f,5.0f},
  .fbk  ={0.25f,0.30f,0.35f,0.50f,0.50f,0.70f,0.60f,0.60f},
  .dec  ={0.45f,0.35f,0.25f,0.15f,0.22f,0.30f,0.04f,0.25f},
  .pe   ={0.60f,0.50f,0.40f,0.25f,0.30f,0.05f,0.05f,0.05f},
  .fm   ={0.40f,0.45f,0.50f,0.55f,0.50f,0.55f,0.50f,0.50f},
  .body ={0.20f,0.20f,0.20f,0.20f,0.25f,0.15f,0.15f,0.15f},
  .reso ={0.15f,0.15f,0.15f,0.20f,0.15f,0.10f,0.10f,0.10f},
  .drive={0.35f,0.30f,0.25f,0.25f,0.35f,0.25f,0.20f,0.20f},
  .fx2_send=0.30f,
  .b_drive_add=0.10f,
  .rev_mix=0.18f, .rev_decay=0.5f, .rev_gate=0.55f },

/* 51 House — Chicago house, warm + bouncy */
{ .name="House",
  .algo={D,D,D,D,S,C,H,H}, .note={36,42,46,50,52,60,60,60},
  .click={K,TM,TM,RM,CL,HT,HT,HT},
  .ratio={1.0f,1.4f,1.7f,2.5f,1.5f,4.8f,4.8f,4.8f},
  .fbk  ={0.20f,0.25f,0.30f,0.45f,0.45f,0.65f,0.55f,0.55f},
  .dec  ={0.45f,0.30f,0.25f,0.15f,0.22f,0.35f,0.05f,0.30f},
  .pe   ={0.55f,0.45f,0.35f,0.25f,0.30f,0.05f,0.05f,0.05f},
  .fm   ={0.35f,0.45f,0.45f,0.55f,0.50f,0.50f,0.45f,0.45f},
  .body ={0.15f,0.15f,0.15f,0.15f,0.20f,0.10f,0.10f,0.10f},
  .reso ={0.10f,0.10f,0.10f,0.15f,0.10f,0.05f,0.05f,0.05f},
  .drive={0.15f,0.15f,0.15f,0.15f,0.25f,0.15f,0.10f,0.10f},
  .fx2_send=0.30f,
  .rev_mix=0.18f, .rev_size=0.55f },

/* 52 Detroit — Detroit techno, raw + driving */
{ .name="Detroit",
  .algo={D,D,D,D,S,C,H,H}, .note={34,40,44,48,52,62,62,62},
  .click={K,K,TM,RM,CL,HT,HT,HT},
  .ratio={0.85f,1.0f,1.4f,2.5f,1.6f,4.9f,4.9f,4.9f},
  .fbk  ={0.30f,0.35f,0.40f,0.50f,0.50f,0.70f,0.60f,0.60f},
  .dec  ={0.50f,0.40f,0.30f,0.18f,0.25f,0.35f,0.04f,0.30f},
  .pe   ={0.65f,0.55f,0.45f,0.25f,0.30f,0.05f,0.05f,0.05f},
  .fm   ={0.40f,0.50f,0.55f,0.60f,0.55f,0.55f,0.50f,0.50f},
  .body ={0.25f,0.25f,0.25f,0.25f,0.30f,0.20f,0.20f,0.20f},
  .reso ={0.15f,0.15f,0.15f,0.20f,0.15f,0.10f,0.10f,0.10f},
  .drive={0.45f,0.40f,0.35f,0.35f,0.45f,0.30f,0.25f,0.25f},
  .fx2_send=0.30f,
  .b_drive_add=0.15f,
  .rev_mix=0.20f, .rev_decay=0.65f, .rev_gate=0.50f },

/* 53 Acid — TB-303 flavoured, resonant filter sweep, squelchy */
{ .name="Acid",
  .algo={D,W,W,W,S,C,H,H}, .note={36,40,44,48,52,62,62,62},
  .click={K,RM,RM,SN,SN,HT,HT,HT},
  .ratio={1.0f,1.5f,2.0f,2.7f,1.6f,4.8f,4.8f,4.8f},
  .fbk  ={0.35f,0.50f,0.55f,0.65f,0.55f,0.70f,0.60f,0.60f},
  .dec  ={0.40f,0.25f,0.22f,0.18f,0.22f,0.30f,0.05f,0.25f},
  .pe   ={0.50f,0.45f,0.40f,0.35f,0.30f,0.05f,0.05f,0.05f},
  .fm   ={0.50f,0.60f,0.65f,0.70f,0.55f,0.55f,0.50f,0.50f},
  .body ={0.30f,0.35f,0.35f,0.40f,0.30f,0.20f,0.20f,0.20f},
  .reso ={0.50f,0.55f,0.60f,0.65f,0.40f,0.25f,0.20f,0.20f},
  .drive={0.35f,0.40f,0.40f,0.45f,0.35f,0.30f,0.25f,0.25f},
  .xfm={0,1,1,1,0,0,0,0},
  /* LP2 self-oscillating acid filter on the three Wild bassline voices */
  .filter_mask=0x0E, .f1_cut={0,1400,1000,800,0,0,0,0},
  .f1_type={-1,8,8,8,-1,-1,-1,-1},
  .fx2_send=0.25f,
  .b_reso_add=0.20f,
  .rev_mix=0.15f, .dly_mix=0.15f, .dly_rate=0.25f, .dly_fdbk=0.50f, .dly_pp=1 },

/* 54 Footwork — Chicago footwork, fast aggressive */
{ .name="Footwork",
  .algo={D,D,D,D,S,C,H,H}, .note={36,42,46,50,52,60,60,60},
  .click={K,TM,RM,SN,CL,HT,HT,HT},
  .ratio={1.0f,1.5f,2.0f,3.0f,1.6f,4.7f,4.7f,4.7f},
  .fbk  ={0.30f,0.40f,0.45f,0.55f,0.55f,0.70f,0.60f,0.60f},
  .dec  ={0.30f,0.20f,0.15f,0.10f,0.18f,0.25f,0.03f,0.20f},
  .pe   ={0.50f,0.40f,0.35f,0.25f,0.30f,0.05f,0.05f,0.05f},
  .fm   ={0.45f,0.50f,0.55f,0.60f,0.55f,0.55f,0.50f,0.50f},
  .body ={0.20f,0.20f,0.20f,0.20f,0.25f,0.15f,0.15f,0.15f},
  .reso ={0.15f,0.15f,0.15f,0.20f,0.15f,0.10f,0.10f,0.10f},
  .drive={0.30f,0.25f,0.25f,0.25f,0.35f,0.25f,0.20f,0.20f},
  .fx1_send=0.20f, .fx2_send=0.20f,
  .b_dec_scale=0.5f,
  .dly_mix=0.20f, .dly_rate=0.15f, .dly_pp=1, .rev_mix=0.10f },

/* 55 Jungle — drum'n'bass break-friendly */
{ .name="Jungle",
  .algo={D,D,D,D,S,C,H,H}, .note={32,40,44,48,52,62,62,62},
  .click={K,TM,TM,RM,CL,HT,HT,HT},
  .ratio={0.7f,1.3f,1.6f,2.5f,1.5f,5.0f,5.0f,5.0f},
  .fbk  ={0.25f,0.30f,0.35f,0.50f,0.50f,0.70f,0.60f,0.60f},
  .dec  ={0.55f,0.35f,0.28f,0.15f,0.22f,0.30f,0.04f,0.25f},
  .pe   ={0.75f,0.55f,0.45f,0.25f,0.30f,0.05f,0.05f,0.05f},
  .fm   ={0.35f,0.45f,0.50f,0.55f,0.50f,0.50f,0.45f,0.45f},
  .body ={0.20f,0.20f,0.20f,0.20f,0.25f,0.15f,0.15f,0.15f},
  .reso ={0.10f,0.10f,0.10f,0.15f,0.10f,0.05f,0.05f,0.05f},
  .drive={0.25f,0.20f,0.20f,0.20f,0.30f,0.20f,0.15f,0.15f},
  .fx2_send=0.30f,
  .rev_mix=0.16f, .rev_size=0.55f, .dly_mix=0.10f, .dly_rate=0.20f },

/* 56 Garage — UK garage shuffly */
{ .name="Garage",
  .algo={D,D,D,D,S,C,H,H}, .note={36,42,46,52,52,60,60,60},
  .click={K,TM,RM,SN,CL,HT,HT,HT},
  .ratio={1.0f,1.5f,2.0f,3.0f,1.6f,4.8f,4.8f,4.8f},
  .fbk  ={0.25f,0.30f,0.35f,0.50f,0.50f,0.65f,0.55f,0.55f},
  .dec  ={0.40f,0.25f,0.22f,0.12f,0.20f,0.30f,0.04f,0.22f},
  .pe   ={0.50f,0.40f,0.35f,0.25f,0.30f,0.05f,0.05f,0.05f},
  .fm   ={0.40f,0.45f,0.50f,0.55f,0.50f,0.50f,0.45f,0.45f},
  .body ={0.15f,0.15f,0.15f,0.15f,0.20f,0.10f,0.10f,0.10f},
  .reso ={0.15f,0.15f,0.15f,0.20f,0.15f,0.10f,0.10f,0.10f},
  .drive={0.20f,0.15f,0.15f,0.15f,0.25f,0.15f,0.10f,0.10f},
  .fx2_send=0.30f,
  .rev_mix=0.20f, .rev_size=0.6f },

/* 57 Dubstep — wobble drop, sub-heavy */
{ .name="Dubstep",
  .algo={D,D,D,D,S,C,H,H}, .note={28,32,38,44,50,60,60,60},
  .click={K,K,TM,RM,CL,HT,HT,HT},
  .ratio={0.5f,0.7f,1.2f,2.0f,1.5f,4.7f,4.7f,4.7f},
  .fbk  ={0.15f,0.25f,0.35f,0.50f,0.55f,0.70f,0.60f,0.60f},
  .dec  ={1.30f,0.85f,0.50f,0.25f,0.30f,0.40f,0.06f,0.35f},
  .pe   ={0.70f,0.60f,0.45f,0.30f,0.35f,0.05f,0.05f,0.05f},
  .fm   ={0.30f,0.40f,0.50f,0.60f,0.55f,0.55f,0.50f,0.50f},
  .body ={0.25f,0.20f,0.20f,0.20f,0.25f,0.15f,0.15f,0.15f},
  .reso ={0.10f,0.10f,0.10f,0.15f,0.10f,0.05f,0.05f,0.05f},
  .drive={0.35f,0.30f,0.25f,0.25f,0.40f,0.25f,0.20f,0.20f},
  .fx2_send=0.30f,
  .b_pitch_off=-5,
  .rev_mix=0.22f, .rev_size=0.75f, .rev_decay=0.75f },

/* 58 Bossa — Latin percussion */
{ .name="Bossa",
  .algo={D,D,D,D,S,C,H,H}, .note={38,42,47,51,55,64,64,64},
  .click={K,TM,TM,RM,SN,HT,HT,HT},
  .ratio={1.2f,1.6f,2.0f,2.8f,2.0f,5.3f,5.3f,5.3f},
  .fbk  ={0.20f,0.25f,0.30f,0.45f,0.45f,0.65f,0.55f,0.55f},
  .dec  ={0.30f,0.20f,0.18f,0.10f,0.18f,0.25f,0.05f,0.20f},
  .pe   ={0.50f,0.40f,0.35f,0.25f,0.30f,0.05f,0.05f,0.05f},
  .fm   ={0.45f,0.50f,0.55f,0.60f,0.55f,0.55f,0.50f,0.50f},
  .body ={0.15f,0.15f,0.15f,0.15f,0.20f,0.10f,0.10f,0.10f},
  .reso ={0.20f,0.20f,0.20f,0.25f,0.20f,0.10f,0.10f,0.10f},
  .drive={0.10f,0.10f,0.10f,0.10f,0.20f,0.15f,0.10f,0.10f},
  .fx2_send=0.30f,
  .b_pitch_off=2,
  .rev_mix=0.20f, .rev_size=0.65f },

/* 59 Glitch — broken/error sounds, randomness */
{ .name="Glitch",
  .algo={D,W,D,W,S,W,W,H}, .note={42,55,38,60,52,65,72,64},
  .click={K,SN,RM,RM,SN,HT,SN,HT},
  .ratio={2.3f,3.7f,1.1f,5.3f,2.7f,6.1f,8.0f,4.7f},
  .fbk  ={0.75f,0.85f,0.55f,0.90f,0.75f,0.85f,0.95f,0.65f},
  .dec  ={0.10f,0.06f,0.30f,0.04f,0.12f,0.08f,0.05f,0.20f},
  .pe   ={0.50f,0.30f,0.40f,0.20f,0.30f,0.20f,0.10f,0.05f},
  .fm   ={0.85f,0.90f,0.55f,0.95f,0.75f,0.80f,0.90f,0.55f},
  .body ={0.55f,0.65f,0.30f,0.70f,0.50f,0.55f,0.60f,0.20f},
  .reso ={0.40f,0.50f,0.20f,0.60f,0.30f,0.40f,0.55f,0.15f},
  .drive={0.55f,0.60f,0.30f,0.65f,0.50f,0.55f,0.60f,0.25f},
  .xfm={1,1,0,1,0,1,1,0},
  .bit={0.55f,0,0.40f,0,0.30f,0,0,0},
  .rate={0,0.45f,0,0.55f,0,0.40f,0,0},
  .fx1_send=0.20f, .fx2_send=0.25f,
  .b_drive_add=-0.20f,
  .dly_mix=0.20f, .dly_rate=0.15f, .dly_fdbk=0.55f, .dly_pp=1, .cho_mix=0.20f },

/* ===================== Slots 60-63 : Experimental ===================== */
/* 60 Frost — icy cold high-frequency */
{ .name="Frost",
  .algo={C,C,C,C,W,C,H,H}, .note={48,55,60,65,55,72,76,76},
  .click=V(NN),
  .ratio={4.7f,5.3f,6.1f,7.1f,3.5f,9.0f,5.7f,5.7f},
  .fbk  ={0.40f,0.45f,0.50f,0.55f,0.50f,0.65f,0.55f,0.55f},
  .dec  ={0.50f,0.45f,0.40f,0.35f,0.30f,0.45f,0.04f,0.30f},
  .pe   =V(0.05f),
  .fm   ={0.40f,0.45f,0.50f,0.55f,0.55f,0.55f,0.50f,0.50f},
  .body =V(0.10f),
  .reso ={0.45f,0.50f,0.55f,0.60f,0.40f,0.35f,0.20f,0.20f},
  .drive=V(0.15f),
  .click_type=V(CLICK_IMPULSE),
  .filter_mask=0xFF, .f1_cut=V(7000), .f1_type=V(FILT_HP),
  .fx2_send=0.45f,
  .b_pitch_off=7,
  .rev_mix=0.30f, .rev_size=0.85f, .rev_decay=0.75f, .cho_mix=0.20f },

/* 61 Magma — molten heavy, deep bass + drive */
{ .name="Magma",
  .algo={D,D,D,D,W,W,H,H}, .note={28,32,36,40,48,58,60,60},
  .click={K,K,K,TM,RM,HT,HT,HT},
  .ratio={0.5f,0.6f,0.8f,1.2f,2.0f,4.0f,4.7f,4.7f},
  .fbk  ={0.45f,0.50f,0.55f,0.60f,0.70f,0.80f,0.65f,0.65f},
  .dec  ={0.80f,0.60f,0.45f,0.30f,0.30f,0.40f,0.06f,0.40f},
  .pe   ={0.80f,0.65f,0.55f,0.40f,0.35f,0.20f,0.05f,0.05f},
  .fm   ={0.45f,0.55f,0.60f,0.65f,0.70f,0.75f,0.60f,0.60f},
  .body ={0.60f,0.55f,0.50f,0.45f,0.50f,0.45f,0.30f,0.30f},
  .reso ={0.20f,0.20f,0.25f,0.30f,0.40f,0.45f,0.15f,0.15f},
  .drive={0.65f,0.60f,0.55f,0.55f,0.55f,0.50f,0.40f,0.40f},
  .xfm={0,0,0,0,1,1,0,0},
  .filter_mask=0xFF, .f1_cut={1200,1500,1800,2200,3000,4000,9000,9000},
  .f1_type=V(FILT_LP),
  .fx2_send=0.30f,
  .b_drive_add=0.10f,
  .rev_mix=0.25f, .rev_size=0.75f, .rev_decay=0.75f },

/* 62 Smoke — wispy atmospheric, soft transients */
{ .name="Smoke",
  .algo={D,D,D,D,S,C,H,H}, .note={36,40,44,48,52,60,60,60},
  .click={NN,NN,NN,NN,NN,NN,NN,NN},
  .ratio={1.0f,1.3f,1.6f,2.2f,1.5f,4.5f,4.5f,4.5f},
  .fbk  ={0.20f,0.25f,0.30f,0.40f,0.45f,0.60f,0.50f,0.50f},
  .dec  ={1.20f,0.95f,0.80f,0.55f,0.65f,1.50f,0.25f,1.00f},
  .pe   ={0.30f,0.25f,0.25f,0.20f,0.20f,0.05f,0.05f,0.05f},
  .fm   ={0.40f,0.45f,0.50f,0.55f,0.50f,0.50f,0.45f,0.45f},
  .body ={0.15f,0.15f,0.15f,0.15f,0.20f,0.10f,0.10f,0.10f},
  .reso ={0.30f,0.30f,0.30f,0.35f,0.30f,0.25f,0.20f,0.20f},
  .drive={0.10f,0.10f,0.10f,0.10f,0.15f,0.10f,0.05f,0.05f},
  .click_type=V(CLICK_PHASE),
  .fx2_send=0.65f,
  .b_dec_scale=0.4f,
  .rev_mix=0.50f, .rev_size=0.95f, .rev_decay=0.90f, .cho_mix=0.25f },

/* 63 Chaos — randomized aggressive, all-Wild + max FM */
{ .name="Chaos",
  .algo=V(W), .note={36,42,48,52,55,60,65,70},
  .click={K,RM,SN,RM,SN,HT,HT,HT},
  .ratio={1.3f,2.5f,3.7f,5.1f,2.1f,4.7f,7.0f,9.0f},
  .fbk  ={0.65f,0.75f,0.85f,0.90f,0.70f,0.85f,0.95f,0.95f},
  .dec  ={0.35f,0.25f,0.18f,0.12f,0.20f,0.40f,0.08f,0.30f},
  .pe   ={0.50f,0.40f,0.30f,0.25f,0.30f,0.10f,0.05f,0.05f},
  .fm   ={0.85f,0.90f,0.95f,1.00f,0.85f,0.90f,0.80f,0.80f},
  .body ={0.70f,0.75f,0.80f,0.85f,0.65f,0.65f,0.55f,0.55f},
  .reso ={0.40f,0.50f,0.60f,0.70f,0.40f,0.50f,0.40f,0.40f},
  .drive={0.55f,0.60f,0.65f,0.70f,0.55f,0.55f,0.50f,0.50f},
  .xfm=V(1),
  .fx1_send=0.20f, .fx2_send=0.25f,
  .b_drive_add=-0.30f, .b_reso_add=-0.20f,
  .dly_mix=0.20f, .dly_rate=0.15f, .dly_fdbk=0.55f, .dly_pp=1,
  .rev_mix=0.20f, .cho_mix=0.20f },
};

#undef D
#undef S
#undef C
#undef H
#undef W
#undef K
#undef RM
#undef HT
#undef CL
#undef TM
#undef SN
#undef NN
#undef V

/* Razzmatazz-ported kits (slots 64+) — auto-generated from the .nnr preset
 * dump by dsp_refs/razz_presets/port_to_forge.py. Uses full enum names so it
 * doesn't depend on the short table macros above. */
#include "factory_kits_razz.h"

/* Apply the factory kits to the instance: the 64 Razzmatazz-ported kits go
 * FIRST (slots 0-63), then the 64 Forge originals (slots 64-127). */
static void init_factory_kits(forge_instance_t *inst) {
    int n_razz = NUM_RAZZ_KITS;
    if (n_razz > NUM_KITS) n_razz = NUM_KITS;
    for (int i = 0; i < n_razz; i++) {
        apply_fk_compact(&inst->kits[i], &FACTORY_KITS_RAZZ[i]);
    }
    int n_orig = (int)(sizeof(FACTORY_KITS) / sizeof(FACTORY_KITS[0]));
    for (int i = 0; i < n_orig && (n_razz + i) < NUM_KITS; i++) {
        apply_fk_compact(&inst->kits[n_razz + i], &FACTORY_KITS[i]);
    }
}

/* ────────────────────────────────────────────────────────────────────────────
 * Morph engine — interpolate Kit A → Kit B into live[]
 * ──────────────────────────────────────────────────────────────────────────── */

static void morph_voices(forge_instance_t *inst) {
    float t = inst->morph;
    switch (inst->morph_curve) {
        case 1: t = t * t; break;
        case 2: t = sqrtf(t); break;
        case 3: t = t * t * (3.0f - 2.0f * t); break;
        default: break;
    }
    kit_slot_t *k = &inst->kits[inst->current_kit];
    for (int v = 0; v < NUM_VOICES; v++) {
        voice_bank_t *a = &k->kit_a[v];
        voice_bank_t *b = &k->kit_b[v];
        voice_bank_t *L = &inst->live[v];
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
        L->bit        = a->bit + t * (b->bit - a->bit);
        L->rate       = a->rate + t * (b->rate - a->rate);
        L->kt1        = a->kt1 + t * (b->kt1 - a->kt1);
        L->kt2        = a->kt2 + t * (b->kt2 - a->kt2);
        L->e1_to_cut  = a->e1_to_cut + t * (b->e1_to_cut - a->e1_to_cut);
        L->e2_to_cut  = a->e2_to_cut + t * (b->e2_to_cut - a->e2_to_cut);
        L->e1_atk     = a->e1_atk + t * (b->e1_atk - a->e1_atk);
        L->e1_dec     = a->e1_dec + t * (b->e1_dec - a->e1_dec);
        L->e1_crv     = a->e1_crv + t * (b->e1_crv - a->e1_crv);
        L->e1_rep     = a->e1_rep + t * (b->e1_rep - a->e1_rep);
        L->e1_rep_rate= a->e1_rep_rate + t * (b->e1_rep_rate - a->e1_rep_rate);
        L->e2_atk     = a->e2_atk + t * (b->e2_atk - a->e2_atk);
        L->e2_dec     = a->e2_dec + t * (b->e2_dec - a->e2_dec);
        L->e2_crv     = a->e2_crv + t * (b->e2_crv - a->e2_crv);
        L->e2_dest    = (t < 0.5f) ? a->e2_dest : b->e2_dest;
        L->pe_amt     = a->pe_amt + t * (b->pe_amt - a->pe_amt);
        L->pe_dec     = a->pe_dec + t * (b->pe_dec - a->pe_dec);
        L->pe_crv     = a->pe_crv + t * (b->pe_crv - a->pe_crv);
        L->pe_dest    = (t < 0.5f) ? a->pe_dest : b->pe_dest;
        L->fie1_amt   = a->fie1_amt + t * (b->fie1_amt - a->fie1_amt);
        L->fie1_dec   = a->fie1_dec + t * (b->fie1_dec - a->fie1_dec);
        L->fie1_crv   = a->fie1_crv + t * (b->fie1_crv - a->fie1_crv);
        L->fie2_amt   = a->fie2_amt + t * (b->fie2_amt - a->fie2_amt);
        L->fie2_dec   = a->fie2_dec + t * (b->fie2_dec - a->fie2_dec);
        L->fie2_crv   = a->fie2_crv + t * (b->fie2_crv - a->fie2_crv);
        L->lfo_w      = (t < 0.5f) ? a->lfo_w : b->lfo_w;
        L->lfo_w2     = (t < 0.5f) ? a->lfo_w2 : b->lfo_w2;
        L->lfo_morph  = a->lfo_morph + t * (b->lfo_morph - a->lfo_morph);
        L->lfo_r      = a->lfo_r + t * (b->lfo_r - a->lfo_r);
        L->lfo_s      = (t < 0.5f) ? a->lfo_s : b->lfo_s;
        L->lfo_d      = a->lfo_d + t * (b->lfo_d - a->lfo_d);
        L->lfo_p      = a->lfo_p + t * (b->lfo_p - a->lfo_p);
        L->lfo_pol    = (t < 0.5f) ? a->lfo_pol : b->lfo_pol;
        L->lfo_rt     = (t < 0.5f) ? a->lfo_rt : b->lfo_rt;
        L->xlfo_src   = (t < 0.5f) ? a->xlfo_src : b->xlfo_src;
        L->trig_rst   = (t < 0.5f) ? a->trig_rst : b->trig_rst;
        L->mod_src    = (t < 0.5f) ? a->mod_src : b->mod_src;
        L->mod_dest   = (t < 0.5f) ? a->mod_dest : b->mod_dest;
        L->mod_dpth   = a->mod_dpth + t * (b->mod_dpth - a->mod_dpth);
        L->mod_crv    = (t < 0.5f) ? a->mod_crv : b->mod_crv;
        L->pan        = a->pan + t * (b->pan - a->pan);
        L->fx1_send   = a->fx1_send + t * (b->fx1_send - a->fx1_send);
        L->fx2_send   = a->fx2_send + t * (b->fx2_send - a->fx2_send);
    }
}

/* ────────────────────────────────────────────────────────────────────────────
 * cv_* bank pointer (selects A or B based on context)
 * ──────────────────────────────────────────────────────────────────────────── */

static voice_bank_t *cv_bank(forge_instance_t *inst) {
    kit_slot_t *k = &inst->kits[inst->current_kit];
    return inst->current_kit_context ? &k->kit_b[inst->current_voice]
                                     : &k->kit_a[inst->current_voice];
}

/* ────────────────────────────────────────────────────────────────────────────
 * Save / Load — Phasma-style binary persistence
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
 * FX — Delay (BPF in feedback, ping-pong)
 * ──────────────────────────────────────────────────────────────────────────── */

static void delay_init(delay_state_t *d) {
    memset(d, 0, sizeof(*d));
}

static void delay_process(
    delay_state_t *d,
    float in_l, float in_r,
    float *out_l, float *out_r,
    int   delay_samps,
    float feedback,
    int   pingpong,
    float bpf_lp_coef, float bpf_hp_coef,
    float tone)
{
    if (delay_samps < 16) delay_samps = 16;
    if (delay_samps >= DELAY_BUF_SAMPS) delay_samps = DELAY_BUF_SAMPS - 1;

    int read_idx = d->write_idx - delay_samps;
    while (read_idx < 0) read_idx += DELAY_BUF_SAMPS;

    float echo_l = d->buf_l[read_idx % DELAY_BUF_SAMPS];
    float echo_r = d->buf_r[read_idx % DELAY_BUF_SAMPS];

    /* Tone shaping in feedback path: HPF then LPF */
    d->fb_hp_l += bpf_hp_coef * (echo_l - d->fb_hp_l) + DENORM_EPS;
    d->fb_hp_r += bpf_hp_coef * (echo_r - d->fb_hp_r) + DENORM_EPS;
    float hp_l = echo_l - d->fb_hp_l;
    float hp_r = echo_r - d->fb_hp_r;
    d->fb_lp_l += bpf_lp_coef * (hp_l - d->fb_lp_l) + DENORM_EPS;
    d->fb_lp_r += bpf_lp_coef * (hp_r - d->fb_lp_r) + DENORM_EPS;
    float fb_l = fast_tanh(d->fb_lp_l * 1.05f) * 0.95f;
    float fb_r = fast_tanh(d->fb_lp_r * 1.05f) * 0.95f;

    /* Mix tone (dry blend with feedback-filtered) */
    float tone_blend = clampf(tone, 0.0f, 1.0f);
    fb_l = fb_l * tone_blend + echo_l * (1.0f - tone_blend);
    fb_r = fb_r * tone_blend + echo_r * (1.0f - tone_blend);

    /* Ping-pong cross-feedback */
    if (pingpong) {
        d->buf_l[d->write_idx] = in_l + fb_r * feedback;
        d->buf_r[d->write_idx] = in_r + fb_l * feedback;
    } else {
        d->buf_l[d->write_idx] = in_l + fb_l * feedback;
        d->buf_r[d->write_idx] = in_r + fb_r * feedback;
    }

    d->write_idx++;
    if (d->write_idx >= DELAY_BUF_SAMPS) d->write_idx = 0;

    *out_l = echo_l;
    *out_r = echo_r;
}

/* ────────────────────────────────────────────────────────────────────────────
 * FX — Reverb (Dattorro figure-8 plate, simplified)
 *
 * Topology: input → 4 input diffuser allpasses → 2 cross-coupled tanks.
 * Each tank: modulated allpass → delay1 → damping LPF → delay2.
 * Tank A's delay2 output × decay feeds tank B input, and vice-versa.
 *
 * Concept ported from KrautDrums (which ported Dattorro 1997 JAES paper).
 * ──────────────────────────────────────────────────────────────────────────── */

static void reverb_init(reverb_state_t *r) {
    memset(r, 0, sizeof(*r));
    r->tank_a1_len = 4096;
    r->tank_a2_len = 6144;
    r->tank_b1_len = 5120;
    r->tank_b2_len = 7168;
    r->tank_a_ap_len = 380;
    r->tank_b_ap_len = 444;
}

static inline float ap_tick(float in, float *buf, int *idx, int len, float g) {
    int i = *idx % len;
    float delayed = buf[i];
    float out = -g * in + delayed;
    buf[i] = in + g * out + DENORM_EPS;
    *idx = (i + 1) % len;
    return out;
}

static void reverb_process(
    reverb_state_t *r,
    float in_l, float in_r,
    float *out_l, float *out_r,
    float decay, float damp_amt, float size, float gate)
{
    float in = (in_l + in_r) * 0.5f;

    /* Gate Box (Machinedrum): an envelope follower on the input opens a gate
     * on each transient; the gate then closes fast, chopping the reverb tail
     * for the classic gated-reverb slam. gate=0 → bypass (natural tail). */
    float gate_gain = 1.0f;
    if (gate > 0.001f) {
        float lvl = fabsf(in);
        if (lvl > r->gate_env) r->gate_env = lvl;            /* instant attack */
        else r->gate_env += 0.002f * (lvl - r->gate_env);    /* slow release   */
        /* Open the gate when input transient present; hold time set by `gate`
         * (short at gate=1, longer at low gate). */
        if (r->gate_env > 0.02f) r->gate_level = 1.0f;
        float close = 0.0006f + (1.0f - gate) * 0.004f;      /* gate close rate */
        r->gate_level -= close;
        if (r->gate_level < 0.0f) r->gate_level = 0.0f;
        gate_gain = r->gate_level;
    }

    /* Pre-delay */
    int pre_idx = r->pre_idx;
    float pre_out = r->pre[pre_idx];
    r->pre[pre_idx] = in + DENORM_EPS;
    r->pre_idx = (pre_idx + 1) % REV_PRE_LEN;
    in = pre_out;

    /* Input diffusion: 4 series allpasses */
    in = ap_tick(in, r->ap_a, &r->ap_a_idx, REV_DA_LEN, 0.75f);
    in = ap_tick(in, r->ap_b, &r->ap_b_idx, REV_DB_LEN, 0.75f);
    in = ap_tick(in, r->ap_c, &r->ap_c_idx, REV_DC_LEN, 0.625f);
    in = ap_tick(in, r->ap_d, &r->ap_d_idx, REV_DD_LEN, 0.625f);

    /* Tank A & B sizes scaled by `size` */
    float scale = 0.5f + size * 0.5f;
    int a1_len = (int)(r->tank_a1_len * scale);
    int a2_len = (int)(r->tank_a2_len * scale);
    int b1_len = (int)(r->tank_b1_len * scale);
    int b2_len = (int)(r->tank_b2_len * scale);
    if (a1_len > REV_TANK_LEN) a1_len = REV_TANK_LEN;
    if (a2_len > REV_TANK_LEN) a2_len = REV_TANK_LEN;
    if (b1_len > REV_TANK_LEN) b1_len = REV_TANK_LEN;
    if (b2_len > REV_TANK_LEN) b2_len = REV_TANK_LEN;

    /* Tank A: input + cross-feedback from B */
    float a_in = in + r->out_b * decay;
    a_in = ap_tick(a_in, r->tank_a_ap, &r->tank_a_ap_idx, r->tank_a_ap_len, -0.7f);
    /* delay1 */
    int a1_i = r->tank_a1_idx % a1_len;
    float a1_out = r->tank_a1[a1_i];
    r->tank_a1[a1_i] = a_in + DENORM_EPS;
    r->tank_a1_idx = (a1_i + 1) % a1_len;
    /* damping LPF */
    r->damp_a += damp_amt * (a1_out - r->damp_a) + DENORM_EPS;
    /* static allpass + delay2 */
    float a_mid = ap_tick(r->damp_a, r->tank_a2, &r->tank_a2_idx, a2_len, 0.5f);
    r->out_a = a_mid;

    /* Tank B */
    float b_in = in + r->out_a * decay;
    b_in = ap_tick(b_in, r->tank_b_ap, &r->tank_b_ap_idx, r->tank_b_ap_len, -0.7f);
    int b1_i = r->tank_b1_idx % b1_len;
    float b1_out = r->tank_b1[b1_i];
    r->tank_b1[b1_i] = b_in + DENORM_EPS;
    r->tank_b1_idx = (b1_i + 1) % b1_len;
    r->damp_b += damp_amt * (b1_out - r->damp_b) + DENORM_EPS;
    float b_mid = ap_tick(r->damp_b, r->tank_b2, &r->tank_b2_idx, b2_len, 0.5f);
    r->out_b = b_mid;

    /* Stereo output: take taps from opposite tanks, chopped by the gate. */
    *out_l = (r->out_a * 0.6f + r->out_b * 0.3f) * gate_gain;
    *out_r = (r->out_b * 0.6f + r->out_a * 0.3f) * gate_gain;
}

/* ────────────────────────────────────────────────────────────────────────────
 * FX — Panoramic chorus (DaisySP-inspired, multi-tap stereo)
 *
 * 4 detuned LFO-modulated taps with stereo width control.
 * ──────────────────────────────────────────────────────────────────────────── */

static void chorus_init(chorus_state_t *c) {
    memset(c, 0, sizeof(*c));
    /* Stagger initial phases */
    c->lfo_phase[0] = 0.00f;
    c->lfo_phase[1] = 0.25f;
    c->lfo_phase[2] = 0.50f;
    c->lfo_phase[3] = 0.75f;
}

static void chorus_process(
    chorus_state_t *c,
    float in_l, float in_r,
    float *out_l, float *out_r,
    float rate_hz, float depth, float width, int voices, float fb,
    float tone)
{
    if (voices < 2) voices = 2;
    if (voices > CHO_TAPS) voices = CHO_TAPS;

    /* Mix L+R into the buffer (mono-input chorus, stereo output via per-tap pan) */
    float in_mono = (in_l + in_r) * 0.5f + c->fb_state * fb;
    c->buf[c->write_idx] = in_mono + DENORM_EPS;

    float lfo_inc = rate_hz * SR_INV;
    float base_delay_samps = 5.0f * 0.001f * SAMPLE_RATE;     /* 5 ms base */
    float depth_samps      = 4.0f * 0.001f * SAMPLE_RATE * depth;

    float l_sum = 0.0f, r_sum = 0.0f;
    for (int t = 0; t < voices; t++) {
        c->lfo_phase[t] += lfo_inc;
        if (c->lfo_phase[t] >= 1.0f) c->lfo_phase[t] -= 1.0f;
        float lfo = lookup_sine(c->lfo_phase[t]);
        float delay = base_delay_samps + depth_samps * (1.0f + lfo) * 0.5f
                    + (float)t * 1.5f * 0.001f * SAMPLE_RATE;
        float read_pos = (float)c->write_idx - delay;
        while (read_pos < 0.0f) read_pos += (float)CHO_BUF_SIZE;
        int   r0 = (int)read_pos;
        float frac = read_pos - (float)r0;
        int   r1 = (r0 + 1) % CHO_BUF_SIZE;
        float tap = c->buf[r0] * (1.0f - frac) + c->buf[r1] * frac;
        /* Pan each tap based on width */
        float pan = ((float)t / (float)(voices - 1) - 0.5f) * width;
        l_sum += tap * (1.0f - (pan + 0.5f));
        r_sum += tap * (pan + 0.5f);
    }

    float scale = 1.0f / (float)voices;
    l_sum *= scale * 1.5f;
    r_sum *= scale * 1.5f;

    /* Tone: 1-pole LP applied to wet */
    /* (caller passes tone_coef effectively via damping; for v0.1 just apply
       per-channel one-pole using fb_state as tone-LP shared state) */
    /* (Skipping per-block coefficient calc here — tone is folded into
       caller via the already-tone-mixed signal upstream.) */
    (void)tone;

    c->fb_state = (l_sum + r_sum) * 0.5f;
    c->write_idx = (c->write_idx + 1) % CHO_BUF_SIZE;
    *out_l = l_sum;
    *out_r = r_sum;
}

/* ────────────────────────────────────────────────────────────────────────────
 * FX — Bus compressor (EMT-156 / Neumann broadcast-limiter style)
 * Concept ported from KrautDrums (MIT, same author). Sub-ms attack for drums.
 * ──────────────────────────────────────────────────────────────────────────── */

static void compressor_init(compressor_state_t *c) {
    c->env  = 0.0f;
    c->gain = 1.0f;
}

static inline void compressor_process(
    compressor_state_t *c,
    float *l, float *r,
    float amount)
{
    if (amount < 0.001f) return;

    float prev_l = (*l) * c->gain;
    float prev_r = (*r) * c->gain;
    float det = fmaxf(fabsf(prev_l), fabsf(prev_r));

    float attack_ms = 1.0f - amount * 0.95f;
    float attack_coef = 1.0f - expf(-1.0f / (attack_ms * 0.001f * SAMPLE_RATE));
    const float release_coef = 0.000151f;
    float coef = (det > c->env) ? attack_coef : release_coef;
    c->env += coef * (det - c->env) + DENORM_EPS;

    float threshold = 1.0f - amount * 0.75f;
    const float ratio_inv = 0.25f;

    float target_gain = 1.0f;
    if (c->env > threshold) {
        float over = c->env - threshold;
        target_gain = (threshold + over * ratio_inv) / c->env;
        target_gain *= 1.0f + amount * 0.4f;
    }
    c->gain += 0.04f * (target_gain - c->gain);

    *l *= c->gain;
    *r *= c->gain;
}

/* ────────────────────────────────────────────────────────────────────────────
 * FX — 3-band EQ (low shelf + mid peak + high shelf via 1-pole tricks)
 *
 * Cheap shelf approximations using one-pole splits. For drum machine
 * tone-shaping a peaking biquad in the mid is more useful than a true shelf.
 * ──────────────────────────────────────────────────────────────────────────── */

static inline void eq_process(
    float *in_out_l, float *in_out_r,
    eq_state_t *st,
    float lo_db, float mid_db, float hi_db,
    float lo_coef, float mid_lp_coef, float mid_hp_coef, float hi_coef)
{
    /* Per-band gain factors */
    float lo_g = powf(10.0f, lo_db / 20.0f);
    float mid_g = powf(10.0f, mid_db / 20.0f);
    float hi_g = powf(10.0f, hi_db / 20.0f);

    /* L */
    float l = *in_out_l;
    /* Lo shelf via 1-pole LP boost */
    st->lo_st += lo_coef * (l - st->lo_st) + DENORM_EPS;
    float lo = st->lo_st * (lo_g - 1.0f);
    /* Hi shelf via 1-pole HP boost */
    st->hi_st += hi_coef * (l - st->hi_st) + DENORM_EPS;
    float hi = (l - st->hi_st) * (hi_g - 1.0f);
    /* Mid band: HP-then-LP cascade for bandpass-like response */
    st->mid_st1 += mid_hp_coef * (l - st->mid_st1) + DENORM_EPS;
    float mid_hp = l - st->mid_st1;
    st->mid_st2 += mid_lp_coef * (mid_hp - st->mid_st2) + DENORM_EPS;
    float mid = st->mid_st2 * (mid_g - 1.0f);
    *in_out_l = l + lo + mid + hi;
    /* R: simple — apply same shelf gains using the L-driven state (cheap) */
    /* For v0.1 we use the L state for both — acceptable for drum-bus EQ. */
    float r = *in_out_r;
    *in_out_r = r + (st->lo_st * (lo_g - 1.0f))
                  + (st->mid_st2 * (mid_g - 1.0f))
                  + ((r - st->hi_st) * (hi_g - 1.0f));
}

/* ────────────────────────────────────────────────────────────────────────────
 * Lifecycle
 * ──────────────────────────────────────────────────────────────────────────── */

static void *create_instance(const char *module_dir, const char *json_defaults) {
    (void)module_dir; (void)json_defaults;
    forge_instance_t *inst = calloc(1, sizeof(forge_instance_t));
    if (!inst) return NULL;

    init_sine_table();

    /* Init all 64 kit slots with defaults, install factory kits in slots 0-9,
     * then overlay user saves (which may override the factory kits in-place). */
    for (int i = 0; i < NUM_KITS; i++) kit_slot_init_default(&inst->kits[i]);
    init_factory_kits(inst);
    forge_load_kits(inst);

    inst->current_kit = 0;
    inst->morph = 0.0f;
    inst->morph_src = 0;
    inst->morph_curve = 0;
    inst->current_voice = 0;
    inst->current_kit_context = 0;
    inst->current_page = PAGE_PATCH;
    inst->all_decay_mult = 1.0f;
    inst->all_punch = 0.5f; inst->all_bright = 0.5f; inst->all_drive = 0.5f;
    inst->all_snap = 0.5f; inst->all_bend = 0.5f; inst->all_tune = 0.5f;
    inst->all_fx = 0.5f;
    inst->rev_gate = 0.0f;
    inst->save_kit_state = 0;
    inst->rng = 0xC0FFEEu;

    for (int i = 0; i < NUM_VOICES; i++) inst->v_lvl[i] = 0.85f;

    inst->rev_decay = 0.5f; inst->rev_size = 0.5f; inst->rev_predelay = 0.05f; inst->rev_damping = 0.5f;
    inst->dly_rate = 0.3f; inst->dly_fdbk = 0.3f; inst->dly_tone = 0.5f;
    inst->dly_bpf_cut = 2000; inst->dly_bpf_w = 0.5f;
    inst->cho_rate = 0.5f; inst->cho_depth = 0.3f; inst->cho_width = 0.5f;
    inst->cho_voices = 4; inst->cho_tone = 0.5f; inst->cho_fb = 0.0f;

    inst->master = 0.85f;
    inst->lo_freq = 200.0f; inst->mid_freq = 1000.0f; inst->hi_freq = 5000.0f;
    inst->q_lo = 0.7f; inst->q_mid = 0.7f; inst->q_hi = 0.7f;
    inst->limiter = 1;
    inst->midi_ch = 1;
    inst->same_freq = 440;

    /* Init FX state */
    delay_init(&inst->delay_st);
    reverb_init(&inst->reverb_st);
    chorus_init(&inst->chorus_st);
    compressor_init(&inst->comp_st);

    /* Generate click sample bank */
    init_click_bank(inst);

    /* Init RNGs in voice states for noise */
    for (int v = 0; v < NUM_VOICES; v++) {
        inst->voice[v].rng = 0xBEEF0001u + (uint32_t)v * 0x100u;
    }

    load_kit_fx(inst, inst->current_kit);   /* apply kit 0's authored FX at startup */
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

    /* Choke groups */
    if (vb->choke > 0) {
        for (int j = 0; j < NUM_VOICES; j++) {
            if (j == idx) continue;
            if (inst->live[j].choke == vb->choke) {
                inst->voice[j].active = 0;
                inst->voice[j].e1_state = 0;
                inst->voice[j].e2_state = 0;
                inst->voice[j].pe_state = 0;
            }
        }
    }

    vs->active = 1;
    vs->velocity = vel;
    vs->e1_state = 1; vs->e1_v = 0.0f; vs->e1_t = 0.0f; vs->e1_rep_cnt = 0;
    vs->e2_state = 1; vs->e2_v = 0.0f; vs->e2_t = 0.0f;
    vs->pe_state = 1; vs->pe_v = 1.0f; vs->pe_t = 0.0f;
    vs->fie1_state = 1; vs->fie1_v = 1.0f; vs->fie1_t = 0.0f;
    vs->fie2_state = 1; vs->fie2_v = 1.0f; vs->fie2_t = 0.0f;
    vs->ph_a = vb->phase;
    vs->ph_b = 0.0f;
    vs->ph_c = 0.0f;
    vs->ph_mod = 0.0f;
    vs->ph_body = vb->phase;
    vs->fb_state[0] = vs->fb_state[1] = 0.0f;
    vs->click_idx = 0;
    vs->click_active = 1;
    vs->lfo_phase_prev = 0.0f;
    if (vb->lfo_rt) vs->lfo_phase = vb->lfo_p;

    /* LFO phase reset on trigger sources (cross-voice trigger reset) */
    for (int j = 0; j < NUM_VOICES; j++) {
        voice_bank_t *jvb = &inst->live[j];
        if (jvb->trig_rst > 0 && jvb->trig_rst - 1 == idx) {
            inst->voice[j].lfo_phase = jvb->lfo_p;
            inst->voice[j].lfo_phase_prev = 0.0f;
        }
    }
}

static void on_midi(void *instance, const uint8_t *msg, int len, int source) {
    (void)source;
    forge_instance_t *inst = (forge_instance_t *)instance;
    if (!inst || len < 1) return;

    uint8_t status = msg[0] & 0xF0;
    uint8_t note   = (len >= 2) ? msg[1] : 0;
    uint8_t vel    = (len >= 3) ? msg[2] : 0;

    if (status == 0x90 && vel > 0) {
        int pad = (int)note - FIRST_PAD_NOTE;
        if (pad >= 0 && pad < 16) {
            int voice_idx = pad % NUM_VOICES;
            inst->current_voice = voice_idx;
            inst->current_kit_context = (pad >= NUM_VOICES) ? 1 : 0;
            trigger_voice(inst, voice_idx, vel / 127.0f);
        }
    }
    /* Note-off is a no-op: Forge voices are one-shot (drum machine semantics);
     * the AD envelope decays autonomously through Env1's set decay time. */
}

/* ────────────────────────────────────────────────────────────────────────────
 * Parameters — cv_* routing
 * ──────────────────────────────────────────────────────────────────────────── */

static int handle_cv_set(forge_instance_t *inst, const char *key, const char *val) {
    voice_bank_t *vb = cv_bank(inst);
    float f = (float)atof(val);
    int   n = atoi(val);

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
    if (strcmp(key, "cv_f1_type") == 0)    { vb->f1_type = clampi(n, 0, 8); return 1; }
    if (strcmp(key, "cv_bw_cut") == 0)     { vb->bw_cut = clampi(n, 20, 20000); return 1; }
    if (strcmp(key, "cv_bw_w") == 0)       { vb->bw_w = clampf(f, 0.0f, 1.0f); return 1; }
    if (strcmp(key, "cv_routing") == 0)    { vb->routing = clampi(n, 0, 3); return 1; }
    if (strcmp(key, "cv_f1_drv") == 0)     { vb->f1_drv = clampf(f, 0.0f, 1.0f); return 1; }
    if (strcmp(key, "cv_f2_cut") == 0)     { vb->f2_cut = clampi(n, 20, 20000); return 1; }
    if (strcmp(key, "cv_f2_res") == 0)     { vb->f2_res = clampf(f, 0.5f, 20.0f); return 1; }
    if (strcmp(key, "cv_f2_type") == 0)    { vb->f2_type = clampi(n, 0, 8); return 1; }
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
    if (strcmp(key, "cv_fie1_amt") == 0)   { vb->fie1_amt = clampf(f, 0.0f, 1.0f); return 1; }
    if (strcmp(key, "cv_fie1_dec") == 0)   { vb->fie1_dec = clampf(f, 0.002f, 2.0f); return 1; }
    if (strcmp(key, "cv_fie2_amt") == 0)   { vb->fie2_amt = clampf(f, 0.0f, 1.0f); return 1; }
    if (strcmp(key, "cv_fie2_dec") == 0)   { vb->fie2_dec = clampf(f, 0.002f, 2.0f); return 1; }

    if (strcmp(key, "cv_lfo_w") == 0)      { vb->lfo_w = clampi(n, 0, 5); return 1; }
    if (strcmp(key, "cv_lfo_w2") == 0)     { vb->lfo_w2 = clampi(n, 0, 5); return 1; }
    if (strcmp(key, "cv_lfo_morph") == 0)  { vb->lfo_morph = clampf(f, 0.0f, 1.0f); return 1; }
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

/* Trigger handlers */
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
/* Random pitch using a tonic + scale (simple Major Pentatonic for v0.1) */
static void do_rnd_pitch(forge_instance_t *inst) {
    static const int SCALE[] = { 0, 2, 4, 7, 9 };  /* major pentatonic */
    int tonic = 36 + (int)(randf(&inst->rng) * 24.0f);  /* C2..C4 */
    kit_slot_t *k = &inst->kits[inst->current_kit];
    for (int v = 0; v < NUM_VOICES; v++) {
        int degree = (int)(randf(&inst->rng) * 5.0f);
        int octave = (int)(randf(&inst->rng) * 3.0f);
        int note = tonic + SCALE[degree] + 12 * octave;
        if (note > 96) note = 96;
        k->kit_a[v].midi_note = note;
    }
}

/* ────────────────────────────────────────────────────────────────────────────
 * set_param
 * ──────────────────────────────────────────────────────────────────────────── */

static void set_param(void *instance, const char *key, const char *val) {
    forge_instance_t *inst = (forge_instance_t *)instance;
    if (!inst || !key || !val) return;

    if (key[0] == 'c' && key[1] == 'v' && key[2] == '_') {
        if (handle_cv_set(inst, key, val)) return;
    }

    if (strcmp(key, "_level") == 0 || strcmp(key, "current_level") == 0) {
        if      (strcmp(val, "Patch") == 0)   inst->current_page = PAGE_PATCH;
        else if (strcmp(val, "Voice") == 0)   inst->current_page = PAGE_VOICE;
        else if (strcmp(val, "Osc") == 0)     inst->current_page = PAGE_OSC;
        else if (strcmp(val, "Filter") == 0)  inst->current_page = PAGE_FILTER;
        else if (strcmp(val, "Env") == 0)     inst->current_page = PAGE_ENV;
        else if (strcmp(val, "Mod") == 0)     inst->current_page = PAGE_MOD;
        else if (strcmp(val, "Setup") == 0)   inst->current_page = PAGE_SETUP;
        else if (strcmp(val, "Mix") == 0)     inst->current_page = PAGE_MIX;
        else if (strcmp(val, "FX") == 0)      inst->current_page = PAGE_FX;
        else if (strcmp(val, "Perf") == 0)    inst->current_page = PAGE_PERF;
        else if (strcmp(val, "General") == 0) inst->current_page = PAGE_GENERAL;
        else if (strcmp(val, "root") == 0 || strcmp(val, "Forge") == 0)
            inst->current_page = PAGE_PATCH;
        return;
    }

    float f = (float)atof(val);
    int   n = atoi(val);

    if (strcmp(key, "kit") == 0)       { inst->current_kit = clampi(n, 0, NUM_KITS - 1); load_kit_fx(inst, inst->current_kit); return; }
    if (strcmp(key, "morph") == 0)     { inst->morph = clampf(f, 0.0f, 1.0f); return; }
    if (strcmp(key, "all_decay") == 0) { inst->all_decay_mult = clampf(f, 1.0f, 4.0f); return; }
    if (strcmp(key, "all_punch") == 0)  { inst->all_punch = clampf(f, 0, 1); return; }
    if (strcmp(key, "all_bright") == 0) { inst->all_bright = clampf(f, 0, 1); return; }
    if (strcmp(key, "all_drive") == 0)  { inst->all_drive = clampf(f, 0, 1); return; }
    if (strcmp(key, "all_snap") == 0)   { inst->all_snap = clampf(f, 0, 1); return; }
    if (strcmp(key, "all_bend") == 0)   { inst->all_bend = clampf(f, 0, 1); return; }
    if (strcmp(key, "all_tune") == 0)   { inst->all_tune = clampf(f, 0, 1); return; }
    if (strcmp(key, "all_fx") == 0)     { inst->all_fx = clampf(f, 0, 1); return; }
    if (strcmp(key, "morph_src") == 0)  { inst->morph_src = clampi(n, 0, 3); return; }
    if (strcmp(key, "morph_curve") == 0){ inst->morph_curve = clampi(n, 0, 3); return; }
    if (strcmp(key, "same_freq") == 0)  { inst->same_freq = clampi(n, 20, 20000); return; }

    if (strcmp(key, "save_kit") == 0) {
        if (strcmp(val, "Save") == 0) {
            forge_save_kits(inst);
            inst->save_kit_state = 0;
        }
        return;
    }

    if (strcmp(key, "rnd_kit") == 0) {
        if (atoi(val) == 1 || strcmp(val, "1") == 0) { do_rnd_kit(inst); }
        inst->tr_rnd_kit = 0; return;
    }
    if (strcmp(key, "rnd_voice") == 0) {
        if (atoi(val) == 1 || strcmp(val, "1") == 0) { do_rnd_voice(inst, inst->current_voice); }
        inst->tr_rnd_voice = 0; return;
    }
    if (strcmp(key, "rnd_pitch") == 0) {
        if (atoi(val) == 1 || strcmp(val, "1") == 0) { do_rnd_pitch(inst); }
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
        if (atoi(val) == 1 || strcmp(val, "1") == 0) {
            kit_slot_t *k = &inst->kits[inst->current_kit];
            for (int v = 0; v < NUM_VOICES; v++) k->kit_a[v].midi_note = FIRST_PAD_NOTE + v;
        }
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

    if (key[0] == 'v' && key[1] >= '1' && key[1] <= '8' && key[2] == '_') {
        int vi = key[1] - '1';
        if (strcmp(key + 2, "_lvl") == 0) { inst->v_lvl[vi] = clampf(f, 0.0f, 1.0f); return; }
        if (strcmp(key + 2, "_pan") == 0) { inst->kits[inst->current_kit].kit_a[vi].pan = clampf(f, -1.0f, 1.0f); return; }
        if (strcmp(key + 2, "_fx1") == 0) { inst->kits[inst->current_kit].kit_a[vi].fx1_send = clampf(f, 0.0f, 1.0f); return; }
        if (strcmp(key + 2, "_fx2") == 0) { inst->kits[inst->current_kit].kit_a[vi].fx2_send = clampf(f, 0.0f, 1.0f); return; }
    }

    if (strcmp(key, "rev_mix") == 0)       { inst->rev_mix = clampf(f, 0, 1); return; }
    if (strcmp(key, "rev_decay") == 0)     { inst->rev_decay = clampf(f, 0, 1); return; }
    if (strcmp(key, "rev_size") == 0)      { inst->rev_size = clampf(f, 0, 1); return; }
    if (strcmp(key, "rev_gate") == 0)      { inst->rev_gate = clampf(f, 0, 1); return; }
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
 * get_param — module.json caches loaded lazily for chain_params/ui_hierarchy
 * ──────────────────────────────────────────────────────────────────────────── */

static char *g_chain_params_cache = NULL;
static char *g_ui_hierarchy_cache = NULL;
static char  g_module_dir[512] = {0};

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

    /* Dynamic knob popup labels (Weird Dreams pattern).
     * On voice-related pages, prefix with "V<N> " so the popup makes clear
     * which voice the knob edits. Schwung calls this on every knob change. */
    if (strncmp(key, "knob_", 5) == 0 && strstr(key, "_name")) {
        int knob = atoi(key + 5) - 1;
        if (knob < 0 || knob > 7) return -1;
        int page = inst->current_page;
        int v    = clampi(inst->current_voice, 0, NUM_VOICES - 1);
        int algo = clampi(inst->live[v].algo, 0, NUM_ALGOS - 1);
        switch (page) {
            case PAGE_PATCH:   return snprintf(buf, buf_len, "%s", PATCH_KNOB_NAMES[knob]);
            case PAGE_MIX:     return snprintf(buf, buf_len, "%s", MIX_KNOB_NAMES[knob]);
            case PAGE_FX:      return snprintf(buf, buf_len, "%s", FX_KNOB_NAMES[knob]);
            case PAGE_PERF:    return snprintf(buf, buf_len, "%s", PERF_KNOB_NAMES[knob]);
            case PAGE_GENERAL: return snprintf(buf, buf_len, "%s", GEN_KNOB_NAMES[knob]);
            case PAGE_VOICE:   return snprintf(buf, buf_len, "V%d %s", v + 1, VOICE_MACRO_NAMES[algo][knob]);
            case PAGE_OSC:     return snprintf(buf, buf_len, "V%d %s", v + 1, OSC_KNOB_NAMES   [knob]);
            case PAGE_FILTER:  return snprintf(buf, buf_len, "V%d %s", v + 1, FILTER_KNOB_NAMES[knob]);
            case PAGE_ENV:     return snprintf(buf, buf_len, "V%d %s", v + 1, ENV_KNOB_NAMES   [knob]);
            case PAGE_MOD:     return snprintf(buf, buf_len, "V%d %s", v + 1, MOD_KNOB_NAMES   [knob]);
            case PAGE_SETUP:   return snprintf(buf, buf_len, "V%d %s", v + 1, SETUP_KNOB_NAMES [knob]);
            default:           return snprintf(buf, buf_len, "Knob %d", knob + 1);
        }
    }

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
    if (strcmp(key, "all_punch") == 0)  return snprintf(buf, buf_len, "%.4f", inst->all_punch);
    if (strcmp(key, "all_bright") == 0) return snprintf(buf, buf_len, "%.4f", inst->all_bright);
    if (strcmp(key, "all_drive") == 0)  return snprintf(buf, buf_len, "%.4f", inst->all_drive);
    if (strcmp(key, "all_snap") == 0)   return snprintf(buf, buf_len, "%.4f", inst->all_snap);
    if (strcmp(key, "all_bend") == 0)   return snprintf(buf, buf_len, "%.4f", inst->all_bend);
    if (strcmp(key, "all_tune") == 0)   return snprintf(buf, buf_len, "%.4f", inst->all_tune);
    if (strcmp(key, "all_fx") == 0)     return snprintf(buf, buf_len, "%.4f", inst->all_fx);
    if (strcmp(key, "morph_src") == 0) {
        static const char *N[] = {"Knob","LFO","Macro","Wheel"};
        return snprintf(buf, buf_len, "%s", N[clampi(inst->morph_src, 0, 3)]);
    }
    if (strcmp(key, "morph_curve") == 0) {
        static const char *N[] = {"Linear","Exp","Log","S-Curve"};
        return snprintf(buf, buf_len, "%s", N[clampi(inst->morph_curve, 0, 3)]);
    }
    if (strcmp(key, "same_freq") == 0) return snprintf(buf, buf_len, "%d", inst->same_freq);

    if (key[0] == 'v' && key[1] >= '1' && key[1] <= '8' && key[2] == '_') {
        int vi = key[1] - '1';
        if (strcmp(key + 2, "_lvl") == 0) return snprintf(buf, buf_len, "%.4f", inst->v_lvl[vi]);
        voice_bank_t *vb = &inst->kits[inst->current_kit].kit_a[vi];
        if (strcmp(key + 2, "_pan") == 0) return snprintf(buf, buf_len, "%.4f", vb->pan);
        if (strcmp(key + 2, "_fx1") == 0) return snprintf(buf, buf_len, "%.4f", vb->fx1_send);
        if (strcmp(key + 2, "_fx2") == 0) return snprintf(buf, buf_len, "%.4f", vb->fx2_send);
    }

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
            static const char *N[] = {"LP","HP","BP","BPu","Notch","Peak","Comb+","Comb-","LP2"};
            return snprintf(buf, buf_len, "%s", N[clampi(vb->f1_type, 0, 8)]);
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
            static const char *N[] = {"LP","HP","BP","BPu","Notch","Peak","Comb+","Comb-","LP2"};
            return snprintf(buf, buf_len, "%s", N[clampi(vb->f2_type, 0, 8)]);
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
        if (strcmp(key, "cv_fie1_amt") == 0)   return snprintf(buf, buf_len, "%.4f", vb->fie1_amt);
        if (strcmp(key, "cv_fie1_dec") == 0)   return snprintf(buf, buf_len, "%.4f", vb->fie1_dec);
        if (strcmp(key, "cv_fie2_amt") == 0)   return snprintf(buf, buf_len, "%.4f", vb->fie2_amt);
        if (strcmp(key, "cv_fie2_dec") == 0)   return snprintf(buf, buf_len, "%.4f", vb->fie2_dec);
        if (strcmp(key, "cv_lfo_w2") == 0) {
            static const char *N[] = {"Sine","Tri","Saw","Square","S&H","Random"};
            return snprintf(buf, buf_len, "%s", N[clampi(vb->lfo_w2, 0, 5)]);
        }
        if (strcmp(key, "cv_lfo_morph") == 0)  return snprintf(buf, buf_len, "%.4f", vb->lfo_morph);

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

    if (strcmp(key, "rev_mix") == 0)       return snprintf(buf, buf_len, "%.4f", inst->rev_mix);
    if (strcmp(key, "rev_decay") == 0)     return snprintf(buf, buf_len, "%.4f", inst->rev_decay);
    if (strcmp(key, "rev_size") == 0)      return snprintf(buf, buf_len, "%.4f", inst->rev_size);
    if (strcmp(key, "rev_gate") == 0)      return snprintf(buf, buf_len, "%.4f", inst->rev_gate);
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
 * Per-voice render helpers
 *
 * Each algorithm renderer returns one mono sample. The shared apply_filter_chain
 * + apply_post_chain + LFO/envelope advance run after each sample.
 * ──────────────────────────────────────────────────────────────────────────── */

static inline void recompute_voice_coefs(voice_state_t *vs, voice_bank_t *vb,
                                         float bright) {
    /* Per-block coefficient cache — call once per block per voice.
     * `bright` is the Ctrl-All cutoff multiplier (1.0 = neutral, ±1 octave). */
    float f1 = clampf((float)vb->f1_cut * bright, 20.0f, 18000.0f);
    float f2 = clampf((float)vb->f2_cut * bright, 20.0f, 18000.0f);
    /* Chamberlin SVF coef = 2*sin(π*f/Fs). The svf_process function runs
     * TWO passes per sample (sharper rolloff, better stability at high Q
     * for drum-percussive transients). The two-pass cascade halves the
     * effective stable region — clamping coef at 0.95 ensures both passes
     * stay within the stable boundary. (v0.1.1 attempt to relax to 1.35
     * caused integrator runaway → voice silence after ~1 s; reverted.)
     * Cymbal/hat brightness above ~7 kHz is left to v0.2's filter rework. */
    vs->f1_coef = clampf(2.0f * lookup_sine(f1 * SR_INV * 0.5f), 0.0f, 0.95f);
    vs->f2_coef = clampf(2.0f * lookup_sine(f2 * SR_INV * 0.5f), 0.0f, 0.95f);
    vs->f1_q = 1.0f / clampf(vb->f1_res, 0.5f, 20.0f);
    vs->f2_q = 1.0f / clampf(vb->f2_res, 0.5f, 20.0f);
    /* Comb lengths (when the filter mode is Comb+/-): use cutoff to set period */
    int  c1 = (int)(SAMPLE_RATE / clampf(f1, 30.0f, 18000.0f));
    int  c2 = (int)(SAMPLE_RATE / clampf(f2, 30.0f, 18000.0f));
    vs->f1_comb_len = clampi(c1, 2, COMB_MAX_SAMPS - 1);
    vs->f2_comb_len = clampi(c2, 2, COMB_MAX_SAMPS - 1);
    /* Resonator length: track cutoff of f1 (cheapest tracking choice) */
    vs->reso_len = vs->f1_comb_len;
    /* Base-Width pre-filter coefs */
    float bw = clampf((float)vb->bw_cut, 20.0f, 18000.0f);
    float bw_w = clampf(vb->bw_w, 0.0f, 1.0f);
    /* Width controls bandwidth: LP higher, HP lower as width grows */
    float w_octaves = bw_w * 4.0f;             /* 0..4 octaves */
    float lp_freq = bw * powf(2.0f, +0.5f * w_octaves);
    float hp_freq = bw * powf(2.0f, -0.5f * w_octaves);
    vs->bw_lp_coef = onepole_coef(lp_freq);
    vs->bw_hp_coef = onepole_coef(hp_freq);
}

static inline float apply_one_filter(
    float x, int type, float coef, float q, float drive,
    float *lp_st, float *bp_st,
    int comb_len, float *comb_buf, int *comb_idx)
{
    if (type == FILT_COMBP) {
        return comb_process(x, +1, comb_len, 0.7f, comb_buf, comb_idx);
    } else if (type == FILT_COMBN) {
        return comb_process(x, -1, comb_len, 0.7f, comb_buf, comb_idx);
    }
    float y = svf_process(x, type, coef, q, lp_st, bp_st);
    if (drive > 0.001f) {
        y = drive_sample(DRIVE_TUBE, y, drive);
    }
    return y;
}

/* Apply filter chain (Single / Per-Osc / Serial / Parallel) +
 * Base-Width pre-stage. Returns the final filtered sample. */
static inline float apply_filter_chain(
    voice_state_t *vs, voice_bank_t *vb, float x)
{
    /* Base-Width pre: serial LP + HP */
    if (vb->bw_on) {
        x = onepole_lp(x, vs->bw_lp_coef, &vs->bw_lp_st);
        x = onepole_hp(x, vs->bw_hp_coef, &vs->bw_hp_st);
    }
    int routing = vb->routing;
    if (routing == ROUTING_SINGLE) {
        return apply_one_filter(x, vb->f1_type, vs->f1_coef, vs->f1_q, vb->f1_drv,
                                &vs->svf1_lp, &vs->svf1_bp,
                                vs->f1_comb_len, vs->comb1_buf, &vs->comb1_idx);
    }
    if (routing == ROUTING_SERIAL) {
        float y = apply_one_filter(x, vb->f1_type, vs->f1_coef, vs->f1_q, vb->f1_drv,
                                   &vs->svf1_lp, &vs->svf1_bp,
                                   vs->f1_comb_len, vs->comb1_buf, &vs->comb1_idx);
        return apply_one_filter(y, vb->f2_type, vs->f2_coef, vs->f2_q, vb->f2_drv,
                                &vs->svf2_lp, &vs->svf2_bp,
                                vs->f2_comb_len, vs->comb2_buf, &vs->comb2_idx);
    }
    /* PARALLEL or PER_OSC (single-osc fallback for v0.1: same as parallel) */
    float y1 = apply_one_filter(x, vb->f1_type, vs->f1_coef, vs->f1_q, vb->f1_drv,
                                &vs->svf1_lp, &vs->svf1_bp,
                                vs->f1_comb_len, vs->comb1_buf, &vs->comb1_idx);
    float y2 = apply_one_filter(x, vb->f2_type, vs->f2_coef, vs->f2_q, vb->f2_drv,
                                &vs->svf2_lp, &vs->svf2_bp,
                                vs->f2_comb_len, vs->comb2_buf, &vs->comb2_idx);
    return (y1 + y2) * 0.5f;
}

/* ────────────────────────────────────────────────────────────────────────────
 * Voice algorithm renderers
 * ──────────────────────────────────────────────────────────────────────────── */

/* Common: get the click contribution from sample bank or impulse */
static inline float voice_click_sample(
    voice_state_t *vs, voice_bank_t *vb, forge_instance_t *inst)
{
    if (!vs->click_active) return 0.0f;
    /* Ctrl-All "Snap" scales every voice's transient level (0.5 = neutral). */
    float snap = 0.5f + inst->all_snap;   /* 0.5 .. 1.5 */
    if (vb->click_type == CLICK_SAMPLE) {
        if (vs->click_idx >= CSMP_LEN) {
            vs->click_active = 0;
            return 0.0f;
        }
        float s = inst->click_bank[clampi(vb->click_smp, 0, NUM_CSMP - 1)][vs->click_idx];
        vs->click_idx++;
        return s * vb->click_lvl * snap;
    } else if (vb->click_type == CLICK_IMPULSE) {
        /* Synthesised impulse: short noise burst with exp envelope */
        float t = (float)vs->click_idx / (vb->click_dec * SAMPLE_RATE);
        if (t > 1.0f) {
            vs->click_active = 0;
            return 0.0f;
        }
        float env = expf(-t * 30.0f);
        float n = wnoise(&vs->rng);
        vs->click_idx++;
        return n * env * vb->click_lvl * snap;
    }
    /* CLICK_PHASE: handled by phase init at trigger; no audio here */
    vs->click_active = 0;
    return 0.0f;
}

/* Drum renderer. Returns the FM "colour" + click (which goes through the
 * resonator + filter), and writes the clean parallel BODY sine to *body_out.
 * The body is added back POST-filter in the main loop, so the kick's low
 * fundamental always survives whatever filter the kit uses (a BP/HP that
 * would otherwise annihilate a 65 Hz kick no longer silences it). */
/* fie1_add / fie2_add carry the FM-index envelope contributions (plus any
 * mod-slot FM modulation) in index units, blooming the FM amount at attack. */
static inline float render_drum_voice(
    voice_state_t *vs, voice_bank_t *vb, forge_instance_t *inst,
    float pitch_mod_semitones, float fie1_add, float fie2_add, float cut_mod_l,
    float *body_out)
{
    (void)cut_mod_l; (void)fie2_add;
    float base = note_to_freq(vb->midi_note + (int)vb->voice_tune)
                 * powf(2.0f, pitch_mod_semitones / 12.0f);

    /* FM "colour" layer (index blooms via fie1_add). */
    float fm_idx = vb->m[3] * 8.0f + fie1_add;
    if (fm_idx < 0.0f) fm_idx = 0.0f;
    float ratio = vb->ratio_c + vb->ratio_f * 0.05f;
    float fm = fm_op_2op(base, ratio, fm_idx, vb->fbk, vb->wave, vb->pwm,
                         &vs->ph_a, &vs->ph_mod, vs->fb_state, &vs->rng);

    /* Parallel BODY sine at the fundamental (weight/punch), soft-saturated. */
    vs->ph_body += base * SR_INV;
    if (vs->ph_body >= 1.0f) vs->ph_body -= floorf(vs->ph_body);
    float body = fast_tanh(lookup_sine(vs->ph_body) * 1.4f);

    /* Body macro + Ctrl-All "Punch" set the body weight. */
    float bm = clampf(vb->m[4] + (inst->all_punch - 0.5f) * 0.7f, 0.0f, 1.0f);
    *body_out = body * bm * vb->level;

    float colour = fm * (1.0f - 0.45f * bm) * vb->level;
    colour += voice_click_sample(vs, vb, inst);
    return colour;
}

static inline float render_snare_voice(
    voice_state_t *vs, voice_bank_t *vb, forge_instance_t *inst,
    float pitch_mod_semitones, float fie1_add, float fie2_add, float cut_mod_l)
{
    (void)cut_mod_l; (void)fie2_add;
    float base = note_to_freq(vb->midi_note + (int)vb->voice_tune)
                 * powf(2.0f, pitch_mod_semitones / 12.0f);
    float fm_idx = vb->m[3] * 6.0f + fie1_add;
    if (fm_idx < 0.0f) fm_idx = 0.0f;
    float body = fm_op_2op(base, vb->ratio_c + vb->ratio_f * 0.05f,
                           fm_idx, vb->fbk, OSC_SINE, 0.5f,
                           &vs->ph_a, &vs->ph_mod, vs->fb_state, &vs->rng);
    float noise = wnoise(&vs->rng);
    float lp_coef = onepole_coef(2000.0f + vb->m[2] * 6000.0f);
    vs->noise_lp += lp_coef * (noise - vs->noise_lp) + DENORM_EPS;
    float n = vs->noise_lp;
    float mix = vb->m[3];
    float out = body * (1.0f - mix) + n * mix;
    out *= vb->level;
    out += voice_click_sample(vs, vb, inst);
    return out;
}

static inline float render_cymbal_voice(
    voice_state_t *vs, voice_bank_t *vb, forge_instance_t *inst,
    float pitch_mod_semitones, float fie1_add, float fie2_add, float cut_mod_l)
{
    (void)cut_mod_l;
    float base = note_to_freq(vb->midi_note + (int)vb->voice_tune)
                 * powf(2.0f, pitch_mod_semitones / 12.0f);
    /* 3-op cascade A→B→C, FB at A. Each index blooms with its own FM env:
     * fie1 → A→B index, fie2 → B→C index (the Digitone dual-index bloom). */
    float idx_ab = vb->m[2] * 8.0f + fie1_add;
    float idx_bc = vb->m[4] * 6.0f + fie2_add;
    if (idx_ab < 0.0f) idx_ab = 0.0f;
    if (idx_bc < 0.0f) idx_bc = 0.0f;
    float ratio_b = 1.0f + vb->m[3] * 6.0f;
    float ratio_c = 1.0f + vb->m[4] * 4.0f;
    float osc = fm_op_3op_serial(base,
                                 vb->ratio_c, ratio_b, ratio_c,
                                 idx_ab, idx_bc,
                                 vb->fbk,
                                 &vs->ph_a, &vs->ph_b, &vs->ph_c,
                                 vs->fb_state);
    osc *= vb->level;
    osc += voice_click_sample(vs, vb, inst);
    return osc;
}

static inline float render_hat_voice(
    voice_state_t *vs, voice_bank_t *vb, forge_instance_t *inst,
    float pitch_mod_semitones, float fie1_add, float fie2_add, float cut_mod_l)
{
    return render_cymbal_voice(vs, vb, inst, pitch_mod_semitones, fie1_add, fie2_add, cut_mod_l);
}

static inline float render_wild_voice(
    voice_state_t *vs, voice_bank_t *vb, forge_instance_t *inst,
    float pitch_mod_semitones, float fie1_add, float fie2_add, float cut_mod_l)
{
    (void)cut_mod_l;
    float base = note_to_freq(vb->midi_note + (int)vb->voice_tune)
                 * powf(2.0f, pitch_mod_semitones / 12.0f);
    float fm_idx = vb->m[3] * 12.0f + fie1_add;
    if (fm_idx < 0.0f) fm_idx = 0.0f;
    float osc = fm_op_2op(base, vb->ratio_c + vb->ratio_f * 0.1f,
                          fm_idx, vb->fbk, vb->wave, vb->pwm,
                          &vs->ph_a, &vs->ph_mod, vs->fb_state, &vs->rng);
    if (vb->xfm) {
        /* Cross-FM: second op's index blooms with fie2. */
        osc = fm_op_2op(base * 1.5f, 1.0f, vb->m[2] * 4.0f + fie2_add, 0.0f, OSC_SINE, 0.5f,
                        &vs->ph_b, &vs->ph_c, vs->fb_state, &vs->rng) + osc;
        osc *= 0.5f;
    }
    osc *= vb->level;
    osc = wavefold(osc, vb->m[4] * 4.0f);
    osc += voice_click_sample(vs, vb, inst);
    return osc;
}

/* ────────────────────────────────────────────────────────────────────────────
 * Render block
 * ──────────────────────────────────────────────────────────────────────────── */

static void render_block(void *instance, int16_t *out_lr, int frames) {
    forge_instance_t *inst = (forge_instance_t *)instance;
    if (!inst) {
        memset(out_lr, 0, frames * 2 * sizeof(int16_t));
        return;
    }

    /* Per-block: morph kits A→B into live[], recompute filter coefs.
     * Ctrl-All "Bright" is a ±1-octave cutoff multiplier (0.5 = neutral). */
    morph_voices(inst);
    float all_bright_mul = powf(2.0f, (inst->all_bright - 0.5f) * 2.0f);
    for (int v = 0; v < NUM_VOICES; v++) {
        recompute_voice_coefs(&inst->voice[v], &inst->live[v], all_bright_mul);
    }

    /* Compute global FX coefs once per block */
    int   delay_samps = (int)(0.01f * SAMPLE_RATE
                              + powf(100.0f, inst->dly_rate) * 0.01f * SAMPLE_RATE);
    if (delay_samps < 16) delay_samps = 16;
    if (delay_samps >= DELAY_BUF_SAMPS) delay_samps = DELAY_BUF_SAMPS - 1;
    float dly_lp_coef = onepole_coef((float)inst->dly_bpf_cut * (1.0f + inst->dly_bpf_w * 2.0f));
    float dly_hp_coef = onepole_coef((float)inst->dly_bpf_cut / (1.0f + inst->dly_bpf_w * 2.0f));

    float rev_decay = clampf(inst->rev_decay * 0.7f, 0.0f, 0.95f);
    float rev_damp  = onepole_coef(2000.0f + (1.0f - inst->rev_damping) * 6000.0f);

    float cho_rate_hz = 0.05f + inst->cho_rate * 4.95f;

    /* EQ coefficients */
    float lo_coef = onepole_coef(inst->lo_freq);
    float hi_coef = onepole_coef(inst->hi_freq);
    float mid_lp_coef = onepole_coef(inst->mid_freq * 2.0f);
    float mid_hp_coef = onepole_coef(inst->mid_freq * 0.5f);

    for (int i = 0; i < frames; i++) {
        /* Per-sample voice rendering */
        float main_l = 0.0f, main_r = 0.0f;
        float fx1_in = 0.0f, fx2_in = 0.0f;

        for (int v = 0; v < NUM_VOICES; v++) {
            voice_state_t *vs = &inst->voice[v];
            voice_bank_t  *vb = &inst->live[v];
            if (!vs->active || vb->mute) continue;

            /* Advance envelopes */
            float effective_e1_dec = vb->e1_dec * inst->all_decay_mult;
            float e1 = ad_env_advance(&vs->e1_state, &vs->e1_v, &vs->e1_t,
                                      vb->e1_atk, effective_e1_dec,
                                      vb->e1_crv, vb->e1_rep, vb->e1_rep_rate,
                                      &vs->e1_rep_cnt);
            int e2_dummy_cnt = 0;
            float e2 = ad_env_advance(&vs->e2_state, &vs->e2_v, &vs->e2_t,
                                      vb->e2_atk, vb->e2_dec,
                                      vb->e2_crv, 0.0f, 0.0f,
                                      &e2_dummy_cnt);
            float pe = pitch_env_advance(&vs->pe_state, &vs->pe_v, &vs->pe_t,
                                         vb->pe_dec, vb->pe_crv);

            /* Voice end check */
            if (e1 <= 0.00005f && vs->e1_state == 0) {
                vs->active = 0;
                continue;
            }

            /* FM-index envelopes (Digitone-style enveloped FM). Bloom the FM
             * index at attack then collapse. fie1 → primary index, fie2 →
             * secondary (B→C / cross-FM). */
            float fie1 = pitch_env_advance(&vs->fie1_state, &vs->fie1_v, &vs->fie1_t,
                                           vb->fie1_dec, vb->fie1_crv);
            float fie2 = pitch_env_advance(&vs->fie2_state, &vs->fie2_v, &vs->fie2_t,
                                           vb->fie2_dec, vb->fie2_crv);

            /* LFO advance with dual-shape morph (Machinedrum SHMIX). The S&H
             * value is sampled once per cycle wrap so morphing doesn't
             * double-advance it. */
            float lfo_inc = vb->lfo_r * SR_INV;
            float lfo_prev = vs->lfo_phase;
            vs->lfo_phase += lfo_inc;
            if (vs->lfo_phase >= 1.0f) vs->lfo_phase -= floorf(vs->lfo_phase);
            if (vs->lfo_phase < lfo_prev) vs->lfo_sah_v = wnoise(&vs->rng);
            float self_lfo = lfo_morph_sample(vb->lfo_w, vb->lfo_w2, vb->lfo_morph,
                                              vs->lfo_phase, vs->lfo_sah_v);
            if (vb->lfo_pol) self_lfo = (self_lfo + 1.0f) * 0.5f;
            float lfo_v = self_lfo * vb->lfo_d;
            vs->lfo_v = lfo_v;
            /* Cross-LFO source override */
            float lfo_active = lfo_v;
            if (vb->xlfo_src > 0 && vb->xlfo_src - 1 != v) {
                lfo_active = inst->voice[vb->xlfo_src - 1].lfo_v;
            }

            /* Modulation accumulators (1 mod slot) */
            float mod_pitch = 0.0f, mod_fm_idx = 0.0f, mod_cut = 0.0f;
            float mod_pan = 0.0f, mod_body = 0.0f, mod_lvl = 1.0f;

            /* Mod slot */
            if (vb->mod_dest > 0 && fabsf(vb->mod_dpth) > 0.001f) {
                float src = 0.0f;
                switch (vb->mod_src) {
                    case 0: src = lfo_active; break;
                    case 1: src = (vb->xlfo_src > 0) ? lfo_active : self_lfo * vb->lfo_d; break;
                    case 2: src = e1; break;
                    case 3: src = e2; break;
                    case 4: src = pe; break;
                    case 5: src = vs->velocity; break;
                    case 6: src = 0.0f; break;  /* AT not implemented in v0.1 */
                    case 7: src = 0.0f; break;  /* MW not implemented in v0.1 */
                }
                float depth = src * vb->mod_dpth;
                switch (vb->mod_dest) {
                    case 1: mod_pitch  += depth * 12.0f; break;   /* ±12 semitones */
                    case 2: mod_fm_idx += depth * 8.0f; break;
                    case 3: mod_cut    += depth; break;
                    case 4: /* Reso FB — not wired into resonator block here */ break;
                    case 5: mod_body   += depth; break;
                    case 6: mod_pan    += depth; break;
                    case 7: mod_lvl    += depth; break;
                }
            }

            /* Pitch envelope = the "bend". Widened to ±24 semitones (2 oct)
             * for a deep click→boom. For Drum voices the Bend macro (m[2])
             * adds on top of the kit's authored pe_amt, so the Voice-page
             * "Bend" knob is a live, bouncy control. Non-drum algos use small
             * pe_amt so the wide range stays inaudible there. */
            float bend_amt = vb->pe_amt;
            if (vb->algo == ALGO_DRUM) bend_amt += vb->m[2] * 0.5f;
            /* Ctrl-All "Bend" adds pitch-sweep across every voice; "Tune"
             * offsets global pitch (both 0.5 = neutral). */
            bend_amt += (inst->all_bend - 0.5f) * 0.8f;
            mod_pitch += pe * bend_amt * 24.0f;
            mod_pitch += (inst->all_tune - 0.5f) * 24.0f;
            /* E2 destination */
            switch (vb->e2_dest) {
                case 0: mod_fm_idx += e2 * vb->v_e2_amt * 4.0f; break;     /* FM */
                case 1: mod_cut    += e2 * vb->v_e2_amt; break;             /* Filter */
                case 3: mod_pan    += e2 * vb->v_e2_amt - 0.5f; break;      /* Pan */
                default: break;
            }

            /* FM-index envelope contributions in index units (up to ~8 each). */
            float fie1_add = mod_fm_idx + fie1 * vb->fie1_amt * 8.0f;
            float fie2_add = fie2 * vb->fie2_amt * 8.0f;

            /* Render algorithm. Drum returns the filterable colour and writes
             * its clean body sine to drum_body (added back post-filter). */
            float osc = 0.0f;
            float drum_body = 0.0f;
            switch (vb->algo) {
                case ALGO_DRUM:   osc = render_drum_voice  (vs, vb, inst, mod_pitch, fie1_add, fie2_add, mod_cut, &drum_body); break;
                case ALGO_SNARE:  osc = render_snare_voice (vs, vb, inst, mod_pitch, fie1_add, fie2_add, mod_cut); break;
                case ALGO_CYMBAL: osc = render_cymbal_voice(vs, vb, inst, mod_pitch, fie1_add, fie2_add, mod_cut); break;
                case ALGO_HAT:    osc = render_hat_voice   (vs, vb, inst, mod_pitch, fie1_add, fie2_add, mod_cut); break;
                case ALGO_WILD:   osc = render_wild_voice  (vs, vb, inst, mod_pitch, fie1_add, fie2_add, mod_cut); break;
                default: break;
            }

            /* Resonator (driven by macro M6 amount on Drum/Cymbal/Hat) */
            float reso_amount = 0.0f;
            if (vb->algo == ALGO_DRUM)        reso_amount = vb->m[5] * 1.6f;
            else if (vb->algo == ALGO_CYMBAL) reso_amount = vb->m[5] * 1.6f;
            else if (vb->algo == ALGO_WILD)   reso_amount = vb->m[5] * 1.8f;
            osc = resonator_process(osc, reso_amount, vs->reso_len, vs->reso_buf, &vs->reso_idx);

            /* Filter chain (shapes the colour/noise, not the body) */
            osc = apply_filter_chain(vs, vb, osc);

            /* Clean Drum body sine, added POST-filter so the kick's low
             * fundamental always survives (0 for non-Drum algos). */
            osc += drum_body;

            /* Per-voice drive (M8 "Drive") + Ctrl-All "Drive" (0.5 neutral) —
             * a fattening saturation applied to body+colour together. */
            float drv_amt = clampf(vb->m[7] + (inst->all_drive - 0.5f) * 0.8f, 0.0f, 1.0f);
            if (drv_amt > 0.001f) {
                osc = drive_sample(DRIVE_TUBE, osc, drv_amt);
            }

            /* Per-voice bit/rate crush */
            if (vb->bit > 0.001f) osc = bit_crush(osc, vb->bit);
            if (vb->rate > 0.001f) osc = rate_crush_l(osc, vb->rate, &vs->crush_held_l, &vs->crush_accum);

            /* Apply Env1 (amp), velocity, voice-level, mod-level */
            float vel_lvl = vs->velocity * (1.0f - vb->v_e1_lvl) + vb->v_e1_lvl;
            float amp = e1 * vel_lvl * vb->voice_lvl * inst->v_lvl[v] * mod_lvl;
            osc *= amp;

            /* Pan */
            float pan = clampf(vb->pan + mod_pan, -1.0f, 1.0f);
            float pl = 0.5f - 0.5f * pan;
            float pr = 0.5f + 0.5f * pan;
            float vl = osc * pl;
            float vr = osc * pr;

            /* Bus routing (Main / Aux1 / Aux2 / FX-Only) */
            if (vb->bus != 3) {
                main_l += vl;
                main_r += vr;
            }
            /* FX sends */
            fx1_in += osc * vb->fx1_send;
            fx2_in += osc * vb->fx2_send;
        }

        /* Ctrl-All "FX" scales every voice's send into the buses (0.5 = 1×). */
        float all_fx_scale = 0.5f + inst->all_fx;   /* 0.5 .. 1.5 */
        fx1_in *= all_fx_scale;
        fx2_in *= all_fx_scale;

        /* === FX1: Delay === */
        float dly_l = 0.0f, dly_r = 0.0f;
        if (inst->dly_mix > 0.001f) {
            delay_process(&inst->delay_st, fx1_in * 0.7f, fx1_in * 0.7f,
                          &dly_l, &dly_r,
                          delay_samps, inst->dly_fdbk,
                          inst->dly_pp, dly_lp_coef, dly_hp_coef,
                          inst->dly_tone);
        }

        /* === FX2: Reverb (with optional Gate Box) === */
        float rev_l = 0.0f, rev_r = 0.0f;
        if (inst->rev_mix > 0.001f) {
            reverb_process(&inst->reverb_st, fx2_in, fx2_in, &rev_l, &rev_r,
                           rev_decay, rev_damp, inst->rev_size, inst->rev_gate);
        }

        /* === FX3: Chorus (master tap, processes the dry main mix) === */
        float cho_l = 0.0f, cho_r = 0.0f;
        if (inst->cho_mix > 0.001f) {
            chorus_process(&inst->chorus_st, main_l, main_r, &cho_l, &cho_r,
                           cho_rate_hz, inst->cho_depth, inst->cho_width,
                           inst->cho_voices, inst->cho_fb, inst->cho_tone);
        }

        /* Sum dry + FX wets */
        float l = main_l
                + dly_l * inst->dly_mix
                + rev_l * inst->rev_mix
                + cho_l * inst->cho_mix;
        float r = main_r
                + dly_r * inst->dly_mix
                + rev_r * inst->rev_mix
                + cho_r * inst->cho_mix;

        /* === Master section === */
        compressor_process(&inst->comp_st, &l, &r, inst->comp);
        eq_process(&l, &r, &inst->eq_st,
                   inst->eq_lo, inst->eq_mid, inst->eq_hi,
                   lo_coef, mid_lp_coef, mid_hp_coef, hi_coef);
        if (inst->drive > 0.001f) {
            l = drive_sample(inst->drive_type, l, inst->drive);
            r = drive_sample(inst->drive_type, r, inst->drive);
        }
        if (inst->bit > 0.001f) {
            l = bit_crush(l, inst->bit);
            r = bit_crush(r, inst->bit);
        }
        if (inst->rate > 0.001f) {
            l = rate_crush_l(l, inst->rate, &inst->master_crush_held_l, &inst->master_crush_accum);
            r = rate_crush_l(r, inst->rate, &inst->master_crush_held_r, &inst->master_crush_accum);
        }
        l *= inst->master;
        r *= inst->master;

        if (inst->limiter) {
            l = soft_clip(l);
            r = soft_clip(r);
        } else {
            l = clampf(l, -1.0f, 1.0f);
            r = clampf(r, -1.0f, 1.0f);
        }

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
