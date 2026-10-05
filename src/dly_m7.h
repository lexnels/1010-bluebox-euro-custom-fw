/*
 * FX1 delay additions, M7 side: Resonance on the stock band-pass, Pitch (each repeat shifted again), Flutter, Send
 * to Reverb. Included by hall_m7.c (same cave; it needs the reverb hook for the send).
 *
 * Stock delay (vtable 0x0806ba64, 0x278 bytes, slot 0x14, bus 13, wet only on its bus): process FUN_08053b30 =
 * FUN_08053234(obj, ctx) (events + DSP) then the next node. Per block it reads the delayed signal S0 from its lines
 * (FUN_08059d54(line, bus, ch) at 0x08053574/94, 0x08053816, 0x080538ec), runs its band-pass biquad on S0 in place
 * when Filt (+0x270) is on (FUN_08056dd8(buf, n, coef, state) at 0x08053602/10; Cutoff = centre 80 Hz x 2^(7c),
 * Width = 0.5 x 2^(4w) octaves), feeds S0 x feedback back into the lines and copies S0 to its bus. So S0 is both the
 * repeats' path and the output.
 *
 * Hooks:
 *   dly_process  replaces vtable slot 3 (0x0806ba70): picks up our ids from the event queue (the stock loop ignores
 *                them), runs the stock body, then keeps Send x the wet output for the reverb.
 *   dly_read     replaces the 4 S0 line reads: the stock read (its glide after a time change is kept), then
 *                - flutter: while the line is steady, the same samples re-read a little further back, by a slowly
 *                  wobbling amount, inside the loop like tape;
 *                - pitch: a two-tap granular shifter (50 ms Hann windows). It sits in the loop, so every repeat is
 *                  shifted again: +12 makes each echo an octave above the last. It uses lines E and F, which the
 *                  stock delay only needs in QUAD mode, as its buffers, so it is off in QUAD mode.
 *   dly_tone     replaces the 2 band-pass calls (Filt on): the stock band-pass, then Resonance, a peak at the
 *                Cutoff that narrows the band and lifts its centre a little (up to +4.5 dB), with a soft limit so
 *                high Feedback can ring out but never blow up.
 *   Send: the reverb runs before the delay in each block, so the kept wet signal is added to the reverb's bus
 *   (14) at the start of the next block's hall_process: one block (32 samples, 0.7 ms) late.
 *
 * Params (new ids, free in both cores' tables; defined on the M4 by src/dly_m4.c):
 *   0x3a Flutter 0..1000, 0x3d Send 0..1000, 0x43 Resonance 0..1000, 0x4a Pitch -12..12 semitones, 0x4b Pitch on/off
 */
#define fw_dly_body ((process_fn)FN(0x08053234))
#define fw_line_read ((line_fn)FN(0x08059d54))
#define fw_bpf ((bpf_fn)FN(0x08056dd8))
typedef void (*line_fn)(uint32_t *line, void *bus, int ch);
typedef void (*bpf_fn)(float *x, unsigned n, const float *coef, float *state);

#define D_CUTOFF  0x0e           /* stock: we read it too, for the resonance peak */
#define D_FLUTTER 0x3a
#define D_SEND    0x3d
#define D_RESO    0x43
#define D_PITCH   0x4a
#define D_PITCHON 0x4b
#define D_FS      48000.f        /* the stock delay is hard-wired to 48 kHz */
#define D_MAXN    32u
#define P_RING    8192u          /* pitch shifter ring per channel, in lines E / F */
#define P_WIN     2400.f         /* its window: 50 ms */

struct bus { uint32_t cap, frames; float *l, *r; uint8_t silent, stereo; };

struct dly_shared {
    uint32_t magic;
    uint8_t *obj;                /* the delay we work on (its biquad states tell us the channel) */
    int32_t flutter, send, reso, pitch, pitch_on, cutoff;
    float pk[5];                 /* resonance peak: b0 b1 b2 a1 a2 (transposed DF-II), output scale folded in */
    float st[2][2];              /* per channel peak state */
    float fl_ph, fl_noise, fl_target, m0, m1;
    uint32_t rng, fl_count;
    float send_g;
    uint32_t stash_n;
    float stash[2][D_MAXN];
    uint32_t p_live;             /* pitch rings cleared and running */
    uint32_t p_w[2];
    float p_ph[2], p_ratio;
};
#define DS ((struct dly_shared *)0x38800d00u)   /* after hall_shared, before the CPU meter at 0xf00 */
#define DS_MAGIC 0x44454c5au                     /* "ZLED" */
_Static_assert(sizeof(struct hall_shared) <= 0xd00, "hall state below the delay state");
_Static_assert(0xd00 + sizeof(struct dly_shared) <= 0xec0, "delay state below the M4 panel record (0xec0) and the CPU meter (0xf00)");

static struct dly_shared *dly_state(void)
{
    bkp_enable();
    PWR_CR1 |= 1u << 8;
    struct dly_shared *d = DS;
    if (d->magic != DS_MAGIC) {
        uint32_t *w = (uint32_t *)d;
        for (unsigned i = 0; i < sizeof *d / 4; i++)
            w[i] = 0;
        d->cutoff = 120;                      /* the M4 list's default; the rest default to 0 (off) */
        d->rng = 0x1234567u;
        d->magic = DS_MAGIC;
    }
    return d;
}

/* sin and cos for |x| <= pi, accurate at small x too (filter coefficients at low frequencies need that) */
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

static float d_rand(struct dly_shared *d)    /* -1..1 */
{
    d->rng = d->rng * 1664525u + 1013904223u;
    return (float)(int32_t)d->rng * (1.f / 2147483648.f);
}

/* Once per block, before the stock body: the resonance peak and the flutter amount for this block. */
static void dly_prepare(struct dly_shared *d, unsigned n)
{
    /* RBJ peaking EQ at the band-pass centre: +18 dB and Q 8 at full Resonance, then scaled down so the centre ends
     * up at +4.5 dB and the rest of the band drops: a narrower, peakier band */
    float r = rv_clamp((float)d->reso * 1e-3f, 0.f, 1.f);
    float f0 = 80.f * rv_exp2(7.f * rv_clamp((float)d->cutoff * 1e-3f, 0.f, 1.f));
    float w = 6.2831853f * f0 / D_FS;
    float a = rv_exp2(18.f * r / 40.f * 3.3219281f);          /* 10^(dB/40) */
    float alpha = d_sin(w) / (2.f * (0.7f + 7.3f * r)), cw = d_cos(w);
    float a0 = 1.f / (1.f + alpha / a);
    float s15 = rv_exp2(-1.5f * 18.f * r / 40.f * 3.3219281f);   /* a^-1.5: centre gain a^2 -> a^0.5 */
    d->pk[0] = (1.f + alpha * a) * a0 * s15;
    d->pk[1] = -2.f * cw * a0 * s15;
    d->pk[2] = (1.f - alpha * a) * a0 * s15;
    d->pk[3] = -2.f * cw * a0;
    d->pk[4] = (1.f - alpha / a) * a0;

    /* flutter: a slow 0.6 Hz wobble plus a slower random drift; extra delay 0 .. 2A, A up to 2.5 ms */
    float depth = rv_clamp((float)d->flutter * 1e-3f, 0.f, 1.f);
    float fa = depth * depth * 0.0025f * D_FS;
    d->fl_ph += 0.6f * (float)n / D_FS;
    if (d->fl_ph >= 1.f) d->fl_ph -= 1.f;
    if ((d->fl_count++ & 511u) == 0) d->fl_target = d_rand(d);          /* a new drift target every ~0.34 s */
    d->fl_noise += (d->fl_target - d->fl_noise) * 0.004f;
    float s = d_sin(d->fl_ph * 6.2831853f - 3.1415927f);
    d->m0 = d->m1;
    d->m1 = fa > 0.f ? fa * (1.f + 0.8f * s + 0.2f * d->fl_noise) : 0.f;
    if (d->m0 <= 0.f && d->m1 > 0.f) d->m0 = d->m1;                   /* flutter just turned on: no jump from 0 */

    d->p_ratio = rv_exp2((float)d->pitch * (1.f / 12.f));
}

static float d_hann(float p)                 /* sin^2(pi p), p in [0, 1) */
{
    return 0.5f + 0.5f * d_cos(6.2831853f * p - 3.1415927f);
}

/* Pitch shift x[0..n) in place: write into the ring, read two taps whose delay sweeps at (1 - ratio), half a
 * window apart, each faded by a Hann window (the two always sum to 1). */
static void d_pitch(struct dly_shared *d, int c, float *ring, float *x, unsigned n)
{
    const uint32_t M = P_RING - 1;
    uint32_t w = d->p_w[c];
    float ph = d->p_ph[c], dph = (1.f - d->p_ratio) * (1.f / P_WIN);
    for (unsigned i = 0; i < n; i++) {
        ring[w & M] = x[i];
        float y = 0.f;
        for (int t = 0; t < 2; t++) {
            float p = t ? (ph >= 0.5f ? ph - 0.5f : ph + 0.5f) : ph;
            float dl = 1.f + p * P_WIN;
            int32_t di = (int32_t)dl;
            float fr = dl - (float)di;
            float a = ring[(w - (uint32_t)di) & M], b = ring[(w - (uint32_t)di - 1u) & M];
            y += (a + (b - a) * fr) * d_hann(p);
        }
        x[i] = y;
        w++;
        ph += dph;
        if (ph >= 1.f) ph -= 1.f;
        if (ph < 0.f) ph += 1.f;
    }
    d->p_w[c] = w;
    d->p_ph[c] = ph;
}

/* Replaces bl FUN_08059d54 at the four S0 reads. line = {buf, wr, rd, frac, len, size, steady}. */
void dly_read(uint32_t *line, void *bus, int ch)
{
    uint32_t wr = line[1], rd0 = line[2], len = line[4], size = line[5];
    uint32_t dist = wr > rd0 ? wr - rd0 : wr + size - rd0;
    fw_line_read(line, bus, ch);
    struct dly_shared *d = DS;
    if (d->magic != DS_MAGIC || !d->obj)
        return;
    float *l, *r;
    unsigned n = fw_frames(bus);
    if (n > D_MAXN) n = D_MAXN;
    fw_stereo(bus, &l, &r);
    float *out = ch ? l : r;

    /* flutter, unless gliding to a new time */
    const float *buf = (const float *)line[0];
    float room = (float)size - (float)len - (float)n - 4.f;   /* reading further back stays behind the write head */
    if (dist == len && (d->m0 > 0.f || d->m1 > 0.f) && buf && room > 0.f) {
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

    /* pitch, in lines E (left) and F (right) */
    if (d->p_live) {
        float *ring = *(float **)(d->obj + (ch ? 0xa0 : 0xbc));
        if (ring) d_pitch(d, ch ? 0 : 1, ring, out, n);
    }
}

/* Replaces bl FUN_08056dd8 (the band-pass) at 0x08053602 (L) and 0x08053610 (R): the stock band-pass, then
 * Resonance, in place. */
void dly_tone(float *x, unsigned n, const float *coef, float *state)
{
    fw_bpf(x, n, coef, state);
    struct dly_shared *d = DS;
    if (d->magic != DS_MAGIC || !d->obj || d->reso <= 0)
        return;
    float *z = d->st[state == (float *)(d->obj + 0x114) ? 1 : 0];
    const float *k = d->pk;
    float z1 = z[0], z2 = z[1];
    for (unsigned i = 0; i < n; i++) {
        float v = x[i] + 1e-18f;              /* keeps the states out of denormals */
        float y = k[0] * v + z1;
        z1 = k[1] * v - k[3] * y + z2;
        z2 = k[2] * v - k[4] * y;
        /* soft limit above 1 (bounded at 2): resonance with high Feedback rings out instead of running away */
        if (y > 1.f) y = 2.f - 1.f / y;
        else if (y < -1.f) y = -2.f - 1.f / y;
        x[i] = y;
    }
    z[0] = z1; z[1] = z2;
}

/* Replaces the delay's vtable slot 3 (0x0806ba70, stock FUN_08053b30). */
unsigned dly_process(uint8_t *obj, void *ctx)
{
    struct dly_shared *d = dly_state();
    if (d->obj != obj) {
        d->obj = obj;
        d->st[0][0] = d->st[0][1] = d->st[1][0] = d->st[1][1] = 0.f;
        d->stash_n = 0;
        d->p_live = 0;
    }
    void *q = fw_queue(ctx, *(uint32_t *)(obj + 0x18));
    struct fw_ev ev;
    for (unsigned i = 0; fw_next_ev(q, i, &ev); i++) {
        if (ev.type != 0x39)
            continue;
        if (ev.id == D_FLUTTER) d->flutter = ev.value;
        else if (ev.id == D_SEND) d->send = ev.value;
        else if (ev.id == D_RESO) d->reso = ev.value;
        else if (ev.id == D_PITCH) d->pitch = ev.value;
        else if (ev.id == D_PITCHON) d->pitch_on = ev.value;
        else if (ev.id == D_CUTOFF) d->cutoff = ev.value;
    }
    port_fn port = (*(port_fn **)obj)[0x54 / 4];
    void *bus = fw_bus(ctx, port(obj));
    unsigned n = fw_frames(bus);
    if (n > D_MAXN) n = D_MAXN;
    dly_prepare(d, n);

    /* pitch runs with its switch on and a non-zero amount, in lines E and F, so not while QUAD+PING uses them;
     * starting it clears its rings, which hold old QUAD echoes otherwise */
    int want = d->pitch_on && d->pitch != 0 && !(obj[0x276] && obj[0x274]);
    if (want && !d->p_live) {
        for (int c = 0; c < 2; c++) {
            float *ring = *(float **)(obj + (c ? 0xbc : 0xa0));
            if (ring)
                for (unsigned i = 0; i < P_RING; i++)
                    ring[i] = 0.f;
            d->p_w[c] = 0;
            d->p_ph[c] = 0.f;
        }
    }
    d->p_live = (uint32_t)want;
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
