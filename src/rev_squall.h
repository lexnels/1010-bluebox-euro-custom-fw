/*
 * Squall: the Mutable Instruments Clouds reverb, as voiced in Daniel Majid Mirzakhani's Squall for the Korg NTS-1
 * (github.com/DanielMajid/squall_reverb, src/reverb.h). Reverb core copyright 2014 Emilie Gillet, MIT licence.
 * Rewritten here in plain C on the same FxEngine semantics: one 16384-float ring, a write pointer that counts down,
 * delay line k starting at the sum of the earlier lines' lengths (+1 each).
 *
 *   4 allpasses (one LFO-smeared) -> two cross-coupled loops, each: modulated/plain delay, low-pass, 2 allpasses.
 *
 *   Time      decay (Squall's DPTH curve: 0.35 .. 0.98 loop gain)
 *   HI C      tone (loop low-pass, Squall's 0.6 .. 0.97)
 *   Diffusion allpass coefficient 0.5 .. 0.74 (Squall's range)
 *   Spread    stereo width (wrapper)
 *   Size      loop delays x0.5 .. x2 (x1 = Clouds' tuning at the middle), gliding
 *   Freeze    loop gain 1, low-pass open, input muted
 */
#ifndef REV_SQUALL_H
#define REV_SQUALL_H
#include "rev_common.h"

#define SQ_SIZE 32768u           /* room for the loop lines at Size x2 */
#define SQ_FIRST_LOOP 4          /* lines 4.. scale with Size */
static const uint16_t sq_len[10] = { 113, 162, 241, 399, 1653, 2038, 3411, 1913, 1663, 4782 };
enum { SQ_AP1, SQ_AP2, SQ_AP3, SQ_AP4, SQ_DAP1A, SQ_DAP1B, SQ_DEL1, SQ_DAP2A, SQ_DAP2B, SQ_DEL2 };

struct sq_state {
    int32_t wp;
    uint32_t base[10];
    float lp1, lp2;
    float lfo_ph[2], lfo_val[2];
    uint32_t count;
    float scale_s;               /* smoothed Size scale, < 0 = not set yet */
};
#define SQ_STATE_FLOATS 64u
#define SQ_MEM_FLOATS (SQ_STATE_FLOATS + SQ_SIZE)

static void sq_init(float *m, float fs)
{
    (void)fs;
    struct sq_state *st = (struct sq_state *)m;
    uint32_t b = 0;
    for (int i = 0; i < 10; i++) { st->base[i] = b; b += sq_len[i] * (i >= SQ_FIRST_LOOP ? 2u : 1u) + 1u; }
    st->scale_s = -1.f;
    st->wp = 0;
    st->lp1 = st->lp2 = 0.f;
    st->lfo_ph[0] = st->lfo_ph[1] = 0.f;
    st->lfo_val[0] = st->lfo_val[1] = 0.5f;
    st->count = 0;
}

static void sq_process(float *m, const struct rev_knobs *k, float *L, float *R, unsigned n)
{
    struct sq_state *st = (struct sq_state *)m;
    float *buf = m + SQ_STATE_FLOATS;
    const uint32_t MASK = SQ_SIZE - 1;
    float t = rv_clamp(k->time * 0.5f, 0.f, 1.f);
    float krt = 0.35f + 0.63f * t;
    float klp = 0.6f + 0.37f * rv_clamp(k->hicut, 0.f, 1.f);
    float kap = 0.5f + 0.24f * rv_clamp(k->diffusion, 0.f, 1.f);
    float gain = 0.2f;
    float scale_t = rv_exp2(2.f * (rv_clamp(k->size, 0.f, 1.f) - 0.5f));
    if (st->scale_s < 0.f) st->scale_s = scale_t;
    st->scale_s += (scale_t - st->scale_s) * 0.02f;       /* glide: Size bends pitch, never clicks */
    const float sc = st->scale_s;
    uint32_t len[10];
    for (int i = 0; i < 10; i++)
        len[i] = i >= SQ_FIRST_LOOP ? (uint32_t)((float)sq_len[i] * sc) : sq_len[i];
    if (k->freeze) { krt = 1.f; klp = 1.f; gain = 0.f; }
    /* LFOs: Clouds' 0.5 Hz and 0.3 Hz at its 32 kHz rate; refreshed every 32 samples like FxEngine */
    const float inc[2] = { 0.5f / 32000.f * 32.f, 0.3f / 32000.f * 32.f };
    float lp1 = st->lp1, lp2 = st->lp2;
    int32_t wp = st->wp;
#define AT(line, ofs) buf[(uint32_t)(wp + (int32_t)st->base[line] + (int32_t)(ofs)) & MASK]
#define TAIL(line) AT(line, len[line] - 1)
    for (unsigned s = 0; s < n; s++) {
        if (--wp < 0) wp += SQ_SIZE;
        if ((st->count++ & 31u) == 0) {
            for (int i = 0; i < 2; i++) {
                st->lfo_ph[i] += inc[i];
                if (st->lfo_ph[i] >= 1.f) st->lfo_ph[i] -= 1.f;
                st->lfo_val[i] = 0.5f + 0.5f * rv_sin(st->lfo_ph[i] * 6.2831853f - 3.1415927f);   /* 0..1 */
            }
        }
        float acc, prev, apout, wet;
        /* c.Interpolate(ap1, 10, LFO_1, 60, 1); c.Write(ap1, 100, 0) */
        {
            float o = 10.f + 60.f * st->lfo_val[0];
            int32_t oi = (int32_t)o;
            float fr = o - (float)oi, a = AT(SQ_AP1, oi), b = AT(SQ_AP1, oi + 1);
            acc = a + (b - a) * fr;
            AT(SQ_AP1, 100) = acc;
            acc = 0.f;
        }
        acc += (L[s] + R[s]) * gain;
        /* input allpasses: Read(ap TAIL, kap); WriteAllPass(ap, -kap) */
        for (int i = SQ_AP1; i <= SQ_AP4; i++) {
            prev = TAIL(i);
            acc += prev * kap;
            AT(i, 0) = acc;
            acc = acc * -kap + prev;
        }
        apout = acc;

        /* left loop */
        {
            float o = (4680.f + 100.f * st->lfo_val[1]) * sc;
            int32_t oi = (int32_t)o;
            float fr = o - (float)oi, a = AT(SQ_DEL2, oi), b = AT(SQ_DEL2, oi + 1);
            acc = apout + (a + (b - a) * fr) * krt;
        }
        lp1 += klp * (acc - lp1); acc = lp1;
        prev = TAIL(SQ_DAP1A); acc += prev * -kap; AT(SQ_DAP1A, 0) = acc; acc = acc * kap + prev;
        prev = TAIL(SQ_DAP1B); acc += prev * kap; AT(SQ_DAP1B, 0) = acc; acc = acc * -kap + prev;
        AT(SQ_DEL1, 0) = acc; acc *= 2.f;
        wet = acc;
        L[s] = wet;

        /* right loop */
        acc = apout + TAIL(SQ_DEL1) * krt;
        lp2 += klp * (acc - lp2); acc = lp2;
        prev = TAIL(SQ_DAP2A); acc += prev * kap; AT(SQ_DAP2A, 0) = acc; acc = acc * -kap + prev;
        prev = TAIL(SQ_DAP2B); acc += prev * -kap; AT(SQ_DAP2B, 0) = acc; acc = acc * kap + prev;
        AT(SQ_DEL2, 0) = acc; acc *= 2.f;
        wet = acc;
        R[s] = wet;
    }
#undef AT
#undef TAIL
    st->lp1 = lp1; st->lp2 = lp2; st->wp = wp;
}
#endif
