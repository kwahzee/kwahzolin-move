/*
 * kwahzolin — Chaotic rungler synthesizer for Ableton Move
 *
 * Signal chain:
 *   Osc1 (triangle, freq modulated by rungler CV + chaos)  → ★ soft clip
 *   Osc2 (triangle, free-running; clocks the rungler)      → ★ soft clip
 *   XOR-style mix of Osc1 + Osc2                           → ★ soft clip
 *   Ring mod: Osc1 × Osc2 mixed in via Ring Mod knob       → ★ soft clip
 *   Drive: tanh saturation before filter                   → ★ drive clip
 *   SVF lowpass (high resonance, self-oscillation capable) → ★ BP limiting
 *   Sequencer gate (16-step, MIDI-clock or Osc2 fallback)
 *   Final output                                           → ★ output clip
 *
 * Loop system:
 *   JS sends loop_active and loop_beats.
 *   DSP reads host->get_bpm() each block to compute loop_samples.
 *   On loop_active 0→1: snapshot current shift register state.
 *   Per-sample: when loop_counter >= loop_samples, reset shift_reg to snapshot.
 *
 * Sequencer gate:
 *   Primary: count MIDI 0xF8 ticks; 24 ticks = one beat = advance step.
 *   Fallback (no MIDI clock): advance step every FALLBACK_CROSSINGS Osc2 cycles.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "include/host/plugin_api_v1.h"

/* ====================================================================
 * Constants
 * ==================================================================== */

#define SAMPLE_RATE    44100.0f
#define INV_SR         (1.0f / 44100.0f)
#define ONE_PI         3.14159265359f

/* Oscillator frequency mapping: p in [0,1] → 0.5 Hz to 5000 Hz (exponential) */
#define OSC_FREQ_BASE  0.5f
#define OSC_FREQ_TOP   10000.0f   /* powf(OSC_FREQ_TOP, 1.0) * OSC_FREQ_BASE = 5000 */

/* Filter cutoff mapping: p in [0,1] → 20 Hz to 20000 Hz (exponential) */
#define CUTOFF_BASE    20.0f
#define CUTOFF_RATIO   1000.0f    /* 20 * 1000 = 20000 */

/* Clock detection: samples of silence before fallback to Osc2-based gate */
#define CLOCK_TIMEOUT_SAMPLES  (44100 * 2)

/* Osc2 positive zero-crossings per step advance in fallback mode */
#define FALLBACK_CROSSINGS  4

/* ====================================================================
 * Utilities
 * ==================================================================== */

static inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

static inline float soft_clip(float x) {
    return tanhf(x);
}

/* Exponential parameter → oscillator frequency */
static inline float param_to_osc_freq(float p) {
    return OSC_FREQ_BASE * powf(OSC_FREQ_TOP, p);
}

/* Exponential parameter → filter cutoff Hz */
static inline float param_to_cutoff(float p) {
    return CUTOFF_BASE * powf(CUTOFF_RATIO, p);
}

/* SVF f-coefficient from cutoff Hz */
static inline float cutoff_to_svf_f(float fc) {
    fc = clampf(fc, 20.0f, 18000.0f);
    float f = 2.0f * sinf(ONE_PI * fc * INV_SR);
    return clampf(f, 0.001f, 1.85f);
}

/* ====================================================================
 * Instance state
 * ==================================================================== */

typedef struct {
    /* Oscillator phases [0, 1) */
    float osc1_phase;
    float osc2_phase;
    float osc2_prev;    /* previous Osc2 output, for zero-crossing detection */

    /* Rungler 8-bit shift register */
    uint8_t shift_reg;
    float   rungler_cv;     /* shift_reg / 255.0f, in [0, 1] */

    /* Loop */
    int     loop_active;    /* 0 = free, 1 = looping */
    int     loop_beats;     /* 1–32 */
    uint8_t loop_snapshot;  /* shift_reg captured when loop was armed */
    double  loop_counter;   /* sample counter within current loop cycle */

    /* Sequencer gate */
    uint16_t step_mask;         /* 16-bit: bit i = step i enabled */
    int      current_step;      /* 0–15 */
    int      clock_ticks;       /* 0xF8 counter within one beat (0–23) */
    int      samples_no_clock;  /* samples since last 0xF8 (clock-loss detection) */
    int      osc2_cross_count;  /* Osc2 crossings since last gate step (fallback) */
    int      gate_on;           /* 1 = audio passes */

    /* SVF state and cached coefficient */
    float svf_lp;
    float svf_bp;
    float svf_f;            /* cached f-coeff, updated on each rungler shift */

    /* Parameters [0, 1] */
    float p_osc1_rate;
    float p_osc2_rate;
    float p_chaos;
    float p_cutoff;
    float p_resonance;
    float p_drive;
    float p_rungler_mod;
    float p_ring_mod;

    /* Host API */
    const host_api_v1_t *host;

} kwahzolin_t;

/* ====================================================================
 * Global host reference (set at init, used by create_instance)
 * ==================================================================== */

static const host_api_v1_t *g_host = NULL;

/* ====================================================================
 * Helper: recompute SVF f-coefficient from current parameters
 * Called whenever rungler_cv changes (Osc2 zero-crossing).
 * ==================================================================== */

static inline void update_svf_f(kwahzolin_t *k) {
    float cp  = clampf(k->p_cutoff + k->p_rungler_mod * k->rungler_cv * 0.45f, 0.01f, 0.99f);
    float fc  = param_to_cutoff(cp);
    k->svf_f  = cutoff_to_svf_f(fc);
}

/* ====================================================================
 * Plugin API v2 — lifecycle
 * ==================================================================== */

static void *kwahzolin_create(const char *module_dir, const char *json_defaults) {
    (void)module_dir; (void)json_defaults;

    kwahzolin_t *k = (kwahzolin_t *)calloc(1, sizeof(kwahzolin_t));
    if (!k) return NULL;

    /* Defaults: a useful starting point — slow rungler, medium chaos */
    k->p_osc1_rate   = 0.20f;   /* ~8 Hz */
    k->p_osc2_rate   = 0.15f;   /* ~5 Hz — clocks rungler slowly */
    k->p_chaos       = 0.50f;
    k->p_cutoff      = 0.50f;   /* ~630 Hz */
    k->p_resonance   = 0.30f;
    k->p_drive       = 0.20f;
    k->p_rungler_mod = 0.50f;
    k->p_ring_mod    = 0.00f;

    k->shift_reg   = 0xA5;      /* 1010 0101 — interesting starting pattern */
    k->rungler_cv  = k->shift_reg / 255.0f;
    k->step_mask   = 0xFFFF;    /* all 16 steps on */
    k->gate_on     = 1;
    k->loop_beats  = 4;

    /* Start in fallback mode so sound is immediate even without MIDI clock */
    k->samples_no_clock = CLOCK_TIMEOUT_SAMPLES + 1;

    k->host = g_host;
    update_svf_f(k);
    return k;
}

static void kwahzolin_destroy(void *inst) {
    free(inst);
}

/* ====================================================================
 * Plugin API v2 — MIDI
 * ==================================================================== */

static void kwahzolin_on_midi(void *inst, const uint8_t *msg, int len, int source) {
    kwahzolin_t *k = (kwahzolin_t *)inst;
    if (!k || len < 1) return;
    (void)source;

    /* MIDI Timing Clock */
    if (msg[0] == 0xF8) {
        k->samples_no_clock = 0;
        k->clock_ticks++;
        if (k->clock_ticks >= 24) {
            k->clock_ticks = 0;
            k->current_step = (k->current_step + 1) & 15;
            k->gate_on      = (k->step_mask >> k->current_step) & 1;
        }
        return;
    }

    /* MIDI Start / Continue: reset step counter */
    if (msg[0] == 0xFA || msg[0] == 0xFB) {
        k->clock_ticks   = 0;
        k->current_step  = 0;
        k->gate_on       = (k->step_mask >> 0) & 1;
        return;
    }

    /* MIDI Stop */
    if (msg[0] == 0xFC) {
        k->clock_ticks = 0;
        return;
    }

    /* Note-on / note-off: handled entirely in JS */
}

/* ====================================================================
 * Plugin API v2 — parameters
 * ==================================================================== */

static void kwahzolin_set_param(void *inst, const char *key, const char *val) {
    kwahzolin_t *k = (kwahzolin_t *)inst;
    if (!k || !key || !val) return;

    float f = (float)atof(val);
    int   v = atoi(val);

    if      (!strcmp(key, "osc1_rate"))   k->p_osc1_rate   = clampf(f, 0.0f, 1.0f);
    else if (!strcmp(key, "osc2_rate"))   k->p_osc2_rate   = clampf(f, 0.0f, 1.0f);
    else if (!strcmp(key, "chaos"))       k->p_chaos       = clampf(f, 0.0f, 1.0f);
    else if (!strcmp(key, "cutoff")) {
        k->p_cutoff = clampf(f, 0.0f, 1.0f);
        update_svf_f(k);
    }
    else if (!strcmp(key, "resonance"))   k->p_resonance   = clampf(f, 0.0f, 1.0f);
    else if (!strcmp(key, "drive"))       k->p_drive       = clampf(f, 0.0f, 1.0f);
    else if (!strcmp(key, "rungler_mod")) {
        k->p_rungler_mod = clampf(f, 0.0f, 1.0f);
        update_svf_f(k);
    }
    else if (!strcmp(key, "ring_mod"))    k->p_ring_mod    = clampf(f, 0.0f, 1.0f);
    else if (!strcmp(key, "loop_active")) {
        int new_active = (v != 0) ? 1 : 0;
        if (new_active && !k->loop_active) {
            /* 0 → 1 transition: snapshot current shift register */
            k->loop_snapshot = k->shift_reg;
            k->loop_counter  = 0.0;
        }
        k->loop_active = new_active;
    }
    else if (!strcmp(key, "loop_beats")) {
        int beats = (v < 1) ? 1 : (v > 32) ? 32 : v;
        k->loop_beats = beats;
        if (k->loop_active) {
            /* Re-snapshot on length change for clean re-arm */
            k->loop_snapshot = k->shift_reg;
            k->loop_counter  = 0.0;
        }
    }
    else if (!strcmp(key, "step_mask")) {
        k->step_mask = (uint16_t)(v & 0xFFFF);
        k->gate_on   = (k->step_mask >> k->current_step) & 1;
    }
}

static int kwahzolin_get_param(void *inst, const char *key, char *buf, int buf_len) {
    kwahzolin_t *k = (kwahzolin_t *)inst;
    if (!k || !key || !buf || buf_len < 2) return -1;

    if (!strcmp(key, "osc1_rate"))   return snprintf(buf, buf_len, "%.4f", k->p_osc1_rate);
    if (!strcmp(key, "osc2_rate"))   return snprintf(buf, buf_len, "%.4f", k->p_osc2_rate);
    if (!strcmp(key, "chaos"))       return snprintf(buf, buf_len, "%.4f", k->p_chaos);
    if (!strcmp(key, "cutoff"))      return snprintf(buf, buf_len, "%.4f", k->p_cutoff);
    if (!strcmp(key, "resonance"))   return snprintf(buf, buf_len, "%.4f", k->p_resonance);
    if (!strcmp(key, "drive"))       return snprintf(buf, buf_len, "%.4f", k->p_drive);
    if (!strcmp(key, "rungler_mod")) return snprintf(buf, buf_len, "%.4f", k->p_rungler_mod);
    if (!strcmp(key, "ring_mod"))    return snprintf(buf, buf_len, "%.4f", k->p_ring_mod);
    if (!strcmp(key, "loop_active")) return snprintf(buf, buf_len, "%d",   k->loop_active);
    if (!strcmp(key, "loop_beats"))  return snprintf(buf, buf_len, "%d",   k->loop_beats);
    if (!strcmp(key, "step_mask"))   return snprintf(buf, buf_len, "%u",   (unsigned)k->step_mask);

    if (!strcmp(key, "chain_params")) {
        /* Parameter metadata for Signal Chain Shadow UI */
        static const char *cp =
            "["
            "{\"key\":\"osc1_rate\",\"name\":\"Osc1 Rate\","
                "\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01,\"default\":0.2},"
            "{\"key\":\"osc2_rate\",\"name\":\"Osc2 Rate\","
                "\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01,\"default\":0.15},"
            "{\"key\":\"chaos\",\"name\":\"Chaos\","
                "\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01,\"default\":0.5},"
            "{\"key\":\"cutoff\",\"name\":\"Cutoff\","
                "\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01,\"default\":0.5},"
            "{\"key\":\"resonance\",\"name\":\"Resonance\","
                "\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01,\"default\":0.3},"
            "{\"key\":\"drive\",\"name\":\"Drive\","
                "\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01,\"default\":0.2},"
            "{\"key\":\"rungler_mod\",\"name\":\"Rungler Mod\","
                "\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01,\"default\":0.5},"
            "{\"key\":\"ring_mod\",\"name\":\"Ring Mod\","
                "\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01,\"default\":0.0}"
            "]";
        int n = (int)strlen(cp);
        if (n >= buf_len) n = buf_len - 1;
        memcpy(buf, cp, n);
        buf[n] = '\0';
        return n;
    }

    return -1;
}

static int kwahzolin_get_error(void *inst, char *buf, int buf_len) {
    (void)inst;
    if (buf && buf_len > 0) buf[0] = '\0';
    return 0;
}

/* ====================================================================
 * Plugin API v2 — audio rendering
 * ==================================================================== */

static void kwahzolin_render_block(void *inst, int16_t *out_lr, int frames) {
    kwahzolin_t *k = (kwahzolin_t *)inst;
    if (!k) { memset(out_lr, 0, frames * 4); return; }

    /* ---- Per-block constants ---------------------------------------- */

    /* Compute loop_samples from host BPM each block (auto-follows tempo) */
    double loop_samples = 0.0;
    if (k->loop_active && k->loop_beats > 0) {
        float bpm = 120.0f;
        if (k->host && k->host->get_bpm) {
            float hbpm = k->host->get_bpm();
            if (hbpm >= 20.0f && hbpm <= 300.0f) bpm = hbpm;
        }
        loop_samples = (60.0 / (double)bpm) * (double)k->loop_beats * 44100.0;
    }

    /* Osc2 increment — Osc2 freq is stable within a block */
    float osc2_freq = param_to_osc_freq(k->p_osc2_rate);
    float osc2_inc  = osc2_freq * INV_SR;

    /* Osc1 base frequency — modulated per-sample by rungler CV */
    float osc1_base_freq  = param_to_osc_freq(k->p_osc1_rate);
    float osc1_chaos_gain = k->p_chaos * 1.8f;  /* rungler depth on osc1 freq */

    /* Drive gain: 0 → 1x, 1 → 20x */
    float drive_gain = 1.0f + k->p_drive * 19.0f;

    /* SVF damping: resonance=0 → 2.0 (safe), resonance≈1 → 0.02 (screaming) */
    float damping = 2.0f * (1.0f - k->p_resonance * 0.99f);
    if (damping < 0.02f) damping = 0.02f;

    /* Update clock-loss counter */
    k->samples_no_clock += frames;
    int clock_present = (k->samples_no_clock < CLOCK_TIMEOUT_SAMPLES);

    /* ---- Per-sample loop -------------------------------------------- */

    for (int i = 0; i < frames; i++) {

        /* Osc1 frequency: base + rungler modulation */
        float osc1_freq = osc1_base_freq * (1.0f + osc1_chaos_gain * k->rungler_cv);
        osc1_freq = clampf(osc1_freq, 0.1f, 20000.0f);
        float osc1_inc = osc1_freq * INV_SR;

        /* --- Osc1: triangle wave → ★ soft clip --- */
        float o1 = (k->osc1_phase < 0.5f)
            ? (4.0f * k->osc1_phase - 1.0f)
            : (3.0f - 4.0f * k->osc1_phase);
        o1 = soft_clip(o1);

        /* --- Osc2: triangle wave → ★ soft clip --- */
        float o2 = (k->osc2_phase < 0.5f)
            ? (4.0f * k->osc2_phase - 1.0f)
            : (3.0f - 4.0f * k->osc2_phase);
        o2 = soft_clip(o2);

        /* --- Rungler: clock on Osc2 positive zero-crossing --- */
        if (k->osc2_prev < 0.0f && o2 >= 0.0f) {
            /* New bit from Osc1's sign */
            uint8_t new_bit = (o1 >= 0.0f) ? 1 : 0;
            k->shift_reg    = ((k->shift_reg << 1) | new_bit) & 0xFF;
            k->rungler_cv   = k->shift_reg / 255.0f;

            /* Recompute SVF coefficient with new rungler CV */
            update_svf_f(k);

            /* Fallback gate advance (no MIDI clock) */
            if (!clock_present) {
                k->osc2_cross_count++;
                if (k->osc2_cross_count >= FALLBACK_CROSSINGS) {
                    k->osc2_cross_count = 0;
                    k->current_step = (k->current_step + 1) & 15;
                    k->gate_on      = (k->step_mask >> k->current_step) & 1;
                }
            }
        }
        k->osc2_prev = o2;

        /* --- Loop: reset shift register at loop boundary --- */
        if (k->loop_active && loop_samples > 0.0) {
            k->loop_counter += 1.0;
            if (k->loop_counter >= loop_samples) {
                k->loop_counter -= loop_samples;
                k->shift_reg    = k->loop_snapshot;
                k->rungler_cv   = k->shift_reg / 255.0f;
                update_svf_f(k);
            }
        }

        /* --- Mix Osc1 + Osc2 → ★ soft clip --- */
        float sig = o1 * 0.6f + o2 * 0.4f;
        sig = soft_clip(sig);

        /* --- Ring mod: Osc1 × Osc2 blended by knob → ★ soft clip --- */
        if (k->p_ring_mod > 0.001f) {
            float ring = o1 * o2;
            sig = sig * (1.0f - k->p_ring_mod) + ring * k->p_ring_mod;
            sig = soft_clip(sig);
        }

        /* --- Drive → ★ saturation --- */
        float driven = tanhf(sig * drive_gain);

        /* --- SVF lowpass (Chamberlin) --- */
        float hp     = driven - damping * k->svf_bp - k->svf_lp;
        float new_bp = k->svf_f * hp + k->svf_bp;
        /* ★ Limit BP to prevent DC runaway at extreme resonance;
           tanh still allows the filter to scream/self-oscillate */
        new_bp    = clampf(tanhf(new_bp * 0.5f) * 2.0f, -3.0f, 3.0f);
        float new_lp = k->svf_f * new_bp + k->svf_lp;
        k->svf_bp = new_bp;
        k->svf_lp = new_lp;

        /* ★ Filter output clip */
        float filtered = tanhf(k->svf_lp * 0.6f);

        /* --- Sequencer gate --- */
        float gated = k->gate_on ? filtered : 0.0f;

        /* ★ Final output soft clip + level */
        float out = tanhf(gated * 0.8f);

        /* Scale to int16 with headroom */
        int32_t s = (int32_t)(out * 25000.0f);
        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;

        out_lr[i * 2]     = (int16_t)s;  /* L */
        out_lr[i * 2 + 1] = (int16_t)s;  /* R */

        /* Advance phases */
        k->osc1_phase += osc1_inc;
        if (k->osc1_phase >= 1.0f) k->osc1_phase -= 1.0f;

        k->osc2_phase += osc2_inc;
        if (k->osc2_phase >= 1.0f) k->osc2_phase -= 1.0f;
    }
}

/* ====================================================================
 * Plugin API v2 export
 * ==================================================================== */

static plugin_api_v2_t s_plugin = {
    .api_version      = MOVE_PLUGIN_API_VERSION_2,
    .create_instance  = kwahzolin_create,
    .destroy_instance = kwahzolin_destroy,
    .on_midi          = kwahzolin_on_midi,
    .set_param        = kwahzolin_set_param,
    .get_param        = kwahzolin_get_param,
    .get_error        = kwahzolin_get_error,
    .render_block     = kwahzolin_render_block,
};

plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host) {
    g_host = host;
    return &s_plugin;
}
