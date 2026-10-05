/*
 * FX1 delay additions, M7 side: Pitch (every repeat shifted again), Drift, Send to Reverb. Included by hall_m7.c
 * (same cave; it needs the reverb hook for the send).
 *
 * Stock delay (vtable 0x0806ba64, 0x278 bytes, slot 0x14, bus 13, wet only on its bus): process FUN_08053b30 =
 * FUN_08053234(obj, ctx) (events + DSP) then the next node. Per block it reads the delayed signal S0 from its lines,
 * runs its band-pass on S0 when Filt is on, feeds S0 x feedback back into the lines and copies S0 to its bus. Lines
 * {buf, wr, rd, frac, len, size, steady} at +0x30 (A), +0x4c (B), +0x68 (C), +0x84 (D), +0xa0 (E), +0xbc (F):
 *   plain:     L = read A, R = read C, each fed back into itself
 *   PING:      L = read B; A <- fb x L; B <- read A + in L (A and B are T long, so L repeats at T, 3T, 5T ...);
 *              R = read C, C <- fb x R + in R (C is 2T long: R repeats at 2T, 4T ...)
 *   QUAD+PING: six lines, outputs 3/4 from E and F
 * All of these reads are FUN_08059d54(line, bus, ch); we hook every one of them (9 call sites).
 *
 * Hooks:
 *   dly_process  replaces vtable slot 3 (0x0806ba70): picks up our ids from the event queue (the stock loop ignores
 *                them), runs the stock body, then keeps Send x the wet output for the reverb.
 *   dly_read     replaces the line reads: the stock read (its glide after a time change is kept), then re-reads the
 *                same line a variable amount further back (or nearer the write head):
 *                - drift (the old Flutter, still id 0x3a): a slowly wobbling, wandering extra delay while the line
 *                  is steady, like tape (up to 16 ms, about +-2.5 % pitch);
 *                - pitch: plain resampling, like changing tape speed: the read slides through the line at the pitch
 *                  ratio and jumps back every delay time (at most 100 ms), with a 5 ms crossfade. No grains or
 *                  windows beyond that splice, so going up repeats a little and going down skips a little. Each pass
 *                  through a line shifts it once, so with Feedback every repeat climbs or falls again. In PING mode
 *                  line C (2T, one hop for two of L's) shifts twice, so every step, left or right, is one more shift.
 *   Send: the reverb runs before the delay in each block, so the kept wet signal is added to the reverb's bus
 *   (14) at the start of the next block's hall_process: one block (32 samples, 0.7 ms) late.
 *
 * Params (new ids, free in both cores' tables; defined on the M4 by src/dly_m4.c):
 *   0x3a Drift 0..1000, 0x3d Send 0..1000, 0x4a Pitch -12..12 semitones, 0x4b Pitch on/off
 */
#define fw_dly_body ((process_fn)FN(0x08053234))
#define fw_line_read ((line_fn)FN(0x08059d54))
typedef void (*line_fn)(uint32_t *line, void *bus, int ch);

#define D_FLUTTER 0x3a           /* shown as Drift */
#define D_SEND    0x3d
#define D_PITCH   0x4a
#define D_PITCHON 0x4b
#define D_FS      48000.f        /* the stock delay is hard-wired to 48 kHz */
#define D_MAXN    32u
#define P_FADE    240.f          /* pitch: crossfade at each jump, 5 ms */
#define P_SPAN    4800.f         /* pitch: the read jumps back at least every 100 ms */

struct bus { uint32_t cap, frames; float *l, *r; uint8_t silent, stereo; };

struct dly_shared {
    uint32_t magic;
    uint8_t *obj;                /* the delay we work on */
    int32_t flutter, send, pitch, pitch_on;
    float fl_ph, fl_noise, fl_target, m0, m1;
    uint32_t rng, fl_count;
    float send_g;
    uint32_t stash_n;
    float stash[2][D_MAXN];
    float p_ratio, p_ratio2;     /* 2^(semitones/12), and squared for PING's line C; 1 = off */
    float p_ph[6];               /* per line: samples since its last jump */
};
#define DS ((struct dly_shared *)0x38800d00u)   /* after hall_shared, before the CPU meter at 0xf00 */
#define DS_MAGIC 0x44454c5bu
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
        d->rng = 0x1234567u;
        d->p_ratio = d->p_ratio2 = 1.f;
        d->magic = DS_MAGIC;
    }
    return d;
}

static float d_sin(float x)                  /* |x| <= pi */
{
    float x2 = x * x;
    return x * (1.f - x2 / 6.f * (1.f - x2 / 20.f * (1.f - x2 / 42.f * (1.f - x2 / 72.f * (1.f - x2 / 110.f)))));
}

static float d_rand(struct dly_shared *d)    /* -1..1 */
{
    d->rng = d->rng * 1664525u + 1013904223u;
    return (float)(int32_t)d->rng * (1.f / 2147483648.f);
}

/* Once per block, before the stock body: the drift amount and the pitch ratio for this block. */
static void dly_prepare(struct dly_shared *d, unsigned n)
{
    /* drift (id "flutter"): a slow 0.6 Hz wobble plus a slower random wander; extra delay 0 .. 2A, A up to 8 ms
     * (about +-2.5 % pitch at full) */
    float depth = rv_clamp((float)d->flutter * 1e-3f, 0.f, 1.f);
    float fa = depth * 0.008f * D_FS;
    d->fl_ph += 0.6f * (float)n / D_FS;
    if (d->fl_ph >= 1.f) d->fl_ph -= 1.f;
    if ((d->fl_count++ & 511u) == 0) d->fl_target = d_rand(d);          /* a new wander target every ~0.34 s */
    d->fl_noise += (d->fl_target - d->fl_noise) * 0.004f;
    float s = d_sin(d->fl_ph * 6.2831853f - 3.1415927f);
    d->m0 = d->m1;
    d->m1 = fa > 0.f ? fa * (1.f + 0.6f * s + 0.4f * d->fl_noise) : 0.f;
    if (d->m0 <= 0.f && d->m1 > 0.f) d->m0 = d->m1;                   /* drift just turned on: no jump from 0 */

    int st = d->pitch_on ? d->pitch : 0;
    if (st < -12) st = -12;
    if (st > 12) st = 12;
    d->p_ratio = rv_exp2((float)st * (1.f / 12.f));
    d->p_ratio2 = d->p_ratio * d->p_ratio;
}

/* the line's sample m samples older than the stock read's (m < 0: newer), linear interpolation */
static float d_tap(const float *buf, uint32_t size, uint32_t pos, float m)
{
    int32_t mi = (int32_t)m;
    if ((float)mi > m) mi--;
    float fr = m - (float)mi;
    uint32_t p1 = (uint32_t)((int32_t)(pos + 2u * size) - mi) % size;
    uint32_t p0 = p1 ? p1 - 1u : size - 1u;
    return buf[p1] + (buf[p0] - buf[p1]) * fr;
}

/* Replaces bl FUN_08059d54 at every line read. */
void dly_read(uint32_t *line, void *bus, int ch)
{
    uint32_t wr = line[1], rd0 = line[2], len = line[4], size = line[5];
    uint32_t dist = wr >= rd0 ? wr - rd0 : wr + size - rd0;
    fw_line_read(line, bus, ch);
    struct dly_shared *d = DS;
    if (d->magic != DS_MAGIC || !d->obj)
        return;
    const float *buf = (const float *)line[0];
    int32_t li = (int32_t)((uint8_t *)line - (d->obj + 0x30));
    if (!buf || li < 0 || li > 5 * 0x1c || li % 0x1c)
        return;
    li /= 0x1c;
    unsigned n = fw_frames(bus);
    if (n > D_MAXN) n = D_MAXN;
    float *l, *r;
    fw_stereo(bus, &l, &r);
    float *out = ch ? l : r;

    /* how far we may move from the stock read: back, while staying ahead of the write head going round; forward,
     * while staying behind this block's not yet written samples */
    float hi = (float)size - (float)dist - (float)n - 4.f, lo = -((float)dist - (float)n - 2.f);
    if (hi < 0.f) hi = 0.f;
    if (lo > 0.f) lo = 0.f;

    int flutter = dist == len && (d->m0 > 0.f || d->m1 > 0.f);
    uint8_t *obj = d->obj;
    float ratio = li == 2 && obj[0x274] && !obj[0x276] ? d->p_ratio2 : d->p_ratio;
    /* the read slides by (1 - ratio) per sample and jumps back every P samples: P is the delay time, at most 100 ms
     * (so a repeat arrives at most 100 ms early or late), shortened if the slide would run out of room */
    float k = 1.f - ratio, P = (float)dist < P_SPAN ? (float)dist : P_SPAN;
    if (k < -1e-4f && P * -k > -lo) P = -lo / -k;
    if (k > 1e-4f && P * k > hi) P = hi / k;
    int pitch = (k < -1e-4f || k > 1e-4f) && P >= 4.f * P_FADE;
    if (!flutter && !pitch) {
        d->p_ph[li] = 0.f;
        return;
    }
    float m = flutter ? d->m0 : 0.f, dm = flutter ? (d->m1 - d->m0) / (float)n : 0.f;
    float ph = d->p_ph[li];
    if (!pitch) ph = 0.f;
    if (ph >= P) ph = 0.f;
    for (unsigned i = 0; i < n; i++, m += dm) {
        float a = rv_clamp(m + k * ph, lo, hi);
        float y = d_tap(buf, size, rd0 + i, a);
        if (ph > P - P_FADE) {               /* crossfade into the read after the jump */
            float g = (ph - (P - P_FADE)) * (1.f / P_FADE);
            float b = rv_clamp(m + k * (ph - P), lo, hi);
            y += (d_tap(buf, size, rd0 + i, b) - y) * g;
        }
        out[i] = y;
        if (pitch && (ph += 1.f) >= P) ph -= P;
    }
    d->p_ph[li] = ph;
}

/* Replaces the delay's vtable slot 3 (0x0806ba70, stock FUN_08053b30). */
unsigned dly_process(uint8_t *obj, void *ctx)
{
    struct dly_shared *d = dly_state();
    if (d->obj != obj) {
        d->obj = obj;
        d->stash_n = 0;
    }
    void *q = fw_queue(ctx, *(uint32_t *)(obj + 0x18));
    struct fw_ev ev;
    for (unsigned i = 0; fw_next_ev(q, i, &ev); i++) {
        if (ev.type != 0x39)
            continue;
        if (ev.id == D_FLUTTER) d->flutter = ev.value;
        else if (ev.id == D_SEND) d->send = ev.value;
        else if (ev.id == D_PITCH) d->pitch = ev.value;
        else if (ev.id == D_PITCHON) d->pitch_on = ev.value;
    }
    port_fn port = (*(port_fn **)obj)[0x54 / 4];
    void *bus = fw_bus(ctx, port(obj));
    unsigned n = fw_frames(bus);
    if (n > D_MAXN) n = D_MAXN;
    dly_prepare(d, n);
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
