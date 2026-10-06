/*
 * Send FX, M7 side: a Juno-style chorus, a warm drive and a second delay, each switched on and off on its own.
 *
 * The graph runs the reverb (bus 14) and the delay (bus 13), then node 0x30 (vtable 0x0806b9f0, process FUN_08050e6c),
 * which adds both FX returns into the master bus 12 and chains on to the master chain. sfx_process replaces that
 * process: on entry bus 12 holds the dry mix of all channels (post-fader), which feeds the three FX (for now through a
 * Send knob on each); their wet outputs are added into bus 12 next to the stock returns. So they are in the main out,
 * the master compressor and saturator, and recordings of the main mix.
 *
 * Params (src/sfx_ids.h) come with the reverb slot's messages; the M7 copies every message to the master queue (0xc),
 * which we read like src/mst_m7.c does. An FX that is off costs nothing.
 *
 * Memory: about 1 MB (delay 2 lines of 2^17 samples, 2.7 s, per side) from the firmware's SDRAM allocator
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
#define fw_returns  ((process_fn)FN(0x08050e6c))
#define fw_dly_ctor ((ctor_fn)FN(0x08052e30))
#define fw_alloc    ((alloc_fn)FN(0x080413d0))
#define fw_bus      ((bus_fn)FN(0x08053cd4))
#define fw_frames   ((frames_fn)FN(0x0804d4c0))
#define fw_queue    ((queue_fn)FN(0x08053c8c))
#define fw_next_ev  ((event_fn)FN(0x0804e938))
#define fw_stereo   ((stereo_fn)FN(0x0804d598))
#define HEAP_TOP    (*(volatile uint32_t *)0x24000004u)    /* FUN_080413d0's SDRAM bump pointer */

struct fw_ev { uint8_t type, _p0[11]; uint16_t id, _p1; int32_t value; uint32_t _p2; };
struct bus { uint32_t cap, frames; float *l, *r; uint8_t silent, stereo; };

#define FS 48000.f
#define MAXN 32u
#define CHO_N 1024u                 /* 21 ms */
#define D2_N 131072u                /* 2.73 s */
#define D2_CLEAR 4096u              /* samples per side cleared per block after switching on */
#define TWO_PI 6.2831853f

struct sfx {
    uint32_t magic;
    struct sfx *self;
    /* params, as the M4 sends them */
    int32_t cho_on, cho_send, cho_mode, cho_level;
    int32_t drv_on, drv_send, drv_drive, drv_tone, drv_level;
    int32_t d2_on, d2_send, d2_time, d2_fb, d2_tone, d2_ping, d2_level;
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
    float d2_l[D2_N], d2_r[D2_N];
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
    s->cho_on = 0; s->cho_send = 1000; s->cho_mode = 1; s->cho_level = 1000;
    s->drv_on = 0; s->drv_send = 1000; s->drv_drive = 500; s->drv_tone = 600; s->drv_level = 500;
    s->d2_on = 0; s->d2_send = 500; s->d2_time = 700; s->d2_fb = 400; s->d2_tone = 600; s->d2_ping = 0; s->d2_level = 700;
}

/* derived values for the current params */
static void update(struct sfx *s)
{
    static const float rate[3] = { 0.513f, 0.863f, 9.75f };      /* Juno-60: I, II, I+II */
    static const float ctr[3] = { 3.505f, 3.505f, 3.5f };        /* ms: I and II sweep 1.66..5.35 ms */
    static const float dep[3] = { 1.845f, 1.845f, 0.2f };
    int m = s->cho_mode < 1 ? 0 : s->cho_mode > 3 ? 2 : s->cho_mode - 1;
    s->cho_inc = rate[m] / FS;
    s->cho_ctr = ctr[m] * FS * 0.001f;
    s->cho_dep = dep[m] * FS * 0.001f;
    s->cho_gt = s->cho_on ? s->cho_level * 0.001f : 0.f;

    float d = s->drv_drive * 0.001f;
    s->drv_gain = exp2f_(6.f * d);                                    /* 0 .. +36 dB into the clipper */
    float up = warm(0.25f * s->drv_gain + 0.3f) - warm(0.3f), dn = warm(0.3f) - warm(0.3f - 0.25f * s->drv_gain);
    s->drv_k = 0.25f / (up > dn ? up : dn);                           /* -12 dBFS peaks stay put as Drive rises */
    s->drv_lp = lp_coef(500.f * exp2f_(5.3f * s->drv_tone * 0.001f));   /* 500 Hz .. 20 kHz */
    s->drv_gt = s->drv_on ? s->drv_level * 0.001f : 0.f;

    s->d2_tgt = 10.f * exp2f_(7.643856f * s->d2_time * 0.001f) * FS * 0.001f;  /* 10 ms * 200^t */
    if (s->d2_tgt > (float)(D2_N - 4)) s->d2_tgt = (float)(D2_N - 4);
    s->d2_fbg = s->d2_fb * 0.00098f;                                  /* up to 0.98 */
    s->d2_lp = lp_coef(800.f * exp2f_(4.5f * s->d2_tone * 0.001f));   /* 800 Hz .. 18 kHz in the loop */
    s->d2_gt = s->d2_on ? s->d2_level * 0.001f : 0.f;
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
    case SFX_CHO_SEND: s->cho_send = v; break;
    case SFX_CHO_MODE: s->cho_mode = v; break;
    case SFX_CHO_LEVEL: s->cho_level = v; break;
    case SFX_DRV_ON: s->drv_on = v; break;
    case SFX_DRV_SEND: s->drv_send = v; break;
    case SFX_DRV_DRIVE: s->drv_drive = v; break;
    case SFX_DRV_TONE: s->drv_tone = v; break;
    case SFX_DRV_LEVEL: s->drv_level = v; break;
    case SFX_D2_ON:
        if (v && !s->d2_on)
            s->d2_clear = 0;                /* wipe what the lines held when it was last on */
        s->d2_on = v;
        break;
    case SFX_D2_SEND: s->d2_send = v; break;
    case SFX_D2_TIME: s->d2_time = v; break;
    case SFX_D2_FB: s->d2_fb = v; break;
    case SFX_D2_TONE: s->d2_tone = v; break;
    case SFX_D2_PING: s->d2_ping = v; break;
    case SFX_D2_LEVEL: s->d2_level = v; break;
    default: return 0;
    }
    return 1;
}

static void chorus(struct sfx *s, const float *il, const float *ir, float *l, float *r, unsigned n)
{
    float send = s->cho_send * 0.0005f, pre = s->cho_pre, pl = s->cho_postl, pr = s->cho_postr, g = s->cho_g;
    const float a = 0.6f, b = 0.55f;                /* ~9 kHz in, ~7 kHz out: the BBD's band limit */
    float ph = s->cho_ph;
    uint32_t wp = s->cho_wp;
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
        g += 0.002f * (s->cho_gt - g);
        l[i] += pl * g;
        r[i] += pr * g;
    }
    s->cho_pre = pre; s->cho_postl = pl; s->cho_postr = pr; s->cho_g = g; s->cho_ph = ph; s->cho_wp = wp;
}

static void drive(struct sfx *s, const float *il, const float *ir, float *l, float *r, unsigned n)
{
    float send = s->drv_send * 0.001f, gain = s->drv_gain, k = s->drv_k, a = s->drv_lp, g = s->drv_g;
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
        g += 0.002f * (s->drv_gt - g);
        l[i] += y[0] * g;
        r[i] += y[1] * g;
    }
    s->drv_g = g;
}

static float d2_read(const float *line, uint32_t wp, float d)
{
    int di = (int)d;
    float f = d - (float)di;
    float x0 = line[(wp - di) & (D2_N - 1)], x1 = line[(wp - di - 1) & (D2_N - 1)];
    return x0 + f * (x1 - x0);
}

static void delay2(struct sfx *s, const float *il, const float *ir, float *l, float *r, unsigned n)
{
    if (s->d2_clear < D2_N) {           /* switching on: clear the lines first (a few ms, silent) */
        zero(s->d2_l + s->d2_clear, D2_CLEAR * 4);
        zero(s->d2_r + s->d2_clear, D2_CLEAR * 4);
        s->d2_clear += D2_CLEAR;
        s->d2_lpl = s->d2_lpr = 0.f;
        s->d2_hpl[0] = s->d2_hpl[1] = s->d2_hpr[0] = s->d2_hpr[1] = 0.f;
        s->d2_cur = s->d2_tgt;
        return;
    }
    float send = s->d2_send * 0.001f, fb = s->d2_fbg, a = s->d2_lp, g = s->d2_g, cur = s->d2_cur;
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
        s->d2_l[wp] = sat(xl);
        s->d2_r[wp] = sat(xr);
        wp = (wp + 1) & (D2_N - 1);
        g += 0.002f * (s->d2_gt - g);
        l[i] += wl * g;
        r[i] += wr * g;
    }
    s->d2_g = g; s->d2_cur = cur; s->d2_wp = wp;
}

static int silent(const float *x, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        if (x[i] > 1e-6f || x[i] < -1e-6f)
            return 0;
    return 1;
}

/* Node 0x30's process (vtable slot 0x0806b9fc): our FX into bus 12, then the stock FX returns and the chain. */
unsigned sfx_process(void *obj, void *ctx)
{
    bkp_enable();
    struct sfx *s = SP->s;
    if (SP->magic != SFX_MAGIC || !s || s->magic != SFX_MAGIC)
        return fw_returns(obj, ctx);
    void *q = fw_queue(ctx, 0xc);
    struct fw_ev ev;
    int changed = 0;
    for (unsigned i = 0; fw_next_ev(q, i, &ev); i++)
        if (ev.type == 0x39)
            changed |= set(s, ev.id, ev.value);
    if (changed)
        update(s);
    int cho = s->cho_on || s->cho_g > 1e-4f, drv = s->drv_on || s->drv_g > 1e-4f, d2 = s->d2_on || s->d2_g > 1e-4f;
    if (!cho && !drv && !d2)
        return fw_returns(obj, ctx);

    struct bus *b = fw_bus(ctx, 12);
    unsigned n = fw_frames(b);
    if (n > MAXN) n = MAXN;
    /* a silent mix: chorus and drive have nothing to do once their short tails are out (the delay always runs) */
    if (b->silent) {
        if (cho && ++s->cho_quiet > CHO_N / MAXN) cho = 0;
        if (drv && ++s->drv_quiet > 4) drv = 0;
        if (!cho && !drv && !d2)
            return fw_returns(obj, ctx);
    }
    float *l, *r;
    fw_stereo(b, &l, &r);               /* zero-filled if it was silent */
    float il[MAXN], ir[MAXN];
    for (unsigned i = 0; i < n; i++) {
        il[i] = l[i];
        ir[i] = r[i];
    }
    if (!silent(il, n) || !silent(ir, n))
        s->cho_quiet = s->drv_quiet = 0;
    if (cho) chorus(s, il, ir, l, r, n);
    if (drv) drive(s, il, ir, l, r, n);
    if (d2) delay2(s, il, ir, l, r, n);
    return fw_returns(obj, ctx);
}
