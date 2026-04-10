#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "host/plugin_api_v1.h"

#define KWAH_SR    44100.0f
#define KWAH_PI    3.14159265359f

#define LFO_COUNT          3
#define LFO_SHAPE_TRIANGLE 0
#define LFO_SHAPE_SINE     1
#define LFO_SHAPE_SQUARE   2
#define LFO_SHAPE_SAW      3
#define LFO_SHAPE_SH       4
#define LFO_SHAPE_WANDER   5

#define LFO_TARGET_OSC1   0
#define LFO_TARGET_OSC2   1
#define LFO_TARGET_CHAOS  2
#define LFO_TARGET_CUTOFF 3
#define LFO_TARGET_RES    4
#define LFO_TARGET_FCHAOS 5
#define LFO_TARGET_XMOD   6
#define LFO_TARGET_LOOP   7

#define DIST_OFF       0
#define DIST_OVERDRIVE 1
#define DIST_HARD      2
#define DIST_FUZZ      3

#define SL1 1553
#define SL2 2311
#define SL3 3779
#define SR1 1699
#define SR2 2503
#define SR3 4001
#define AP1  557
#define AP2  389

static inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

static inline float param_to_osc_hz(float p) {
    return 10.0f * powf(500.0f, clampf(p, 0.0f, 1.0f));
}

static inline float param_to_cutoff_hz(float p) {
    return 20.0f * powf(400.0f, clampf(p, 0.0f, 1.0f));
}

static inline float apply_dist(float x, int type, float amount) {
    if (type == DIST_OVERDRIVE) {
        return tanhf(x * (1.0f + amount * 9.0f)) * 0.9f;
    }
    if (type == DIST_HARD) {
        float d = x * (1.0f + amount * 19.0f);
        if (d >  0.8f) d =  0.8f + (d - 0.8f) * 0.1f;
        if (d < -0.8f) d = -0.8f + (d + 0.8f) * 0.1f;
        return d * 0.8f;
    }
    if (type == DIST_FUZZ) {
        float d = x * (1.0f + amount * 49.0f);
        if (d >  0.5f) d =  0.5f;
        if (d < -0.7f) d = -0.7f;
        return d * 1.4f;
    }
    return x;
}

typedef struct {
    float phase;
    float rate;
    float amount;
    int   shape;
    int   target;
    float sh_value;
    float wander_target;
    float wander_current;
} lfo_t;

typedef struct {
    float osc1_phase;
    float osc2_phase;
    float osc2_prev;
    float prev_osc1;
    float prev_osc2;

    uint8_t shift_reg;
    float   rungler_cv;

    float svf_lp;
    float svf_bp;
    float svf_f;

    int   inject_flag;
    float inject_amount;

    float chaos_held_cv;
    float prev_held_chaos_cv;

    float cutoff_hz_smoothed;
    float cutoff_coeff;
    float hz_coeff;

    float t_osc1_freq;
    float t_osc2_freq;
    float t_osc_chaos;
    float t_filter_cutoff;
    float t_filter_resonance;
    float t_filter_chaos;
    float t_cross_mod;
    float t_loop;

    float p_osc1_freq;
    float p_osc2_freq;
    float p_osc_chaos;
    float p_filter_cutoff;
    float p_filter_resonance;
    float p_filter_chaos;
    float p_cross_mod;
    float p_loop;

    float smooth_coeff;

    lfo_t lfo[LFO_COUNT];

    int   dist_type;
    float dist_amount;
    float dist_mix;

    float sl1[SL1], sl2[SL2], sl3[SL3];
    float sr1[SR1], sr2[SR2], sr3[SR3];
    int   sl1w, sl2w, sl3w;
    int   sr1w, sr2w, sr3w;

    float ap_l1[AP1], ap_l2[AP2];
    float ap_r1[AP1], ap_r2[AP2];
    int   ap_l1w, ap_l2w;
    int   ap_r1w, ap_r2w;

    float rev_lp_l;
    float rev_lp_r;

    int   reverb_on;
    float t_reverb_decay, p_reverb_decay;
    float t_reverb_tone,  p_reverb_tone;
    float t_reverb_mix,   p_reverb_mix;

} kwahzolin_t;

static const host_api_v1_t *g_host = NULL;

static void *kwahzolin_create(const char *module_dir, const char *json_defaults) {
    (void)module_dir; (void)json_defaults;

    kwahzolin_t *k = (kwahzolin_t *)calloc(1, sizeof(kwahzolin_t));
    if (!k) return NULL;

    k->smooth_coeff = 1.0f - expf(-(float)MOVE_FRAMES_PER_BLOCK / (KWAH_SR * 0.015f));
    k->cutoff_coeff = 1.0f - expf(-1.0f / (KWAH_SR * 0.05f));
    k->hz_coeff     = 1.0f - expf(-1.0f / (KWAH_SR * 0.005f));

    k->t_osc1_freq        = 110.0f;
    k->t_osc2_freq        = 170.0f;
    k->t_osc_chaos        = 0.3f;
    k->t_filter_cutoff    = 800.0f;
    k->t_filter_resonance = 0.5f;
    k->t_filter_chaos     = 0.4f;
    k->t_cross_mod        = 0.0f;
    k->t_loop             = 0.0f;

    k->p_osc1_freq        = k->t_osc1_freq;
    k->p_osc2_freq        = k->t_osc2_freq;
    k->p_osc_chaos        = k->t_osc_chaos;
    k->p_filter_cutoff    = k->t_filter_cutoff;
    k->p_filter_resonance = k->t_filter_resonance;
    k->p_filter_chaos     = k->t_filter_chaos;
    k->p_cross_mod        = k->t_cross_mod;
    k->p_loop             = k->t_loop;

    k->prev_osc1          = 0.0f;
    k->prev_osc2          = 0.0f;

    k->shift_reg          = 0xA5;
    k->rungler_cv         = k->shift_reg / 255.0f;
    k->chaos_held_cv      = 0.5f;
    k->prev_held_chaos_cv = 0.5f;
    k->cutoff_hz_smoothed = 800.0f;
    k->svf_f              = clampf(2.0f * sinf(KWAH_PI * 800.0f / KWAH_SR), 0.001f, 0.99f);

    for (int i = 0; i < LFO_COUNT; i++) {
        k->lfo[i].rate           = 0.5f;
        k->lfo[i].amount         = 0.0f;
        k->lfo[i].shape          = LFO_SHAPE_TRIANGLE;
        k->lfo[i].target         = LFO_TARGET_CUTOFF;
        k->lfo[i].sh_value       = 0.0f;
        k->lfo[i].wander_target  = 0.0f;
        k->lfo[i].wander_current = 0.0f;
    }

    k->dist_type   = DIST_OFF;
    k->dist_amount = 0.0f;
    k->dist_mix    = 1.0f;

    k->t_reverb_decay = k->p_reverb_decay = 0.6f;
    k->t_reverb_tone  = k->p_reverb_tone  = 0.5f;
    k->t_reverb_mix   = k->p_reverb_mix   = 0.5f;

    return k;
}

static void kwahzolin_destroy(void *inst) {
    free(inst);
}

static void kwahzolin_on_midi(void *inst, const uint8_t *msg, int len, int source) {
    (void)inst; (void)msg; (void)len; (void)source;
}

static int parse_lfo_prefix(const char *key, const char **suffix) {
    if (strncmp(key, "lfo", 3) != 0) return -1;
    int idx = key[3] - '1';
    if (idx < 0 || idx > 2) return -1;
    if (key[4] != '_') return -1;
    *suffix = key + 5;
    return idx;
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
    } else if (!strcmp(key, "filter_chaos")) {
        k->t_filter_chaos = clampf(f, 0.0f, 1.0f);
    } else if (!strcmp(key, "cross_mod")) {
        k->t_cross_mod = clampf(f, 0.0f, 1.0f);
    } else if (!strcmp(key, "loop")) {
        k->t_loop = clampf(f, 0.0f, 1.0f);
    } else if (!strcmp(key, "dist_type")) {
        k->dist_type = (int)clampf(f, 0.0f, 3.0f);
    } else if (!strcmp(key, "dist_amount")) {
        k->dist_amount = clampf(f, 0.0f, 1.0f);
    } else if (!strcmp(key, "dist_mix")) {
        k->dist_mix = clampf(f, 0.0f, 1.0f);
    } else if (!strcmp(key, "reverb_on")) {
        k->reverb_on = atoi(val);
    } else if (!strcmp(key, "reverb_decay")) {
        k->t_reverb_decay = clampf(f, 0.0f, 1.0f);
    } else if (!strcmp(key, "reverb_tone")) {
        k->t_reverb_tone = clampf(f, 0.0f, 1.0f);
    } else if (!strcmp(key, "reverb_mix")) {
        k->t_reverb_mix = clampf(f, 0.0f, 1.0f);
    } else {
        const char *suffix;
        int idx = parse_lfo_prefix(key, &suffix);
        if (idx >= 0) {
            if (!strcmp(suffix, "rate")) {
                k->lfo[idx].rate = clampf(f, 0.05f, 100.0f);
            } else if (!strcmp(suffix, "amount")) {
                k->lfo[idx].amount = clampf(f, 0.0f, 1.0f);
            } else if (!strcmp(suffix, "shape")) {
                k->lfo[idx].shape = (int)clampf(f, 0.0f, 5.0f);
            } else if (!strcmp(suffix, "target")) {
                k->lfo[idx].target = (int)clampf(f, 0.0f, 7.0f);
            }
        }
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
    if (!strcmp(key, "filter_chaos"))     return snprintf(buf, buf_len, "%.4f", k->t_filter_chaos);
    if (!strcmp(key, "cross_mod"))        return snprintf(buf, buf_len, "%.4f", k->t_cross_mod);
    if (!strcmp(key, "loop"))             return snprintf(buf, buf_len, "%.4f", k->t_loop);
    if (!strcmp(key, "dist_type"))        return snprintf(buf, buf_len, "%d",   k->dist_type);
    if (!strcmp(key, "dist_amount"))      return snprintf(buf, buf_len, "%.4f", k->dist_amount);
    if (!strcmp(key, "dist_mix"))         return snprintf(buf, buf_len, "%.4f", k->dist_mix);
    if (!strcmp(key, "reverb_on"))        return snprintf(buf, buf_len, "%d",   k->reverb_on);
    if (!strcmp(key, "reverb_decay"))     return snprintf(buf, buf_len, "%.4f", k->p_reverb_decay);
    if (!strcmp(key, "reverb_tone"))      return snprintf(buf, buf_len, "%.4f", k->p_reverb_tone);
    if (!strcmp(key, "reverb_mix"))       return snprintf(buf, buf_len, "%.4f", k->p_reverb_mix);

    const char *suffix;
    int idx = parse_lfo_prefix(key, &suffix);
    if (idx >= 0) {
        if (!strcmp(suffix, "rate"))   return snprintf(buf, buf_len, "%.4f", k->lfo[idx].rate);
        if (!strcmp(suffix, "amount")) return snprintf(buf, buf_len, "%.4f", k->lfo[idx].amount);
        if (!strcmp(suffix, "shape"))  return snprintf(buf, buf_len, "%d",   k->lfo[idx].shape);
        if (!strcmp(suffix, "target")) return snprintf(buf, buf_len, "%d",   k->lfo[idx].target);
    }

    if (!strcmp(key, "chain_params")) {
        static const char *cp =
            "["
            "{\"key\":\"osc1_freq\",\"name\":\"Osc 1 Frequency\","
                "\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01,\"default\":0.386},"
            "{\"key\":\"osc2_freq\",\"name\":\"Osc 2 Frequency\","
                "\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01,\"default\":0.456},"
            "{\"key\":\"osc_chaos\",\"name\":\"Osc Chaos\","
                "\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01,\"default\":0.0},"
            "{\"key\":\"filter_cutoff\",\"name\":\"Filter Cutoff\","
                "\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01,\"default\":0.616},"
            "{\"key\":\"filter_resonance\",\"name\":\"Filter Resonance\","
                "\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01,\"default\":0.0},"
            "{\"key\":\"filter_chaos\",\"name\":\"Filter Chaos\","
                "\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01,\"default\":0.0},"
            "{\"key\":\"cross_mod\",\"name\":\"Cross Mod\","
                "\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01,\"default\":0.0},"
            "{\"key\":\"loop\",\"name\":\"Loop\","
                "\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01,\"default\":0.0},"
            "{\"key\":\"reverb_on\",\"name\":\"Reverb On\","
                "\"type\":\"int\",\"min\":0,\"max\":1,\"default\":0},"
            "{\"key\":\"reverb_decay\",\"name\":\"Reverb Decay\","
                "\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01,\"default\":0.6},"
            "{\"key\":\"reverb_tone\",\"name\":\"Reverb Tone\","
                "\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01,\"default\":0.5},"
            "{\"key\":\"reverb_mix\",\"name\":\"Reverb Mix\","
                "\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01,\"default\":0.5}"
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

static inline float allpass_process(float in, float *buf, int *w, int size, float g) {
    float delayed = buf[*w];
    float new_val = in - g * delayed;
    buf[*w] = new_val;
    *w = (*w + 1) % size;
    return delayed + g * new_val;
}

static void kwahzolin_render_block(void *inst, int16_t *out_lr, int frames) {
    kwahzolin_t *k = (kwahzolin_t *)inst;
    if (!k) { memset(out_lr, 0, frames * 4); return; }

    const float sc = k->smooth_coeff;
    k->p_osc1_freq        += (k->t_osc1_freq        - k->p_osc1_freq)        * sc;
    k->p_osc2_freq        += (k->t_osc2_freq        - k->p_osc2_freq)        * sc;
    k->p_osc_chaos        += (k->t_osc_chaos        - k->p_osc_chaos)        * sc;
    k->p_filter_resonance += (k->t_filter_resonance - k->p_filter_resonance) * sc;
    k->p_filter_chaos     += (k->t_filter_chaos     - k->p_filter_chaos)     * sc;
    k->p_cross_mod        += (k->t_cross_mod        - k->p_cross_mod)        * sc;
    k->p_loop             += (k->t_loop             - k->p_loop)             * sc;
    k->p_reverb_decay     += (k->t_reverb_decay     - k->p_reverb_decay)     * sc;
    k->p_reverb_tone      += (k->t_reverb_tone      - k->p_reverb_tone)      * sc;
    k->p_reverb_mix       += (k->t_reverb_mix       - k->p_reverb_mix)       * sc;

    const float damping   = 2.0f * (1.0f - k->p_filter_resonance * 0.995f);
    const float bp_amount = k->p_filter_resonance * 0.7f;
    const float lp_amount = 1.0f - bp_amount;

    float wander_coeff[LFO_COUNT];
    for (int li = 0; li < LFO_COUNT; li++) {
        wander_coeff[li] = 1.0f - expf(-k->lfo[li].rate * 4.0f / KWAH_SR);
    }

    for (int i = 0; i < frames; i++) {

        k->p_filter_cutoff += (k->t_filter_cutoff - k->p_filter_cutoff) * k->cutoff_coeff;

        float lfo_osc1_mod   = 0.0f;
        float lfo_osc2_mod   = 0.0f;
        float lfo_chaos_mod  = 0.0f;
        float lfo_cutoff_mod = 0.0f;
        float lfo_res_mod    = 0.0f;
        float lfo_fchaos_mod = 0.0f;
        float lfo_xmod_mod   = 0.0f;
        float lfo_loop_mod   = 0.0f;

        for (int li = 0; li < LFO_COUNT; li++) {
            if (k->lfo[li].amount <= 0.0f) continue;

            k->lfo[li].phase += k->lfo[li].rate / KWAH_SR;
            int wrapped = (k->lfo[li].phase >= 1.0f);
            if (wrapped) k->lfo[li].phase -= 1.0f;

            float lfo_out;
            switch (k->lfo[li].shape) {
                case LFO_SHAPE_TRIANGLE:
                    lfo_out = (k->lfo[li].phase < 0.5f)
                        ? (4.0f * k->lfo[li].phase - 1.0f)
                        : (3.0f - 4.0f * k->lfo[li].phase);
                    break;
                case LFO_SHAPE_SINE:
                    lfo_out = sinf(2.0f * KWAH_PI * k->lfo[li].phase);
                    break;
                case LFO_SHAPE_SQUARE:
                    lfo_out = (k->lfo[li].phase < 0.5f) ? 1.0f : -1.0f;
                    break;
                case LFO_SHAPE_SAW:
                    lfo_out = 2.0f * k->lfo[li].phase - 1.0f;
                    break;
                case LFO_SHAPE_SH:
                    if (wrapped) {
                        k->lfo[li].sh_value =
                            ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f;
                    }
                    lfo_out = k->lfo[li].sh_value;
                    break;
                case LFO_SHAPE_WANDER:
                    if (wrapped) {
                        k->lfo[li].wander_target =
                            ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f;
                    }
                    k->lfo[li].wander_current +=
                        (k->lfo[li].wander_target - k->lfo[li].wander_current)
                        * wander_coeff[li];
                    lfo_out = k->lfo[li].wander_current;
                    break;
                default:
                    lfo_out = 0.0f;
                    break;
            }

            float contrib = lfo_out * k->lfo[li].amount;
            switch (k->lfo[li].target) {
                case LFO_TARGET_OSC1:   lfo_osc1_mod   += contrib * 500.0f;  break;
                case LFO_TARGET_OSC2:   lfo_osc2_mod   += contrib * 500.0f;  break;
                case LFO_TARGET_CHAOS:  lfo_chaos_mod  += contrib * 0.5f;    break;
                case LFO_TARGET_CUTOFF: lfo_cutoff_mod += contrib * 2000.0f; break;
                case LFO_TARGET_RES:    lfo_res_mod    += contrib * 0.3f;    break;
                case LFO_TARGET_FCHAOS: lfo_fchaos_mod += contrib * 0.5f;    break;
                case LFO_TARGET_XMOD:   lfo_xmod_mod   += contrib * 0.5f;    break;
                case LFO_TARGET_LOOP:   lfo_loop_mod   += contrib * 0.5f;    break;
            }
        }

        float eff_osc_chaos    = clampf(k->p_osc_chaos    + lfo_chaos_mod,  0.0f, 1.0f);
        float eff_filter_chaos = clampf(k->p_filter_chaos + lfo_fchaos_mod, 0.0f, 1.0f);
        float eff_cross_mod    = clampf(k->p_cross_mod    + lfo_xmod_mod,   0.0f, 1.0f);
        float eff_loop         = clampf(k->p_loop         + lfo_loop_mod,   0.0f, 1.0f);

        float eff_damping   = damping;
        float eff_bp_amount = bp_amount;
        float eff_lp_amount = lp_amount;
        if (lfo_res_mod != 0.0f) {
            float eff_res = clampf(k->p_filter_resonance + lfo_res_mod, 0.0f, 0.995f);
            eff_damping   = 2.0f * (1.0f - eff_res * 0.995f);
            eff_bp_amount = eff_res * 0.7f;
            eff_lp_amount = 1.0f - eff_bp_amount;
        }

        float xmod_depth = eff_cross_mod * 500.0f;
        float osc1_base  = k->p_osc1_freq + lfo_osc1_mod;
        float osc2_base  = k->p_osc2_freq + lfo_osc2_mod;
        float osc1_freq  = clampf(
            osc1_base * (1.0f + k->rungler_cv * eff_osc_chaos * 2.0f) + k->prev_osc2 * xmod_depth,
            1.0f, 8000.0f);
        float osc2_freq  = clampf(
            osc2_base * (1.0f + k->rungler_cv * eff_osc_chaos * 2.0f) + k->prev_osc1 * xmod_depth,
            1.0f, 8000.0f);

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

        k->prev_osc1 = osc1;
        k->prev_osc2 = osc2;

        if (osc2 > 0.0f && k->osc2_prev <= 0.0f) {
            uint8_t new_bit = (osc1 > 0.0f) ? 1 : 0;
            uint8_t top_bit = (k->shift_reg >> 7) & 1;
            uint8_t chosen;

            if (eff_loop >= 0.99f) {
                chosen = top_bit;
            } else if (eff_loop <= 0.01f) {
                chosen = new_bit;
            } else {
                float r = (float)rand() / (float)RAND_MAX;
                chosen = (r < eff_loop) ? top_bit : new_bit;
            }

            k->shift_reg  = ((k->shift_reg << 1) | chosen) & 0xFF;
            k->rungler_cv = k->shift_reg / 255.0f;

            k->inject_amount      = (k->rungler_cv - k->prev_held_chaos_cv) * 1.5f;
            k->chaos_held_cv      = k->rungler_cv;
            k->prev_held_chaos_cv = k->chaos_held_cv;
            k->inject_flag        = 1;
        }
        k->osc2_prev = osc2;

        float chaos_mod  = (k->chaos_held_cv - 0.5f) * 2.0f * eff_filter_chaos * 3000.0f;
        float raw_cutoff = k->p_filter_cutoff + chaos_mod + lfo_cutoff_mod;
        float cutoff_target = (k->p_filter_cutoff <= 20.5f)
                              ? 20.0f
                              : clampf(raw_cutoff, 20.0f, 8000.0f);

        k->cutoff_hz_smoothed += (cutoff_target - k->cutoff_hz_smoothed) * k->hz_coeff;
        k->svf_f = clampf(2.0f * sinf(KWAH_PI * k->cutoff_hz_smoothed / KWAH_SR),
                          0.001f, 0.99f);

        float pulse1    = (osc1 > 0.0f) ? 1.0f : -1.0f;
        float pulse2    = (osc2 > 0.0f) ? 1.0f : -1.0f;
        float xor_out   = (pulse1 != pulse2) ? 1.0f : -1.0f;
        float input_sig = xor_out;
        if (k->inject_flag) {
            input_sig += k->inject_amount * 2.0f;
            k->inject_flag = 0;
        }
        float driven = tanhf(input_sig * 2.5f);

        float hp     = driven - eff_damping * k->svf_bp - k->svf_lp;
        float new_bp = k->svf_f * hp + k->svf_bp;
        new_bp       = clampf(tanhf(new_bp * 0.5f) * 2.0f, -3.0f, 3.0f);
        float new_lp = k->svf_f * new_bp + k->svf_lp;
        k->svf_bp = new_bp;
        k->svf_lp = new_lp;

        float mixed    = (k->svf_lp * eff_lp_amount) + (k->svf_bp * eff_bp_amount);
        float filtered = tanhf(mixed * 0.6f);

        float final_out;
        if (k->dist_type != DIST_OFF) {
            float comp;
            switch (k->dist_type) {
                case DIST_OVERDRIVE: comp = 0.7f; break;
                case DIST_HARD:      comp = 0.6f; break;
                case DIST_FUZZ:      comp = 0.5f; break;
                default:             comp = 1.0f; break;
            }
            float wet = apply_dist(filtered, k->dist_type, k->dist_amount) * comp;
            final_out = filtered * (1.0f - k->dist_mix) + wet * k->dist_mix;
        } else {
            final_out = filtered;
        }

        if (k->reverb_on) {
            float diff_l = allpass_process(final_out, k->ap_l1, &k->ap_l1w, AP1, 0.5f);
            diff_l       = allpass_process(diff_l,    k->ap_l2, &k->ap_l2w, AP2, 0.5f);
            float diff_r = allpass_process(final_out, k->ap_r1, &k->ap_r1w, AP1, 0.5f);
            diff_r       = allpass_process(diff_r,    k->ap_r2, &k->ap_r2w, AP2, 0.5f);

            float fb = 0.3f + k->p_reverb_decay * 0.64f;

            float l1_out = k->sl1[(k->sl1w + SL1 - 1) % SL1];
            float l2_out = k->sl2[(k->sl2w + SL2 - 1) % SL2];
            float l3_out = k->sl3[(k->sl3w + SL3 - 1) % SL3];
            float r1_out = k->sr1[(k->sr1w + SR1 - 1) % SR1];
            float r2_out = k->sr2[(k->sr2w + SR2 - 1) % SR2];
            float r3_out = k->sr3[(k->sr3w + SR3 - 1) % SR3];

            float wet_l = l1_out * 0.4f + l2_out * 0.35f + l3_out * 0.25f;
            float wet_r = r1_out * 0.4f + r2_out * 0.35f + r3_out * 0.25f;

            wet_l = tanhf(wet_l * 1.5f) * 0.7f;
            wet_r = tanhf(wet_r * 1.5f) * 0.7f;

            float tone_cutoff = 500.0f + k->p_reverb_tone * 7500.0f;
            float tone_f = clampf(2.0f * sinf(KWAH_PI * tone_cutoff / KWAH_SR), 0.001f, 0.99f);
            k->rev_lp_l += tone_f * (wet_l - k->rev_lp_l);
            k->rev_lp_r += tone_f * (wet_r - k->rev_lp_r);
            wet_l = k->rev_lp_l;
            wet_r = k->rev_lp_r;

            k->sl1[k->sl1w] = tanhf((diff_l + wet_l * fb + wet_r * 0.10f              ) * 0.8f);
            k->sl2[k->sl2w] = tanhf((diff_l + wet_l * fb + wet_r * 0.12f + l1_out * 0.05f) * 0.8f);
            k->sl3[k->sl3w] = tanhf((diff_l + wet_l * fb + wet_r * 0.08f + l2_out * 0.04f) * 0.8f);
            k->sr1[k->sr1w] = tanhf((diff_r + wet_r * fb + wet_l * 0.10f              ) * 0.8f);
            k->sr2[k->sr2w] = tanhf((diff_r + wet_r * fb + wet_l * 0.12f + r1_out * 0.05f) * 0.8f);
            k->sr3[k->sr3w] = tanhf((diff_r + wet_r * fb + wet_l * 0.08f + r2_out * 0.04f) * 0.8f);

            k->sl1w = (k->sl1w + 1) % SL1;
            k->sl2w = (k->sl2w + 1) % SL2;
            k->sl3w = (k->sl3w + 1) % SL3;
            k->sr1w = (k->sr1w + 1) % SR1;
            k->sr2w = (k->sr2w + 1) % SR2;
            k->sr3w = (k->sr3w + 1) % SR3;

            float out_l = (final_out * (1.0f - k->p_reverb_mix) + wet_l * k->p_reverb_mix) * 0.8f;
            float out_r = (final_out * (1.0f - k->p_reverb_mix) + wet_r * k->p_reverb_mix) * 0.8f;

            out_lr[i * 2]     = (int16_t)clampf(out_l * 28000.0f, -32767.0f, 32767.0f);
            out_lr[i * 2 + 1] = (int16_t)clampf(out_r * 28000.0f, -32767.0f, 32767.0f);
        } else {
            int16_t sample = (int16_t)clampf(final_out * 28000.0f, -32767.0f, 32767.0f);
            out_lr[i * 2]     = sample;
            out_lr[i * 2 + 1] = sample;
        }
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
