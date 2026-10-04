/*
 * Freeverb, after Jezar at Dreampoint's public-domain original (github.com/sinshu/freeverb, Components/): 8 damped
 * comb filters in parallel per side, then 4 allpasses in series. Tunings from tuning.h, rescaled from 44.1 kHz.
 *
 *   Time      decay 0.5 s .. 12.5 s (comb feedback, at most Freeverb's 0.98)
 *   HI C      damping (bright at the top)
 *   Diffusion allpass feedback 0.3 .. 0.7 (stock Freeverb: 0.5)
 *   Spread    stereo width (applied by the wrapper)
 *   Freeze    combs hold, input muted
 */
#ifndef REV_FREEVERB_H
#define REV_FREEVERB_H
#include "rev_common.h"

#define FV_COMBS 8
#define FV_APS 4
#define FV_LINE 2048u            /* longest comb at 48 kHz is ~1785; power of two for masking */

static const float fv_comb44[FV_COMBS] = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
static const float fv_ap44[FV_APS] = { 556, 441, 341, 225 };
#define FV_SPREAD44 23.f

struct fv_state {
    uint32_t w;
    uint32_t comb_len[2][FV_COMBS], ap_len[2][FV_APS];
    float store[2][FV_COMBS];
};
#define FV_STATE_FLOATS 128u
#define FV_MEM_FLOATS (FV_STATE_FLOATS + 2u * (FV_COMBS + FV_APS) * FV_LINE)

static void fv_init(float *m, float fs)
{
    struct fv_state *st = (struct fv_state *)m;
    float r = fs / 44100.f;
    st->w = 0;
    for (int c = 0; c < 2; c++) {
        for (int i = 0; i < FV_COMBS; i++) {
            uint32_t l = (uint32_t)((fv_comb44[i] + c * FV_SPREAD44) * r);
            st->comb_len[c][i] = l < FV_LINE ? l : FV_LINE - 1;
            st->store[c][i] = 0.f;
        }
        for (int i = 0; i < FV_APS; i++) {
            uint32_t l = (uint32_t)((fv_ap44[i] + c * FV_SPREAD44) * r);
            st->ap_len[c][i] = l < FV_LINE ? l : FV_LINE - 1;
        }
    }
}

static void fv_process(float *m, const struct rev_knobs *k, float *L, float *R, unsigned n)
{
    struct fv_state *st = (struct fv_state *)m;
    float *lines = m + FV_STATE_FLOATS;
    /* Time picks a decay of 0.5 s, 2.5 s, 12.5 s (bottom, middle, top); comb feedback follows from the mean comb
     * length (~32 ms), capped at Freeverb's own maximum of 0.98 */
    float rt = 0.5f * rv_exp2(2.3219281f * rv_clamp(k->time, 0.f, 2.f));
    float room = rv_exp2(-9.9657843f * 0.0318f / rt);
    if (room > 0.98f) room = 0.98f;
    float damp = 0.4f * (1.f - rv_clamp(k->hicut, 0.f, 1.f));
    float gain = 0.015f, apfb = 0.3f + 0.4f * rv_clamp(k->diffusion, 0.f, 1.f);
    if (k->freeze) { room = 1.f; damp = 0.f; gain = 0.f; }
    float damp2 = 1.f - damp;
    const uint32_t M = FV_LINE - 1;
    uint32_t w = st->w;
    for (unsigned s = 0; s < n; s++) {
        float in = (L[s] + R[s]) * gain;
        float out[2];
        for (int c = 0; c < 2; c++) {
            float acc = 0.f;
            float *base = lines + (uint32_t)c * (FV_COMBS + FV_APS) * FV_LINE;
#pragma GCC unroll 8
            for (int i = 0; i < FV_COMBS; i++) {
                float *b = base + i * FV_LINE;
                float o = b[(w - st->comb_len[c][i]) & M];
                st->store[c][i] = o * damp2 + st->store[c][i] * damp;
                b[w & M] = in + st->store[c][i] * room;
                acc += o;
            }
#pragma GCC unroll 4
            for (int i = 0; i < FV_APS; i++) {
                float *b = base + (FV_COMBS + i) * FV_LINE;
                float bo = b[(w - st->ap_len[c][i]) & M];
                b[w & M] = acc + bo * apfb;
                acc = bo - acc;
            }
            out[c] = acc;
        }
        L[s] = out[0];            /* Freeverb's default wet (1/scalewet x scalewet) */
        R[s] = out[1];
        w++;
    }
    st->w = w;
}
#endif
