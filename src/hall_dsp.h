/*
 * FDN Hall: an 8-line modulated feedback delay network (FDN) with input diffusion and early reflections.
 * Pure DSP, no firmware addresses: builds for the M7 cave and for the host test (tests/hall_host.c).
 *
 *   in L/R -> low cut, high cut -> pre-delay -+-> early reflections (taps on the pre-delay line) ---------------+
 *                                             +-> 4 allpasses per side (diffusion) -> 8-line FDN -> out taps -+-> width, level -> out
 *
 * FDN: each line is read with a slowly modulated fractional delay, passed through a two-band decay (bass and
 * mid/high gains set from the decay time) and a one-pole damping low-pass, then an allpass (in-loop diffusion).
 * An 8x8 Hadamard matrix mixes the lines back into each other. All delay memory is one float array the caller
 * provides (the stock reverb's 1 MB buffer on the device); every line is a power of two long, indexed by one
 * shared write counter.
 */
#include <stdint.h>

/* knobs, already scaled to 0..1 (or as noted) by the caller */
struct hall_knobs {
    float size;        /* 0..1   room size (delay lengths) */
    float decay;       /* 0..1   decay time 0.3 s .. 30 s */
    float diffusion;   /* 0..1   input diffusion span: tight .. wide smear */
    float density;     /* 0..1   in-loop diffusion ("Feedback" knob) */
    float predelay_s;  /* seconds, 0..1 */
    float er_time_s;   /* seconds, 0..0.09 */
    float er_db;       /* dB, -36..0 (-36 = off) */
    float level;       /* output gain, linear (1 = unity) */
    float spread;      /* -1..1  stereo width, 0 = normal */
    float lowcut;      /* 0..1   input high-pass 20 Hz .. 1 kHz */
    float hicut;       /* 0..1   tone: input low-pass 1.5k .. 20 kHz, damping 1.5k .. 6 kHz */
    float bass;        /* 0..1   bass decay multiplier 0.5x .. 2x ("Resonance" knob), 0.5 = 1x */
    float mod_rate;    /* 0..1   0.05 .. 5 Hz */
    float mod_depth;   /* 0..1   0 .. ~1 ms */
    int freeze;
};

#define HALL_N 8
#define FDN_LEN   8192      /* per line */
#define AP_LEN    1024      /* per allpass */
#define PRE_LEN   32768     /* per side: 0.68 s at 48 kHz */
#define DIF_LEN   4096      /* per diffuser channel, all stages */

/* layout of the delay memory, in floats (state struct first) */
#define HALL_STATE_FLOATS 1024
#define OFS_PRE   HALL_STATE_FLOATS
#define OFS_FDN   (OFS_PRE + 2 * PRE_LEN)
#define OFS_DIF   (OFS_FDN + HALL_N * FDN_LEN)
#define DIF_STAGES 3
#define OFS_LAP   (OFS_DIF + DIF_STAGES * HALL_N * DIF_LEN)
#define HALL_MEM_FLOATS (OFS_LAP + HALL_N * AP_LEN)          /* 238592 floats = 932 KB */

struct hall {
    uint32_t magic;
    uint32_t w;                               /* write counter */
    float fs;
    float in_lp[2], in_hp[2];                 /* input filter states */
    float bass_z[HALL_N], damp_z[HALL_N];     /* loop filter states */
    float dly[HALL_N];                        /* current read delay per line (samples, fractional) */
    float lfo_ph[HALL_N];                     /* 0..1, one LFO per line, each at its own rate */
    float size_s;                             /* smoothed size scale */
    float pre_d;                              /* smoothed pre-delay, samples */
    float fade;                               /* output fade-in 0..1 */
};
#define HALL_MAGIC 0x4c4c4148u   /* "HALL" */

/* delay lengths in samples at 48 kHz and full size */
static const float fdn_len48[HALL_N] = { 2473.f, 2767.f, 3217.f, 3571.f, 3923.f, 4273.f, 4723.f, 5101.f };
/* input diffuser: 3 stages of 8 channels. In each stage every channel gets its own delay (spread across the stage's
 * span), then the channels are shuffled, some flipped, and mixed by a Hadamard matrix: 8^3 = 512 echoes from one click
 * within ~70 ms, so the onset is already a smooth cloud (Signalsmith's "Let's write a reverb", MIT). */
static const float dif_span48[DIF_STAGES] = { 480.f, 1056.f, 2208.f };   /* 10, 22, 46 ms at full size */
static const float dif_frac[DIF_STAGES][HALL_N] = {
    { 0.07f, 0.98f, 0.31f, 0.62f, 0.19f, 0.83f, 0.45f, 0.71f },
    { 0.55f, 0.13f, 0.88f, 0.36f, 0.95f, 0.24f, 0.67f, 0.42f },
    { 0.29f, 0.76f, 0.05f, 0.92f, 0.51f, 0.17f, 0.85f, 0.60f } };
static const uint8_t dif_perm[DIF_STAGES][HALL_N] = { { 3, 6, 0, 5, 1, 7, 2, 4 }, { 5, 2, 7, 0, 6, 3, 4, 1 }, { 1, 4, 6, 2, 7, 0, 5, 3 } };
static const float dif_sign[DIF_STAGES][HALL_N] = {
    { 1, -1, 1, 1, -1, 1, -1, -1 }, { -1, 1, 1, -1, 1, -1, -1, 1 }, { 1, 1, -1, 1, -1, -1, 1, -1 } };
static const float lfo_rate[HALL_N] = { 0.70f, 1.33f, 0.91f, 1.17f, 0.78f, 1.25f, 1.04f, 0.84f };
static const float lap_len48[HALL_N] = { 173.f, 211.f, 263.f, 293.f, 331.f, 367.f, 401.f, 449.f };
/* early reflections: tap position as a fraction of ER time, and gain (sign = polarity) */
static const float er_pos[2][6] = { { 0.11f, 0.23f, 0.37f, 0.52f, 0.71f, 1.00f },
                                    { 0.08f, 0.19f, 0.31f, 0.47f, 0.66f, 0.93f } };
static const float er_gain[6] = { 0.80f, -0.68f, 0.57f, -0.47f, 0.39f, -0.31f };

/* --- small math, no libm --- */
static inline float h_exp2(float x)                 /* 2^x, ~1e-4 relative, x in [-126, 126] */
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
static inline float h_db(float db) { return h_exp2(db * 0.16609640f); }
static inline float h_onepole(float hz, float fs)    /* coefficient for y += c * (x - y) */
{
    float c = 1.f - h_exp2(-9.0647202f * hz / fs);   /* 1 - e^(-2 pi f / fs) */
    return c > 1.f ? 1.f : c;
}
static inline float h_sin01(float ph)               /* sin(2 pi ph), ph in [0, 1) */
{
    float x = ph < 0.5f ? ph * 4.f - 1.f : 3.f - ph * 4.f;      /* triangle -1..1 */
    return x * (1.5f - 0.5f * x * x);                            /* soft triangle, close to a sine */
}
static inline float h_clamp(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }

/* Start from silence. The delay memory must already be zero (hall_reset clears it). */
static void hall_init(struct hall *h, float fs)
{
    h->magic = HALL_MAGIC;
    h->fs = fs;
    h->size_s = -1.f;
    h->pre_d = -1.f;
    h->fade = 0.f;
    for (int i = 0; i < HALL_N; i++)
        h->lfo_ph[i] = (float)i * 0.125f;
}

static void hall_reset(struct hall *h, float *mem, float fs)
{
    for (uint32_t i = 0; i < HALL_MEM_FLOATS; i++)
        mem[i] = 0.f;
    hall_init(h, fs);
}

/* Process n frames in place: L/R hold the reverb send on entry, the wet reverb on return. */
static void hall_process_block(struct hall *h, float *mem, const struct hall_knobs *k,
                               float *__restrict L, float *__restrict R, unsigned n)
{
    if (!n) return;
    const float fs = h->fs, rate = fs * (1.f / 48000.f), inv_n = 1.f / (float)n;

    /* --- per-block coefficients --- */
    float size_t = (0.3f + 0.7f * h_clamp(k->size, 0.f, 1.f)) * rate;
    if (h->size_s < 0.f) h->size_s = size_t;
    float size0 = h->size_s;
    h->size_s += (size_t - h->size_s) * 0.02f;                  /* glide: size changes bend pitch, never click */
    float size1 = h->size_s;

    float rt = 0.3f * h_exp2(6.6438562f * h_clamp(k->decay, 0.f, 1.f));    /* 0.3 .. 30 s */
    float bassm = 0.5f * h_exp2(2.f * h_clamp(k->bass, 0.f, 1.f));          /* 0.5 .. 2 */
    float c_damp = h_onepole(1500.f * h_exp2(2.f * h_clamp(k->hicut, 0.f, 1.f)), fs);         /* 1.5k..6k */
    float c_hp = h_onepole(20.f * h_exp2(5.6438562f * h_clamp(k->lowcut, 0.f, 1.f)), fs);      /* 20..1k */
    float c_inlp = h_onepole(1500.f * h_exp2(3.7369656f * h_clamp(k->hicut, 0.f, 1.f)), fs);   /* 1.5k..20k */
    float c_bass = h_onepole(250.f, fs);
    float g_mid[HALL_N], g_low[HALL_N];
    for (int i = 0; i < HALL_N; i++) {
        float len = fdn_len48[i] * size1;
        if (k->freeze) { g_mid[i] = g_low[i] = 1.f; continue; }
        g_mid[i] = h_exp2(-9.9657843f * len / (rt * fs));            /* 10^(-3 len / (rt fs)): -60 dB after rt */
        g_low[i] = h_exp2(-9.9657843f * len / (rt * bassm * fs));
        if (g_low[i] > 0.9995f) g_low[i] = 0.9995f;
    }
    if (k->freeze) c_damp = 1.f;
    float in_gain = k->freeze ? 0.f : 1.f;
    float dif_span = (0.3f + 0.7f * h_clamp(k->diffusion, 0.f, 1.f)) * (0.5f + 0.5f * size0) * rate;
    float g_lap = 0.6f * h_clamp(k->density, 0.f, 1.f);

    /* pre-delay and early reflections */
    float pre_t = h_clamp(k->predelay_s * fs, 0.f, (float)(PRE_LEN - 5000));
    if (h->pre_d < 0.f) h->pre_d = pre_t;
    h->pre_d += (pre_t - h->pre_d) * 0.1f;
    uint32_t pre = (uint32_t)h->pre_d;
    float ert = h_clamp(k->er_time_s, 0.f, 0.09f) * fs;
    uint32_t er_d[2][6];
    for (int c = 0; c < 2; c++)
        for (int j = 0; j < 6; j++)
            er_d[c][j] = pre + 1u + (uint32_t)(er_pos[c][j] * ert);
    float er_lv = k->er_db <= -35.9f ? 0.f : 0.5f * h_db(k->er_db);
    float out_lv = h_clamp(k->level, 0.f, 4.f);
    float wid = 1.f + h_clamp(k->spread, -1.f, 1.f);                         /* 0 mono, 1 normal, 2 wide */

    /* modulation: one LFO, each line at its own phase; delays ramp linearly across the block */
    float mrate = 0.05f * h_exp2(6.6438562f * h_clamp(k->mod_rate, 0.f, 1.f));    /* 0.05 .. 5 Hz */
    float mdepth = 48.f * rate * h_clamp(k->mod_depth, 0.f, 1.f);                /* samples */
    float d_step[HALL_N];
    for (int i = 0; i < HALL_N; i++) {
        /* rates spread 0.7x .. 1.33x so the lines never move in step (in-step modulation sounds like chorus, not air) */
        float ph = h->lfo_ph[i] + mrate * lfo_rate[i] * (float)n / fs;
        if (ph >= 1.f) ph -= 1.f;
        h->lfo_ph[i] = ph;
        float target = fdn_len48[i] * size1 + mdepth * (1.f + h_sin01(ph)) + 2.f;
        target = h_clamp(target, 4.f, (float)(FDN_LEN - 4));
        if (h->dly[i] < 4.f || h->dly[i] > (float)(FDN_LEN - 4)) h->dly[i] = target;
        d_step[i] = (target - h->dly[i]) * inv_n;
    }
    uint32_t dif_d[DIF_STAGES][HALL_N], lap_d[HALL_N];
    for (int st = 0; st < DIF_STAGES; st++)
        for (int c = 0; c < HALL_N; c++)
            dif_d[st][c] = 1u + (uint32_t)(dif_span48[st] * dif_frac[st][c] * dif_span);
    for (int i = 0; i < HALL_N; i++) lap_d[i] = (uint32_t)(lap_len48[i] * (0.5f + 0.5f * size0) * rate);

    float *__restrict pre_l = mem + OFS_PRE, *__restrict pre_r = pre_l + PRE_LEN;
    float *__restrict fdn = mem + OFS_FDN, *__restrict dif = mem + OFS_DIF, *__restrict lap = mem + OFS_LAP;
    float dly[HALL_N], bz[HALL_N], dz[HALL_N];
    for (int i = 0; i < HALL_N; i++) { dly[i] = h->dly[i]; bz[i] = h->bass_z[i]; dz[i] = h->damp_z[i]; }
    float lp0 = h->in_lp[0], lp1 = h->in_lp[1], hp0 = h->in_hp[0], hp1 = h->in_hp[1];
    float fade = h->fade, fade_step = 20.f / fs;     /* 50 ms fade-in after a reset */
    uint32_t w = h->w;

    for (unsigned s = 0; s < n; s++) {
        /* input filters */
        float xl = L[s] * in_gain, xr = R[s] * in_gain;
        lp0 += c_inlp * (xl - lp0);
        lp1 += c_inlp * (xr - lp1);
        hp0 += c_hp * (lp0 - hp0);
        hp1 += c_hp * (lp1 - hp1);
        xl = lp0 - hp0;
        xr = lp1 - hp1;

        /* pre-delay line, early reflections */
        const uint32_t PM = PRE_LEN - 1;
        pre_l[w & PM] = xl;
        pre_r[w & PM] = xr;
        float dl = pre_l[(w - pre) & PM], dr = pre_r[(w - pre) & PM];
        float erl = 0.f, err = 0.f;
        if (er_lv != 0.f) {
            erl = er_gain[0] * pre_l[(w - er_d[0][0]) & PM] + er_gain[1] * pre_r[(w - er_d[0][1]) & PM]
                      + er_gain[2] * pre_l[(w - er_d[0][2]) & PM] + er_gain[3] * pre_r[(w - er_d[0][3]) & PM]
                      + er_gain[4] * pre_l[(w - er_d[0][4]) & PM] + er_gain[5] * pre_r[(w - er_d[0][5]) & PM];
            err = er_gain[0] * pre_r[(w - er_d[1][0]) & PM] + er_gain[1] * pre_l[(w - er_d[1][1]) & PM]
                      + er_gain[2] * pre_r[(w - er_d[1][2]) & PM] + er_gain[3] * pre_l[(w - er_d[1][3]) & PM]
                      + er_gain[4] * pre_r[(w - er_d[1][4]) & PM] + er_gain[5] * pre_l[(w - er_d[1][5]) & PM];
        }

        /* input diffusion: 3 stages x 8 channels */
        const uint32_t AM = AP_LEN - 1, aw = w & AM, DM = DIF_LEN - 1, dw = w & DM;
        float x8[HALL_N] = { dl, dr, -dl, -dr, dl, -dr, -dl, dr };
#pragma GCC unroll 3
        for (int st = 0; st < DIF_STAGES; st++) {
            float t[HALL_N];
#pragma GCC unroll 8
            for (int c = 0; c < HALL_N; c++) {
                float *__restrict b_ = dif + (st * HALL_N + c) * DIF_LEN;
                b_[dw] = x8[c];
                t[c] = b_[(w - dif_d[st][c]) & DM];
            }
#pragma GCC unroll 8
            for (int c = 0; c < HALL_N; c++) x8[c] = dif_sign[st][c] * t[dif_perm[st][c]];
            float p0 = x8[0] + x8[1], p1 = x8[0] - x8[1], p2 = x8[2] + x8[3], p3 = x8[2] - x8[3];
            float p4 = x8[4] + x8[5], p5 = x8[4] - x8[5], p6 = x8[6] + x8[7], p7 = x8[6] - x8[7];
            float q0 = p0 + p2, q1 = p1 + p3, q2 = p0 - p2, q3 = p1 - p3;
            float q4 = p4 + p6, q5 = p5 + p7, q6 = p4 - p6, q7 = p5 - p7;
            const float hn = 0.35355339f;
            x8[0] = (q0 + q4) * hn; x8[1] = (q1 + q5) * hn; x8[2] = (q2 + q6) * hn; x8[3] = (q3 + q7) * hn;
            x8[4] = (q0 - q4) * hn; x8[5] = (q1 - q5) * hn; x8[6] = (q2 - q6) * hn; x8[7] = (q3 - q7) * hn;
        }
        dl = 0.5f * (x8[0] - x8[2] + x8[4] - x8[6]);
        dr = 0.5f * (x8[1] - x8[3] + x8[5] - x8[7]);

        /* FDN: read, two-band decay, damping, in-loop allpass */
        const uint32_t FM = FDN_LEN - 1, fw = w & FM;
        float y[HALL_N];
#pragma GCC unroll 8
        for (int i = 0; i < HALL_N; i++) {
            const float *__restrict b = fdn + i * FDN_LEN;
            dly[i] += d_step[i];
            uint32_t di = (uint32_t)dly[i];
            float fr = dly[i] - (float)di;
            float a = b[(w - di) & FM], c = b[(w - di - 1u) & FM];
            float v = a + fr * (c - a);
            bz[i] += c_bass * (v - bz[i]);
            v = g_low[i] * bz[i] + g_mid[i] * (v - bz[i]);
            dz[i] += c_damp * (v - dz[i]);
            float *__restrict ab = lap + i * AP_LEN;
            float d = ab[(w - lap_d[i]) & AM];
            float u = dz[i] + g_lap * d;
            ab[aw] = u;
            y[i] = d - g_lap * u;
        }
        float outl = y[0] - y[2] + y[4] - y[6] + 0.5f * (y[1] - y[7]);
        float outr = y[1] - y[3] + y[5] - y[7] + 0.5f * (y[6] - y[0]);

        /* Hadamard 8 (unnormalised butterflies, scaled once) */
        float a0 = y[0] + y[1], a1 = y[0] - y[1], a2 = y[2] + y[3], a3 = y[2] - y[3];
        float a4 = y[4] + y[5], a5 = y[4] - y[5], a6 = y[6] + y[7], a7 = y[6] - y[7];
        float b0 = a0 + a2, b1 = a1 + a3, b2 = a0 - a2, b3 = a1 - a3;
        float b4 = a4 + a6, b5 = a5 + a7, b6 = a4 - a6, b7 = a5 - a7;
        const float hs = 0.35355339f;   /* 1/sqrt(8) */
        const float gi = 0.5f;
        fdn[0 * FDN_LEN + fw] = (b0 + b4) * hs + gi * x8[0];
        fdn[1 * FDN_LEN + fw] = (b1 + b5) * hs + gi * x8[1];
        fdn[2 * FDN_LEN + fw] = (b2 + b6) * hs + gi * x8[2];
        fdn[3 * FDN_LEN + fw] = (b3 + b7) * hs + gi * x8[3];
        fdn[4 * FDN_LEN + fw] = (b0 - b4) * hs + gi * x8[4];
        fdn[5 * FDN_LEN + fw] = (b1 - b5) * hs + gi * x8[5];
        fdn[6 * FDN_LEN + fw] = (b2 - b6) * hs + gi * x8[6];
        fdn[7 * FDN_LEN + fw] = (b3 - b7) * hs + gi * x8[7];

        /* output: tail + early reflections, width, level, fade-in */
        /* the diffused input bridges the gap before the first FDN round trip, so the onset is a cloud, not echoes */
        float ol = 0.6f * outl + er_lv * erl + 0.35f * dl, orr = 0.6f * outr + er_lv * err + 0.35f * dr;
        float m = 0.5f * (ol + orr), sd = 0.5f * (ol - orr) * wid;
        float g = out_lv;
        if (fade < 1.f) { fade += fade_step; g *= fade * fade; }
        L[s] = (m + sd) * g;
        R[s] = (m - sd) * g;
        w++;
    }
    for (int i = 0; i < HALL_N; i++) { h->dly[i] = dly[i]; h->bass_z[i] = bz[i]; h->damp_z[i] = dz[i]; }
    h->in_lp[0] = lp0; h->in_lp[1] = lp1; h->in_hp[0] = hp0; h->in_hp[1] = hp1;
    h->fade = fade > 1.f ? 1.f : fade;
    h->w = w;

    /* guard: if anything blew up (it shouldn't), start over silently */
    float chk = h->damp_z[0] + h->damp_z[3] + h->bass_z[5];
    if (!(chk > -1e4f && chk < 1e4f))
        hall_reset(h, mem, fs);
}
