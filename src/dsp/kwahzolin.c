#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "host/plugin_api_v1.h"

#define KWAH_SR    44100.0f
#define KWAH_PI    3.14159265359f

static inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

static inline float param_to_osc_hz(float p) {
    return 10.0f * powf(500.0f, clampf(p, 0.0f, 1.0f));
}

static inline float param_to_cutoff_hz(float p) {
    return 20.0f * powf(400.0f, clampf(p, 0.0f, 1.0f));
}

typedef struct {
    float osc1_phase;
    float osc2_phase;
    float osc2_prev;

    uint8_t shift_reg;
    float   rungler_cv;

    float svf_lp;
    float svf_bp;

    float lfo_phase;
    int   inject_impulse;
    float impulse_val;

    float t_osc1_freq;
    float t_osc2_freq;
    float t_osc_chaos;
    float t_filter_cutoff;
    float t_filter_resonance;
    float t_filter_lfo;
    float t_filter_chaos;
    float t_loop;

    float p_osc1_freq;
    float p_osc2_freq;
    float p_osc_chaos;
    float p_filter_cutoff;
    float p_filter_resonance;
    float p_filter_lfo;
    float p_filter_chaos;
    float p_loop;

    float smooth_coeff;

} kwahzolin_t;

static const host_api_v1_t *g_host = NULL;

static void *kwahzolin_create(const char *module_dir, const char *json_defaults) {
    (void)module_dir; (void)json_defaults;

    kwahzolin_t *k = (kwahzolin_t *)calloc(1, sizeof(kwahzolin_t));
    if (!k) return NULL;

    k->smooth_coeff = 1.0f - expf(-(float)MOVE_FRAMES_PER_BLOCK / (KWAH_SR * 0.015f));

    k->t_osc1_freq        = 110.0f;
    k->t_osc2_freq        = 170.0f;
    k->t_osc_chaos        = 0.3f;
    k->t_filter_cutoff    = 800.0f;
    k->t_filter_resonance = 0.5f;
    k->t_filter_lfo       = 0.0f;
    k->t_filter_chaos     = 0.4f;
    k->t_loop             = 0.0f;

    k->p_osc1_freq        = k->t_osc1_freq;
    k->p_osc2_freq        = k->t_osc2_freq;
    k->p_osc_chaos        = k->t_osc_chaos;
    k->p_filter_cutoff    = k->t_filter_cutoff;
    k->p_filter_resonance = k->t_filter_resonance;
    k->p_filter_lfo       = k->t_filter_lfo;
    k->p_filter_chaos     = k->t_filter_chaos;
    k->p_loop             = k->t_loop;

    k->shift_reg  = 0xA5;
    k->rungler_cv = k->shift_reg / 255.0f;

    return k;
}

static void kwahzolin_destroy(void *inst) {
    free(inst);
}

static void kwahzolin_on_midi(void *inst, const uint8_t *msg, int len, int source) {
    (void)inst; (void)msg; (void)len; (void)source;
}

static void kwahzolin_set_param(void *inst, const char *key, const char *val) {
    kwahzolin_t *k = (kwahzolin_t *)inst;
    if (!k || !key || !val) return;

    float f = (float)atof(val);

    if (!strcmp(key, "osc1_freq")) {
        k->t_osc1_freq = param_to_osc_hz(f);
    } else if (!strcmp(key, "osc2_freq")) {
        k->t_osc2_freq = param_to_osc_hz(f);
    } else if (!strcmp(key, "filter_cutoff")) {
        k->t_filter_cutoff = param_to_cutoff_hz(f);
    } else if (!strcmp(key, "osc_chaos")) {
        k->t_osc_chaos = clampf(f, 0.0f, 1.0f);
    } else if (!strcmp(key, "filter_resonance")) {
        k->t_filter_resonance = clampf(f, 0.0f, 1.0f);
    } else if (!strcmp(key, "filter_lfo")) {
        k->t_filter_lfo = clampf(f, 0.0f, 1.0f);
    } else if (!strcmp(key, "filter_chaos")) {
        k->t_filter_chaos = clampf(f, 0.0f, 1.0f);
    } else if (!strcmp(key, "loop")) {
        k->t_loop = clampf(f, 0.0f, 1.0f);
    }
}

static int kwahzolin_get_param(void *inst, const char *key, char *buf, int buf_len) {
    kwahzolin_t *k = (kwahzolin_t *)inst;
    if (!k || !key || !buf || buf_len < 2) return -1;

    if (!strcmp(key, "osc1_freq"))        return snprintf(buf, buf_len, "%.4f", k->t_osc1_freq);
    if (!strcmp(key, "osc2_freq"))        return snprintf(buf, buf_len, "%.4f", k->t_osc2_freq);
    if (!strcmp(key, "osc_chaos"))        return snprintf(buf, buf_len, "%.4f", k->t_osc_chaos);
    if (!strcmp(key, "filter_cutoff"))    return snprintf(buf, buf_len, "%.4f", k->t_filter_cutoff);
    if (!strcmp(key, "filter_resonance")) return snprintf(buf, buf_len, "%.4f", k->t_filter_resonance);
    if (!strcmp(key, "filter_lfo"))       return snprintf(buf, buf_len, "%.4f", k->t_filter_lfo);
    if (!strcmp(key, "filter_chaos"))     return snprintf(buf, buf_len, "%.4f", k->t_filter_chaos);
    if (!strcmp(key, "loop"))             return snprintf(buf, buf_len, "%.4f", k->t_loop);

    if (!strcmp(key, "chain_params")) {
        static const char *cp =
            "["
            "{\"key\":\"osc1_freq\",\"name\":\"Osc 1 Frequency\","
                "\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01,\"default\":0.386},"
            "{\"key\":\"osc2_freq\",\"name\":\"Osc 2 Frequency\","
                "\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01,\"default\":0.456},"
            "{\"key\":\"osc_chaos\",\"name\":\"Osc Chaos\","
                "\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01,\"default\":0.3},"
            "{\"key\":\"filter_cutoff\",\"name\":\"Filter Cutoff\","
                "\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01,\"default\":0.616},"
            "{\"key\":\"filter_resonance\",\"name\":\"Filter Resonance\","
                "\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01,\"default\":0.5},"
            "{\"key\":\"filter_lfo\",\"name\":\"Filter LFO\","
                "\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01,\"default\":0.0},"
            "{\"key\":\"filter_chaos\",\"name\":\"Filter Chaos\","
                "\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01,\"default\":0.4},"
            "{\"key\":\"loop\",\"name\":\"Loop\","
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

static void kwahzolin_render_block(void *inst, int16_t *out_lr, int frames) {
    kwahzolin_t *k = (kwahzolin_t *)inst;
    if (!k) { memset(out_lr, 0, frames * 4); return; }

    const float sc = k->smooth_coeff;
    k->p_osc1_freq        += (k->t_osc1_freq        - k->p_osc1_freq)        * sc;
    k->p_osc2_freq        += (k->t_osc2_freq        - k->p_osc2_freq)        * sc;
    k->p_osc_chaos        += (k->t_osc_chaos        - k->p_osc_chaos)        * sc;
    k->p_filter_cutoff    += (k->t_filter_cutoff    - k->p_filter_cutoff)    * sc;
    k->p_filter_resonance += (k->t_filter_resonance - k->p_filter_resonance) * sc;
    k->p_filter_lfo       += (k->t_filter_lfo       - k->p_filter_lfo)       * sc;
    k->p_filter_chaos     += (k->t_filter_chaos     - k->p_filter_chaos)     * sc;
    k->p_loop             += (k->t_loop             - k->p_loop)             * sc;

    const float damping = 2.0f * (1.0f - k->p_filter_resonance * 0.995f);

    for (int i = 0; i < frames; i++) {

        float osc1_freq = clampf(k->p_osc1_freq * (1.0f + k->rungler_cv * k->p_osc_chaos * 2.0f), 0.5f, 20000.0f);
        float osc2_freq = clampf(k->p_osc2_freq * (1.0f + k->rungler_cv * k->p_osc_chaos * 2.0f), 0.5f, 20000.0f);

        k->osc1_phase += osc1_freq / KWAH_SR;
        if (k->osc1_phase >= 1.0f) k->osc1_phase -= 1.0f;
        float osc1 = (k->osc1_phase < 0.5f)
            ? (4.0f * k->osc1_phase - 1.0f)
            : (3.0f - 4.0f * k->osc1_phase);

        k->osc2_phase += osc2_freq / KWAH_SR;
        if (k->osc2_phase >= 1.0f) k->osc2_phase -= 1.0f;
        float osc2 = (k->osc2_phase < 0.5f)
            ? (4.0f * k->osc2_phase - 1.0f)
            : (3.0f - 4.0f * k->osc2_phase);

        k->lfo_phase += 0.2f / KWAH_SR;
        if (k->lfo_phase >= 1.0f) k->lfo_phase -= 1.0f;
        float lfo_out = (k->lfo_phase < 0.5f)
            ? (4.0f * k->lfo_phase - 1.0f)
            : (3.0f - 4.0f * k->lfo_phase);

        if (osc2 > 0.0f && k->osc2_prev <= 0.0f) {
            uint8_t new_bit = (osc1 > 0.0f) ? 1 : 0;
            uint8_t top_bit = (k->shift_reg >> 7) & 1;
            uint8_t chosen;

            if (k->p_loop >= 0.99f) {
                chosen = top_bit;
            } else if (k->p_loop <= 0.01f) {
                chosen = new_bit;
            } else {
                float r = (float)rand() / (float)RAND_MAX;
                chosen = (r < k->p_loop) ? top_bit : new_bit;
            }

            k->shift_reg  = ((k->shift_reg << 1) | chosen) & 0xFF;
            k->rungler_cv = k->shift_reg / 255.0f;

            k->inject_impulse = 1;
            k->impulse_val    = (k->rungler_cv - 0.5f) * 1.0f;
        }
        k->osc2_prev = osc2;

        float pulse1  = (osc1 > 0.0f) ? 1.0f : -1.0f;
        float pulse2  = (osc2 > 0.0f) ? 1.0f : -1.0f;
        float xor_out = (pulse1 != pulse2) ? 1.0f : -1.0f;

        float mod_chaos = k->rungler_cv * k->p_filter_chaos * 4000.0f;
        float mod_lfo   = lfo_out * k->p_filter_lfo * 2000.0f;
        float cutoff_hz;
        if (k->p_filter_cutoff <= 20.5f) {
            cutoff_hz = 20.0f;
        } else {
            cutoff_hz = clampf(k->p_filter_cutoff + mod_chaos + mod_lfo, 20.0f, 8000.0f);
        }
        float svf_f = clampf(2.0f * sinf(KWAH_PI * cutoff_hz / KWAH_SR), 0.001f, 0.99f);

        float input_sig = xor_out;
        if (k->inject_impulse) {
            input_sig += k->impulse_val;
            k->inject_impulse = 0;
        }
        float driven = tanhf(input_sig * 2.0f);

        float hp     = driven - damping * k->svf_bp - k->svf_lp;
        float new_bp = svf_f * hp + k->svf_bp;
        new_bp       = clampf(tanhf(new_bp * 0.5f) * 2.0f, -3.0f, 3.0f);
        float new_lp = svf_f * new_bp + k->svf_lp;
        k->svf_bp = new_bp;
        k->svf_lp = new_lp;

        float mixed    = (k->svf_lp * 0.5f) + (k->svf_bp * 0.5f);
        float filtered = tanhf(mixed * 0.6f);

        int16_t sample = (int16_t)clampf(filtered * 28000.0f, -32767.0f, 32767.0f);
        out_lr[i * 2]     = sample;
        out_lr[i * 2 + 1] = sample;
    }
}

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
