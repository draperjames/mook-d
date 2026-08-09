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

/* ------------------------------------------------------------------ utils */
static inline float clampf(float x, float lo, float hi){
    return x < lo ? lo : (x > hi ? hi : x);
}
static inline float lerpf(float a, float b, float t){ return a + (b - a) * t; }

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
    float dt = freq / SR;
    if (dt > 0.5f) dt = 0.5f;
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
    return v;
}

/* ------------------------------------------------ transistor ladder (Model D-style) */
/* Huovilainen-style nonlinear 4-pole, 2x oversampled for stability. */
typedef struct { float s[4], z; float cut, res; } ladder_t;

static inline void ladder_set(ladder_t *L, float cut_hz, float res01){
    L->cut = clampf(cut_hz, 20.0f, 18000.0f);
    L->res = clampf(res01, 0.0f, 1.0f) * 4.0f;   /* up to self-oscillation */
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
    p->m1=0.8f; p->m2=0.7f; p->m3=0.6f; p->mnoise=0.0f; p->noise_pink=0;
    p->cutoff01=0.45f; p->emphasis=0.35f; p->contour=0.65f;
    p->f_a=0.005f; p->f_d=0.35f; p->f_s=0.15f; p->kbd_track=1;
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
 * Minimoog sound vocabulary. Not copied from any project. */
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
 {"Init", 3, 1, 3, 1, -0.07f, 2, 1, 0.05f, 1, 0.8f, 0.7f, 0.6f, 0.0f, 0, 0.45f, 0.35f, 0.65f, 0.005f, 0.35f, 0.15f, 1, 0.005f, 0.5f, 0.8f, 1, 0, 0, 0, 0, 0.35f, 0, 0, 0, 0, 4, 0, 0.00f, 0.8f},
 {"Fat Bass", 3, 1, 3, 1, -0.10f, 2, 1, 0.06f, 1, 0.85f, 0.75f, 0.55f, 0, 0, 0.30f, 0.40f, 0.75f, 0.004f, 0.30f, 0.05f, 1, 0.004f, 0.35f, 0.6f, 1, 0, 0, 0, 0, 0.35f, 0, 0, 0, 0, 4, 0, 0.00f, 0.85f},
 {"Sub Bass", 3, 0, 2, 0, 0.0f, 1, 0, 0.0f, 1, 0.9f, 0.6f, 0.7f, 0, 0, 0.22f, 0.20f, 0.45f, 0.004f, 0.25f, 0.0f, 1, 0.004f, 0.30f, 0.7f, 1, 0, 0, 0, 0, 0.35f, 0, 0, 0, 0, 4, 0, 0.00f, 0.9f},
 {"Rubber Bass", 3, 2, 3, 2, -0.08f, 2, 2, 0.05f, 1, 0.8f, 0.7f, 0.4f, 0, 0, 0.28f, 0.55f, 0.80f, 0.003f, 0.18f, 0.0f, 1, 0.003f, 0.22f, 0.4f, 1, 0, 0, 0, 0, 0.35f, 0, 0, 0, 0, 4, 0, 0.00f, 0.85f},
 {"Classic Lead", 3, 1, 3, 1, -0.06f, 4, 1, 0.04f, 1, 0.8f, 0.75f, 0.6f, 0, 0, 0.55f, 0.30f, 0.55f, 0.006f, 0.4f, 0.7f, 1, 0.006f, 0.5f, 0.9f, 1, 0.08f, 0, 0, 0, 0.30f, 0, 0.12f, 0, 0, 4, 0, 0.00f, 0.8f},
 {"Brass", 3, 1, 3, 1, -0.05f, 3, 1, 0.0f, 1, 0.8f, 0.7f, 0.5f, 0, 0, 0.42f, 0.28f, 0.60f, 0.08f, 0.5f, 0.55f, 1, 0.06f, 0.6f, 0.85f, 1, 0, 0, 0, 0, 0.30f, 0, 0, 0, 0, 4, 0, 0.00f, 0.8f},
 {"Funk Lead", 4, 4, 4, 4, -0.05f, 3, 1, 0.0f, 1, 0.85f, 0.5f, 0.45f, 0, 0, 0.40f, 0.65f, 0.85f, 0.003f, 0.14f, 0.0f, 1, 0.003f, 0.25f, 0.5f, 1, 0, 0, 0, 0, 0.30f, 0, 0, 0, 0, 4, 0, 0.00f, 0.8f},
 {"Wobble Bass", 3, 2, 3, 2, -0.08f, 2, 2, 0.0f, 1, 0.85f, 0.7f, 0.4f, 0, 0, 0.22f, 0.60f, 0.30f, 0.004f, 0.3f, 0.4f, 1, 0.004f, 0.4f, 0.8f, 1, 0, 0, 0, 0, 0.18f, 2, 0.0f, 0.7f, 1, 5, 0, 0.00f, 0.85f},
 {"Vibrato Lead", 3, 1, 3, 1, -0.04f, 4, 1, 0.0f, 1, 0.8f, 0.75f, 0.5f, 0, 0, 0.55f, 0.30f, 0.45f, 0.01f, 0.5f, 0.9f, 1, 0.01f, 0.5f, 0.9f, 1, 0.05f, 0, 0, 0, 0.30f, 0, 0.25f, 0, 0, 4, 0, 0.00f, 0.8f},
 {"Sci-Fi Sweep", 3, 1, 2, 1, -0.20f, 2, 1, 0.12f, 1, 0.7f, 0.7f, 0.7f, 0, 0, 0.20f, 0.70f, 0.30f, 0.4f, 1.0f, 0.6f, 0, 0.3f, 1.0f, 0.9f, 1, 0, 0, 0, 0, 0.06f, 0, 0.0f, 0.8f, 0, 4, 0, 0.00f, 0.8f},
 {"Bright Pluck", 4, 1, 3, 1, -0.05f, 3, 1, 0.0f, 1, 0.8f, 0.6f, 0.5f, 0, 0, 0.60f, 0.35f, 0.70f, 0.002f, 0.12f, 0.0f, 1, 0.002f, 0.18f, 0.0f, 1, 0, 0, 0, 0, 0.30f, 0, 0, 0, 0, 4, 0, 0.00f, 0.8f},
 {"Detuned Stab", 3, 2, 3, 2, -0.5f, 2, 2, 0.5f, 1, 0.8f, 0.8f, 0.7f, 0, 0, 0.45f, 0.40f, 0.60f, 0.004f, 0.2f, 0.0f, 1, 0.004f, 0.25f, 0.3f, 1, 0, 0, 0, 0, 0.30f, 0, 0, 0, 0, 4, 0, 0.00f, 0.82f},
 {"Whistle", 5, 0, 4, 0, 0.0f, 4, 0, 0.0f, 1, 0.9f, 0.4f, 0.3f, 0, 0, 0.75f, 0.20f, 0.20f, 0.03f, 0.3f, 0.9f, 1, 0.02f, 0.4f, 0.95f, 1, 0.04f, 0, 0, 0, 0.25f, 0, 0.10f, 0, 0, 4, 0, 0.00f, 0.75f},
 {"Noise Sweep", 3, 1, 2, 1, 0.0f, 2, 1, 0.0f, 1, 0.3f, 0.3f, 0.2f, 0.9f, 0, 0.25f, 0.55f, 0.35f, 0.2f, 0.8f, 0.4f, 0, 0.2f, 0.9f, 0.8f, 1, 0, 0, 0, 0, 0.05f, 0, 0.0f, 0.6f, 0, 4, 0, 0.00f, 0.8f},
 {"Sync Lead",   3,1,3,1,0.0f,4,1,0.0f,1, 0.85f,0.8f,0.0f,0,0, 0.55f,0.30f,0.55f,0.006f,0.4f,0.7f,1, 0.006f,0.5f,0.9f,1, 0.05f,0,0,0, 0.30f,0,0.10f,0, 0,4,1,0.0f, 0.8f},
 {"FM Growl",    3,1,2,1,-0.1f,4,1,0.0f,0, 0.8f,0.6f,0.0f,0,0, 0.30f,0.55f,0.40f,0.01f,0.4f,0.3f,1, 0.006f,0.5f,0.7f,1, 0,0,0,0, 0.30f,0,0,0, 0,4,0,0.55f, 0.82f},
 /* --- expansion bank: 16 more, inspired by classic Model D patch vocabulary
  *     (Bass/Lead/Pad/Bell/FX categories seen across open-source Minimoog
  *     emulators such as stevebarakat/Minimoog) but authored fresh for this
  *     module's own DSP and parameter ranges. */
 {"Taurus Sub",    1,1,1,3,-0.05f,0,0,0.0f,1, 0.9f,0.6f,0.5f,0,0, 0.18f,0.30f,0.55f,0.005f,0.4f,0.6f,1, 0.005f,0.6f,0.85f,1, 0,0,0,0, 0.35f,0,0,0, 0,4,0,0.00f, 0.85f},
 {"Slap Bass",     3,1,3,3,-0.10f,2,0,0.08f,1, 0.8f,0.7f,0.3f,0,0, 0.35f,0.65f,0.80f,0.001f,0.10f,0.0f,1, 0.001f,0.15f,0.0f,0, 0,0,0,0, 0.35f,0,0,0, 0,4,0,0.00f, 0.85f},
 {"Dub Bass",      2,1,2,0,-0.06f,1,1,0.0f,1, 0.85f,0.6f,0.5f,0,0, 0.16f,0.35f,0.45f,0.02f,0.6f,0.5f,1, 0.01f,0.7f,0.75f,1, 0.05f,0,0,0, 0.10f,0,0,0.15f, 0,4,0,0.00f, 0.85f},
 {"Growl Bass",    3,1,3,1,-0.15f,2,0,0.0f,0, 0.8f,0.6f,0.0f,0,0, 0.22f,0.50f,0.60f,0.005f,0.3f,0.3f,1, 0.005f,0.35f,0.6f,1, 0,0,0,0, 0.35f,0,0,0, 0,4,0,0.40f, 0.85f},
 {"Warm Pad",      3,1,3,0,-0.10f,4,1,0.10f,1, 0.7f,0.6f,0.4f,0,0, 0.50f,0.20f,0.45f,0.6f,1.2f,0.7f,1, 0.7f,1.0f,0.85f,1, 0.1f,0,0,0, 0.15f,0,0.05f,0, 0,4,0,0.00f, 0.75f},
 {"Glass Pad",     4,0,5,1,0.15f,4,1,-0.12f,1, 0.6f,0.5f,0.5f,0,0, 0.75f,0.15f,0.35f,0.5f,1.5f,0.6f,1, 0.6f,1.3f,0.8f,1, 0.08f,0,0,0, 0.20f,0,0,0.12f, 0,4,0,0.00f, 0.70f},
 {"Cathedral Drone",0,1,1,0,-0.05f,2,1,0.07f,1, 0.6f,0.6f,0.4f,0.08f,1, 0.35f,0.25f,0.30f,1.5f,2.0f,0.6f,0, 1.2f,2.5f,0.9f,1, 0.3f,0,0,0, 0.02f,0,0,0.20f, 1,0,0,0.00f, 0.65f},
 {"Prog Solo",     4,1,4,1,-0.05f,5,0,0.06f,1, 0.85f,0.75f,0.4f,0,0, 0.60f,0.45f,0.60f,0.008f,0.45f,0.75f,1, 0.008f,0.5f,0.9f,1, 0.12f,0,0,0, 0.32f,0,0.15f,0.08f, 0,4,0,0.00f, 0.80f},
 {"Screamer Lead", 4,2,4,1,-0.04f,3,1,0.09f,1, 0.85f,0.7f,0.35f,0,0, 0.65f,0.85f,0.70f,0.003f,0.3f,0.65f,1, 0.003f,0.35f,0.85f,1, 0,0,0,0, 0.35f,0,0,0, 0,4,0,0.00f, 0.80f},
 {"Watery Lead",   3,0,3,1,-0.15f,4,0,0.10f,1, 0.7f,0.65f,0.4f,0,0, 0.45f,0.25f,0.40f,0.05f,0.6f,0.6f,1, 0.05f,0.7f,0.8f,1, 0.05f,0,0,0, 0.12f,0,0.10f,0.15f, 0,4,0,0.00f, 0.75f},
 {"Simple Lead",   3,1,3,1,-0.03f,4,1,0.0f,1, 0.85f,0.5f,0.15f,0,0, 0.50f,0.20f,0.50f,0.005f,0.4f,0.7f,1, 0.005f,0.45f,0.85f,1, 0,0,0,0, 0.35f,0,0,0, 0,4,0,0.00f, 0.80f},
 {"Crystal Bells", 4,0,5,1,0.5f,5,0,-0.7f,0, 0.6f,0.55f,0.0f,0,0, 0.80f,0.30f,0.60f,0.001f,0.4f,0.0f,1, 0.001f,0.9f,0.0f,0, 0,0,0,0, 0.35f,0,0,0, 0,4,0,0.35f, 0.75f},
 {"Wind Chimes",   4,0,5,0,0.35f,5,0,-0.4f,0, 0.5f,0.5f,0.0f,0.05f,0, 0.70f,0.20f,0.30f,0.01f,1.2f,0.0f,1, 0.01f,1.4f,0.0f,0, 0,0.3f,1,0, 0.08f,4,0.05f,0, 0,4,0,0.00f, 0.70f},
 {"Submarine Sonar",2,0,2,0,0.02f,3,1,0.0f,1, 0.7f,0.3f,0.0f,0,0, 0.30f,0.70f,0.50f,0.002f,0.5f,0.2f,0, 0.002f,1.0f,0.3f,0, 0,0,0,0, 0.06f,4,0,0.50f, 0,4,0,0.00f, 0.70f},
 {"Alien Signal",  4,2,4,2,0.30f,5,1,-0.20f,0, 0.7f,0.6f,0.0f,0,0, 0.40f,0.55f,0.65f,0.01f,0.5f,0.4f,0, 0.01f,0.6f,0.6f,1, 0,0,0,0, 0.40f,1,0.10f,0.10f, 0,4,2,0.30f, 0.75f},
 {"Cosmic Drone",  0,1,0,0,0.03f,1,1,-0.04f,1, 0.6f,0.5f,0.4f,0.15f,1, 0.28f,0.40f,0.50f,2.0f,3.0f,0.5f,0, 1.5f,3.5f,0.85f,1, 0.4f,0,0,0, 0.03f,0,0,0.35f, 1,1,0,0.00f, 0.60f},
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
    if (s->nheld < 8) s->held[s->nheld++] = note;
    s->note_freq = midi2hz((float)note);
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
    if (len < 3) { if (len < 1) return; }
    synth_t *s = (synth_t*)inst;
    uint8_t st = m[0] & 0xF0;
    if (st == 0x90 && m[2] > 0)      note_on(s, m[1], m[2]);
    else if (st == 0x80 || (st == 0x90 && m[2] == 0)) note_off(s, m[1]);
    else if (st == 0xE0){            /* pitch bend -> +-2 semis via mtune add */
        int bend = ((int)m[2] << 7 | m[1]) - 8192;
        s->p.mtune = (bend / 8192.0f) * 2.0f;
    }
}

/* ------------------------------------------------------------- params IO */
static int iparse(const char *v){ return (int)strtol(v, NULL, 10); }
static float fparse(const char *v){ return strtof(v, NULL); }

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
    return (int)strtol(v, NULL, 10);
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

static void set_param(void *inst, const char *k, const char *v){
    synth_t *s = (synth_t*)inst; params_t *p = &s->p;
    if      (!strcmp(k,"o1_range")){ p->o1_range=eparse(v,RANGE_NAMES,6); s->o1.range=p->o1_range; }
    else if (!strcmp(k,"o1_wave")){  p->o1_wave =eparse(v,WAVE_NAMES,5); s->o1.wave =p->o1_wave;  }
    else if (!strcmp(k,"o2_range")){ p->o2_range=eparse(v,RANGE_NAMES,6); s->o2.range=p->o2_range; }
    else if (!strcmp(k,"o2_wave")){  p->o2_wave =eparse(v,WAVE_NAMES,5); s->o2.wave =p->o2_wave;  }
    else if (!strcmp(k,"o2_tune"))   p->o2_tune=fparse(v);
    else if (!strcmp(k,"o3_range")){ p->o3_range=eparse(v,RANGE_NAMES,6); s->o3.range=p->o3_range; }
    else if (!strcmp(k,"o3_wave")){  p->o3_wave =eparse(v,WAVE_NAMES,5); s->o3.wave =p->o3_wave;  }
    else if (!strcmp(k,"o3_tune"))   p->o3_tune=fparse(v);
    else if (!strcmp(k,"o3_kbd"))    p->o3_kbd=eparse(v,ONOFF_NAMES,2);
    else if (!strcmp(k,"mix_o1"))    p->m1=fparse(v);
    else if (!strcmp(k,"mix_o2"))    p->m2=fparse(v);
    else if (!strcmp(k,"mix_o3"))    p->m3=fparse(v);
    else if (!strcmp(k,"mix_noise")) p->mnoise=fparse(v);
    else if (!strcmp(k,"noise_color")) p->noise_pink=eparse(v,COLOR_NAMES,2);
    else if (!strcmp(k,"cutoff"))    p->cutoff01=fparse(v);
    else if (!strcmp(k,"emphasis"))  p->emphasis=fparse(v);
    else if (!strcmp(k,"contour"))   p->contour=fparse(v);
    else if (!strcmp(k,"filt_a"))    p->f_a=fparse(v);
    else if (!strcmp(k,"filt_d"))    p->f_d=fparse(v);
    else if (!strcmp(k,"filt_s"))    p->f_s=fparse(v);
    else if (!strcmp(k,"kbd_track")) p->kbd_track=eparse(v,ONOFF_NAMES,2);
    else if (!strcmp(k,"loud_a"))    p->l_a=fparse(v);
    else if (!strcmp(k,"loud_d"))    p->l_d=fparse(v);
    else if (!strcmp(k,"loud_s"))    p->l_s=fparse(v);
    else if (!strcmp(k,"glide"))     p->glide=fparse(v);
    else if (!strcmp(k,"master_tune")) p->mtune=fparse(v);
    else if (!strcmp(k,"mod_mix"))   p->mod_mix=fparse(v);
    else if (!strcmp(k,"mod_osc"))   p->mod_osc_on=eparse(v,ONOFF_NAMES,2);
    else if (!strcmp(k,"mod_filter"))p->mod_filt_on=eparse(v,ONOFF_NAMES,2);
    else if (!strcmp(k,"decay_sw"))  p->decay_sw=eparse(v,ONOFF_NAMES,2);
    else if (!strcmp(k,"lfo_rate"))  p->lfo_rate=fparse(v);
    else if (!strcmp(k,"lfo_shape")) p->lfo_shape=eparse(v,LFO_SHAPE_NAMES,5);
    else if (!strcmp(k,"lfo_pitch")) p->lfo_pitch=fparse(v);
    else if (!strcmp(k,"lfo_filter"))p->lfo_filter=fparse(v);
    else if (!strcmp(k,"lfo_sync"))  p->lfo_sync=eparse(v,ONOFF_NAMES,2);
    else if (!strcmp(k,"lfo_div"))   p->lfo_div=eparse(v,LFO_DIV_NAMES,10);
    else if (!strcmp(k,"osc_sync"))  p->osc_sync=eparse(v,SYNC_NAMES,3);
    else if (!strcmp(k,"filt_fm"))   p->filt_fm=fparse(v);
    else if (!strcmp(k,"volume"))    p->volume=fparse(v);
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
    if      (!strcmp(k,"o1_range"))    i=p->o1_range;
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
    synth_t *s = (synth_t*)inst; params_t *p = &s->p;

    adsr_set(&s->fenv, p->f_a, p->f_d, p->f_s, p->decay_sw ? p->f_d : 0.008f);
    adsr_set(&s->aenv, p->l_a, p->l_d, p->l_s, p->decay_sw ? p->l_d : 0.008f);

    /* glide coefficient: p->glide in 0..1 -> ~0..1.5 s */
    float glide_t = p->glide * 1.5f;
    float glide_c = (glide_t <= 0.0005f) ? 1.0f : seg_coef(glide_t);

    float tune = powf(2.0f, p->mtune / 12.0f);

    for (int i = 0; i < frames; ++i){
        /* portamento toward target note */
        s->glide_freq += (s->note_freq - s->glide_freq) * glide_c;
        float base = s->glide_freq * tune;

        /* osc3 doubles as mod source when not keyboard-tracking */
        float o3freq = (p->o3_kbd ? base : 8.0f)
                       * range_mult(p->o3_range)
                       * powf(2.0f, p->o3_tune / 12.0f);
        float o3 = osc_render(&s->o3, o3freq, NULL);

        float white = frand(s);
        float noise = p->noise_pink ? pink_next(&s->pink, white) : white;

        /* dedicated LFO: free rate 0.05..40 Hz (log), or tempo-synced to host BPM */
        float lfo_hz;
        if (p->lfo_sync){
            float bpm = (s->host && s->host->get_bpm) ? s->host->get_bpm() : 120.0f;
            if (bpm < 1.0f) bpm = 120.0f;
            int d = p->lfo_div; if (d < 0) d = 0; if (d > 9) d = 9;
            lfo_hz = (bpm / 60.0f) / LFO_DIV_BEATS[d];
        } else {
            lfo_hz = 0.05f * powf(800.0f, clampf(p->lfo_rate, 0.0f, 1.0f));
        }
        float lfo = lfo_tick(&s->lfo, lfo_hz, p->lfo_shape);

        /* modulation mix: blend osc3 <-> noise, routed to pitch/filter */
        float modsig = lerpf(o3, noise, clampf(p->mod_mix, 0.0f, 1.0f));
        float pitch_mod = (p->mod_osc_on  ? (modsig * 0.06f) : 0.0f)   /* +-~7% */
                        + p->lfo_pitch * lfo * 0.06f;                  /* vibrato */
        float filt_mod  = (p->mod_filt_on ? (modsig)          : 0.0f)
                        + p->lfo_filter * lfo;                         /* filter LFO */

        float pm = powf(2.0f, pitch_mod);
        float o1f = base * range_mult(p->o1_range) * pm;
        float o2f = base * range_mult(p->o2_range) * powf(2.0f, p->o2_tune/12.0f) * pm;
        int o1w = 0;
        float o1 = osc_render(&s->o1, o1f, &o1w);
        if (p->osc_sync && o1w){          /* hard sync: master osc1 resets slaves */
            s->o2.phase = 0.0f;
            if (p->osc_sync == 2) s->o3.phase = 0.0f;
        }
        float o2 = osc_render(&s->o2, o2f, NULL);

        float mix = o1*p->m1 + o2*p->m2 + o3*p->m3 + noise*p->mnoise;
        mix *= 0.5f;

        /* filter cutoff: base + keytrack + envelope + mod, in log-ish Hz */
        float fenv = adsr_tick(&s->fenv);
        float track = p->kbd_track ? (log2f(base/220.0f) * 0.5f) : 0.0f;
        float cut_norm = p->cutoff01 + track
                       + p->contour * fenv
                       + filt_mod * 0.4f
                       + p->filt_fm * o3 * 0.6f;   /* audio-rate filter FM (osc3) */
        cut_norm = clampf(cut_norm, 0.0f, 1.2f);
        float cut_hz = 20.0f * powf(1000.0f, clampf(cut_norm,0.0f,1.0f)); /* 20..20k */
        ladder_set(&s->lpf, cut_hz, p->emphasis);
        float filtered = ladder_process(&s->lpf, mix);

        float amp = adsr_tick(&s->aenv) * (0.4f + 0.6f * s->velocity);
        float outv = clampf(filtered * amp * p->volume, -1.0f, 1.0f);

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
