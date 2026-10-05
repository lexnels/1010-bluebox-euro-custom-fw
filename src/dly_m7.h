/*
 * FX1 delay additions, M7 side: Flutter, Low Cut / High Cut in place of the band-pass, Send to Reverb.
 * Included by hall_m7.c (same cave; it needs the reverb hook for the send).
 *
 * Stock delay (vtable 0x0806ba64, 0x278 bytes, slot 0x14, bus 13, wet only on its bus): process FUN_08053b30 =
 * FUN_08053234(obj, ctx) (events + DSP) then the next node. Per block it reads the delayed signal S0 from its lines
 * (FUN_08059d54(line, bus, ch) at 0x08053574/94, 0x08053816, 0x080538ec), runs its band-pass biquad on S0 in place
 * when Filt (+0x270) is on (FUN_08056dd8(buf, n, coef, state) at 0x08053602/10), feeds S0 x feedback back into the
 * lines and copies S0 to its bus. So S0 is both the repeats' path and the output.
 *
 * Hooks:
 *   dly_process  replaces vtable slot 3 (0x0806ba70): picks up our ids from the event queue (the stock loop ignores
 *                them), forces Filt on so the filter call below always runs, runs the stock body, then keeps
 *                Send x the wet output for the reverb.
 *   dly_read     replaces the 4 S0 line reads: the stock read (its glide after a time change is kept), then, while
 *                the line is steady, the same samples re-read a little further back, by a slowly wobbling amount:
 *                flutter, inside the loop like tape.
 *   dly_tone     replaces the 2 band-pass calls: a 12 dB/oct high-pass (Low Cut) and low-pass (High Cut), also
 *                inside the loop, so every repeat gets a bit thinner and darker.
 *   Send: the reverb runs before the delay in each block, so the kept wet signal is added to the reverb's bus
 *   (14) at the start of the next block's hall_process: one block (32 samples, 0.7 ms) late.
 *
 * Params (new ids, free in both cores' tables; defined on the M4 by src/dly_m4.c), all 0..1000:
 *   0x3a Flutter (depth), 0x3d Send (to reverb), 0x43 Low Cut (20 Hz .. 2 kHz), 0x4a High Cut (500 Hz .. 20 kHz)
 */
#define fw_dly_body ((process_fn)FN(0x08053234))
#define fw_line_read ((line_fn)FN(0x08059d54))
typedef void (*line_fn)(uint32_t *line, void *bus, int ch);

#define D_FLUTTER 0x3a
#define D_SEND    0x3d
#define D_LOWCUT  0x43
#define D_HICUT   0x4a
#define D_FS      48000.f        /* the stock delay is hard-wired to 48 kHz */
#define D_MAXN    32u

struct bus { uint32_t cap, frames; float *l, *r; uint8_t silent, stereo; };

struct dly_shared {
    uint32_t magic;
    uint8_t *obj;                /* the delay we filter (its biquad states tell us the channel) */
    int32_t flutter, send, lowcut, hicut;
    float lc_hz, hc_hz;          /* smoothed corner frequencies */
    float hp[5], lp[5];          /* b0 b1 b2 a1 a2 (y = b0 x + b1 x1 + b2 x2 - a1 y1 - a2 y2), transposed DF-II */
    float st[2][4];              /* per channel: high-pass z1 z2, low-pass z1 z2 */
    float fl_ph, fl_noise, fl_target, m0, m1;
    uint32_t rng, fl_count;
    float send_g;
    uint32_t stash_n;
    float stash[2][D_MAXN];
};
#define DS ((struct dly_shared *)0x38800d00u)   /* after hall_shared, before the CPU meter at 0xf00 */
#define DS_MAGIC 0x44454c59u                     /* "DELY" */
_Static_assert(sizeof(struct hall_shared) <= 0xd00, "hall state below the delay state");
_Static_assert(0xd00 + sizeof(struct dly_shared) <= 0xf00, "delay state below the CPU meter");

static struct dly_shared *dly_state(void)
{
    bkp_enable();
    PWR_CR1 |= 1u << 8;
    struct dly_shared *d = DS;
    if (d->magic != DS_MAGIC) {
        uint32_t *w = (uint32_t *)d;
        for (unsigned i = 0; i < sizeof *d / 4; i++)
            w[i] = 0;
        d->flutter = 0; d->send = 0; d->lowcut = 200; d->hicut = 600;   /* the M4 list's defaults */
        d->lc_hz = d->hc_hz = -1.f;
        d->rng = 0x1234567u;
        d->magic = DS_MAGIC;
    }
    return d;
}

/* sin and cos for |x| <= pi, accurate at small x too (filter coefficients down at 20 Hz need that) */
static float d_sin(float x)
{
    float x2 = x * x;
    return x * (1.f - x2 / 6.f * (1.f - x2 / 20.f * (1.f - x2 / 42.f * (1.f - x2 / 72.f * (1.f - x2 / 110.f)))));
}
static float d_cos(float x)
{
    float x2 = x * x;
    return 1.f - x2 / 2.f * (1.f - x2 / 12.f * (1.f - x2 / 30.f * (1.f - x2 / 56.f * (1.f - x2 / 90.f * (1.f - x2 / 132.f)))));
}

/* RBJ 2-pole low-pass (hp = 0) or high-pass (hp = 1), Q = 0.7071 */
static void d_biquad(float *c, float hz, int hp)
{
    float w = 6.2831853f * hz / D_FS;
    float cw = d_cos(w), alpha = d_sin(w) * 0.70710678f;
    float a0 = 1.f / (1.f + alpha);
    float b1 = hp ? -(1.f + cw) : 1.f - cw;
    c[0] = c[2] = 0.5f * (hp ? 1.f + cw : 1.f - cw) * a0;
    c[1] = b1 * a0;
    c[3] = -2.f * cw * a0;
    c[4] = (1.f - alpha) * a0;
}

static float d_rand(struct dly_shared *d)    /* -1..1 */
{
    d->rng = d->rng * 1664525u + 1013904223u;
    return (float)(int32_t)d->rng * (1.f / 2147483648.f);
}

/* Once per block, before the stock body: knobs -> filter coefficients and the flutter amount for this block. */
static void dly_prepare(struct dly_shared *d, unsigned n)
{
    float lc = 20.f * rv_exp2(6.6438562f * rv_clamp((float)d->lowcut * 1e-3f, 0.f, 1.f));   /* 20 Hz .. 2 kHz */
    float hc = 500.f * rv_exp2(5.3219281f * rv_clamp((float)d->hicut * 1e-3f, 0.f, 1.f));   /* 500 Hz .. 20 kHz */
    if (d->lc_hz < 0.f) { d->lc_hz = lc; d->hc_hz = hc; }
    d->lc_hz += (lc - d->lc_hz) * 0.1f;
    d->hc_hz += (hc - d->hc_hz) * 0.1f;
    d_biquad(d->hp, d->lc_hz, 1);
    d_biquad(d->lp, d->hc_hz > 20000.f ? 20000.f : d->hc_hz, 0);

    /* flutter: a slow 0.6 Hz wobble plus a slower random drift; extra delay 0 .. 2A, A up to 2.5 ms */
    float depth = rv_clamp((float)d->flutter * 1e-3f, 0.f, 1.f);
    float a = depth * depth * 0.0025f * D_FS;
    d->fl_ph += 0.6f * (float)n / D_FS;
    if (d->fl_ph >= 1.f) d->fl_ph -= 1.f;
    if ((d->fl_count++ & 511u) == 0) d->fl_target = d_rand(d);          /* a new drift target every ~0.34 s */
    d->fl_noise += (d->fl_target - d->fl_noise) * 0.004f;
    float s = d_sin(d->fl_ph * 6.2831853f - 3.1415927f);
    d->m0 = d->m1;
    d->m1 = a > 0.f ? a * (1.f + 0.8f * s + 0.2f * d->fl_noise) : 0.f;
    if (d->m0 <= 0.f && d->m1 > 0.f) d->m0 = d->m1;                   /* flutter just turned on: no jump from 0 */
}

/* Replaces bl FUN_08059d54 at the four S0 reads. line = {buf, wr, rd, frac, len, size, steady}. */
void dly_read(uint32_t *line, void *bus, int ch)
{
    uint32_t wr = line[1], rd0 = line[2], len = line[4], size = line[5];
    uint32_t dist = wr > rd0 ? wr - rd0 : wr + size - rd0;
    fw_line_read(line, bus, ch);
    struct dly_shared *d = DS;
    if (d->magic != DS_MAGIC || dist != len || (d->m0 <= 0.f && d->m1 <= 0.f) || !line[0])
        return;                               /* gliding to a new time, or no flutter: the stock read stands */
    float *l, *r;
    unsigned n = fw_frames(bus);
    fw_stereo(bus, &l, &r);
    float *out = ch ? l : r;
    const float *buf = (const float *)line[0];
    float room = (float)size - (float)len - (float)n - 4.f;   /* reading further back stays behind the write head */
    if (room <= 0.f) return;
    float m = d->m0, dm = (d->m1 - d->m0) / (float)n;
    for (unsigned i = 0; i < n; i++, m += dm) {
        float mm = m < room ? m : room;
        int32_t mi = (int32_t)mm;
        float fr = mm - (float)mi;
        /* sample at rd0 + i - mm: between rd0 + i - mi - 1 and rd0 + i - mi */
        uint32_t p1 = rd0 + i + size - (uint32_t)mi;
        uint32_t p0 = p1 - 1;
        p1 %= size; p0 %= size;
        out[i] = buf[p1] + (buf[p0] - buf[p1]) * fr;
    }
}

/* Replaces bl FUN_08056dd8 (the band-pass) at 0x08053602 (L) and 0x08053610 (R): Low Cut then High Cut, in place. */
void dly_tone(float *x, unsigned n, const float *coef, float *state)
{
    (void)coef;
    struct dly_shared *d = DS;
    if (d->magic != DS_MAGIC || !d->obj)
        return;
    float *z = d->st[state == (float *)(d->obj + 0x114) ? 1 : 0];
    const float *h = d->hp, *p = d->lp;
    float h1 = z[0], h2 = z[1], p1 = z[2], p2 = z[3];
    for (unsigned i = 0; i < n; i++) {
        float v = x[i] + 1e-18f;              /* keeps the states out of denormals */
        float y = h[0] * v + h1;
        h1 = h[1] * v - h[3] * y + h2;
        h2 = h[2] * v - h[4] * y;
        float o = p[0] * y + p1;
        p1 = p[1] * y - p[3] * o + p2;
        p2 = p[2] * y - p[4] * o;
        x[i] = o;
    }
    z[0] = h1; z[1] = h2; z[2] = p1; z[3] = p2;
}

/* Replaces the delay's vtable slot 3 (0x0806ba70, stock FUN_08053b30). */
unsigned dly_process(uint8_t *obj, void *ctx)
{
    struct dly_shared *d = dly_state();
    if (d->obj != obj) {
        d->obj = obj;
        for (int c = 0; c < 2; c++)
            for (int i = 0; i < 4; i++)
                d->st[c][i] = 0.f;
        d->stash_n = 0;
    }
    void *q = fw_queue(ctx, *(uint32_t *)(obj + 0x18));
    struct fw_ev ev;
    for (unsigned i = 0; fw_next_ev(q, i, &ev); i++) {
        if (ev.type != 0x39)
            continue;
        if (ev.id == D_FLUTTER) d->flutter = ev.value;
        else if (ev.id == D_SEND) d->send = ev.value;
        else if (ev.id == D_LOWCUT) d->lowcut = ev.value;
        else if (ev.id == D_HICUT) d->hicut = ev.value;
    }
    port_fn port = (*(port_fn **)obj)[0x54 / 4];
    void *bus = fw_bus(ctx, port(obj));
    unsigned n = fw_frames(bus);
    if (n > D_MAXN) n = D_MAXN;
    dly_prepare(d, n);
    obj[0x270] = 1;                           /* "Filt" on: the stock body then always calls dly_tone */
    fw_dly_body(obj, ctx);

    /* keep Send x wet for the reverb's next block */
    float g = rv_clamp((float)d->send * 1e-3f, 0.f, 1.f);
    float g0 = d->send_g;
    d->send_g = g;
    struct bus *b = (struct bus *)bus;
    d->stash_n = 0;
    if ((g > 0.f || g0 > 0.f) && !b->silent) {
        float *l, *r;
        n = fw_frames(bus);
        if (n > D_MAXN) n = D_MAXN;
        fw_stereo(bus, &l, &r);
        float dg = (g - g0) / (float)n, gg = g0;
        for (unsigned i = 0; i < n; i++, gg += dg) {
            d->stash[0][i] = l[i] * gg;
            d->stash[1][i] = r[i] * gg;
        }
        d->stash_n = n;
    }
    uint8_t *next = *(uint8_t **)(obj + 0x08);
    if (!next)
        return 1;
    return (*(process_fn **)next)[3](next, ctx);
}

/* Called at the start of hall_process (the reverb, which runs before the delay): add last block's send. */
static void dly_send_into(uint8_t *rev, void *ctx)
{
    struct dly_shared *d = DS;
    bkp_enable();
    if (d->magic != DS_MAGIC || !d->stash_n)
        return;
    port_fn port = (*(port_fn **)rev)[0x54 / 4];
    void *bus = fw_bus(ctx, port(rev));
    unsigned n = fw_frames(bus);
    if (n > d->stash_n) n = d->stash_n;
    float *l, *r;
    fw_stereo(bus, &l, &r);                   /* zero-fills a silent bus first */
    for (unsigned i = 0; i < n; i++) {
        l[i] += d->stash[0][i];
        r[i] += d->stash[1][i];
    }
    d->stash_n = 0;
}
