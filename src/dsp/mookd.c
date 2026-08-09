/*
 * Mook D — a Model D-style monophonic synth for Ableton Move (Schwung)
 *
 * API : plugin_api_v2_t (create_instance / render_block / on_midi / set_param ...)
 * Audio: 44100 Hz, 128 frames/block, stereo interleaved int16 output
 *
 * Architecture mirrors the Model D signal path:
 *   3 oscillators (range + waveform, osc2/3 tunable) -> mixer (+noise)
 *   -> 4-pole transistor ladder LPF (cutoff / emphasis / contour)
 *   -> VCA (loudness contour). Mono, last-note priority, glide.
 *
 * Original code written to the Schwung Plugin ABI. No vendor source reused.
 * GLIBC-safe: links only libm + libc (no shm_open, no C11 threads).
 */
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "host/plugin_api_v1.h"

#define SR        44100.0f
#define BLOCK     MOVE_FRAMES_PER_BLOCK   /* 128 */
#define TWO_PI    6.28318530717958648f
#define OSC_OVS   1                        /* PolyBLEP handles alias @1x */
#define FLT_OVS   2                        /* 2x oversample the ladder    */

/* Musical/safety limits for controls that can otherwise concentrate energy
 * near Nyquist. These are mirrored by the public ranges in module.json. */
#define MAX_CUTOFF_CTRL   0.85f
#define MAX_EMPHASIS      0.82f
#define MAX_CONTOUR       0.75f
#define MAX_FILTER_FM     0.40f
#define MAX_LFO_FILTER    0.60f

/* ------------------------------------------------------------------ utils */
static inline float clampf(float x, float lo, float hi){
    return x < lo ? lo : (x > hi ? hi : x);
}
static inline int finitef_strict(float x){
    /* Bitwise check remains effective under the production -ffast-math build,
     * where the compiler is otherwise allowed to assume NaN/Inf never occur. */
    volatile uint32_t bits = 0;
    memcpy((void *)&bits, &x, sizeof(bits));
    return (bits & 0x7f800000u) != 0x7f800000u;
}
static inline float lerpf(float a, float b, float t){ return a + (b - a) * t; }
static inline float smoothf(float current, float target, float coefficient){
    return current + (target - current) * coefficient;
}
static inline float safe_cutoff_norm(float value){
    if (!finitef_strict(value) || value <= 0.0f) return 0.0f;
    /* Preserve normal knob motion, then compress combinations of cutoff,
     * contour and modulation into a gentle 14 kHz asymptote. */
    if (value <= 0.72f) return value;
    return 0.72f + 0.23f * (1.0f - expf(-(value - 0.72f) / 0.23f));
}

/* PolyBLEP band-limiting residual for saw/pulse discontinuities. */
static inline float polyblep(float t, float dt){
    if (t < dt){ t /= dt; return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt){ t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0.0f;
}

/* 3-band-ish pink filter (Paul Kellet economy) applied to white noise. */
typedef struct { float b0,b1,b2; } pink_t;
static inline float pink_next(pink_t *p, float white){
    p->b0 = 0.99765f*p->b0 + white*0.0990460f;
    p->b1 = 0.96300f*p->b1 + white*0.2965164f;
    p->b2 = 0.57000f*p->b2 + white*1.0526913f;
    return (p->b0 + p->b1 + p->b2 + white*0.1848f) * 0.20f;
}

/* --------------------------------------------------------------- envelope */
typedef struct {
    int   stage;           /* 0 idle 1 atk 2 dec 3 sus 4 rel */
    float level, a, d, s, r;
} adsr_t;

static void adsr_set(adsr_t *e, float a, float d, float s, float r){
    e->a = a; e->d = d; e->s = s; e->r = r;
}
static inline void adsr_gate(adsr_t *e, int on){
    if (on){ e->stage = 1; }
    else if (e->stage != 0){ e->stage = 4; }
}
/* One-pole segment coefficient for a target reached in ~t seconds. */
static inline float seg_coef(float t){
    if (t <= 0.0001f) return 1.0f;
    return 1.0f - expf(-1.0f / (t * SR));
}
static inline float adsr_tick(adsr_t *e){
    switch (e->stage){
        case 1: /* attack -> aim slightly past 1 so it actually arrives */
            e->level += (1.2f - e->level) * seg_coef(e->a);
            if (e->level >= 1.0f){ e->level = 1.0f; e->stage = 2; }
            break;
        case 2: /* decay to sustain */
            e->level += (e->s - e->level) * seg_coef(e->d);
            if (fabsf(e->level - e->s) < 0.001f){ e->level = e->s; e->stage = 3; }
            break;
        case 3: e->level = e->s; break;
        case 4: /* release */
            e->level += (0.0f - e->level) * seg_coef(e->r);
            if (e->level < 0.0002f){ e->level = 0.0f; e->stage = 0; }
            break;
        default: e->level = 0.0f;
    }
    return e->level;
}

/* ------------------------------------------------------------- oscillator */
typedef struct {
    float phase;           /* 0..1 */
    int   range;           /* 0=LO 1=32 2=16 3=8 4=4 5=2 */
    int   wave;            /* 0 tri 1 saw 2 square 3 wide-pulse 4 narrow-pulse */
} osc_t;

/* octave multiplier relative to 8' (=range index 3). LO = 5 octaves down. */
static float range_mult(int r){
    switch (r){
        case 0: return 1.0f/32.0f;  /* LO */
        case 1: return 0.25f;       /* 32' */
        case 2: return 0.5f;        /* 16' */
        case 3: return 1.0f;        /* 8'  */
        case 4: return 2.0f;        /* 4'  */
        case 5: return 4.0f;        /* 2'  */
    }
    return 1.0f;
}

static inline float osc_render(osc_t *o, float freq, int *wrapped){
    /* A clamped phase increment turns every out-of-band pitch into a loud
     * Nyquist square wave. Instead, fade oscillators above the highest useful
     * musical fundamental and silence them before Nyquist. */
    if (!finitef_strict(freq) || freq <= 0.0f){
        if (wrapped) *wrapped = 0;
        return 0.0f;
    }
    const float fade_start = SR * 0.18f;  /* 7.94 kHz */
    const float fade_end   = SR * 0.24f;  /* 10.58 kHz, below Nyquist */
    float gain = 1.0f;
    if (freq >= fade_end) gain = 0.0f;
    else if (freq > fade_start){
        float x = (freq - fade_start) / (fade_end - fade_start);
        gain = 1.0f - x*x*(3.0f - 2.0f*x); /* smoothstep fade */
    }
    float dt = clampf(freq, 0.0f, fade_end) / SR;
    o->phase += dt;
    int w = 0;
    if (o->phase >= 1.0f){ o->phase -= 1.0f; w = 1; }
    if (wrapped) *wrapped = w;
    float t = o->phase, v;
    switch (o->wave){
        case 0: { /* triangle */
            v = 4.0f * fabsf(t - 0.5f) - 1.0f;
        } break;
        case 1: { /* sawtooth (band-limited) */
            v = 2.0f * t - 1.0f;
            v -= polyblep(t, dt);
        } break;
        case 2: { /* square */
            v = t < 0.5f ? 1.0f : -1.0f;
            v += polyblep(t, dt);
            float t2 = t + 0.5f; if (t2 >= 1.0f) t2 -= 1.0f;
            v -= polyblep(t2, dt);
        } break;
        default: { /* pulse: wide(3)~0.25, narrow(4)~0.12 */
            float pw = (o->wave == 3) ? 0.25f : 0.12f;
            v = t < pw ? 1.0f : -1.0f;
            v += polyblep(t, dt);
            float t2 = t + (1.0f - pw); if (t2 >= 1.0f) t2 -= 1.0f;
            v -= polyblep(t2, dt);
        }
    }
    return v * gain;
}

/* ------------------------------------------------ transistor ladder (Model D-style) */
/* Huovilainen-style nonlinear 4-pole, 2x oversampled for stability. */
typedef struct { float s[4], z; float cut, res; } ladder_t;

static inline void ladder_set(ladder_t *L, float cut_hz, float res01){
    /* Keep the controllable range below the digital ladder's unstable,
     * whistle-like corner. High emphasis progressively lowers the ceiling,
     * analogous to the bandwidth loss of a driven analogue ladder. */
    float emphasis = clampf(res01, 0.0f, MAX_EMPHASIS);
    float resonance_norm = emphasis / MAX_EMPHASIS;
    float ceiling = lerpf(15000.0f, 8500.0f, resonance_norm * resonance_norm);
    L->cut = clampf(finitef_strict(cut_hz) ? cut_hz : 20.0f, 20.0f, ceiling);
    L->res = emphasis * 3.8f;
}
static inline float ladder_process(ladder_t *L, float in){
    float fc = L->cut / (SR * FLT_OVS);
    float g  = tanf((float)M_PI * fc);            /* prewarped */
    float G  = g / (1.0f + g);
    float out = 0.0f;
    for (int os = 0; os < FLT_OVS; ++os){
        float u = in - L->res * L->z;             /* feedback around 4 poles */
        u = tanhf(u);                             /* input drive / saturation */
        float x = u;
        for (int k = 0; k < 4; ++k){
            float v = (x - L->s[k]) * G;
            float y = v + L->s[k];
            L->s[k] = y + v;
            x = tanhf(y);                         /* per-stage nonlinearity  */
        }
        L->z = x;
        out  = x;
    }
    if (!finitef_strict(out)){
        memset(L, 0, sizeof(*L));
        return 0.0f;
    }
    return out;
}

/* --------------------------------------------------------------- params  */
typedef struct {
    /* oscillators */
    int   o1_range, o1_wave;
    int   o2_range, o2_wave; float o2_tune;   /* semitones */
    int   o3_range, o3_wave; float o3_tune; int o3_kbd;
    /* mixer */
    float m1, m2, m3, mnoise; int noise_pink;
    /* filter */
    float cutoff01, emphasis, contour;        /* cutoff 0..1, res 0..1, env amt 0..1 */
    float f_a, f_d, f_s; int kbd_track;
    /* loudness */
    float l_a, l_d, l_s;
    /* global */
    float glide, mtune, mod_mix; int mod_osc_on, mod_filt_on, decay_sw;
    /* dedicated LFO */
    float lfo_rate; int lfo_shape; float lfo_pitch, lfo_filter;
    int lfo_sync, lfo_div;          /* tempo sync + division index */
    /* osc hard sync + filter FM */
    int osc_sync;                   /* 0 off, 1 o2<-o1, 2 o2+o3<-o1 */
    float filt_fm;                  /* audio-rate osc3 -> cutoff depth */
    float volume;
} params_t;

/* ---------------------------------------------------------------- LFO    */
typedef struct { float phase, sh_val; uint32_t rng; } lfo_t;
static inline float lfo_tick(lfo_t *L, float rate_hz, int shape){
    float dt = rate_hz / SR;
    float prev = L->phase;
    L->phase += dt;
    int wrapped = 0;
    if (L->phase >= 1.0f){ L->phase -= 1.0f; wrapped = 1; }
    float t = L->phase;
    switch (shape){
        case 0: return 4.0f * fabsf(t - 0.5f) - 1.0f;         /* triangle */
        case 1: return 2.0f * t - 1.0f;                        /* ramp up  */
        case 2: return t < 0.5f ? 1.0f : -1.0f;                /* square   */
        case 3: return 1.0f - 2.0f * t;                        /* ramp dn  */
        default:                                               /* S&H      */
            if (wrapped || prev == 0.0f){
                L->rng = L->rng * 1664525u + 1013904223u;
                L->sh_val = ((float)(L->rng >> 8) * (1.0f/16777216.0f)) * 2.0f - 1.0f;
            }
            return L->sh_val;
    }
}

typedef struct synth {
    const host_api_v1_t *host;
    params_t p;
    params_t sm;                    /* click/chirp-safe continuous controls */

    osc_t   o1, o2, o3;
    lfo_t   lfo;
    ladder_t lpf;
    adsr_t  fenv, aenv;
    pink_t  pink;
    uint32_t rng;

    /* voice */
    int   held[8], nheld;
    int   gate;
    float note_freq, glide_freq;   /* target vs current (glide) */
    float velocity;
    float pitch_bend, sm_pitch_bend;
    int   preset_idx;              /* last preset applied via set_param("preset", ...) */

    char  err[128];
} synth_t;

static const host_api_v1_t *g_host = NULL;

static inline float frand(synth_t *s){
    s->rng = s->rng * 1664525u + 1013904223u;
    return ((float)(s->rng >> 8) * (1.0f/16777216.0f)) * 2.0f - 1.0f;
}
static inline float midi2hz(float note){
    return 440.0f * powf(2.0f, (note - 69.0f) / 12.0f);
}

static void init_params(params_t *p){
    p->o1_range=3; p->o1_wave=1;
    p->o2_range=3; p->o2_wave=1; p->o2_tune=-0.07f;
    p->o3_range=2; p->o3_wave=1; p->o3_tune= 0.05f; p->o3_kbd=1;
    p->m1=0.8f; p->m2=0.7f; p->m3=0.5f; p->mnoise=0.0f; p->noise_pink=0;
    p->cutoff01=0.38f; p->emphasis=0.25f; p->contour=0.45f;
    p->f_a=0.005f; p->f_d=0.35f; p->f_s=0.30f; p->kbd_track=1;
    p->l_a=0.005f; p->l_d=0.5f;  p->l_s=0.8f;
    p->glide=0.0f; p->mtune=0.0f; p->mod_mix=0.0f;
    p->mod_osc_on=0; p->mod_filt_on=0; p->decay_sw=1;
    p->lfo_rate=0.35f; p->lfo_shape=0; p->lfo_pitch=0.0f; p->lfo_filter=0.0f;
    p->lfo_sync=0; p->lfo_div=4;            /* 1/4 note */
    p->osc_sync=0; p->filt_fm=0.0f;
    p->volume=0.8f;
}

/* ---------------------------------------------------------- preset bank  */
/* Original patches authored for THIS module's ranges, targeting the classic
 * Minimoog sound vocabulary. Voicing was cross-checked against open-source
 * Model D banks, then tuned through this module's own ladder implementation. */
typedef struct {
    const char *name;
    int o1r,o1w,o2r,o2w; float o2t; int o3r,o3w; float o3t,o3k;
    float m1,m2,m3,mn; int pink;
    float cut,emph,cont,fa,fd,fs; int kt;
    float la,ld,ls; int dsw;
    float glide,modmix; int modosc,modfilt;
    float lr; int lsh; float lp,lf;
    int lsync,ldiv; int osync; float ffm;
    float vol;
} preset_t;

/* fields: name, o1r,o1w,o2r,o2w,o2t,o3r,o3w,o3t,o3k, m1,m2,m3,mn,pink,
 *         cut,emph,cont,fa,fd,fs,kt, la,ld,ls,dsw, glide,modmix,modosc,modfilt,
 *         lr,lsh,lp,lf, vol                                                     */
static const preset_t PRESETS[] = {
 {"Init", 3, 1, 3, 1, -0.07f, 2, 1, 0.05f, 1, 0.8f, 0.7f, 0.5f, 0.0f, 0, 0.38f, 0.25f, 0.45f, 0.005f, 0.35f, 0.30f, 1, 0.005f, 0.5f, 0.8f, 1, 0, 0, 0, 0, 0.35f, 0, 0, 0, 0, 4, 0, 0.00f, 0.8f},
 {"Fat Bass", 2, 1, 2, 1, -0.10f, 1, 1, 0.06f, 1, 0.85f, 0.75f, 0.55f, 0, 0, 0.30f, 0.42f, 0.55f, 0.02f, 0.30f, 0.10f, 1, 0.02f, 0.35f, 0.85f, 1, 0, 0, 0, 0, 0.35f, 0, 0, 0, 0, 4, 0, 0.00f, 0.82f},
 {"Sub Bass", 1, 0, 1, 1, 0.0f, 2, 0, 0.0f, 1, 0.9f, 0.4f, 0.0f, 0, 0, 0.18f, 0.10f, 0.25f, 0.02f, 0.45f, 0.40f, 1, 0.02f, 0.50f, 0.95f, 1, 0, 0, 0, 0, 0.35f, 0, 0, 0, 0, 4, 0, 0.00f, 0.88f},
 {"Rubber Bass", 2, 4, 2, 1, -0.08f, 3, 4, 0.05f, 1, 0.8f, 0.65f, 0.3f, 0, 0, 0.25f, 0.68f, 0.55f, 0.01f, 0.22f, 0.05f, 1, 0.01f, 0.28f, 0.6f, 0, 0, 0, 0, 0, 0.35f, 0, 0, 0, 0, 4, 0, 0.00f, 0.82f},
 {"Classic Lead", 3, 1, 3, 1, -0.06f, 3, 0, 0.04f, 1, 0.8f, 0.75f, 0.45f, 0, 0, 0.45f, 0.55f, 0.40f, 0.05f, 0.40f, 0.35f, 1, 0.03f, 0.45f, 0.90f, 1, 0.08f, 0, 0, 0, 0.30f, 0, 0.12f, 0, 0, 4, 0, 0.00f, 0.78f},
 {"Brass", 3, 1, 3, 1, -0.05f, 2, 1, 0.0f, 1, 0.75f, 0.7f, 0.55f, 0, 0, 0.38f, 0.35f, 0.45f, 0.28f, 0.45f, 0.55f, 1, 0.20f, 0.45f, 0.85f, 1, 0, 0, 0, 0, 0.30f, 0, 0, 0, 0, 4, 0, 0.00f, 0.76f},
 {"Funk Lead", 3, 4, 3, 4, -0.05f, 2, 1, 0.0f, 1, 0.80f, 0.60f, 0.25f, 0, 0, 0.32f, 0.62f, 0.50f, 0.003f, 0.20f, 0.05f, 1, 0.003f, 0.25f, 0.30f, 0, 0, 0, 0, 0, 0.30f, 0, 0, 0, 0, 4, 0, 0.00f, 0.80f},
 {"Wobble Bass", 2, 1, 2, 1, -0.08f, 0, 0, 0.0f, 0, 0.85f, 0.7f, 0.0f, 0, 0, 0.22f, 0.70f, 0.20f, 0.02f, 0.40f, 0.50f, 1, 0.02f, 0.45f, 0.95f, 1, 0, 0, 0, 0, 0.18f, 2, 0.0f, 0.55f, 1, 5, 0, 0.00f, 0.82f},
 {"Vibrato Lead", 3, 1, 3, 1, -0.04f, 0, 0, 0.0f, 0, 0.8f, 0.7f, 0.0f, 0, 0, 0.48f, 0.45f, 0.35f, 0.10f, 0.40f, 0.55f, 1, 0.06f, 0.45f, 0.90f, 1, 0.05f, 0, 0, 0, 0.30f, 0, 0.20f, 0, 0, 4, 0, 0.00f, 0.78f},
 {"Sci-Fi Sweep", 3, 1, 2, 1, -0.20f, 2, 1, 0.12f, 1, 0.75f, 0.65f, 0.0f, 0, 0, 0.12f, 0.82f, 0.75f, 0.60f, 0.65f, 0.30f, 1, 0.10f, 0.60f, 0.90f, 1, 0, 0, 0, 0, 0.06f, 0, 0.0f, 0.45f, 0, 4, 0, 0.00f, 0.72f},
 {"Bright Pluck", 4, 1, 3, 1, -0.05f, 3, 1, 0.0f, 1, 0.75f, 0.55f, 0.35f, 0, 0, 0.32f, 0.52f, 0.48f, 0.002f, 0.16f, 0.0f, 1, 0.002f, 0.20f, 0.0f, 0, 0, 0, 0, 0, 0.30f, 0, 0, 0, 0, 4, 0, 0.00f, 0.78f},
 {"Detuned Stab", 3, 1, 3, 1, -0.5f, 3, 1, 0.5f, 1, 0.8f, 0.8f, 0.7f, 0, 0, 0.44f, 0.35f, 0.35f, 0.01f, 0.24f, 0.15f, 1, 0.008f, 0.28f, 0.45f, 1, 0, 0, 0, 0, 0.30f, 0, 0, 0, 0, 4, 0, 0.00f, 0.76f},
 {"Whistle", 4, 0, 4, 0, 0.0f, 0, 0, 0.0f, 0, 0.8f, 0.0f, 0.0f, 0, 0, 0.70f, 0.30f, 0.15f, 0.15f, 0.40f, 0.80f, 1, 0.18f, 0.40f, 0.92f, 1, 0.04f, 0, 0, 0, 0.25f, 0, 0.10f, 0, 0, 4, 0, 0.00f, 0.70f},
 {"Noise Sweep", 3, 0, 2, 0, 0.0f, 0, 0, 0.0f, 0, 0.0f, 0.0f, 0.0f, 0.9f, 1, 0.35f, 0.55f, 0.25f, 0.55f, 0.60f, 0.70f, 0, 0.50f, 0.60f, 0.90f, 1, 0, 1, 0, 1, 0.05f, 0, 0.0f, 0.35f, 0, 4, 0, 0.00f, 0.70f},
 {"Sync Lead",   3,1,3,1,0.0f,4,1,0.0f,1, 0.85f,0.75f,0.0f,0,0, 0.42f,0.58f,0.40f,0.03f,0.35f,0.40f,1, 0.02f,0.40f,0.90f,1, 0.05f,0,0,0, 0.30f,0,0.10f,0, 0,4,1,0.0f, 0.78f},
 {"FM Growl",    2,1,2,3,-0.1f,1,1,0.0f,0, 0.8f,0.65f,0.0f,0,0, 0.24f,0.48f,0.35f,0.01f,0.35f,0.25f,1, 0.006f,0.45f,0.75f,1, 0,0,0,0, 0.30f,0,0,0, 0,4,0,0.22f, 0.80f},
 /* --- expansion bank: 16 more, inspired by classic Model D patch vocabulary
  *     (Bass/Lead/Pad/Bell/FX categories seen across open-source Minimoog
  *     emulators such as stevebarakat/Minimoog) but authored fresh for this
  *     module's own DSP and parameter ranges. */
 {"Taurus Sub",    1,1,1,1,-0.05f,2,1,0.0f,1, 0.90f,0.70f,0.0f,0,0, 0.22f,0.25f,0.35f,0.02f,0.45f,0.30f,1, 0.02f,0.50f,0.95f,1, 0,0,0,0, 0.35f,0,0,0, 0,4,0,0.00f, 0.85f},
 {"Slap Bass",     2,1,2,3,-0.10f,3,0,0.08f,1, 0.80f,0.65f,0.30f,0,0, 0.25f,0.68f,0.55f,0.001f,0.20f,0.05f,1, 0.001f,0.24f,0.0f,0, 0,0,0,0, 0.35f,0,0,0, 0,4,0,0.00f, 0.82f},
 {"Dub Bass",      1,1,1,3,-0.06f,2,0,0.0f,1, 0.85f,0.65f,0.45f,0,0, 0.20f,0.40f,0.35f,0.02f,0.55f,0.55f,1, 0.01f,0.65f,0.82f,1, 0.05f,0,0,0, 0.10f,0,0,0.10f, 0,4,0,0.00f, 0.84f},
 {"Growl Bass",    2,1,2,3,-0.15f,1,1,0.0f,1, 0.85f,0.75f,0.55f,0,0, 0.26f,0.55f,0.50f,0.02f,0.35f,0.20f,1, 0.02f,0.40f,0.88f,1, 0,0,0,0, 0.35f,0,0,0, 0,4,0,0.18f, 0.78f},
 {"Warm Pad",      3,1,3,0,-0.10f,2,1,0.10f,1, 0.70f,0.65f,0.45f,0,0, 0.40f,0.30f,0.35f,0.45f,0.55f,0.60f,1, 0.42f,0.60f,0.88f,1, 0.10f,0,0,0, 0.15f,0,0.04f,0, 0,4,0,0.00f, 0.72f},
 {"Glass Pad",     3,0,4,0,0.15f,5,0,-0.12f,1, 0.70f,0.55f,0.35f,0,0, 0.52f,0.18f,0.25f,0.50f,0.85f,0.75f,1, 0.55f,0.90f,0.86f,1, 0.08f,0,0,0, 0.20f,0,0,0.06f, 0,4,0,0.00f, 0.68f},
 {"Cathedral Drone",1,0,2,0,-0.05f,3,0,0.07f,1, 0.65f,0.55f,0.40f,0.05f,1, 0.30f,0.20f,0.25f,1.20f,1.80f,0.75f,0, 1.00f,2.20f,0.92f,1, 0.30f,0,0,0, 0.02f,0,0,0.12f, 1,0,0,0.00f, 0.64f},
 {"Prog Solo",     3,1,3,3,-0.05f,4,1,0.06f,1, 0.85f,0.75f,0.45f,0,0, 0.45f,0.52f,0.42f,0.03f,0.40f,0.45f,1, 0.02f,0.45f,0.92f,1, 0.12f,0,0,0, 0.32f,0,0.13f,0.04f, 0,4,0,0.00f, 0.78f},
 {"Screamer Lead", 3,4,3,1,-0.04f,4,3,0.09f,1, 0.85f,0.75f,0.45f,0,0, 0.42f,0.72f,0.45f,0.03f,0.35f,0.40f,1, 0.02f,0.40f,0.92f,1, 0,0,0,0, 0.35f,0,0,0, 0,4,0,0.00f, 0.78f},
 {"Watery Lead",   3,0,3,1,-0.15f,4,0,0.10f,1, 0.75f,0.65f,0.35f,0,0, 0.38f,0.45f,0.32f,0.08f,0.55f,0.65f,1, 0.05f,0.65f,0.85f,1, 0.05f,0,0,0, 0.12f,0,0.08f,0.10f, 0,4,0,0.00f, 0.74f},
 {"Simple Lead",   3,1,3,1,-0.03f,4,0,0.0f,1, 0.85f,0.55f,0.20f,0,0, 0.42f,0.32f,0.35f,0.01f,0.40f,0.45f,1, 0.005f,0.45f,0.88f,1, 0,0,0,0, 0.35f,0,0,0, 0,4,0,0.00f, 0.78f},
 {"Crystal Bells", 4,0,5,0,0.5f,3,2,-0.7f,0, 0.70f,0.50f,0.25f,0,0, 0.60f,0.40f,0.35f,0.001f,0.35f,0.10f,1, 0.001f,0.42f,0.15f,0, 0,0,0,0, 0.35f,0,0,0, 0,4,0,0.16f, 0.70f},
 {"Wind Chimes",   3,0,4,0,0.35f,5,0,-0.4f,0, 0.65f,0.50f,0.25f,0.08f,0, 0.58f,0.28f,0.25f,0.01f,0.90f,0.0f,1, 0.01f,1.10f,0.0f,0, 0,0.3f,1,0, 0.08f,4,0.05f,0, 0,4,0,0.00f, 0.66f},
 {"Submarine Sonar",1,0,1,0,0.02f,2,0,0.0f,1, 0.80f,0.55f,0.20f,0,0, 0.24f,0.62f,0.38f,0.002f,0.40f,0.05f,0, 0.002f,0.85f,0.20f,0, 0,0,0,0, 0.06f,4,0,0.30f, 0,4,0,0.00f, 0.68f},
 {"Alien Signal",  3,2,3,2,0.30f,4,1,-0.20f,0, 0.75f,0.60f,0.0f,0,0, 0.32f,0.55f,0.45f,0.01f,0.45f,0.35f,0, 0.01f,0.55f,0.65f,1, 0,0,0,0, 0.40f,1,0.08f,0.08f, 0,4,2,0.18f, 0.72f},
 {"Cosmic Drone",  0,1,1,0,0.03f,2,0,-0.04f,1, 0.60f,0.50f,0.35f,0.12f,1, 0.30f,0.35f,0.25f,1.50f,2.50f,0.75f,0, 1.20f,3.00f,0.90f,1, 0.40f,0,0,0, 0.03f,0,0,0.22f, 1,1,0,0.00f, 0.60f},
};
#define NPRESETS ((int)(sizeof(PRESETS)/sizeof(PRESETS[0])))

static void apply_preset(params_t *p, int idx){
    if (idx < 0 || idx >= NPRESETS) return;
    const preset_t *q = &PRESETS[idx];
    p->o1_range=q->o1r; p->o1_wave=q->o1w;
    p->o2_range=q->o2r; p->o2_wave=q->o2w; p->o2_tune=q->o2t;
    p->o3_range=q->o3r; p->o3_wave=q->o3w; p->o3_tune=q->o3t; p->o3_kbd=(int)q->o3k;
    p->m1=q->m1; p->m2=q->m2; p->m3=q->m3; p->mnoise=q->mn; p->noise_pink=q->pink;
    p->cutoff01=q->cut; p->emphasis=q->emph; p->contour=q->cont;
    p->f_a=q->fa; p->f_d=q->fd; p->f_s=q->fs; p->kbd_track=q->kt;
    p->l_a=q->la; p->l_d=q->ld; p->l_s=q->ls; p->decay_sw=q->dsw;
    p->glide=q->glide; p->mod_mix=q->modmix; p->mod_osc_on=q->modosc; p->mod_filt_on=q->modfilt;
    p->lfo_rate=q->lr; p->lfo_shape=q->lsh; p->lfo_pitch=q->lp; p->lfo_filter=q->lf;
    p->lfo_sync=q->lsync; p->lfo_div=q->ldiv; p->osc_sync=q->osync; p->filt_fm=q->ffm;
    p->volume=q->vol;
}

/* ---------------------------------------------------------- lifecycle    */
static void *create_instance(const char *dir, const char *json){
    (void)dir; (void)json;
    synth_t *s = (synth_t*)calloc(1, sizeof(synth_t));
    if (!s) return NULL;
    s->host = g_host;
    s->rng  = 0x1234abcdu;
    init_params(&s->p);
    s->sm = s->p;
    s->note_freq = s->glide_freq = 220.0f;
    s->velocity  = 0.7f;
    s->preset_idx = 0;
    s->o1.range=s->p.o1_range; s->o1.wave=s->p.o1_wave;
    s->o2.range=s->p.o2_range; s->o2.wave=s->p.o2_wave;
    s->o3.range=s->p.o3_range; s->o3.wave=s->p.o3_wave;
    s->lfo.rng = 0x9E3779B1u;
    return s;
}
static void destroy_instance(void *inst){ free(inst); }

/* ------------------------------------------------------------- MIDI      */
static void note_on(synth_t *s, int note, int vel){
    int was_idle = (s->nheld == 0);
    if (s->nheld < 8) s->held[s->nheld++] = note;
    s->note_freq = midi2hz((float)note);
    /* Portamento belongs between played notes, not from the constructor's
     * arbitrary 220 Hz seed into the first note. */
    if (was_idle) s->glide_freq = s->note_freq;
    s->velocity  = vel / 127.0f;
    s->gate = 1;
    adsr_gate(&s->fenv, 1);
    adsr_gate(&s->aenv, 1);
}
static void note_off(synth_t *s, int note){
    int i, j = 0;
    for (i = 0; i < s->nheld; ++i) if (s->held[i] != note) s->held[j++] = s->held[i];
    s->nheld = j;
    if (s->nheld > 0){                    /* last-note priority: fall back */
        s->note_freq = midi2hz((float)s->held[s->nheld-1]);
    } else {
        s->gate = 0;
        adsr_gate(&s->fenv, 0);
        adsr_gate(&s->aenv, 0);
    }
}
static void on_midi(void *inst, const uint8_t *m, int len, int src){
    (void)src;
    if (!inst || !m || len < 1) return;
    synth_t *s = (synth_t*)inst;
    uint8_t st = m[0] & 0xF0;
    if ((st == 0x80 || st == 0x90 || st == 0xE0) && len < 3) return;
    if (st == 0x90 && m[2] > 0)      note_on(s, m[1], m[2]);
    else if (st == 0x80 || (st == 0x90 && m[2] == 0)) note_off(s, m[1]);
    else if (st == 0xE0){            /* pitch bend is separate from master tune */
        int bend = ((int)m[2] << 7 | m[1]) - 8192;
        s->pitch_bend = clampf((bend / 8192.0f) * 2.0f, -2.0f, 2.0f);
    }
}

/* ------------------------------------------------------------- params IO */
static float fparse_bounded(const char *v, float current, float lo, float hi){
    if (!v) return current;
    char *end = NULL;
    float parsed = strtof(v, &end);
    if (end == v || !finitef_strict(parsed)) return current;
    return clampf(parsed, lo, hi);
}

/* Match val against option names (case-insensitive); fall back to integer. */
static int eparse(const char *v, const char *const *names, int n){
    for (int i = 0; i < n; ++i){
        const char *a = v, *b = names[i];
        while (*a && *b){
            char ca = *a, cb = *b;
            if (ca>='A'&&ca<='Z') ca += 32;
            if (cb>='A'&&cb<='Z') cb += 32;
            if (ca != cb) break;
            ++a; ++b;
        }
        if (!*a && !*b) return i;
    }
    int parsed = (int)strtol(v ? v : "0", NULL, 10);
    if (parsed < 0) parsed = 0;
    if (parsed >= n) parsed = n - 1;
    return parsed;
}
static const char *const RANGE_NAMES[6] = {"LO","32'","16'","8'","4'","2'"};
static const char *const WAVE_NAMES[5]  = {"Tri","Saw","Square","Wide Pulse","Narrow Pulse"};
static const char *const COLOR_NAMES[2] = {"White","Pink"};
static const char *const ONOFF_NAMES[2] = {"Off","On"};
static const char *const LFO_SHAPE_NAMES[5] = {"Tri","Ramp Up","Square","Ramp Dn","S&H"};
/* Tempo-sync divisions: beats-per-LFO-cycle (4/4). */
static const char *const LFO_DIV_NAMES[10] = {"4 bars","2 bars","1 bar","1/2","1/4","1/8","1/8T","1/16","1/16T","1/32"};
static const float LFO_DIV_BEATS[10]       = { 16.0f,   8.0f,    4.0f,   2.0f, 1.0f, 0.5f, 0.3333f,0.25f, 0.16667f,0.125f };
static const char *const SYNC_NAMES[3]     = {"Off","Osc2","Osc2+3"};

/*
 * Serve the hierarchy through the plugin ABI, matching Osirus. Schwung 0.11.6
 * discards a module.json hierarchy whose inline objects omit repeated "type"
 * metadata, even though Shadow can resolve key-only objects via chain_params.
 */
static const char UI_HIERARCHY_JSON[] =
    "{\"modes\":null,\"levels\":{"
      "\"root\":{"
        "\"name\":\"Mook D\",\"list_param\":\"preset\",\"count_param\":\"preset_count\","
        "\"name_param\":\"preset_name\",\"children\":null,"
        "\"knobs\":[\"cutoff\",\"emphasis\",\"contour\",\"filt_a\",\"filt_d\",\"loud_a\",\"loud_d\",\"glide\"],"
        "\"params\":["
          "{\"key\":\"cutoff\"},{\"key\":\"emphasis\"},{\"key\":\"contour\"},"
          "{\"key\":\"filt_a\"},{\"key\":\"filt_d\"},{\"key\":\"loud_a\"},"
          "{\"key\":\"loud_d\"},{\"key\":\"glide\"},"
          "{\"level\":\"osc1\",\"label\":\"Oscillator 1\"},"
          "{\"level\":\"osc2\",\"label\":\"Oscillator 2\"},"
          "{\"level\":\"osc3\",\"label\":\"Oscillator 3\"},"
          "{\"level\":\"mixer\",\"label\":\"Mixer\"},"
          "{\"level\":\"filter\",\"label\":\"Ladder Filter\"},"
          "{\"level\":\"envelopes\",\"label\":\"Envelopes\"},"
          "{\"level\":\"controllers\",\"label\":\"Controllers\"},"
          "{\"level\":\"lfo\",\"label\":\"LFO\"},"
          "{\"level\":\"lfo_dest\",\"label\":\"LFO Routing\"}"
        "]"
      "},"
      "\"osc1\":{\"name\":\"Oscillator 1\",\"knobs\":[\"o1_range\",\"o1_wave\"],"
        "\"params\":[{\"key\":\"o1_range\"},{\"key\":\"o1_wave\"}]},"
      "\"osc2\":{\"name\":\"Oscillator 2\",\"knobs\":[\"o2_range\",\"o2_wave\",\"o2_tune\"],"
        "\"params\":[{\"key\":\"o2_range\"},{\"key\":\"o2_wave\"},{\"key\":\"o2_tune\"}]},"
      "\"osc3\":{\"name\":\"Oscillator 3\","
        "\"knobs\":[\"o3_range\",\"o3_wave\",\"o3_tune\",\"o3_kbd\",\"osc_sync\"],"
        "\"params\":[{\"key\":\"o3_range\"},{\"key\":\"o3_wave\"},{\"key\":\"o3_tune\"},{\"key\":\"o3_kbd\"},{\"key\":\"osc_sync\"}]},"
      "\"mixer\":{\"name\":\"Mixer\","
        "\"knobs\":[\"mix_o1\",\"mix_o2\",\"mix_o3\",\"mix_noise\",\"noise_color\",\"volume\"],"
        "\"params\":[{\"key\":\"mix_o1\"},{\"key\":\"mix_o2\"},{\"key\":\"mix_o3\"},{\"key\":\"mix_noise\"},{\"key\":\"noise_color\"},{\"key\":\"volume\"}]},"
      "\"filter\":{\"name\":\"Ladder Filter\","
        "\"knobs\":[\"cutoff\",\"emphasis\",\"contour\",\"filt_fm\",\"filt_a\",\"filt_d\",\"filt_s\",\"kbd_track\"],"
        "\"params\":[{\"key\":\"cutoff\"},{\"key\":\"emphasis\"},{\"key\":\"contour\"},{\"key\":\"filt_fm\"},{\"key\":\"filt_a\"},{\"key\":\"filt_d\"},{\"key\":\"filt_s\"},{\"key\":\"kbd_track\"}]},"
      "\"envelopes\":{\"name\":\"Loudness Contour\","
        "\"knobs\":[\"loud_a\",\"loud_d\",\"loud_s\",\"decay_sw\"],"
        "\"params\":[{\"key\":\"loud_a\"},{\"key\":\"loud_d\"},{\"key\":\"loud_s\"},{\"key\":\"decay_sw\"}]},"
      "\"controllers\":{\"name\":\"Controllers\","
        "\"knobs\":[\"preset\",\"glide\",\"master_tune\",\"mod_mix\",\"mod_osc\",\"mod_filter\"],"
        "\"params\":[{\"key\":\"preset\"},{\"key\":\"glide\"},{\"key\":\"master_tune\"},{\"key\":\"mod_mix\"},{\"key\":\"mod_osc\"},{\"key\":\"mod_filter\"}]},"
      "\"lfo\":{\"name\":\"LFO\",\"knobs\":[\"lfo_rate\",\"lfo_shape\",\"lfo_sync\",\"lfo_div\"],"
        "\"params\":[{\"key\":\"lfo_rate\"},{\"key\":\"lfo_shape\"},{\"key\":\"lfo_sync\"},{\"key\":\"lfo_div\"}]},"
      "\"lfo_dest\":{\"name\":\"LFO Routing\",\"knobs\":[\"lfo_pitch\",\"lfo_filter\"],"
        "\"params\":[{\"key\":\"lfo_pitch\"},{\"key\":\"lfo_filter\"}]}"
    "}}";

static void set_param(void *inst, const char *k, const char *v){
    if (!inst || !k || !v) return;
    synth_t *s = (synth_t*)inst; params_t *p = &s->p;
    if      (!strcmp(k,"o1_range")){ p->o1_range=eparse(v,RANGE_NAMES,6); s->o1.range=p->o1_range; }
    else if (!strcmp(k,"o1_wave")){  p->o1_wave =eparse(v,WAVE_NAMES,5); s->o1.wave =p->o1_wave;  }
    else if (!strcmp(k,"o2_range")){ p->o2_range=eparse(v,RANGE_NAMES,6); s->o2.range=p->o2_range; }
    else if (!strcmp(k,"o2_wave")){  p->o2_wave =eparse(v,WAVE_NAMES,5); s->o2.wave =p->o2_wave;  }
    else if (!strcmp(k,"o2_tune"))   p->o2_tune=fparse_bounded(v,p->o2_tune,-12.0f,12.0f);
    else if (!strcmp(k,"o3_range")){ p->o3_range=eparse(v,RANGE_NAMES,6); s->o3.range=p->o3_range; }
    else if (!strcmp(k,"o3_wave")){  p->o3_wave =eparse(v,WAVE_NAMES,5); s->o3.wave =p->o3_wave;  }
    else if (!strcmp(k,"o3_tune"))   p->o3_tune=fparse_bounded(v,p->o3_tune,-12.0f,12.0f);
    else if (!strcmp(k,"o3_kbd"))    p->o3_kbd=eparse(v,ONOFF_NAMES,2);
    else if (!strcmp(k,"mix_o1"))    p->m1=fparse_bounded(v,p->m1,0.0f,1.0f);
    else if (!strcmp(k,"mix_o2"))    p->m2=fparse_bounded(v,p->m2,0.0f,1.0f);
    else if (!strcmp(k,"mix_o3"))    p->m3=fparse_bounded(v,p->m3,0.0f,1.0f);
    else if (!strcmp(k,"mix_noise")) p->mnoise=fparse_bounded(v,p->mnoise,0.0f,1.0f);
    else if (!strcmp(k,"noise_color")) p->noise_pink=eparse(v,COLOR_NAMES,2);
    else if (!strcmp(k,"cutoff"))    p->cutoff01=fparse_bounded(v,p->cutoff01,0.0f,MAX_CUTOFF_CTRL);
    else if (!strcmp(k,"emphasis"))  p->emphasis=fparse_bounded(v,p->emphasis,0.0f,MAX_EMPHASIS);
    else if (!strcmp(k,"contour"))   p->contour=fparse_bounded(v,p->contour,0.0f,MAX_CONTOUR);
    else if (!strcmp(k,"filt_a"))    p->f_a=fparse_bounded(v,p->f_a,0.001f,4.0f);
    else if (!strcmp(k,"filt_d"))    p->f_d=fparse_bounded(v,p->f_d,0.001f,6.0f);
    else if (!strcmp(k,"filt_s"))    p->f_s=fparse_bounded(v,p->f_s,0.0f,1.0f);
    else if (!strcmp(k,"kbd_track")) p->kbd_track=eparse(v,ONOFF_NAMES,2);
    else if (!strcmp(k,"loud_a"))    p->l_a=fparse_bounded(v,p->l_a,0.001f,4.0f);
    else if (!strcmp(k,"loud_d"))    p->l_d=fparse_bounded(v,p->l_d,0.001f,6.0f);
    else if (!strcmp(k,"loud_s"))    p->l_s=fparse_bounded(v,p->l_s,0.0f,1.0f);
    else if (!strcmp(k,"glide"))     p->glide=fparse_bounded(v,p->glide,0.0f,1.0f);
    else if (!strcmp(k,"master_tune")) p->mtune=fparse_bounded(v,p->mtune,-2.0f,2.0f);
    else if (!strcmp(k,"mod_mix"))   p->mod_mix=fparse_bounded(v,p->mod_mix,0.0f,1.0f);
    else if (!strcmp(k,"mod_osc"))   p->mod_osc_on=eparse(v,ONOFF_NAMES,2);
    else if (!strcmp(k,"mod_filter"))p->mod_filt_on=eparse(v,ONOFF_NAMES,2);
    else if (!strcmp(k,"decay_sw"))  p->decay_sw=eparse(v,ONOFF_NAMES,2);
    else if (!strcmp(k,"lfo_rate"))  p->lfo_rate=fparse_bounded(v,p->lfo_rate,0.0f,1.0f);
    else if (!strcmp(k,"lfo_shape")) p->lfo_shape=eparse(v,LFO_SHAPE_NAMES,5);
    else if (!strcmp(k,"lfo_pitch")) p->lfo_pitch=fparse_bounded(v,p->lfo_pitch,0.0f,1.0f);
    else if (!strcmp(k,"lfo_filter"))p->lfo_filter=fparse_bounded(v,p->lfo_filter,0.0f,MAX_LFO_FILTER);
    else if (!strcmp(k,"lfo_sync"))  p->lfo_sync=eparse(v,ONOFF_NAMES,2);
    else if (!strcmp(k,"lfo_div"))   p->lfo_div=eparse(v,LFO_DIV_NAMES,10);
    else if (!strcmp(k,"osc_sync"))  p->osc_sync=eparse(v,SYNC_NAMES,3);
    else if (!strcmp(k,"filt_fm"))   p->filt_fm=fparse_bounded(v,p->filt_fm,0.0f,MAX_FILTER_FM);
    else if (!strcmp(k,"volume"))    p->volume=fparse_bounded(v,p->volume,0.0f,1.0f);
    else if (!strcmp(k,"preset")){
        int idx = -1;
        for (int i = 0; i < NPRESETS; ++i){
            const char *a=v,*b=PRESETS[i].name; int m=1;
            while (*a && *b){ char ca=*a,cb=*b;
                if(ca>='A'&&ca<='Z')ca+=32;
                if(cb>='A'&&cb<='Z')cb+=32;
                if(ca!=cb){m=0;break;}
                ++a;++b; }
            if (m && !*a && !*b){ idx=i; break; }
        }
        if (idx < 0) idx = (int)strtol(v, NULL, 10);  /* numeric fallback */
        if (idx >= 0 && idx < NPRESETS) s->preset_idx = idx;
        apply_preset(p, idx);
        s->o1.range=p->o1_range; s->o1.wave=p->o1_wave;
        s->o2.range=p->o2_range; s->o2.wave=p->o2_wave;
        s->o3.range=p->o3_range; s->o3.wave=p->o3_wave;
    }
}
/* Mirrors set_param's key list so every knob/menu item can read back its
 * current value. Floats render as decimals; enum/int-backed params render
 * as their numeric index (matches eparse()'s wire format on the way in). */
static int get_param(void *inst, const char *k, char *buf, int n){
    synth_t *s = (synth_t*)inst; params_t *p = &s->p; float f; int i;
    if      (!strcmp(k,"ui_hierarchy")) return snprintf(buf, n, "%s", UI_HIERARCHY_JSON);
    else if (!strcmp(k,"o1_range"))    i=p->o1_range;
    else if (!strcmp(k,"o1_wave"))     i=p->o1_wave;
    else if (!strcmp(k,"o2_range"))    i=p->o2_range;
    else if (!strcmp(k,"o2_wave"))     i=p->o2_wave;
    else if (!strcmp(k,"o2_tune"))     { f=p->o2_tune;   goto ffmt; }
    else if (!strcmp(k,"o3_range"))    i=p->o3_range;
    else if (!strcmp(k,"o3_wave"))     i=p->o3_wave;
    else if (!strcmp(k,"o3_tune"))     { f=p->o3_tune;   goto ffmt; }
    else if (!strcmp(k,"o3_kbd"))      i=p->o3_kbd;
    else if (!strcmp(k,"mix_o1"))      { f=p->m1;        goto ffmt; }
    else if (!strcmp(k,"mix_o2"))      { f=p->m2;        goto ffmt; }
    else if (!strcmp(k,"mix_o3"))      { f=p->m3;        goto ffmt; }
    else if (!strcmp(k,"mix_noise"))   { f=p->mnoise;    goto ffmt; }
    else if (!strcmp(k,"noise_color")) i=p->noise_pink;
    else if (!strcmp(k,"cutoff"))      { f=p->cutoff01;  goto ffmt; }
    else if (!strcmp(k,"emphasis"))    { f=p->emphasis;  goto ffmt; }
    else if (!strcmp(k,"contour"))     { f=p->contour;   goto ffmt; }
    else if (!strcmp(k,"filt_a"))      { f=p->f_a;       goto ffmt; }
    else if (!strcmp(k,"filt_d"))      { f=p->f_d;       goto ffmt; }
    else if (!strcmp(k,"filt_s"))      { f=p->f_s;       goto ffmt; }
    else if (!strcmp(k,"kbd_track"))   i=p->kbd_track;
    else if (!strcmp(k,"loud_a"))      { f=p->l_a;       goto ffmt; }
    else if (!strcmp(k,"loud_d"))      { f=p->l_d;       goto ffmt; }
    else if (!strcmp(k,"loud_s"))      { f=p->l_s;       goto ffmt; }
    else if (!strcmp(k,"glide"))       { f=p->glide;     goto ffmt; }
    else if (!strcmp(k,"master_tune")) { f=p->mtune;     goto ffmt; }
    else if (!strcmp(k,"mod_mix"))     { f=p->mod_mix;   goto ffmt; }
    else if (!strcmp(k,"mod_osc"))     i=p->mod_osc_on;
    else if (!strcmp(k,"mod_filter"))  i=p->mod_filt_on;
    else if (!strcmp(k,"decay_sw"))    i=p->decay_sw;
    else if (!strcmp(k,"lfo_rate"))    { f=p->lfo_rate;  goto ffmt; }
    else if (!strcmp(k,"lfo_shape"))   i=p->lfo_shape;
    else if (!strcmp(k,"lfo_pitch"))   { f=p->lfo_pitch; goto ffmt; }
    else if (!strcmp(k,"lfo_filter"))  { f=p->lfo_filter;goto ffmt; }
    else if (!strcmp(k,"lfo_sync"))    i=p->lfo_sync;
    else if (!strcmp(k,"lfo_div"))     i=p->lfo_div;
    else if (!strcmp(k,"osc_sync"))    i=p->osc_sync;
    else if (!strcmp(k,"filt_fm"))     { f=p->filt_fm;   goto ffmt; }
    else if (!strcmp(k,"volume"))      { f=p->volume;    goto ffmt; }
    else if (!strcmp(k,"preset"))      i=s->preset_idx;
    else if (!strcmp(k,"preset_count")) i=NPRESETS;
    else if (!strcmp(k,"preset_name")) return snprintf(buf, n, "%s", PRESETS[s->preset_idx].name);
    else return -1;
    return snprintf(buf, n, "%d", i);
ffmt:
    return snprintf(buf, n, "%.4f", f);
}
static int get_error(void *inst, char *buf, int n){
    synth_t *s = (synth_t*)inst;
    if (!s->err[0]) return 0;
    return snprintf(buf, n, "%s", s->err);
}

/* --------------------------------------------------------------- render  */
static void render_block(void *inst, int16_t *out, int frames){
    if (!inst || !out || frames <= 0) return;
    synth_t *s = (synth_t*)inst; params_t *p = &s->p;
    params_t *c = &s->sm;

    adsr_set(&s->fenv, p->f_a, p->f_d, p->f_s, p->decay_sw ? p->f_d : 0.008f);
    adsr_set(&s->aenv, p->l_a, p->l_d, p->l_s, p->decay_sw ? p->l_d : 0.008f);

    /* glide coefficient: p->glide in 0..1 -> ~0..1.5 s */
    float glide_t = p->glide * 1.5f;
    float glide_c = (glide_t <= 0.0005f) ? 1.0f : seg_coef(glide_t);

    const float fast_control_c = seg_coef(0.008f);
    const float filter_control_c = seg_coef(0.020f);

    for (int i = 0; i < frames; ++i){
        /* Physical knob updates arrive at block boundaries. Slew continuous
         * controls per sample so range/cutoff changes do not become chirps. */
        c->o2_tune   = smoothf(c->o2_tune,   p->o2_tune,   fast_control_c);
        c->o3_tune   = smoothf(c->o3_tune,   p->o3_tune,   fast_control_c);
        c->mtune     = smoothf(c->mtune,     p->mtune,     fast_control_c);
        c->m1        = smoothf(c->m1,        p->m1,        fast_control_c);
        c->m2        = smoothf(c->m2,        p->m2,        fast_control_c);
        c->m3        = smoothf(c->m3,        p->m3,        fast_control_c);
        c->mnoise    = smoothf(c->mnoise,    p->mnoise,    fast_control_c);
        c->mod_mix   = smoothf(c->mod_mix,   p->mod_mix,   fast_control_c);
        c->lfo_pitch = smoothf(c->lfo_pitch, p->lfo_pitch, fast_control_c);
        c->volume    = smoothf(c->volume,    p->volume,    fast_control_c);
        c->cutoff01  = smoothf(c->cutoff01,  p->cutoff01,  filter_control_c);
        c->emphasis  = smoothf(c->emphasis,  p->emphasis,  filter_control_c);
        c->contour   = smoothf(c->contour,   p->contour,   filter_control_c);
        c->filt_fm   = smoothf(c->filt_fm,   p->filt_fm,   filter_control_c);
        c->lfo_filter= smoothf(c->lfo_filter,p->lfo_filter,filter_control_c);
        s->sm_pitch_bend = smoothf(s->sm_pitch_bend, s->pitch_bend, fast_control_c);

        /* portamento toward target note */
        s->glide_freq += (s->note_freq - s->glide_freq) * glide_c;
        float tune = powf(2.0f, (c->mtune + s->sm_pitch_bend) / 12.0f);
        float base = s->glide_freq * tune;

        /* osc3 doubles as mod source when not keyboard-tracking */
        float o3freq = (p->o3_kbd ? base : 8.0f)
                       * range_mult(p->o3_range)
                       * powf(2.0f, c->o3_tune / 12.0f);
        float o3 = osc_render(&s->o3, o3freq, NULL);

        float white = frand(s);
        float noise = p->noise_pink ? pink_next(&s->pink, white) : white;

        /* dedicated LFO: free rate 0.05..40 Hz (log), or tempo-synced to host BPM */
        float lfo_hz;
        if (p->lfo_sync){
            float bpm = (s->host && s->host->get_bpm) ? s->host->get_bpm() : 120.0f;
            if (!finitef_strict(bpm) || bpm < 1.0f) bpm = 120.0f;
            bpm = clampf(bpm, 20.0f, 400.0f);
            int d = p->lfo_div; if (d < 0) d = 0; if (d > 9) d = 9;
            lfo_hz = (bpm / 60.0f) / LFO_DIV_BEATS[d];
        } else {
            lfo_hz = 0.05f * powf(800.0f, clampf(p->lfo_rate, 0.0f, 1.0f));
        }
        float lfo = lfo_tick(&s->lfo, lfo_hz, p->lfo_shape);

        /* modulation mix: blend osc3 <-> noise, routed to pitch/filter */
        float modsig = lerpf(o3, noise, c->mod_mix);
        float pitch_mod = (p->mod_osc_on  ? (modsig * 0.06f) : 0.0f)   /* +-~7% */
                        + c->lfo_pitch * lfo * 0.06f;                  /* vibrato */
        /* The route switch used to apply +/-0.4 normalized cutoff (roughly
         * four octaves) at full depth. Give the switch a useful fixed depth
         * and keep the dedicated LFO depth independently bounded. */
        float filt_mod  = (p->mod_filt_on ? modsig * 0.16f : 0.0f)
                        + c->lfo_filter * lfo * 0.22f;

        float pm = powf(2.0f, pitch_mod);
        float o1f = base * range_mult(p->o1_range) * pm;
        float o2f = base * range_mult(p->o2_range) * powf(2.0f, c->o2_tune/12.0f) * pm;
        int o1w = 0;
        float o1 = osc_render(&s->o1, o1f, &o1w);
        if (p->osc_sync && o1w){          /* hard sync: master osc1 resets slaves */
            s->o2.phase = 0.0f;
            if (p->osc_sync == 2) s->o3.phase = 0.0f;
        }
        float o2 = osc_render(&s->o2, o2f, NULL);

        float mix = o1*c->m1 + o2*c->m2 + o3*c->m3 + noise*c->mnoise;
        mix *= 0.5f;

        /* filter cutoff: base + keytrack + envelope + mod, in log-ish Hz */
        float fenv = adsr_tick(&s->fenv);
        /* One normalized cutoff unit spans roughly ten octaves (20 Hz..20 kHz).
         * Scale key tracking in that domain so one played octave moves the
         * ladder by one octave. The former 0.5 multiplier tracked at ~5x and
         * made low bass notes vanish while upper notes flew open and brittle. */
        float track = p->kbd_track ? (log2f(base/220.0f) * 0.1f) : 0.0f;
        float cut_norm = c->cutoff01 + track
                       + c->contour * fenv
                       + filt_mod
                       + c->filt_fm * o3 * 0.30f;   /* bounded audio-rate filter FM */
        cut_norm = safe_cutoff_norm(cut_norm);
        float cut_hz = 20.0f * powf(1000.0f, cut_norm); /* soft-limited below 20k */
        ladder_set(&s->lpf, cut_hz, c->emphasis);
        float filtered = ladder_process(&s->lpf, mix);

        float amp = adsr_tick(&s->aenv) * (0.4f + 0.6f * s->velocity);
        float outv = clampf(filtered * amp * c->volume, -1.0f, 1.0f);

        int16_t sm = (int16_t)(outv * 32767.0f);
        out[2*i]   = sm;
        out[2*i+1] = sm;
    }
}

/* --------------------------------------------------------------- export  */
plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t *host){
    g_host = host;
    static plugin_api_v2_t api;
    api.api_version     = MOVE_PLUGIN_API_VERSION_2;
    api.create_instance = create_instance;
    api.destroy_instance= destroy_instance;
    api.on_midi         = on_midi;
    api.set_param       = set_param;
    api.get_param       = get_param;
    api.get_error       = get_error;
    api.render_block    = render_block;
    return &api;
}
