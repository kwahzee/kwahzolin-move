/*
 * kwahzolin — Benjolin-style chaotic synthesizer for Ableton Move
 *
 * Autonomous — ignores all MIDI. 8 knobs only.
 *
 * Signal chain:
 *   Osc1 (triangle, modulated by rungler CV × Osc Chaos)
 *   Osc2 (triangle, modulated by rungler CV × Osc Chaos; clocks rungler)
 *   Rungler: 8-bit shift register, clocked by Osc2 positive zero-crossings
 *            new bit = Osc1 sign; CV = weighted bit sum (recent bits heavier)
 *   XOR: Osc1_pulse XOR Osc2_pulse → primary audio signal
 *   Ring mod: Osc1 × Osc2, blended with XOR via Ring Modulation knob
 *   SVF lowpass: tanh drive at input, tanh resonant feedback
 *                cutoff = base + rungler_cv × Filter Chaos range
 *                abrupt cutoff jump on each rungler clock → filter PING
 *   Output: tanh(low × 1.5) scaled to int16
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

#define SAMPLE_RATE  44100.0f
#define INV_SR       (1.0f / 44100.0f)
#define ONE_PI       3.14159265359f

/* ====================================================================
 * Utilities
 * ==================================================================== */

static inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

/* Osc frequency: 10 Hz to 5000 Hz, logarithmic */
static inline float param_to_osc_freq(float p) {
    return 10.0f * powf(500.0f, p);
}

/* Filter base cutoff: 80 Hz to 8000 Hz, logarithmic */
static inline float param_to_cutoff(float p) {
    return 80.0f * powf(100.0f, p);
}

/* Chamberlin SVF integrator coefficient */
static inline float hz_to_F(float fc) {
    fc = clampf(fc, 20.0f, 18000.0f);
    float f = 2.0f * sinf(ONE_PI * fc * INV_SR);
    return clampf(f, 0.001f, 0.999f);
}

/* Rungler CV: weighted bit sum — bit0 (most recent) weight=1.0, bit7 weight=1/128
 * Output normalized to [0, 1]. */
static inline float calc_rungler_cv(uint8_t reg) {
    float cv = 0.0f, weight = 1.0f, total = 0.0f;
    for (int i = 0; i < 8; i++) {
        cv    += ((reg >> i) & 1) * weight;
        total += weight;
        weight *= 0.5f;
    }
    return cv / total;
}

/* ====================================================================
 * Instance state
 * ==================================================================== */

typedef struct {
    /* Oscillator phases [0, 1) */
    float osc1_phase;
    float osc2_phase;
    float osc2_prev;    /* previous Osc2 sample for zero-crossing detection */

    /* Rungler */
    uint8_t shift_reg;
    float   rungler_cv;

    /* SVF filter state */
    float filt_low;
    float filt_band;

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

    /* Block-rate IIR smoothing coefficient (~15ms)
     * coeff = 1 - exp(-FRAMES_PER_BLOCK / (SAMPLE_RATE * 0.015)) */
    float sm_coeff;

    const host_api_v1_t *host;
} kwahzolin_t;

/* ====================================================================
 * Global host reference
 * ==================================================================== */

static const host_api_v1_t *g_host = NULL;

/* ====================================================================
 * Plugin API v2 — lifecycle
 * ==================================================================== */

static void *kwahzolin_create(const char *module_dir, const char *json_defaults) {
    (void)module_dir; (void)json_defaults;

    kwahzolin_t *k = (kwahzolin_t *)calloc(1, sizeof(kwahzolin_t));
    if (!k) return NULL;

    k->sm_coeff = 1.0f - expf(-(float)MOVE_FRAMES_PER_BLOCK / (SAMPLE_RATE * 0.015f));

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

    k->shift_reg  = 0xA5;
    k->rungler_cv = calc_rungler_cv(k->shift_reg);

    k->host = g_host;
    return k;
}

static void kwahzolin_destroy(void *inst) {
    free(inst);
}

/* ====================================================================
 * Plugin API v2 — MIDI (ignored — kwahzolin is fully autonomous)
 * ==================================================================== */

static void kwahzolin_on_midi(void *inst, const uint8_t *msg, int len, int source) {
    (void)inst; (void)msg; (void)len; (void)source;
}

/* ====================================================================
 * Plugin API v2 — parameters
 * ==================================================================== */

static void kwahzolin_set_param(void *inst, const char *key, const char *val) {
    kwahzolin_t *k = (kwahzolin_t *)inst;
    if (!k || !key || !val) return;

    float f = clampf((float)atof(val), 0.0f, 1.0f);

    if      (!strcmp(key, "osc1_rate"))        k->p_osc1_rate        = f;
    else if (!strcmp(key, "osc2_rate"))        k->p_osc2_rate        = f;
    else if (!strcmp(key, "osc_chaos"))        k->p_osc_chaos        = f;
    else if (!strcmp(key, "filter_cutoff"))    k->p_filter_cutoff    = f;
    else if (!strcmp(key, "filter_resonance")) k->p_filter_resonance = f;
    else if (!strcmp(key, "filter_chaos"))     k->p_filter_chaos     = f;
    else if (!strcmp(key, "filter_drive"))     k->p_filter_drive     = f;
    else if (!strcmp(key, "ring_mod"))         k->p_ring_mod         = f;
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

    /* ---- Per-block IIR parameter smoothing (15ms) -------------------- */
    const float sm = k->sm_coeff;
    k->s_osc1_rate        += (k->p_osc1_rate        - k->s_osc1_rate)        * sm;
    k->s_osc2_rate        += (k->p_osc2_rate        - k->s_osc2_rate)        * sm;
    k->s_osc_chaos        += (k->p_osc_chaos        - k->s_osc_chaos)        * sm;
    k->s_filter_cutoff    += (k->p_filter_cutoff    - k->s_filter_cutoff)    * sm;
    k->s_filter_resonance += (k->p_filter_resonance - k->s_filter_resonance) * sm;
    k->s_filter_chaos     += (k->p_filter_chaos     - k->s_filter_chaos)     * sm;
    k->s_filter_drive     += (k->p_filter_drive     - k->s_filter_drive)     * sm;
    k->s_ring_mod         += (k->p_ring_mod         - k->s_ring_mod)         * sm;

    /* ---- Per-block constants ----------------------------------------- */
    float osc1_base    = param_to_osc_freq(k->s_osc1_rate);
    float osc2_base    = param_to_osc_freq(k->s_osc2_rate);
    float chaos_gain   = k->s_osc_chaos * 2.0f;          /* depth: 0 = clean, 1 = ±2× FM */
    float base_cutoff  = param_to_cutoff(k->s_filter_cutoff);
    float chaos_range  = k->s_filter_chaos * 7920.0f;    /* 0..7920 Hz rungler sweep */
    float res_q        = k->s_filter_resonance * 1.99f;  /* SVF Q 0.0..1.99 */
    float drive_amt    = 1.0f + k->s_filter_drive * 9.0f; /* 1× clean .. 10× driven */
    float ring_amt     = k->s_ring_mod;

    /* Compute initial filter F from current rungler CV */
    float filt_cutoff = clampf(base_cutoff + k->rungler_cv * chaos_range, 20.0f, 18000.0f);
    float F = hz_to_F(filt_cutoff);

    /* ---- Per-sample loop --------------------------------------------- */
    for (int i = 0; i < frames; i++) {

        /* Osc frequencies: base modulated by rungler CV × Osc Chaos
         * Both oscillators track the same rungler CV */
        float osc1_freq = clampf(osc1_base * (1.0f + chaos_gain * k->rungler_cv), 0.1f, 20000.0f);
        float osc2_freq = clampf(osc2_base * (1.0f + chaos_gain * k->rungler_cv), 0.1f, 20000.0f);

        /* Osc1: triangle wave, bipolar [-1, +1] */
        float o1 = (k->osc1_phase < 0.5f)
            ? (4.0f * k->osc1_phase - 1.0f)
            : (3.0f - 4.0f * k->osc1_phase);

        /* Osc2: triangle wave, bipolar [-1, +1] */
        float o2 = (k->osc2_phase < 0.5f)
            ? (4.0f * k->osc2_phase - 1.0f)
            : (3.0f - 4.0f * k->osc2_phase);

        /* Rungler: clock on Osc2 positive zero-crossing */
        if (k->osc2_prev < 0.0f && o2 >= 0.0f) {
            uint8_t new_bit = (o1 >= 0.0f) ? 1 : 0;
            k->shift_reg    = ((k->shift_reg << 1) | new_bit) & 0xFF;
            k->rungler_cv   = calc_rungler_cv(k->shift_reg);

            /* Abrupt cutoff update → excites SVF band path → PING */
            filt_cutoff = clampf(base_cutoff + k->rungler_cv * chaos_range, 20.0f, 18000.0f);
            F = hz_to_F(filt_cutoff);
        }
        k->osc2_prev = o2;

        /* XOR: Osc1 pulse XOR Osc2 pulse */
        float pulse1  = (o1 >= 0.0f) ? 1.0f : -1.0f;
        float pulse2  = (o2 >= 0.0f) ? 1.0f : -1.0f;
        float xor_sig = (pulse1 != pulse2) ? 1.0f : -1.0f;

        /* Ring mod: Osc1 × Osc2 (bipolar × bipolar = bipolar) */
        float ring_sig = o1 * o2;

        /* Pre-filter: XOR ↔ ring blend */
        float pre_filter = xor_sig * (1.0f - ring_amt) + ring_sig * ring_amt;

        /* ---- Chamberlin SVF lowpass ----------------------------------
         *
         * driven   = tanh(pre_filter × drive)          — input saturation
         * feedback = tanh(band × res_q × 2.0)          — bounded resonance
         * hp       = driven − low − feedback            — highpass
         * band    += F × hp                             — bandpass integrator
         * low     += F × band                           — lowpass integrator
         * output   = tanh(low × 1.5)
         *
         * At high res_q: abrupt F change (from rungler clock) excites the
         * band integrator, which rings at the new cutoff — the Benjolin ping. */
        float driven   = tanhf(pre_filter * drive_amt);
        float feedback = tanhf(k->filt_band * res_q * 2.0f);
        float hp       = driven - k->filt_low - feedback;
        k->filt_band  += F * hp;
        k->filt_low   += F * k->filt_band;
        float filtered  = tanhf(k->filt_low * 1.5f);

        /* Scale to int16 */
        int32_t s = (int32_t)(filtered * 25000.0f);
        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;

        out_lr[i * 2]     = (int16_t)s;
        out_lr[i * 2 + 1] = (int16_t)s;

        /* Advance oscillator phases */
        k->osc1_phase += osc1_freq * INV_SR;
        if (k->osc1_phase >= 1.0f) k->osc1_phase -= 1.0f;
        k->osc2_phase += osc2_freq * INV_SR;
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
