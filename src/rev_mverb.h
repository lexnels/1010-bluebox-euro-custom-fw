/*
 * MVerb, ported to C from Martin Eastwood's MVerb.h (github.com/martineastwood/mverb).
 *
 * Copyright (c) 2010 Martin Eastwood. This file is a derivative work and is distributed under the terms of the
 * GNU General Public License, version 3 or later (see the original repository's gpl-3.0.txt). Keep that in mind
 * before publishing any build that includes it: the rest of this repository is MIT, and the firmware it patches is
 * 1010music's.
 *
 * A Dattorro-style figure-eight tank: bandwidth filter, pre-delay, 4 input allpasses, two cross-coupled tank halves
 * (allpass, delay, state-variable damping, allpass, delay) with multi-tap outputs, plus 8-tap early reflections.
 * The structure, tunings and tap gains follow MVerb::process()/reset(); only the containers are flattened into one
 * float array, and parameters arrive per block.
 *
 *   Time      decay (MVerb Decay 0..1)
 *   Size      tank size (MVerb Size, 0.05 .. 1)
 *   Diffusion tank density (MVerb Density)
 *   HI C      damping and input bandwidth
 *   (ER mix fixed by the caller: rev_knobs.er_level, MVerb EarlyMix = 1 - er/2)
 *   Pre Delay MVerb's own pre-delay (tank only, like the original)
 *   Spread    stereo width (wrapper)
 */
#ifndef REV_MVERB_H
#define REV_MVERB_H
#include "rev_common.h"

/* line ids */
enum { MV_AP0, MV_AP1, MV_AP2, MV_AP3, MV_A4T0, MV_A4T1, MV_A4T2, MV_A4T3, MV_D0, MV_D1, MV_D2, MV_D3, MV_ER0, MV_ER1,
       MV_PRE, MV_LINES };
/* buffer sizes (floats), enough for 48 kHz at Size 1 with headroom */
static const uint32_t mv_max[MV_LINES] = { 700, 700, 700, 700, 1100, 3000, 1600, 4400, 7400, 6000, 6900, 5400, 4400, 3400,
                                           32768 };

struct mv_line { uint32_t ofs, len, idx[8]; };
struct mv_svf { float f, low, band; };
struct mv_state {
    float fs, size, prev_l, prev_r;
    float decay_s, bw_s, damp_s, pre_s;
    float size_pend;             /* Size waiting to settle before the re-layout (which clears the tank) */
    uint32_t pend_n, fresh;
    struct mv_line ln[MV_LINES];
    struct mv_svf bw[2], dmp[2];
};
#define MV_STATE_FLOATS 512u

static inline uint32_t mv_mem_floats(void)
{
    uint32_t t = MV_STATE_FLOATS;
    for (int i = 0; i < MV_LINES; i++) t += mv_max[i];
    return t;
}

static inline uint32_t mv_clip(uint32_t len, int line) { return len >= mv_max[line] ? mv_max[line] - 1 : len; }

static void mv_set_size(struct mv_state *st, float *m, float size)
{
    const float fs = st->fs;
    static const float a4t[4] = { 0.020f, 0.060f, 0.030f, 0.089f }, dl[4] = { 0.15f, 0.12f, 0.14f, 0.11f };
    st->size = size;
    for (int i = 0; i < 4; i++) {
        struct mv_line *a = &st->ln[MV_A4T0 + i], *d = &st->ln[MV_D0 + i];
        a->len = mv_clip((uint32_t)(a4t[i] * fs * size), MV_A4T0 + i);
        d->len = mv_clip((uint32_t)(dl[i] * fs * size), MV_D0 + i);
        for (int j = 0; j < 8; j++) a->idx[j] = d->idx[j] = 0;
        for (uint32_t j = 0; j < mv_max[MV_A4T0 + i]; j++) m[a->ofs + j] = 0.f;
        for (uint32_t j = 0; j < mv_max[MV_D0 + i]; j++) m[d->ofs + j] = 0.f;
    }
    /* MVerb's SetIndex calls (tap positions), as fractions of a second times Size */
    st->ln[MV_A4T1].idx[1] = (uint32_t)(0.006f * fs * size); st->ln[MV_A4T1].idx[2] = (uint32_t)(0.041f * fs * size);
    st->ln[MV_A4T3].idx[1] = (uint32_t)(0.031f * fs * size); st->ln[MV_A4T3].idx[2] = (uint32_t)(0.011f * fs * size);
    st->ln[MV_D0].idx[1] = (uint32_t)(0.067f * fs * size); st->ln[MV_D0].idx[2] = (uint32_t)(0.011f * fs * size);
    st->ln[MV_D0].idx[3] = (uint32_t)(0.121f * fs * size);
    st->ln[MV_D1].idx[1] = (uint32_t)(0.036f * fs * size); st->ln[MV_D1].idx[2] = (uint32_t)(0.089f * fs * size);
    st->ln[MV_D2].idx[1] = (uint32_t)(0.0089f * fs * size); st->ln[MV_D2].idx[2] = (uint32_t)(0.099f * fs * size);
    st->ln[MV_D3].idx[1] = (uint32_t)(0.067f * fs * size); st->ln[MV_D3].idx[2] = (uint32_t)(0.0041f * fs * size);
    for (int i = MV_A4T0; i <= MV_D3; i++)
        for (int j = 0; j < 8; j++)
            if (st->ln[i].idx[j] >= st->ln[i].len) st->ln[i].idx[j] = 0;
}

static void mv_init(float *m, float fs)
{
    struct mv_state *st = (struct mv_state *)m;
    st->fs = fs;
    uint32_t o = MV_STATE_FLOATS;
    for (int i = 0; i < MV_LINES; i++) {
        st->ln[i].ofs = o;
        o += mv_max[i];
        for (int j = 0; j < 8; j++) st->ln[i].idx[j] = 0;
    }
    static const float ap[4] = { 0.0048f, 0.0036f, 0.0127f, 0.0093f };
    for (int i = 0; i < 4; i++) st->ln[MV_AP0 + i].len = mv_clip((uint32_t)(ap[i] * fs), MV_AP0 + i);
    st->ln[MV_ER0].len = mv_clip((uint32_t)(0.089f * fs), MV_ER0);
    st->ln[MV_ER1].len = mv_clip((uint32_t)(0.069f * fs), MV_ER1);
    static const float er0[8] = { 0, 0.0199f, 0.0219f, 0.0354f, 0.0389f, 0.0414f, 0.0692f, 0 };
    static const float er1[8] = { 0, 0.0099f, 0.011f, 0.0182f, 0.0189f, 0.0213f, 0.0431f, 0 };
    for (int j = 0; j < 8; j++) {
        st->ln[MV_ER0].idx[j] = (uint32_t)(er0[j] * fs);
        st->ln[MV_ER1].idx[j] = (uint32_t)(er1[j] * fs);
    }
    st->ln[MV_PRE].len = 1;
    st->prev_l = st->prev_r = 0.f;
    st->decay_s = st->bw_s = st->damp_s = st->pre_s = -1.f;
    for (int c = 0; c < 2; c++) { st->bw[c].low = st->bw[c].band = 0.f; st->dmp[c].low = st->dmp[c].band = 0.f; }
    mv_set_size(st, m, 0.8f);
    st->size_pend = 0.8f;
    st->pend_n = 0;
    st->fresh = 1;
}

static inline float mv_svf_run(struct mv_svf *f, float in)   /* 4x oversampled SVF low-pass, q = 2 (MVerb's) */
{
    for (int i = 0; i < 4; i++) {
        f->low += f->f * f->band + 1e-25f;
        float high = in - f->low - 2.f * f->band;
        f->band += f->f * high;
    }
    return f->low;
}
static inline float mv_svf_coef(float hz, float fs)  /* 2 sin(pi f / (4 fs)) */
{
    float x = 3.1415927f * hz / (4.f * fs);
    return 2.f * rv_sin(x);
}

/* tap helpers: idx[0] is the read/write head, idx[1..7] extra taps; all advance together */
static inline void mv_adv(struct mv_line *l, int taps)
{
    for (int j = 0; j < taps; j++)
        if (++l->idx[j] >= l->len) l->idx[j] = 0;
}
static inline float mv_ap(float *m, struct mv_line *l, float in, float fb, int taps)
{
    float *b = m + l->ofs;
    float bo = b[l->idx[0]], t = in * -fb, out = bo + t;
    b[l->idx[0]] = in + (bo + t) * fb;
    mv_adv(l, taps);
    return out;
}
static inline float mv_dl(float *m, struct mv_line *l, float in, int taps)
{
    float *b = m + l->ofs;
    float out = b[l->idx[0]];
    b[l->idx[0]] = in;
    mv_adv(l, taps);
    return out;
}
#define MV_TAP(l, j) (m[(l)->ofs + (l)->idx[j]])

static void mv_process(float *m, const struct rev_knobs *k, float *L, float *R, unsigned n)
{
    struct mv_state *st = (struct mv_state *)m;
    const float fs = st->fs;
    float size = 0.95f * rv_clamp(k->size, 0.f, 1.f) + 0.05f;
    /* MVerb re-lays out and clears the tank on a Size change; wait until the knob rests (0.15 s) so turning it
     * costs one dropout, not one per block */
    if (size > st->size + 0.02f || size < st->size - 0.02f) {
        if (st->fresh) mv_set_size(st, m, size);
        else if (size > st->size_pend + 0.005f || size < st->size_pend - 0.005f) { st->size_pend = size; st->pend_n = 0; }
        else if ((st->pend_n += n) >= (uint32_t)(0.15f * fs)) mv_set_size(st, m, size);
    }
    st->fresh = 0;
    float decay_t = 0.7995f * rv_clamp(k->time * 0.5f, 0.f, 1.f) + 0.005f;
    float damp_t = rv_clamp(k->hicut, 0.f, 1.f) * 18400.f + 100.f;
    float bw_t = (0.5f + 0.5f * rv_clamp(k->hicut, 0.f, 1.f)) * 18400.f + 100.f;
    float in_gain = 1.f;
    if (k->freeze) { decay_t = 1.f; damp_t = 18500.f; in_gain = 0.f; }
    float density1 = 0.8f * rv_clamp(k->diffusion, 0.f, 1.f);
    float early = 1.f - 0.5f * rv_clamp(k->er_level, 0.f, 1.f);
    float pre_t = rv_clamp(k->predelay_s * fs, 1.f, (float)(mv_max[MV_PRE] - 1));
    if (st->decay_s < 0.f) { st->decay_s = decay_t; st->bw_s = bw_t; st->damp_s = damp_t; st->pre_s = pre_t; }
    /* per-block smoothing toward targets, like MVerb's per-sample deltas */
    st->decay_s += (decay_t - st->decay_s) * 0.5f;
    st->bw_s += (bw_t - st->bw_s) * 0.5f;
    st->damp_s += (damp_t - st->damp_s) * 0.5f;
    st->pre_s += (pre_t - st->pre_s) * 0.2f;
    float decay = st->decay_s;
    st->bw[0].f = st->bw[1].f = mv_svf_coef(st->bw_s, fs);
    st->dmp[0].f = st->dmp[1].f = mv_svf_coef(st->damp_s, fs);
    struct mv_line *pre = &st->ln[MV_PRE];
    uint32_t pl = (uint32_t)st->pre_s;
    if (pl != pre->len) { pre->len = pl; if (pre->idx[0] >= pl) pre->idx[0] = 0; }
    float density2 = rv_clamp(decay + 0.15f, 0.25f, 0.5f);
    struct mv_line *ln = st->ln;

    for (unsigned s = 0; s < n; s++) {
        float left = L[s] * in_gain, right = R[s] * in_gain;
        float bl = mv_svf_run(&st->bw[0], left), br = mv_svf_run(&st->bw[1], right);
        float erl = mv_dl(m, &ln[MV_ER0], bl * 0.5f + br * 0.3f, 8);
        erl += MV_TAP(&ln[MV_ER0], 2) * 0.6f + MV_TAP(&ln[MV_ER0], 3) * 0.4f + MV_TAP(&ln[MV_ER0], 4) * 0.3f
             + MV_TAP(&ln[MV_ER0], 5) * 0.3f + MV_TAP(&ln[MV_ER0], 6) * 0.1f + MV_TAP(&ln[MV_ER0], 7) * 0.1f
             + (bl * 0.4f + br * 0.2f) * 0.5f;
        float err = mv_dl(m, &ln[MV_ER1], bl * 0.3f + br * 0.5f, 8);
        err += MV_TAP(&ln[MV_ER1], 2) * 0.6f + MV_TAP(&ln[MV_ER1], 3) * 0.4f + MV_TAP(&ln[MV_ER1], 4) * 0.3f
             + MV_TAP(&ln[MV_ER1], 5) * 0.3f + MV_TAP(&ln[MV_ER1], 6) * 0.1f + MV_TAP(&ln[MV_ER1], 7) * 0.1f
             + (bl * 0.2f + br * 0.4f) * 0.5f;

        float x = mv_dl(m, pre, (br + bl) * 0.5f, 1);
        x = mv_ap(m, &ln[MV_AP0], x, 0.75f, 1);
        x = mv_ap(m, &ln[MV_AP1], x, 0.75f, 1);
        x = mv_ap(m, &ln[MV_AP2], x, 0.625f, 1);
        x = mv_ap(m, &ln[MV_AP3], x, 0.625f, 1);

        float lt = mv_ap(m, &ln[MV_A4T0], x + st->prev_r, density1, 4);
        lt = mv_dl(m, &ln[MV_D0], lt, 4);
        lt = mv_svf_run(&st->dmp[0], lt);
        lt = mv_ap(m, &ln[MV_A4T1], lt, density2, 4);
        lt = mv_dl(m, &ln[MV_D1], lt, 4);
        float rt = mv_ap(m, &ln[MV_A4T2], x + st->prev_l, density1, 4);
        rt = mv_dl(m, &ln[MV_D2], rt, 4);
        rt = mv_svf_run(&st->dmp[1], rt);
        rt = mv_ap(m, &ln[MV_A4T3], rt, density2, 4);
        rt = mv_dl(m, &ln[MV_D3], rt, 4);
        st->prev_l = lt * decay;
        st->prev_r = rt * decay;

        float al = 0.6f * (MV_TAP(&ln[MV_D2], 1) + MV_TAP(&ln[MV_D2], 2) - MV_TAP(&ln[MV_A4T3], 1) + MV_TAP(&ln[MV_D3], 1)
                           - MV_TAP(&ln[MV_D0], 1) - MV_TAP(&ln[MV_A4T1], 1) - MV_TAP(&ln[MV_D1], 1));
        float ar = 0.6f * (MV_TAP(&ln[MV_D0], 2) + MV_TAP(&ln[MV_D0], 3) - MV_TAP(&ln[MV_A4T1], 2) + MV_TAP(&ln[MV_D1], 2)
                           - MV_TAP(&ln[MV_D2], 3) - MV_TAP(&ln[MV_A4T3], 2) - MV_TAP(&ln[MV_D3], 2));
        L[s] = al * early + (1.f - early) * erl;
        R[s] = ar * early + (1.f - early) * err;
    }
}
#undef MV_TAP
#endif
