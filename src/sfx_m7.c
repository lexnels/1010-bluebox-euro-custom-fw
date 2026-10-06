/*
 * Send FX, M7 side: a Juno-style chorus, a warm drive and a second delay, each switched on and off on its own.
 *
 * The mixer sums the channels into bus 12 and their FX1/FX2 sends into bus 13 (the delay) and bus 14 (the reverb);
 * the graph then runs the reverb, the delay (each wet-only, in place on its bus) and node 0x30, which adds both
 * returns into bus 12. Each channel also has a send to each of our FX (the track sends, on a third track screen
 * page): sfx_strip, around the mixer's per-channel call, adds the channel's post-fader signal times its sends into
 * our three inputs. sfx_process runs next, in the reverb's vtable slot (0x0806adac, ahead of hall_process): each FX
 * takes its input, and its output goes into bus 12 (Level) and into the delay's and reverb's buses (FX1 Send, FX2
 * Send) before they run. So the FX are in the main out, the master
 * compressor and saturator, and recordings of the main mix.
 *
 * Params (src/sfx_ids.h) come with the reverb slot's messages; the M7 copies every message to the master queue (0xc),
 * which we read like src/mst_m7.c does. An FX that is off costs nothing.
 *
 * Memory: about 1 MB (delay 2 lines of 2^18 16-bit samples, 5.4 s, per side; no more than build 2's, which booted:
 * the firmware halts at boot when its SDRAM pool runs out, and 2 MB did) from the firmware's SDRAM allocator
 * FUN_080413d0, taken while the graph is built (the delay's constructor call @0x080518d2 is ours), so the pointer,
 * kept in backup SRAM, is fresh on every boot.
 */
#include "cpu_shared.h"
#include "sfx_ids.h"

#define FN(addr) ((addr) | 1u)
typedef unsigned (*process_fn)(void *obj, void *ctx);
typedef void *(*bus_fn)(void *ctx, unsigned port);
typedef unsigned (*frames_fn)(void *bus);
typedef void *(*queue_fn)(void *ctx, unsigned id);
typedef int (*event_fn)(void *queue, unsigned index, void *event);
typedef void (*stereo_fn)(void *bus, float **l, float **r);
typedef void *(*ctor_fn)(void *obj);
typedef uint32_t (*alloc_fn)(uint32_t size);
typedef void (*strip_fn)(void *mixer, void *in, unsigned idx, float *ch, void *main, void *ctx);
#define fw_reverb   ((process_fn)FN(HALL_PROCESS))     /* hall_process (src/hall_m7.c), from build.sh */
#define fw_dly_ctor ((ctor_fn)FN(0x08052e30))
#define fw_alloc    ((alloc_fn)FN(0x080413d0))
#define fw_bus      ((bus_fn)FN(0x08053cd4))
#define fw_frames   ((frames_fn)FN(0x0804d4c0))
#define fw_queue    ((queue_fn)FN(0x08053c8c))
#define fw_next_ev  ((event_fn)FN(0x0804e938))
#define fw_stereo   ((stereo_fn)FN(0x0804d598))
#define fw_strip    ((strip_fn)FN(0x0805012c))     /* the mixer's per-channel process (FUN_08050564 calls it) */
#define HEAP_TOP    (*(volatile uint32_t *)0x24000004u)    /* FUN_080413d0's SDRAM bump pointer */

struct fw_ev { uint8_t type, _p0[7]; uint32_t target; uint16_t id, _p1; int32_t value; uint32_t _p2; };
struct bus { uint32_t cap, frames; float *l, *r; uint8_t silent, stereo; };

#define FS 48000.f
#define MAXN 32u
#define NCH 12u
#define CHO_N 512u                  /* 10.7 ms (Depth 1000 sweeps up to 7.2 ms) */
#define D2_N 262144u                /* 5.46 s: a bar at 45 BPM */
#define D2_CLEAR 4096u              /* samples per side cleared per block after switching on */
#define TWO_PI 6.2831853f

struct sfx {
    uint32_t magic;
    struct sfx *self;
    /* params, as the M4 sends them */
    int32_t cho_on, cho_fx1, cho_fx2, cho_mode, cho_level, cho_rate, cho_depth, cho_width;
    int32_t drv_on, drv_fx1, drv_fx2, drv_drive, drv_tone, drv_level;
    int32_t d2_on, d2_fx1, d2_fx2, d2_time, d2_fb, d2_tone, d2_ping, d2_level, d2_beat, d2_sync;
    float bpm;                                                  /* the tempo d2_tgt was worked out for */
    float ts[3][NCH];                                           /* track sends, 0..1, per FX and channel */
    uint32_t act;                                               /* bit per FX: running, so it wants input */
    uint32_t has;                                               /* bit per FX: input this block */
    float in[3][2][MAXN];                                       /* the FX inputs, summed by sfx_strip */
    /* chorus */
    float cho_ph, cho_inc, cho_ctr, cho_dep;                    /* LFO phase 0..1, per sample; delay in samples */
    float cho_pre, cho_postl, cho_postr, cho_g, cho_gt;
    uint32_t cho_wp, cho_quiet;
    /* drive */
    float drv_gain, drv_k, drv_lp, drv_g, drv_gt;
    float drv_hpl[2], drv_hpr[2], drv_lpl, drv_lpr;
    uint32_t drv_quiet;
    /* delay 2 */
    float d2_cur, d2_tgt, d2_fbg, d2_lp, d2_g, d2_gt;
    float d2_lpl, d2_lpr, d2_hpl[2], d2_hpr[2];
    uint32_t d2_wp, d2_clear;
    float cho_buf[CHO_N];
    int16_t d2_l[D2_N], d2_r[D2_N];                             /* +-1 as +-32767 (sat keeps them in range) */
};
#define SFX_MAGIC 0x58464453u       /* "SDFX" */

struct sfx_ptr { uint32_t magic; struct sfx *s; };
#define SP ((volatile struct sfx_ptr *)0x38800fa0u)   /* own 32-byte line, M7 only */

static void zero(void *p, uint32_t bytes)
{
    uint32_t *w = p;
    for (uint32_t i = 0; i < bytes / 4; i++)
        w[i] = 0;
}

static float exp2f_(float x)        /* -126 < x < 126 */
{
    float fl = (float)(int)x;
    if (fl > x) fl -= 1.f;
    float f = x - fl;
    union { float f; uint32_t u; } v = { 1.f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f + f * (0.0096181f + f * 0.0013334f)))) };
    v.u += (uint32_t)(int32_t)fl << 23;
    return v.f;
}
/* one-pole low-pass coefficient for cutoff fc */
static float lp_coef(float fc)
{
    float w = TWO_PI * fc / FS;
    return w / (1.f + w);
}
/* soft clip, tanh-like: x (27 + x^2) / (27 + 9 x^2), exactly +-1 from |x| = 3 (as the master saturator's) */
static float sat(float x)
{
    if (x > 3.f) return 1.f;
    if (x < -3.f) return -1.f;
    float x2 = x * x;
    return x * (27.f + x2) / (27.f + 9.f * x2);
}

/* the drive's curve: like sat, but the negative side clips lower (0.6), so even harmonics stay at full drive */
static float warm(float x)
{
    return x >= 0.f ? sat(x) : 0.6f * sat(x * (1.f / 0.6f));
}

static void defaults(struct sfx *s)
{
    s->cho_on = 0; s->cho_fx1 = 0; s->cho_fx2 = 0; s->cho_mode = 0; s->cho_level = 1000; s->cho_rate = 500; s->cho_depth = 500; s->cho_width = 500;
    s->drv_on = 0; s->drv_fx1 = 0; s->drv_fx2 = 0; s->drv_drive = 500; s->drv_tone = 600; s->drv_level = 500;
    s->d2_on = 0; s->d2_fx1 = 0; s->d2_fx2 = 0; s->d2_time = 700; s->d2_fb = 400; s->d2_tone = 600; s->d2_ping = 0; s->d2_level = 700;
    s->d2_beat = 0; s->d2_sync = 8;
    s->bpm = 120.f;
}

/* derived values for the current params */
static void update(struct sfx *s)
{
    static const float rate[3] = { 0.513f, 0.863f, 9.75f };      /* Juno-60: I, II, I+II */
    static const float ctr[3] = { 3.505f, 3.505f, 3.5f };        /* ms: I and II sweep 1.66..5.35 ms */
    static const float dep[3] = { 1.845f, 1.845f, 0.2f };
    int m = s->cho_mode < 0 ? 0 : s->cho_mode > 2 ? 2 : s->cho_mode;
    s->cho_inc = rate[m] * exp2f_((s->cho_rate - 500) * 0.004f) / FS;  /* Rate: x0.25 .. x4 */
    float dp = dep[m] * s->cho_depth * 0.002f;                         /* Depth: 0 .. x2 */
    float c = ctr[m] > dp + 0.3f ? ctr[m] : dp + 0.3f;                 /* never closer than 0.3 ms */
    s->cho_ctr = c * FS * 0.001f;
    s->cho_dep = dp * FS * 0.001f;
    s->cho_gt = s->cho_on ? 1.f : 0.f;

    float d = s->drv_drive * 0.001f;
    s->drv_gain = exp2f_(6.f * d);                                    /* 0 .. +36 dB into the clipper */
    float up = warm(0.25f * s->drv_gain + 0.3f) - warm(0.3f), dn = warm(0.3f) - warm(0.3f - 0.25f * s->drv_gain);
    s->drv_k = 0.25f / (up > dn ? up : dn);                           /* -12 dBFS peaks stay put as Drive rises */
    s->drv_lp = lp_coef(500.f * exp2f_(5.3f * s->drv_tone * 0.001f));   /* 500 Hz .. 20 kHz */
    s->drv_gt = s->drv_on ? 1.f : 0.f;

    static const uint16_t ticks[12] = { 60, 120, 240, 320, 360, 480, 639, 720, 960, 1280, 1920, 3840 };
    if (s->d2_beat) {               /* the stock delay's note values, in its ticks (960 a beat; its 1/4T is 639) */
        int k = s->d2_sync < 0 ? 0 : s->d2_sync > 11 ? 11 : s->d2_sync;
        s->d2_tgt = ticks[k] * (60.f / 960.f) * FS / s->bpm;
    } else
        s->d2_tgt = 10.f * exp2f_(7.643856f * s->d2_time * 0.001f) * FS * 0.001f;  /* 10 ms * 200^t */
    if (s->d2_tgt > (float)(D2_N - 4)) s->d2_tgt = (float)(D2_N - 4);
    s->d2_fbg = s->d2_fb * 0.00098f;                                  /* up to 0.98 */
    s->d2_lp = lp_coef(800.f * exp2f_(4.5f * s->d2_tone * 0.001f));   /* 800 Hz .. 18 kHz in the loop */
    s->d2_gt = s->d2_on ? 1.f : 0.f;
}

/* Our state, once sfx_ctor has set it up in this heap's life; 0 before (backup SRAM keeps the pointer, and SDRAM
 * keeps the old state over a reset, so after one both look valid until the graph is built again: the bump pointer
 * tells, it is back below them until then). */
static struct sfx *state(void)
{
    bkp_enable();
    struct sfx *s = SP->s;
    uint32_t a = (uint32_t)s;
    if (SP->magic != SFX_MAGIC || a < 0xc0000000u || a + sizeof(struct sfx) > HEAP_TOP
        || s->magic != SFX_MAGIC || s->self != s)
        return 0;
    return s;
}

/* Replaces bl FUN_08052e30 @0x080518d2 (the delay's constructor, while the graph is built). */
void *sfx_ctor(void *obj)
{
    void *r = fw_dly_ctor(obj);
    bkp_enable();
    PWR_CR1 |= 1u << 8;
    struct sfx *s = SP->s;
    uint32_t a = (uint32_t)s;
    /* the same heap life (a rebuilt graph): keep ours; after a reset the bump pointer is back below it */
    if (!(SP->magic == SFX_MAGIC && a >= 0xc0000000u && a + sizeof(struct sfx) <= HEAP_TOP
          && s->magic == SFX_MAGIC && s->self == s)) {
        s = (struct sfx *)fw_alloc(sizeof(struct sfx));
        zero(s, sizeof(struct sfx));
        defaults(s);
        update(s);
        s->self = s;
        s->magic = SFX_MAGIC;
        SP->s = s;
        SP->magic = SFX_MAGIC;
    }
    return r;
}

static int set(struct sfx *s, unsigned id, int32_t v)
{
    switch (id) {
    case SFX_CHO_ON: s->cho_on = v; break;
    case SFX_CHO_FX1: s->cho_fx1 = v; break;
    case SFX_CHO_FX2: s->cho_fx2 = v; break;
    case SFX_CHO_MODE: s->cho_mode = v; break;
    case SFX_CHO_LEVEL: s->cho_level = v; break;
    case SFX_CHO_RATE: s->cho_rate = v; break;
    case SFX_CHO_DEPTH: s->cho_depth = v; break;
    case SFX_CHO_WIDTH: s->cho_width = v; break;
    case SFX_DRV_ON: s->drv_on = v; break;
    case SFX_DRV_FX1: s->drv_fx1 = v; break;
    case SFX_DRV_FX2: s->drv_fx2 = v; break;
    case SFX_DRV_DRIVE: s->drv_drive = v; break;
    case SFX_DRV_TONE: s->drv_tone = v; break;
    case SFX_DRV_LEVEL: s->drv_level = v; break;
    case SFX_D2_ON:
        if (v && !s->d2_on)
            s->d2_clear = 0;                /* wipe what the lines held when it was last on */
        s->d2_on = v;
        break;
    case SFX_D2_FX1: s->d2_fx1 = v; break;
    case SFX_D2_FX2: s->d2_fx2 = v; break;
    case SFX_D2_TIME: s->d2_time = v; break;
    case SFX_D2_FB: s->d2_fb = v; break;
    case SFX_D2_TONE: s->d2_tone = v; break;
    case SFX_D2_PING: s->d2_ping = v; break;
    case SFX_D2_LEVEL: s->d2_level = v; break;
    case SFX_D2_BEAT: s->d2_beat = v; break;
    case SFX_D2_SYNC: s->d2_sync = v; break;
    default: return 0;
    }
    return 1;
}

static void set_ts(struct sfx *s, unsigned id, unsigned ch, int32_t v)
{
    if (ch < NCH && id >= SFX_TS_CHO && id <= SFX_TS_D2)
        s->ts[id - SFX_TS_CHO][ch] = (v < 0 ? 0 : v > 1000 ? 1000 : v) * 0.001f;
}

/* Each FX writes its wet output into wl, wr. */
static void chorus(struct sfx *s, const float *il, const float *ir, float *wl, float *wr, unsigned n)
{
    const float send = 0.5f;            /* mono in, as the Juno */
    float pre = s->cho_pre, pl = s->cho_postl, pr = s->cho_postr;
    const float a = 0.6f, b = 0.55f;                /* ~9 kHz in, ~7 kHz out: the BBD's band limit */
    float ph = s->cho_ph;
    uint32_t wp = s->cho_wp;
    int wv = s->cho_width < 0 ? 0 : s->cho_width > 1000 ? 1000 : s->cho_width;
    const float w = wv * 0.002f;        /* Width: the side signal x0 .. x2 */
    for (unsigned i = 0; i < n; i++) {
        pre += a * ((il[i] + ir[i]) * send - pre);
        s->cho_buf[wp] = pre;
        float tri = ph < 0.5f ? 4.f * ph - 1.f : 3.f - 4.f * ph;
        ph += s->cho_inc;
        if (ph >= 1.f) ph -= 1.f;
        float out[2];
        for (int c = 0; c < 2; c++) {
            float d = s->cho_ctr + (c ? -tri : tri) * s->cho_dep;
            int di = (int)d;
            float f = d - (float)di;
            float x0 = s->cho_buf[(wp - di) & (CHO_N - 1)], x1 = s->cho_buf[(wp - di - 1) & (CHO_N - 1)];
            out[c] = x0 + f * (x1 - x0);
        }
        wp = (wp + 1) & (CHO_N - 1);
        pl += b * (out[0] - pl);
        pr += b * (out[1] - pr);
        float m = 0.5f * (pl + pr), sd = 0.5f * (pl - pr) * w;
        wl[i] = m + sd;
        wr[i] = m - sd;
    }
    s->cho_pre = pre; s->cho_postl = pl; s->cho_postr = pr; s->cho_ph = ph; s->cho_wp = wp;
}

static void drive(struct sfx *s, const float *il, const float *ir, float *wl, float *wr, unsigned n)
{
    const float send = 1.f;
    float gain = s->drv_gain, k = s->drv_k, a = s->drv_lp;
    const float bias = 0.3f, sb = warm(bias);      /* the bias and the uneven curve: even harmonics, warmth */
    for (unsigned i = 0; i < n; i++) {
        float x[2] = { il[i] * send, ir[i] * send };
        float y[2];
        float *hp[2] = { s->drv_hpl, s->drv_hpr }, *lp[2] = { &s->drv_lpl, &s->drv_lpr };
        for (int c = 0; c < 2; c++) {
            float v = warm(gain * x[c] + bias) - sb;
            float h = v - hp[c][0] + 0.995f * hp[c][1];     /* DC blocker, ~40 Hz */
            hp[c][0] = v;
            hp[c][1] = h;
            *lp[c] += a * (h - *lp[c]);
            y[c] = *lp[c] * k;
        }
        wl[i] = y[0];
        wr[i] = y[1];
    }
}

static float d2_read(const int16_t *line, uint32_t wp, float d)
{
    int di = (int)d;
    float f = d - (float)di;
    float x0 = (float)line[(wp - di) & (D2_N - 1)], x1 = (float)line[(wp - di - 1) & (D2_N - 1)];
    return (x0 + f * (x1 - x0)) * (1.f / 32767.f);
}

static void delay2(struct sfx *s, const float *il, const float *ir, float *wl_, float *wr_, unsigned n)
{
    if (s->d2_clear < D2_N) {           /* switching on: clear the lines first (a few ms, silent) */
        zero(s->d2_l + s->d2_clear, D2_CLEAR * 2);
        zero(s->d2_r + s->d2_clear, D2_CLEAR * 2);
        s->d2_clear += D2_CLEAR;
        s->d2_lpl = s->d2_lpr = 0.f;
        s->d2_hpl[0] = s->d2_hpl[1] = s->d2_hpr[0] = s->d2_hpr[1] = 0.f;
        s->d2_cur = s->d2_tgt;
        zero(wl_, n * 4);
        zero(wr_, n * 4);
        return;
    }
    const float send = 1.f;
    float fb = s->d2_fbg, a = s->d2_lp, cur = s->d2_cur;
    uint32_t wp = s->d2_wp;
    int ping = s->d2_ping;
    for (unsigned i = 0; i < n; i++) {
        cur += 0.0002f * (s->d2_tgt - cur);             /* time changes glide, tape style (~100 ms) */
        float wl = d2_read(s->d2_l, wp, cur), wr = d2_read(s->d2_r, wp, cur);
        s->d2_lpl += a * (wl - s->d2_lpl);
        s->d2_lpr += a * (wr - s->d2_lpr);
        float fl = s->d2_lpl - s->d2_hpl[0] + 0.99f * s->d2_hpl[1];   /* ~80 Hz high-pass in the loop */
        s->d2_hpl[0] = s->d2_lpl; s->d2_hpl[1] = fl;
        float fr = s->d2_lpr - s->d2_hpr[0] + 0.99f * s->d2_hpr[1];
        s->d2_hpr[0] = s->d2_lpr; s->d2_hpr[1] = fr;
        float xl, xr;
        if (ping) {                                     /* mono in on the left, then left <-> right */
            xl = (il[i] + ir[i]) * 0.5f * send + fb * fr;
            xr = fb * fl;
        } else {
            xl = il[i] * send + fb * fl;
            xr = ir[i] * send + fb * fr;
        }
        s->d2_l[wp] = (int16_t)(sat(xl) * 32767.f);
        s->d2_r[wp] = (int16_t)(sat(xr) * 32767.f);
        wp = (wp + 1) & (D2_N - 1);
        wl_[i] = wl;
        wr_[i] = wr;
    }
    s->d2_cur = cur; s->d2_wp = wp;
}

/* An FX's wet output into the main mix (Level) and into the delay's and reverb's send buses (FX1 Send, FX2 Send);
 * g ramps it in and out as the FX is switched on and off. */
struct dest { float *l12, *r12, *l13, *r13, *l14, *r14; };
static void out(const struct dest *d, float *g, float gt, int32_t level, int32_t fx1, int32_t fx2,
                const float *wl, const float *wr, unsigned n)
{
    float lv = level * 0.001f, f1 = d->l13 ? fx1 * 0.001f : 0.f, f2 = d->l14 ? fx2 * 0.001f : 0.f, gg = *g;
    for (unsigned i = 0; i < n; i++) {
        gg += 0.002f * (gt - gg);
        float a = wl[i] * gg, b = wr[i] * gg;
        d->l12[i] += a * lv;
        d->r12[i] += b * lv;
        if (f1 != 0.f) { d->l13[i] += a * f1; d->r13[i] += b * f1; }
        if (f2 != 0.f) { d->l14[i] += a * f2; d->r14[i] += b * f2; }
    }
    *g = gg;
}

/* The reverb's process (vtable slot 0x0806adac): our FX, then the reverb (hall_process) and the chain. */
unsigned sfx_process(void *obj, void *ctx)
{
    struct sfx *s = state();
    if (!s)
        return fw_reverb(obj, ctx);
    void *q = fw_queue(ctx, 0xc);
    struct fw_ev ev;
    int changed = 0;
    for (unsigned i = 0; fw_next_ev(q, i, &ev); i++)
        if (ev.type == 0x39) {
            if (ev.id >= SFX_TS_CHO && ev.id <= SFX_TS_D2)
                set_ts(s, ev.id, ev.target, ev.value);
            else
                changed |= set(s, ev.id, ev.value);
        }
    if (s->d2_beat) {               /* the song tempo, as the stock delay reads it (FUN_08053234: ctx[0] + 0x18) */
        float bpm = *(volatile float *)(*(uint8_t **)ctx + 0x18);
        if (!(bpm >= 20.f && bpm <= 400.f)) bpm = 120.f;
        if (bpm != s->bpm) { s->bpm = bpm; changed = 1; }
    }
    if (changed)
        update(s);
    int cho = s->cho_on || s->cho_g > 1e-4f, drv = s->drv_on || s->drv_g > 1e-4f, d2 = s->d2_on || s->d2_g > 1e-4f;
    uint32_t has = s->has;
    s->has = 0;
    s->act = (cho ? 1u : 0u) | (drv ? 2u : 0u) | (d2 ? 4u : 0u);   /* sfx_strip fills only these, from next block */
    if (!cho && !drv && !d2)
        return fw_reverb(obj, ctx);

    struct bus *b = fw_bus(ctx, 12);
    unsigned n = fw_frames(b);
    if (n > MAXN) n = MAXN;
    /* no input: chorus and drive have nothing to do once their short tails are out (the delay always runs) */
    if (has & 1) s->cho_quiet = 0; else if (cho && ++s->cho_quiet > CHO_N / MAXN) cho = 0;
    if (has & 2) s->drv_quiet = 0; else if (drv && ++s->drv_quiet > 4) drv = 0;
    if (!cho && !drv && !d2)
        return fw_reverb(obj, ctx);
    for (unsigned f = 0; f < 3; f++)    /* an FX with no input this block runs on silence */
        if (!(has >> f & 1))
            for (unsigned i = 0; i < n; i++)
                s->in[f][0][i] = s->in[f][1][i] = 0.f;
    struct dest d;
    fw_stereo(b, &d.l12, &d.r12);       /* zero-filled if it was silent */
    float wl[MAXN], wr[MAXN];
    /* the send buses only when something goes to them (fw_stereo wakes a silent bus, and its FX with it) */
    int to13 = (cho && s->cho_fx1) || (drv && s->drv_fx1) || (d2 && s->d2_fx1);
    int to14 = (cho && s->cho_fx2) || (drv && s->drv_fx2) || (d2 && s->d2_fx2);
    d.l13 = d.r13 = d.l14 = d.r14 = 0;
    if (to13) fw_stereo(fw_bus(ctx, 13), &d.l13, &d.r13);
    if (to14) fw_stereo(fw_bus(ctx, 14), &d.l14, &d.r14);
    if (cho) {
        chorus(s, s->in[0][0], s->in[0][1], wl, wr, n);
        out(&d, &s->cho_g, s->cho_gt, s->cho_level, s->cho_fx1, s->cho_fx2, wl, wr, n);
    }
    if (drv) {
        drive(s, s->in[1][0], s->in[1][1], wl, wr, n);
        out(&d, &s->drv_g, s->drv_gt, s->drv_level, s->drv_fx1, s->drv_fx2, wl, wr, n);
    }
    if (d2) {
        delay2(s, s->in[2][0], s->in[2][1], wl, wr, n);
        out(&d, &s->d2_g, s->d2_gt, s->d2_level, s->d2_fx1, s->d2_fx2, wl, wr, n);
    }
    return fw_reverb(obj, ctx);
}

/* Replaces the mixer's bl FUN_0805012c @0x0805070a (per channel, before our FX run): the channel as stock, then its
 * post-fader signal (the bus at ch + 20, after fader, pan and mute) times its track sends into the FX inputs. */
void sfx_strip(void *mixer, void *in, unsigned idx, float *ch, void *mainb, void *ctx)
{
    fw_strip(mixer, in, idx, ch, mainb, ctx);
    if (idx >= NCH)
        return;
    struct sfx *s = state();            /* the mixer runs from boot, maybe before the graph's FX are built */
    if (!s || !s->act)
        return;
    struct bus *b = (struct bus *)((uint8_t *)ch + 20);
    if (b->silent || !b->l)
        return;
    unsigned n = b->frames > MAXN ? MAXN : b->frames;
    const float *l = b->l, *r = b->stereo && b->r ? b->r : b->l;
    for (unsigned f = 0; f < 3; f++) {
        float g = s->ts[f][idx];
        if (!(s->act >> f & 1) || g <= 0.f)
            continue;
        float *il = s->in[f][0], *ir = s->in[f][1];
        if (s->has >> f & 1)
            for (unsigned i = 0; i < n; i++) { il[i] += g * l[i]; ir[i] += g * r[i]; }
        else
            for (unsigned i = 0; i < n; i++) { il[i] = g * l[i]; ir[i] = g * r[i]; }
        s->has |= 1u << f;
    }
}
