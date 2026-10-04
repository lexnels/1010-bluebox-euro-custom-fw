/*
 * Lush hall: an 8-line modulated feedback delay network (FDN) with input diffusion and early reflections.
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
    float diffusion;   /* 0..1   input diffusion */
    float density;     /* 0..1   in-loop diffusion ("Feedback" knob) */
    float predelay_s;  /* seconds, 0..1 */
    float er_time_s;   /* seconds, 0..0.09 */
    float er_db;       /* dB, -36..0 (-36 = off) */
    float level_db;    /* dB, -36..0 (-36 = off) */
    float spread;      /* -1..1  stereo width, 0 = normal */
    float lowcut;      /* 0..1   input high-pass 20 Hz .. 1 kHz */
    float hicut;       /* 0..1   damping 1 kHz .. 20 kHz */
    float bass;        /* 0..1   bass decay multiplier 0.5x .. 2x ("Resonance" knob), 0.5 = 1x */
    float mod_rate;    /* 0..1   0.05 .. 5 Hz */
    float mod_depth;   /* 0..1   0 .. ~1 ms */
    int freeze;
};

#define HALL_N 8
#define FDN_LEN   8192      /* per line */
#define AP_LEN    1024      /* per allpass */
#define PRE_LEN   65536     /* per side */

/* layout of the delay memory, in floats (state struct first) */
#define HALL_STATE_FLOATS 1024
#define OFS_PRE   HALL_STATE_FLOATS
#define OFS_FDN   (OFS_PRE + 2 * PRE_LEN)
#define OFS_IAP   (OFS_FDN + HALL_N * FDN_LEN)
#define OFS_LAP   (OFS_IAP + 8 * AP_LEN)
#define HALL_MEM_FLOATS (OFS_LAP + HALL_N * AP_LEN)          /* 214016 floats = 836 KB */

struct hall {
    uint32_t magic;
    uint32_t w;                               /* write counter */
    float fs;
    float in_lp[2], in_hp[2];                 /* input filter states */
    float bass_z[HALL_N], damp_z[HALL_N];     /* loop filter states */
    float dly[HALL_N];                        /* current read delay per line (samples, fractional) */
    float lfo_ph;                             /* 0..1 */
    float size_s;                             /* smoothed size scale */
    float pre_d;                              /* smoothed pre-delay, samples */
    float fade;                               /* output fade-in 0..1 */
};
#define HALL_MAGIC 0x4c4c4148u   /* "HALL" */

/* delay lengths in samples at 48 kHz and full size */
static const float fdn_len48[HALL_N] = { 2473.f, 2767.f, 3217.f, 3571.f, 3923.f, 4273.f, 4723.f, 5101.f };
static const float iap_len48[8] = { 229.f, 172.f, 610.f, 446.f,      /* left (Dattorro, scaled to 48 kHz) */
                                    241.f, 163.f, 587.f, 467.f };    /* right */
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
    float c_damp = h_onepole(1000.f * h_exp2(4.3219281f * h_clamp(k->hicut, 0.f, 1.f)), fs);   /* 1k..20k */
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
    float g_dif1 = 0.75f * h_clamp(k->diffusion, 0.f, 1.f), g_dif2 = 0.625f * h_clamp(k->diffusion, 0.f, 1.f);
    float g_lap = 0.6f * h_clamp(k->density, 0.f, 1.f);

    /* pre-delay and early reflections */
    float pre_t = h_clamp(k->predelay_s * fs, 0.f, 60000.f);
    if (h->pre_d < 0.f) h->pre_d = pre_t;
    h->pre_d += (pre_t - h->pre_d) * 0.1f;
    uint32_t pre = (uint32_t)h->pre_d;
    float ert = h_clamp(k->er_time_s, 0.f, 0.09f) * fs;
    uint32_t er_d[2][6];
    for (int c = 0; c < 2; c++)
        for (int j = 0; j < 6; j++)
            er_d[c][j] = pre + 1u + (uint32_t)(er_pos[c][j] * ert);
    float er_lv = k->er_db <= -35.9f ? 0.f : 0.5f * h_db(k->er_db);
    float out_lv = k->level_db <= -35.9f ? 0.f : h_db(k->level_db);
    float wid = 1.f + h_clamp(k->spread, -1.f, 1.f);                         /* 0 mono, 1 normal, 2 wide */

    /* modulation: one LFO, each line at its own phase; delays ramp linearly across the block */
    float mrate = 0.05f * h_exp2(6.6438562f * h_clamp(k->mod_rate, 0.f, 1.f));    /* 0.05 .. 5 Hz */
    float mdepth = 48.f * rate * h_clamp(k->mod_depth, 0.f, 1.f);                /* samples */
    h->lfo_ph += mrate * (float)n / fs;
    if (h->lfo_ph >= 1.f) h->lfo_ph -= 1.f;
    float d_step[HALL_N];
    for (int i = 0; i < HALL_N; i++) {
        float ph = h->lfo_ph + (float)i * 0.125f + (i & 1) * 0.0625f;
        if (ph >= 1.f) ph -= 1.f;
        float target = fdn_len48[i] * size1 + mdepth * (1.f + h_sin01(ph)) + 2.f;
        target = h_clamp(target, 4.f, (float)(FDN_LEN - 4));
        if (h->dly[i] < 4.f || h->dly[i] > (float)(FDN_LEN - 4)) h->dly[i] = target;
        d_step[i] = (target - h->dly[i]) * inv_n;
    }
    uint32_t iap_d[8], lap_d[HALL_N];
    for (int j = 0; j < 8; j++) iap_d[j] = (uint32_t)(iap_len48[j] * (0.5f + 0.5f * size0) * rate);
    for (int i = 0; i < HALL_N; i++) lap_d[i] = (uint32_t)(lap_len48[i] * (0.5f + 0.5f * size0) * rate);

    float *__restrict pre_l = mem + OFS_PRE, *__restrict pre_r = pre_l + PRE_LEN;
    float *__restrict fdn = mem + OFS_FDN, *__restrict iap = mem + OFS_IAP, *__restrict lap = mem + OFS_LAP;
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
        float erl = er_gain[0] * pre_l[(w - er_d[0][0]) & PM] + er_gain[1] * pre_r[(w - er_d[0][1]) & PM]
                  + er_gain[2] * pre_l[(w - er_d[0][2]) & PM] + er_gain[3] * pre_r[(w - er_d[0][3]) & PM]
                  + er_gain[4] * pre_l[(w - er_d[0][4]) & PM] + er_gain[5] * pre_r[(w - er_d[0][5]) & PM];
        float err = er_gain[0] * pre_r[(w - er_d[1][0]) & PM] + er_gain[1] * pre_l[(w - er_d[1][1]) & PM]
                  + er_gain[2] * pre_r[(w - er_d[1][2]) & PM] + er_gain[3] * pre_l[(w - er_d[1][3]) & PM]
                  + er_gain[4] * pre_r[(w - er_d[1][4]) & PM] + er_gain[5] * pre_l[(w - er_d[1][5]) & PM];

        /* input diffusion: 4 allpasses per side */
        const uint32_t AM = AP_LEN - 1, aw = w & AM;
#define AP(j, x, g) do { float *__restrict b_ = iap + (j) * AP_LEN; float d_ = b_[(w - iap_d[j]) & AM]; \
                         float u_ = (x) + (g) * d_; b_[aw] = u_; (x) = d_ - (g) * u_; } while (0)
        AP(0, dl, g_dif1); AP(1, dl, g_dif1); AP(2, dl, g_dif2); AP(3, dl, g_dif2);
        AP(4, dr, g_dif1); AP(5, dr, g_dif1); AP(6, dr, g_dif2); AP(7, dr, g_dif2);
#undef AP

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
        float inj_l = dl * 0.5f, inj_r = dr * 0.5f;
        fdn[0 * FDN_LEN + fw] = (b0 + b4) * hs + inj_l;
        fdn[1 * FDN_LEN + fw] = (b1 + b5) * hs + inj_r;
        fdn[2 * FDN_LEN + fw] = (b2 + b6) * hs - inj_l;
        fdn[3 * FDN_LEN + fw] = (b3 + b7) * hs - inj_r;
        fdn[4 * FDN_LEN + fw] = (b0 - b4) * hs + inj_l;
        fdn[5 * FDN_LEN + fw] = (b1 - b5) * hs + inj_r;
        fdn[6 * FDN_LEN + fw] = (b2 - b6) * hs - inj_l;
        fdn[7 * FDN_LEN + fw] = (b3 - b7) * hs - inj_r;

        /* output: tail + early reflections, width, level, fade-in */
        float ol = 0.6f * outl + er_lv * erl, orr = 0.6f * outr + er_lv * err;
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
