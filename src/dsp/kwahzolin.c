/*
 * kwahzolin — Benjolin-style chaotic synthesizer for Ableton Move
 *
 * Signal chain:
 *   Osc1 (triangle, freq modulated by rungler CV × Osc Chaos)  → soft clip
 *   Osc2 (triangle, free-running; clocks the rungler)          → soft clip
 *   XOR-style mix of Osc1 + Osc2                               → soft clip
 *   Ring mod: Osc1 × Osc2 blended via Ring Modulation knob (true bipolar)
 *   Chamberlin SVF: feedback = Q*band; hp = driven - low - feedback;
 *                   band += F*hp; low += F*band; output = tanh(low)
 *   Sequencer gate (16-step, MIDI-clock or Osc2 fallback)
 *   Final output clip
 *
 * Loop system:
 *   Loop length is measured in Osc2 zero-crossing events (rungler clocks),
 *   not in samples or beats. This ties the loop boundary to the oscillator
 *   speed, producing authentic Benjolin loop behavior.
 *   JS sends loop_active and loop_beats (now meaning: N Osc2 crossings).
 *   On crossing count reaching loop_beats: reset shift_reg to snapshot.
 *
 * IIR parameter smoothing:
 *   All 8 knob parameters are smoothed at block rate to eliminate clicks
 *   from parameter changes. The filter coefficient is recomputed abruptly
 *   on each rungler zero-crossing — this abrupt jump IS the Benjolin ping.
 *
 * Sequencer gate:
 *   Primary:  count MIDI 0xF8 ticks; 24 ticks = one beat = advance step.
 *   Fallback: advance step every FALLBACK_CROSSINGS Osc2 crossings.
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

/* Oscillator frequency mapping: p in [0,1] → 0.5 Hz to 5000 Hz */
#define OSC_FREQ_BASE  0.5f
#define OSC_FREQ_TOP   10000.0f

/* Filter cutoff mapping: p in [0,1] → 20 Hz to 20000 Hz */
#define CUTOFF_BASE    20.0f
#define CUTOFF_RATIO   1000.0f

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

static inline float param_to_osc_freq(float p) {
    return OSC_FREQ_BASE * powf(OSC_FREQ_TOP, p);
}

static inline float param_to_cutoff(float p) {
    return CUTOFF_BASE * powf(CUTOFF_RATIO, p);
}

/* One-pole integrator coefficient for Chamberlin-style filter */
static inline float cutoff_to_filt_f(float fc) {
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
    float osc2_prev;    /* previous Osc2 sample for zero-crossing detection */

    /* Rungler 8-bit shift register */
    uint8_t shift_reg;
    float   rungler_cv;     /* shift_reg / 255.0f, in [0, 1] */

    /* Loop (Osc2 crossing-based) */
    int     loop_active;
    int     loop_beats;          /* loop length in Osc2 crossings */
    uint8_t loop_snapshot;       /* shift_reg captured when loop was armed */
    int     loop_cross_count;    /* crossings elapsed since last loop reset */

    /* Sequencer gate */
    uint16_t step_mask;
    int      current_step;
    int      clock_ticks;
    int      samples_no_clock;
    int      osc2_cross_count;
    int      gate_on;

    /* Chamberlin SVF filter state */
    float filt_low;      /* lowpass integrator */
    float filt_band;     /* bandpass integrator (resonance source) */
    float filt_f;        /* cached integrator coefficient, updated at crossings */

    /* Target parameters [0, 1] — written by set_param */
    float p_osc1_rate;
    float p_osc2_rate;
    float p_osc_chaos;
    float p_filter_cutoff;
    float p_filter_resonance;
    float p_filter_chaos;
    float p_filter_drive;
    float p_ring_mod;

    /* IIR-smoothed parameters — updated per-block in render_block */
    float s_osc1_rate;
    float s_osc2_rate;
    float s_osc_chaos;
    float s_filter_cutoff;
    float s_filter_resonance;
    float s_filter_chaos;
    float s_filter_drive;
    float s_ring_mod;

    /* Block-rate IIR smoothing coefficients (precomputed at create time)
     * coeff = 1 - exp(-FRAMES_PER_BLOCK / (SAMPLE_RATE * T_seconds)) */
    float sm_fast;   /* ~5ms  */
    float sm_med;    /* ~10ms */
    float sm_slow;   /* ~15ms */
    float sm_vslow;  /* ~20ms */

    /* Host API */
    const host_api_v1_t *host;

} kwahzolin_t;

/* ====================================================================
 * Global host reference
 * ==================================================================== */

static const host_api_v1_t *g_host = NULL;

/* ====================================================================
 * Helper: recompute filter coefficient from smoothed params + rungler CV
 *
 * Called at the start of each render block (knob changes) and on every
 * Osc2 zero-crossing (rungler step). The abrupt update on crossings is
 * intentional — it makes the filter ping at the new cutoff frequency.
 * ==================================================================== */

static inline void update_filt_f(kwahzolin_t *k) {
    float cp = clampf(k->s_filter_cutoff + k->s_filter_chaos * k->rungler_cv * 0.45f,
                      0.01f, 0.99f);
    float fc = param_to_cutoff(cp);
    k->filt_f = cutoff_to_filt_f(fc);
}

/* ====================================================================
 * Plugin API v2 — lifecycle
 * ==================================================================== */

static void *kwahzolin_create(const char *module_dir, const char *json_defaults) {
    (void)module_dir; (void)json_defaults;

    kwahzolin_t *k = (kwahzolin_t *)calloc(1, sizeof(kwahzolin_t));
    if (!k) return NULL;

    /* Precompute block-rate IIR smoothing coefficients */
    k->sm_fast  = 1.0f - expf(-(float)MOVE_FRAMES_PER_BLOCK / (SAMPLE_RATE * 0.005f));
    k->sm_med   = 1.0f - expf(-(float)MOVE_FRAMES_PER_BLOCK / (SAMPLE_RATE * 0.010f));
    k->sm_slow  = 1.0f - expf(-(float)MOVE_FRAMES_PER_BLOCK / (SAMPLE_RATE * 0.015f));
    k->sm_vslow = 1.0f - expf(-(float)MOVE_FRAMES_PER_BLOCK / (SAMPLE_RATE * 0.020f));

    /* Parameter defaults */
    k->p_osc1_rate        = 0.20f;
    k->p_osc2_rate        = 0.15f;
    k->p_osc_chaos        = 0.50f;
    k->p_filter_cutoff    = 0.50f;
    k->p_filter_resonance = 0.30f;
    k->p_filter_chaos     = 0.50f;
    k->p_filter_drive     = 0.20f;
    k->p_ring_mod         = 0.00f;

    /* Initialize smoothed values to match targets — no startup ramp */
    k->s_osc1_rate        = k->p_osc1_rate;
    k->s_osc2_rate        = k->p_osc2_rate;
    k->s_osc_chaos        = k->p_osc_chaos;
    k->s_filter_cutoff    = k->p_filter_cutoff;
    k->s_filter_resonance = k->p_filter_resonance;
    k->s_filter_chaos     = k->p_filter_chaos;
    k->s_filter_drive     = k->p_filter_drive;
    k->s_ring_mod         = k->p_ring_mod;

    k->shift_reg         = 0xA5;
    k->rungler_cv        = k->shift_reg / 255.0f;
    k->step_mask         = 0xFFFF;
    k->gate_on           = 1;
    k->loop_beats        = 4;

    /* Start in fallback mode so sound is immediate without MIDI clock */
    k->samples_no_clock  = CLOCK_TIMEOUT_SAMPLES + 1;

    k->host = g_host;
    update_filt_f(k);
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

    if (msg[0] == 0xF8) {
        k->samples_no_clock = 0;
        k->clock_ticks++;
        if (k->clock_ticks >= 24) {
            k->clock_ticks  = 0;
            k->current_step = (k->current_step + 1) & 15;
            k->gate_on      = (k->step_mask >> k->current_step) & 1;
        }
        return;
    }

    if (msg[0] == 0xFA || msg[0] == 0xFB) {
        k->clock_ticks  = 0;
        k->current_step = 0;
        k->gate_on      = (k->step_mask >> 0) & 1;
        return;
    }

    if (msg[0] == 0xFC) {
        k->clock_ticks = 0;
        return;
    }
}

/* ====================================================================
 * Plugin API v2 — parameters
 * ==================================================================== */

static void kwahzolin_set_param(void *inst, const char *key, const char *val) {
    kwahzolin_t *k = (kwahzolin_t *)inst;
    if (!k || !key || !val) return;

    float f = (float)atof(val);
    int   v = atoi(val);

    if      (!strcmp(key, "osc1_rate"))          k->p_osc1_rate        = clampf(f, 0.0f, 1.0f);
    else if (!strcmp(key, "osc2_rate"))          k->p_osc2_rate        = clampf(f, 0.0f, 1.0f);
    else if (!strcmp(key, "osc_chaos"))          k->p_osc_chaos        = clampf(f, 0.0f, 1.0f);
    else if (!strcmp(key, "filter_cutoff"))      k->p_filter_cutoff    = clampf(f, 0.0f, 1.0f);
    else if (!strcmp(key, "filter_resonance"))   k->p_filter_resonance = clampf(f, 0.0f, 1.0f);
    else if (!strcmp(key, "filter_chaos"))       k->p_filter_chaos     = clampf(f, 0.0f, 1.0f);
    else if (!strcmp(key, "filter_drive"))       k->p_filter_drive     = clampf(f, 0.0f, 1.0f);
    else if (!strcmp(key, "ring_mod"))           k->p_ring_mod         = clampf(f, 0.0f, 1.0f);
    else if (!strcmp(key, "loop_active")) {
        int new_active = (v != 0) ? 1 : 0;
        if (new_active && !k->loop_active) {
            k->loop_snapshot    = k->shift_reg;
            k->loop_cross_count = 0;
        }
        k->loop_active = new_active;
    }
    else if (!strcmp(key, "loop_beats")) {
        int beats = (v < 1) ? 1 : (v > 32) ? 32 : v;
        k->loop_beats = beats;
        if (k->loop_active) {
            k->loop_snapshot    = k->shift_reg;
            k->loop_cross_count = 0;
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

    if (!strcmp(key, "osc1_rate"))        return snprintf(buf, buf_len, "%.4f", k->p_osc1_rate);
    if (!strcmp(key, "osc2_rate"))        return snprintf(buf, buf_len, "%.4f", k->p_osc2_rate);
    if (!strcmp(key, "osc_chaos"))        return snprintf(buf, buf_len, "%.4f", k->p_osc_chaos);
    if (!strcmp(key, "filter_cutoff"))    return snprintf(buf, buf_len, "%.4f", k->p_filter_cutoff);
    if (!strcmp(key, "filter_resonance")) return snprintf(buf, buf_len, "%.4f", k->p_filter_resonance);
    if (!strcmp(key, "filter_chaos"))     return snprintf(buf, buf_len, "%.4f", k->p_filter_chaos);
    if (!strcmp(key, "filter_drive"))     return snprintf(buf, buf_len, "%.4f", k->p_filter_drive);
    if (!strcmp(key, "ring_mod"))         return snprintf(buf, buf_len, "%.4f", k->p_ring_mod);
    if (!strcmp(key, "loop_active"))      return snprintf(buf, buf_len, "%d",   k->loop_active);
    if (!strcmp(key, "loop_beats"))       return snprintf(buf, buf_len, "%d",   k->loop_beats);
    if (!strcmp(key, "step_mask"))        return snprintf(buf, buf_len, "%u",   (unsigned)k->step_mask);

    if (!strcmp(key, "chain_params")) {
        static const char *cp =
            "["
            "{\"key\":\"osc1_rate\",\"name\":\"Osc 1 Frequency\","
                "\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01,\"default\":0.2},"
            "{\"key\":\"osc2_rate\",\"name\":\"Osc 2 Frequency\","
                "\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01,\"default\":0.15},"
            "{\"key\":\"osc_chaos\",\"name\":\"Osc Chaos\","
                "\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01,\"default\":0.5},"
            "{\"key\":\"filter_cutoff\",\"name\":\"Filter Cutoff\","
                "\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01,\"default\":0.5},"
            "{\"key\":\"filter_resonance\",\"name\":\"Filter Resonance\","
                "\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01,\"default\":0.3},"
            "{\"key\":\"filter_chaos\",\"name\":\"Filter Chaos\","
                "\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01,\"default\":0.5},"
            "{\"key\":\"filter_drive\",\"name\":\"Filter Drive\","
                "\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01,\"default\":0.2},"
            "{\"key\":\"ring_mod\",\"name\":\"Ring Modulation\","
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

    /* ---- Per-block IIR parameter smoothing ----------------------------
     * Each target param is chased by its smoothed counterpart once per block.
     * This eliminates clicks from knob turns without affecting the rungler-
     * driven cutoff jumps, which are updated per-crossing and produce the
     * characteristic Benjolin ping. */
    k->s_osc1_rate        += (k->p_osc1_rate        - k->s_osc1_rate)        * k->sm_slow;
    k->s_osc2_rate        += (k->p_osc2_rate        - k->s_osc2_rate)        * k->sm_slow;
    k->s_osc_chaos        += (k->p_osc_chaos        - k->s_osc_chaos)        * k->sm_slow;
    k->s_filter_cutoff    += (k->p_filter_cutoff    - k->s_filter_cutoff)    * k->sm_slow;
    k->s_filter_resonance += (k->p_filter_resonance - k->s_filter_resonance) * k->sm_slow;
    k->s_filter_chaos     += (k->p_filter_chaos     - k->s_filter_chaos)     * k->sm_slow;
    k->s_filter_drive     += (k->p_filter_drive     - k->s_filter_drive)     * k->sm_slow;
    k->s_ring_mod         += (k->p_ring_mod         - k->s_ring_mod)         * k->sm_slow;

    /* Refresh filter coefficient from smoothed knob values */
    update_filt_f(k);

    /* ---- Per-block constants ------------------------------------------ */
    float osc2_inc        = param_to_osc_freq(k->s_osc2_rate) * INV_SR;
    float osc1_base_freq  = param_to_osc_freq(k->s_osc1_rate);
    float osc1_chaos_gain = k->s_osc_chaos * 1.8f;

    /* SVF Q 0→2.1: resonance builds toward self-oscillation at top */
    float q_amt   = k->s_filter_resonance * 2.1f;
    /* Drive 1→16×: light dirt at low end, heavy saturation at top */
    float drv_amt = 1.0f + k->s_filter_drive * 15.0f;
    float ring_amt = k->s_ring_mod;

    k->samples_no_clock += frames;
    int clock_present = (k->samples_no_clock < CLOCK_TIMEOUT_SAMPLES);

    /* ---- Per-sample loop ---------------------------------------------- */
    for (int i = 0; i < frames; i++) {

        /* Osc1 frequency: base modulated by rungler CV × Osc Chaos */
        float osc1_freq = osc1_base_freq * (1.0f + osc1_chaos_gain * k->rungler_cv);
        osc1_freq       = clampf(osc1_freq, 0.1f, 20000.0f);
        float osc1_inc  = osc1_freq * INV_SR;

        /* Osc1: triangle wave → soft clip */
        float o1 = (k->osc1_phase < 0.5f)
            ? (4.0f * k->osc1_phase - 1.0f)
            : (3.0f - 4.0f * k->osc1_phase);
        o1 = soft_clip(o1);

        /* Osc2: triangle wave → soft clip */
        float o2 = (k->osc2_phase < 0.5f)
            ? (4.0f * k->osc2_phase - 1.0f)
            : (3.0f - 4.0f * k->osc2_phase);
        o2 = soft_clip(o2);

        /* Rungler: advance shift register on Osc2 positive zero-crossing */
        if (k->osc2_prev < 0.0f && o2 >= 0.0f) {
            uint8_t new_bit = (o1 >= 0.0f) ? 1 : 0;
            k->shift_reg    = ((k->shift_reg << 1) | new_bit) & 0xFF;
            k->rungler_cv   = k->shift_reg / 255.0f;

            /* Abruptly recompute filt_f → filter pings at the new cutoff.
             * This sudden coefficient change excites the resonant feedback
             * path, producing the dripping/plucked-string Benjolin ping. */
            update_filt_f(k);

            /* Loop: count crossings, reset shift register at loop boundary */
            if (k->loop_active) {
                k->loop_cross_count++;
                if (k->loop_cross_count >= k->loop_beats) {
                    k->loop_cross_count = 0;
                    k->shift_reg        = k->loop_snapshot;
                    k->rungler_cv       = k->shift_reg / 255.0f;
                    update_filt_f(k);
                }
            }

            /* Fallback sequencer advance (no MIDI clock present) */
            if (!clock_present) {
                k->osc2_cross_count++;
                if (k->osc2_cross_count >= FALLBACK_CROSSINGS) {
                    k->osc2_cross_count = 0;
                    k->current_step     = (k->current_step + 1) & 15;
                    k->gate_on          = (k->step_mask >> k->current_step) & 1;
                }
            }
        }
        k->osc2_prev = o2;

        /* Mix Osc1 + Osc2 → soft clip */
        float sig = o1 * 0.6f + o2 * 0.4f;
        sig = soft_clip(sig);

        /* Ring mod: true bipolar Osc1 × Osc2, no extra distortion */
        if (ring_amt > 0.001f) {
            sig = sig * (1.0f - ring_amt) + (o1 * o2) * ring_amt;
        }

        /* ---- Chamberlin SVF (state-variable filter) -----------------------
         *
         * feedback     = Q × band                — resonant feedback tap
         * input_driven = tanh((sig + feedback) × drive) — drive at input
         * hp           = input_driven − low − feedback   — highpass output
         * band        += F × hp                  — bandpass integrator
         * low         += F × band                — lowpass integrator
         * output       = tanh(low)
         *
         * This topology produces true resonant pinging. An abrupt filt_f
         * change (from a rungler step) excites the bandpass path, causing
         * the filter to ring at the new cutoff — the Benjolin ping. */
        float feedback     = q_amt * k->filt_band;
        float input_driven = tanhf((sig + feedback) * drv_amt);
        float hp           = input_driven - k->filt_low - feedback;
        k->filt_band      += k->filt_f * hp;
        k->filt_low       += k->filt_f * k->filt_band;

        float filtered = tanhf(k->filt_low);

        /* Sequencer gate: step ON = pass audio, step OFF = silence */
        float gated = k->gate_on ? filtered : 0.0f;

        /* Final output soft clip + scale to int16 with headroom */
        float out = tanhf(gated * 0.8f);
        int32_t s = (int32_t)(out * 25000.0f);
        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;

        out_lr[i * 2]     = (int16_t)s;
        out_lr[i * 2 + 1] = (int16_t)s;

        /* Advance oscillator phases */
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
