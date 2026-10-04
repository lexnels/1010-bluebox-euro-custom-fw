/*
 * Shared plumbing for the alternative reverb styles (MVerb, Squall, Freeverb): the knobs as the hooks hand them over,
 * and a wrapper that adds what those engines lack on their own (low cut, pre-delay, stereo width, level, fade-in).
 * All delay memory lives in one float array the caller provides; nothing here allocates.
 */
#ifndef REV_COMMON_H
#define REV_COMMON_H
#include <stdint.h>

/* panel values, scaled by the caller */
struct rev_knobs {
    float time;        /* 0..2, 1 = default ("Time") */
    float level;       /* linear, 1 = unity ("Level") */
    float predelay_s;  /* seconds */
    float lowcut;      /* 0..1 */
    float hicut;       /* 0..1, 1 = brightest */
    float spread;      /* -1..1, 0 = normal width */
    float diffusion;   /* 0..1 */
    float size;        /* 0..1 */
    float feedback;    /* 0..1 (engine-specific: density / allpass feedback) */
    float mod_rate;    /* 0..1 */
    float mod_depth;   /* 0..1 */
    float er_level;    /* 0..1 */
    int freeze;
};

static inline float rv_clamp(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
static inline float rv_exp2(float x)
{
    if (x < -126.f) return 0.f;
    if (x > 126.f) x = 126.f;
    float fl = (float)(int)x;
    if (fl > x) fl -= 1.f;
    float f = x - fl;
    float p = 1.f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f + f * (0.0096181f + f * 0.0013334f))));
    union { float f; uint32_t u; } v = { p };
    v.u += (uint32_t)((int)fl) << 23;
    return v.f;
}
static inline float rv_onepole(float hz, float fs)
{
    float c = 1.f - rv_exp2(-9.0647202f * hz / fs);
    return c > 1.f ? 1.f : c;
}
static inline float rv_sin(float x)          /* sin(x) for x in [-pi, pi], ~1e-3 */
{
    const float B = 1.2732395f, C = -0.4052847f;
    float y = B * x + C * x * (x < 0.f ? -x : x);
    return 0.225f * (y * (y < 0.f ? -y : y) - y) + y;
}

/* wrapper state + pre-delay lines, at the start of the memory; the engine's memory follows */
#define RW_PRE_LEN 32768u
struct rev_wrap {
    uint32_t magic, engine, w;
    float fs, hp[2], fade, pre_d;
};
#define RW_HEAD_FLOATS 64u
#define RW_ENGINE_OFS (RW_HEAD_FLOATS + 2u * RW_PRE_LEN)

static inline void rw_init(struct rev_wrap *rw, uint32_t engine, float fs)
{
    rw->magic = 0x56455257u;   /* "WREV" */
    rw->engine = engine;
    rw->w = 0;
    rw->fs = fs;
    rw->hp[0] = rw->hp[1] = 0.f;
    rw->fade = 0.f;
    rw->pre_d = -1.f;
}

/* Before the engine: low cut and pre-delay, in place. */
static inline void rw_pre(struct rev_wrap *rw, float *mem, const struct rev_knobs *k, float *L, float *R, unsigned n)
{
    float *pl = mem + RW_HEAD_FLOATS, *pr = pl + RW_PRE_LEN;
    float c_hp = rv_onepole(20.f * rv_exp2(5.6438562f * rv_clamp(k->lowcut, 0.f, 1.f)), rw->fs);
    float target = rv_clamp(k->predelay_s * rw->fs, 0.f, (float)(RW_PRE_LEN - 2));
    if (rw->pre_d < 0.f) rw->pre_d = target;
    rw->pre_d += (target - rw->pre_d) * 0.1f;
    uint32_t d = (uint32_t)rw->pre_d, w = rw->w;
    float in_gain = k->freeze ? 0.f : 1.f;
    for (unsigned s = 0; s < n; s++) {
        float xl = L[s] * in_gain, xr = R[s] * in_gain;
        rw->hp[0] += c_hp * (xl - rw->hp[0]);
        rw->hp[1] += c_hp * (xr - rw->hp[1]);
        pl[w & (RW_PRE_LEN - 1)] = xl - rw->hp[0];
        pr[w & (RW_PRE_LEN - 1)] = xr - rw->hp[1];
        L[s] = pl[(w - d) & (RW_PRE_LEN - 1)];
        R[s] = pr[(w - d) & (RW_PRE_LEN - 1)];
        w++;
    }
    rw->w = w;
}

/* After the engine: width, level, fade-in. */
static inline void rw_post(struct rev_wrap *rw, const struct rev_knobs *k, float *L, float *R, unsigned n)
{
    float wid = 1.f + rv_clamp(k->spread, -1.f, 1.f), g = rv_clamp(k->level, 0.f, 4.f);
    float fade = rw->fade, step = 20.f / rw->fs;
    for (unsigned s = 0; s < n; s++) {
        float m = 0.5f * (L[s] + R[s]), sd = 0.5f * (L[s] - R[s]) * wid;
        float gg = g;
        if (fade < 1.f) { fade += step; gg *= fade * fade; }
        L[s] = (m + sd) * gg;
        R[s] = (m - sd) * gg;
    }
    rw->fade = fade > 1.f ? 1.f : fade;
}
#endif
