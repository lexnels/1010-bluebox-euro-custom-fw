/* Host test for the alternative reverbs (MVerb, Squall, Freeverb) and their wrapper.
 *   cc -O2 -o out/rev_host tests/rev_host.c -lm && out/rev_host [wav-dir]   */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "../src/rev_common.h"
#include "../src/rev_freeverb.h"
#include "../src/rev_squall.h"
#include "../src/rev_mverb.h"

#define FS 48000.f
#define BLK 32
static float mem[262144];
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: "); } else printf("ok:   "); printf(__VA_ARGS__); printf("\n"); } while (0)

typedef void (*init_fn)(float *, float);
typedef void (*proc_fn)(float *, const struct rev_knobs *, float *, float *, unsigned);
struct eng { const char *name; init_fn init; proc_fn proc; uint32_t mem; };

static struct rev_knobs defaults(void)
{
    struct rev_knobs k = { .time = 1.f, .level = 1.f, .predelay_s = 0.01f, .lowcut = 0.f, .hicut = 0.8f, .spread = 0.f,
                           .diffusion = 0.7f, .size = 0.8f, .feedback = 0.5f, .mod_rate = 0.4f, .mod_depth = 0.5f,
                           .er_level = 0.3f, .freeze = 0 };
    return k;
}

static void run(const struct eng *e, const struct rev_knobs *k, float *L, float *R, int total, int impulse, int noise_s)
{
    struct rev_wrap *rw = (struct rev_wrap *)mem;
    for (uint32_t i = 0; i < RW_ENGINE_OFS + e->mem; i++) mem[i] = 0.f;
    rw_init(rw, 1, FS);
    rw->fade = 1.f;
    e->init(mem + RW_ENGINE_OFS, FS);
    srand(3);
    for (int s = 0; s < total; s += BLK) {
        float bl[BLK], br[BLK];
        for (int i = 0; i < BLK; i++) {
            float v = 0.f;
            if (impulse && s + i == 0) v = 1.f;
            if (s + i < noise_s * 48000) v = rand() / (float)RAND_MAX - 0.5f;
            bl[i] = br[i] = v;
        }
        struct rev_knobs kk = *k;
        if (noise_s < 0 && s > 48000) kk.freeze = 1;
        rw_pre(rw, mem, &kk, bl, br, BLK);
        e->proc(mem + RW_ENGINE_OFS, &kk, bl, br, BLK);
        rw_post(rw, &kk, bl, br, BLK);
        for (int i = 0; i < BLK; i++) { L[s + i] = bl[i]; R[s + i] = br[i]; }
    }
}

static float rt60(const float *L, const float *R, int n)
{
    int frames = n / 480;
    double *edc = malloc(frames * sizeof(double)), tot = 0;
    for (int f = frames - 1; f >= 0; f--) {
        double e = 0;
        for (int i = f * 480; i < f * 480 + 480; i++) e += L[i] * L[i] + R[i] * R[i];
        tot += e; edc[f] = tot;
    }
    int i5 = -1, i25 = -1;
    for (int f = 0; f < frames; f++) {
        double db = 10 * log10(edc[f] / edc[0] + 1e-30);
        if (i5 < 0 && db < -5) i5 = f;
        if (i25 < 0 && db < -25) i25 = f;
    }
    free(edc);
    return (i5 < 0 || i25 < 0) ? -1 : (i25 - i5) * 0.01f * 3.f;
}

static void wav(const char *path, const float *L, const float *R, int n)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
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
    static float L[48000 * 30], R[48000 * 30];
    struct eng engs[] = {
        { "Freeverb", fv_init, fv_process, FV_MEM_FLOATS },
        { "Squall", sq_init, sq_process, SQ_MEM_FLOATS },
        { "MVerb", mv_init, mv_process, 0 },
    };
    engs[2].mem = mv_mem_floats();
    for (int e = 0; e < 3; e++) {
        const struct eng *g = &engs[e];
        printf("== %s: %u floats with the wrapper\n", g->name, RW_ENGINE_OFS + g->mem);
        CHECK(RW_ENGINE_OFS + g->mem <= 262144, "%s fits the stock reverb buffer", g->name);
        float prev = 0.f;
        int mono = 1;
        for (int t = 0; t <= 2; t++) {
            struct rev_knobs k = defaults(); k.time = (float)t;
            run(g, &k, L, R, 48000 * 12, 1, 0);
            float r = rt60(L, R, 48000 * 12);
            printf("      Time %d: RT60 %.2f s\n", t, r);
            if (r <= prev) mono = 0;
            prev = r;
        }
        CHECK(mono, "%s: decay grows with Time", g->name);
        prev = 0.f; mono = 1;
        for (int z = 0; z <= 2; z++) {
            struct rev_knobs k = defaults(); k.size = 0.5f * z;
            run(g, &k, L, R, 48000 * 12, 1, 0);
            float r = rt60(L, R, 48000 * 12);
            printf("      Size %.1f: RT60 %.2f s\n", 0.5f * z, r);
            if (r <= prev) mono = 0;
            prev = r;
        }
        CHECK(mono, "%s: decay grows with Size", g->name);
        {
            /* noise in while Size sweeps 0 -> 1 -> 0 over 4 s: no blow-up, no NaN */
            struct rev_knobs k = defaults();
            struct rev_wrap *rw = (struct rev_wrap *)mem;
            for (uint32_t i = 0; i < RW_ENGINE_OFS + g->mem; i++) mem[i] = 0.f;
            rw_init(rw, 1, FS); rw->fade = 1.f; g->init(mem + RW_ENGINE_OFS, FS);
            float mx = 0; int finite = 1; srand(7);
            for (int s = 0; s < 48000 * 4; s += BLK) {
                float bl[BLK], br[BLK], ph = s / (48000.f * 2.f);
                for (int i = 0; i < BLK; i++) bl[i] = br[i] = rand() / (float)RAND_MAX - 0.5f;
                k.size = ph < 1.f ? ph : 2.f - ph;
                rw_pre(rw, mem, &k, bl, br, BLK); g->proc(mem + RW_ENGINE_OFS, &k, bl, br, BLK); rw_post(rw, &k, bl, br, BLK);
                for (int i = 0; i < BLK; i++) { if (!isfinite(bl[i]) || !isfinite(br[i])) finite = 0; mx = fmaxf(mx, fabsf(bl[i])); }
            }
            CHECK(finite && mx < 8.f, "%s: Size sweep is stable (peak %.2f)", g->name, mx);
        }
        struct rev_knobs k = defaults();
        run(g, &k, L, R, 48000 * 6, 1, 0);
        double eL = 0, eR = 0, c = 0; float pk = 0;
        for (int i = 0; i < 48000 * 6; i++) { eL += L[i] * L[i]; eR += R[i] * R[i]; c += L[i] * R[i]; pk = fmaxf(pk, fmaxf(fabsf(L[i]), fabsf(R[i]))); }
        printf("      energy L %.3f R %.3f, correlation %.2f, peak %.3f\n", eL, eR, c / sqrt(eL * eR), pk);
        CHECK(eL > 0.005 && eR > 0.005 && pk < 2.f, "%s: sane level", g->name);
        if (argc > 1) { char p[512]; snprintf(p, sizeof p, "%s/%s.wav", argv[1], g->name); wav(p, L, R, 48000 * 6); }
        /* noise in, then nothing: steady and stable; then freeze after 1 s */
        k = defaults(); k.time = 2.f; k.diffusion = 1.f; k.hicut = 1.f; k.size = 1.f;
        run(g, &k, L, R, 48000 * 20, 0, 1);
        float mx = 0; int finite = 1;
        for (int i = 0; i < 48000 * 20; i++) { if (!isfinite(L[i]) || !isfinite(R[i])) finite = 0; mx = fmaxf(mx, fabsf(L[i])); }
        CHECK(finite && mx < 8.f, "%s: stable at extreme settings (peak %.2f)", g->name, mx);
        k = defaults();
        run(g, &k, L, R, 48000 * 20, 0, -1);   /* noise for 1 s? no: freeze from 1 s with noise for 0 s */
        k = defaults();
        {
            /* noise for 1 s, freeze from 1 s on: energy at 2-3 s vs 18-19 s */
            struct rev_wrap *rw = (struct rev_wrap *)mem;
            for (uint32_t i = 0; i < RW_ENGINE_OFS + g->mem; i++) mem[i] = 0.f;
            rw_init(rw, 1, FS); rw->fade = 1.f; g->init(mem + RW_ENGINE_OFS, FS);
            double e1 = 0, e2 = 0; srand(5);
            for (int s = 0; s < 48000 * 20; s += BLK) {
                float bl[BLK], br[BLK];
                for (int i = 0; i < BLK; i++) bl[i] = br[i] = s < 48000 ? rand() / (float)RAND_MAX - 0.5f : 0.5f;
                k.freeze = s >= 48000;
                rw_pre(rw, mem, &k, bl, br, BLK); g->proc(mem + RW_ENGINE_OFS, &k, bl, br, BLK); rw_post(rw, &k, bl, br, BLK);
                for (int i = 0; i < BLK; i++) { if (s >= 96000 && s < 144000) e1 += bl[i] * bl[i]; if (s >= 48000 * 18 && s < 48000 * 19) e2 += bl[i] * bl[i]; }
            }
            float drop = 10 * log10(e2 / e1);
            CHECK(drop > -12.f && drop < 3.f, "%s: freeze holds (%.1f dB over 16 s)", g->name, drop);
        }
    }
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
