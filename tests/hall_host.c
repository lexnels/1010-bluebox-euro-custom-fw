/* Host test for the hall DSP: impulse responses, decay times, stability, freeze, levels.
 *   cc -O2 -o out/hall_host tests/hall_host.c -lm && out/hall_host [out.wav]   */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/hall_dsp.h"

#define FS 48000.f
#define BLK 32
static float mem[HALL_MEM_FLOATS];
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: "); } else printf("ok:   "); printf(__VA_ARGS__); printf("\n"); } while (0)

static struct hall_knobs defaults(void)
{
    /* what the device uses (src/hall_m7.c knobs_from), Time and Level at their defaults */
    struct hall_knobs k = { .size = 0.8f, .decay = 0.5141f, .diffusion = 1.0f, .density = 0.8f, .predelay_s = 0.02f,
                            .er_time_s = 0.025f, .er_db = -36.f, .level = 1.f, .spread = 0.25f, .lowcut = 0.f,
                            .hicut = 1.f, .bass = 0.6f, .mod_rate = 0.4f, .mod_depth = 0.5f, .freeze = 0 };
    return k;
}

/* run an impulse through and return the energy envelope in 10 ms frames */
static int impulse(const struct hall_knobs *k, float secs, float *env, float *outL, float *outR)
{
    struct hall *h = (struct hall *)mem;
    hall_reset(h, mem, FS);
    h->fade = 1.f;
    int total = (int)(secs * FS), frames = 0;
    float L[BLK], R[BLK], acc = 0;
    for (int s = 0; s < total; s += BLK) {
        for (int i = 0; i < BLK; i++) L[i] = R[i] = (s + i == 0) ? 1.f : 0.f;
        hall_process_block(h, mem, k, L, R, BLK);
        for (int i = 0; i < BLK; i++) {
            if (outL) { outL[s + i] = L[i]; outR[s + i] = R[i]; }
            acc += L[i] * L[i] + R[i] * R[i];
            if ((s + i + 1) % 480 == 0) { env[frames++] = acc; acc = 0; }
        }
    }
    return frames;
}

static float ned(const float *x, int at)
{
    const int w = 960;   /* 20 ms */
    double e = 0; int c = 0;
    for (int i = at - w / 2; i < at + w / 2; i++) e += x[i] * x[i];
    float sd = sqrt(e / w);
    for (int i = at - w / 2; i < at + w / 2; i++) c += fabsf(x[i]) > sd;
    return c / (float)w / 0.3173f;
}

/* How metallic/echoey a tail is: (a) the strongest repeat in its autocorrelation (lags 2..80 ms), (b) how far the
 * tallest spectral peaks stand above the typical level (narrow resonances ring). Measured on 0.25..0.42 s. */
static void ringiness(const float *x, float *ac_peak, float *spec_peak_db)
{
    const int off = 12000, n = 8192;
    double e = 0, best = 0;
    for (int i = 0; i < n; i++) e += x[off + i] * x[off + i];
    for (int lag = 96; lag < 3840; lag++) {
        double c = 0;
        for (int i = 0; i < n; i++) c += x[off + i] * x[off + i + lag];
        if (c / e > best) best = c / e;
    }
    *ac_peak = best;
    static float mag[4096];
    for (int b = 1; b < 2048; b++) {               /* up to 12 kHz */
        double re = 0, im = 0, w = 2 * M_PI * b / n;
        for (int i = 0; i < n; i++) { double hw = 0.5 - 0.5 * cos(2 * M_PI * i / n); re += hw * x[off + i] * cos(w * i); im -= hw * x[off + i] * sin(w * i); }
        mag[b] = re * re + im * im;
    }
    /* peak vs local median-ish (mean of a +-40 bin window), worst over 100 Hz..10 kHz */
    float worst = 0;
    for (int b = 20; b < 1700; b++) {
        double m = 0; for (int j = -40; j <= 40; j++) m += mag[b + j]; m /= 81;
        float r = 10 * log10(mag[b] / m);
        if (r > worst) worst = r;
    }
    *spec_peak_db = worst;
}

/* RT60 from the slope of the energy decay curve (Schroeder integration), -5 to -25 dB */
static float rt60(const float *env, int n)
{
    double tot = 0, *edc = malloc(n * sizeof(double));
    for (int i = n - 1; i >= 0; i--) { tot += env[i]; edc[i] = tot; }
    int i5 = -1, i25 = -1;
    for (int i = 0; i < n; i++) {
        double db = 10 * log10(edc[i] / edc[0] + 1e-30);
        if (i5 < 0 && db < -5) i5 = i;
        if (i25 < 0 && db < -25) i25 = i;
    }
    free(edc);
    if (i5 < 0 || i25 < 0) return -1;
    return (i25 - i5) * 0.01f * 3.f;   /* 20 dB span x3 */
}

static void wav(const char *path, const float *L, const float *R, int n)
{
    FILE *f = fopen(path, "wb");
    int bytes = n * 4;
    int hdr[] = { 0x46464952, 36 + bytes, 0x45564157, 0x20746d66, 16, 0x00020001, 48000, 48000 * 4, 0x00100004, 0x61746164, bytes };
    fwrite(hdr, 4, 11, f);
    for (int i = 0; i < n; i++) {
        short s[2] = { (short)(fmaxf(-1, fminf(1, L[i])) * 32767), (short)(fmaxf(-1, fminf(1, R[i])) * 32767) };
        fwrite(s, 2, 2, f);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    static float env[4000], L[48000 * 40], R[48000 * 40];
    struct hall_knobs k = defaults();
    printf("delay memory: %d floats (%d KB) of 262144\n", HALL_MEM_FLOATS, HALL_MEM_FLOATS * 4 / 1024);
    CHECK(HALL_MEM_FLOATS <= 262144, "fits the stock reverb buffer");

    /* decay knob tracks RT60 */
    float knobs[] = { 0.f, 0.25f, 0.5f, 0.75f };
    for (int j = 0; j < 4; j++) {
        k = defaults(); k.decay = knobs[j]; k.hicut = 1.f; k.bass = 0.5f;
        float want = 0.3f * powf(100.f, knobs[j]);
        int n = impulse(&k, want * 2.5f + 1.f, env, NULL, NULL);
        float got = rt60(env, n);
        CHECK(got > want * 0.6f && got < want * 1.5f, "decay %.2f: RT60 %.2f s (target %.2f s)", knobs[j], got, want);
    }

    /* default sound: level, stereo, echo density */
    k = defaults();
    int n = impulse(&k, 6.f, env, L, R);
    double eL = 0, eR = 0, cLR = 0; float peak = 0;
    for (int i = 0; i < 48000 * 6; i++) { eL += L[i] * L[i]; eR += R[i] * R[i]; cLR += L[i] * R[i]; peak = fmaxf(peak, fmaxf(fabsf(L[i]), fabsf(R[i]))); }
    float corr = cLR / sqrt(eL * eR);
    printf("      default: RT60 %.2f s, energy L %.3f R %.3f, L/R correlation %.2f, peak %.3f\n", rt60(env, n), eL, eR, corr, peak);
    CHECK(fabs(10 * log10(eL / eR)) < 1.5, "left/right balanced");
    CHECK(fabsf(corr) < 0.3f, "stereo decorrelated");
    CHECK(peak < 1.f && peak > 0.02f, "sane level");
    /* normalised echo density (Abel & Huang): ~1 once the tail is noise-like, low while echoes are still discrete */
    int echoes = 0;
    for (int i = 960; i < 4800; i++) echoes += fabsf(L[i]) > 1e-3f * peak;
    CHECK(echoes > 2500, "dense onset: %d of 3840 samples carry echoes in 20..100 ms", echoes);
    CHECK(ned(L, 7200) > 0.85f && ned(L, 9600) > 0.85f, "tail is noise-like by 150 ms (echo density %.2f, %.2f at 200 ms)", ned(L, 7200), ned(L, 9600));
    if (argc > 1) wav(argv[1], L, R, 48000 * 6);
    float ac_new, sp_new;
    ringiness(L, &ac_new, &sp_new);
    printf("      tallest spectral peak in the tail: %.1f dB above its neighbourhood\n", sp_new);
    CHECK(ac_new < 0.25f, "no audible repeats in the tail (autocorrelation peak %.2f)", ac_new);

    /* width: spread -1 is mono */
    k = defaults(); k.spread = -1.f;
    impulse(&k, 1.f, env, L, R);
    float diff = 0; for (int i = 0; i < 48000; i++) diff = fmaxf(diff, fabsf(L[i] - R[i]));
    CHECK(diff < 1e-6f, "spread -1 gives mono");

    /* freeze holds the tail and ignores new input; extreme knobs stay stable */
    {
        struct hall *h = (struct hall *)mem;
        k = defaults(); k.decay = 1.f; k.size = 1.f; k.density = 1.f; k.diffusion = 1.f; k.mod_depth = 1.f; k.mod_rate = 1.f; k.bass = 1.f; k.hicut = 1.f;
        hall_reset(h, mem, FS); h->fade = 1.f;
        float blkL[BLK], blkR[BLK]; double e1 = 0, e2 = 0, emax = 0;
        srand(1);
        for (int s = 0; s < 48000 * 30; s += BLK) {
            k.freeze = s >= 48000 * 2;
            for (int i = 0; i < BLK; i++) blkL[i] = blkR[i] = (s < 48000) ? (rand() / (float)RAND_MAX - 0.5f) : (s > 48000 * 10 ? 0.9f * ((s / BLK) & 1 ? 1 : -1) : 0);
            hall_process_block(h, mem, &k, blkL, blkR, BLK);
            for (int i = 0; i < BLK; i++) {
                double e = blkL[i] * blkL[i];
                if (s >= 48000 * 3 && s < 48000 * 4) e1 += e;
                if (s >= 48000 * 29) e2 += e;
                if (e > emax) emax = e;
            }
        }
        float drop = 10 * log10(e2 / e1);
        CHECK(drop > -6.f && drop < 1.f, "freeze holds the tail for 25 s (%.1f dB), ignores input", drop);
        CHECK(emax < 16.0, "no blow-up at extreme settings (peak %.2f)", sqrt(emax));
    }
    /* a reset fades in */
    {
        struct hall *h = (struct hall *)mem;
        k = defaults(); hall_reset(h, mem, FS);
        float blkL[BLK], blkR[BLK];
        for (int i = 0; i < BLK; i++) blkL[i] = blkR[i] = 1.f;
        hall_process_block(h, mem, &k, blkL, blkR, BLK);
        CHECK(fabsf(blkL[BLK - 1]) < 1e-3f, "fades in after a reset");
    }
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
