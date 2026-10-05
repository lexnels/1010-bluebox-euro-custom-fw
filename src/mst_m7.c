/*
 * Master bus, M7 side: a meter for the master compressor and a saturator after it.
 *
 * The master compressor (graph node 0x0a, vtable 0x0806acb0, queue/slot 0xc, on the master bus) is the only one in
 * the firmware. Its process FUN_08042a90 (vtable slot 3, 0x0806acbc) is replaced by mst_process, which
 *   - picks up Drive (id 0x43, 0..1000) from the compressor's queue (the stock loop ignores unknown ids);
 *   - runs the stock compressor alone (next node cleared for the call, so it doesn't chain);
 *   - publishes, every ~20 ms, the most gain reduction it applied, for the M4 to draw (src/mst_shared.h);
 *   - saturates the master bus (Drive > 0), then runs the next node (master level, meters, click, output; the
 *     recorder and USB take bus 12 after those, so the saturation is in recordings).
 * So the saturator sits after the compressor (also when it is off) and before the master level.
 *
 * Compressor object (unlinked stereo, per side L at +0xbc, R at +0xd4): +0 detector envelope (linear), +4 smoothed
 * gain in log2 units with makeup included. +0xf4 threshold, +0xfc makeup, both log2 units. Byte +0x28 != 0: off.
 */
#include "cpu_shared.h"
#include "mst_shared.h"

#define FN(addr) ((addr) | 1u)
typedef unsigned (*process_fn)(uint8_t *obj, void *ctx);
typedef void *(*bus_fn)(void *ctx, unsigned port);
typedef unsigned (*frames_fn)(void *bus);
typedef void *(*queue_fn)(void *ctx, unsigned id);
typedef int (*event_fn)(void *queue, unsigned index, void *event);
typedef void (*stereo_fn)(void *bus, float **l, float **r);
typedef unsigned (*port_fn)(uint8_t *obj);
#define fw_comp    ((process_fn)FN(0x08042a90))
#define fw_bus     ((bus_fn)FN(0x08053cd4))
#define fw_frames  ((frames_fn)FN(0x0804d4c0))
#define fw_queue   ((queue_fn)FN(0x08053c8c))
#define fw_next_ev ((event_fn)FN(0x0804e938))
#define fw_stereo  ((stereo_fn)FN(0x0804d598))
#define SCB_DCCMVAC (*(volatile uint32_t *)0xe000ef68u)

#define M_DRIVE 0x43
#define M_MAXN 32u
#define REPORT_BLOCKS 32u           /* ~21 ms */
#define SAT_REF 0.2f                /* -14 dBFS: the peak level Saturate keeps in place */

struct fw_ev { uint8_t type, _p0[11]; uint16_t id, _p1; int32_t value; uint32_t _p2; };
struct bus { uint32_t cap, frames; float *l, *r; uint8_t silent, stereo; };

struct mst_state {
    uint32_t magic;
    int32_t drive;
    float g, mix;                   /* saturator now: input gain, wet amount (mix 0 = off) */
    float gr;                       /* most gain reduction since the last report, log2 units */
    uint32_t blocks;
};
#define MS ((volatile struct mst_state *)0x38800f60u)
#define MS_MAGIC 0x5453414du

static float exp2f_(float x)        /* 0 <= x < 8 */
{
    int i = (int)x;
    float f = x - (float)i;
    union { float f; uint32_t u; } v = { 1.f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f + f * (0.0096181f + f * 0.0013334f)))) };
    v.u += (uint32_t)i << 23;
    return v.f;
}
/* soft clip, tanh-like: x (27 + x^2) / (27 + 9 x^2), exactly +-1 from |x| = 3 */
static float sat(float x)
{
    if (x > 3.f) return 1.f;
    if (x < -3.f) return -1.f;
    float x2 = x * x;
    return x * (27.f + x2) / (27.f + 9.f * x2);
}

static void report(volatile struct mst_state *s, int on)
{
    PWR_CR1 |= 1u << 8;
    volatile struct mst_meter *m = MST_METER;
    float gr = s->gr * 60.206f;                             /* log2 units -> 0.1 dB */
    if (gr < 0.f) gr = 0.f;
    if (gr > 1200.f) gr = 1200.f;
    m->over = 0;
    m->gr = (uint16_t)gr;
    m->on = (uint8_t)on;
    m->seq = m->seq + 1;
    m->magic = MST_MAGIC;
    __asm volatile("dsb" ::: "memory");
    SCB_DCCMVAC = (uint32_t)m;
    __asm volatile("dsb\n isb" ::: "memory");
    s->gr = 0.f;
    s->blocks = 0;
}

/* Replaces the compressor's vtable slot 3 (0x0806acbc, stock FUN_08042a90). */
unsigned mst_process(uint8_t *obj, void *ctx)
{
    bkp_enable();
    PWR_CR1 |= 1u << 8;
    volatile struct mst_state *s = MS;
    if (s->magic != MS_MAGIC) {
        s->drive = 0;
        s->g = 1.f;
        s->mix = 0.f;
        s->gr = 0.f;
        s->blocks = 0;
        s->magic = MS_MAGIC;
    }
    void *q = fw_queue(ctx, *(uint32_t *)(obj + 0x18));
    struct fw_ev ev;
    for (unsigned i = 0; fw_next_ev(q, i, &ev); i++)
        if (ev.type == 0x39 && ev.id == M_DRIVE)
            s->drive = ev.value;

    uint8_t **pnext = (uint8_t **)(obj + 0x08), *next = *pnext;
    *pnext = 0;
    fw_comp(obj, ctx);
    *pnext = next;

    /* meter */
    int on = obj[0x28] == 0;
    if (on) {
        float g = *(float *)(obj + 0xc0), g_r = *(float *)(obj + 0xd8);
        if (g_r < g) g = g_r;
        float gr = *(float *)(obj + 0xfc) - g;
        if (gr > s->gr) s->gr = gr;
    }
    if (++s->blocks >= REPORT_BLOCKS)
        report(s, on);

    /* saturator: y = x + mix (sat(g x) k - x), g = 2^(6 Drive) (up to +36 dB into the curve), k = R / sat(g R).
     * k keeps a peak of R (-14 dBFS) where it was, so a full mix gets a little quieter as Drive goes up: quieter parts
     * are lifted less, louder ones are squashed down, and at full Drive the output tops out at -14 dBFS. mix fades
     * the effect in over the first 5 % of the knob so leaving 0 doesn't jump. */
    float d = (float)s->drive * 1e-3f;
    if (d < 0.f) d = 0.f;
    if (d > 1.f) d = 1.f;
    float g1 = exp2f_(6.f * d), k1 = SAT_REF / sat(g1 * SAT_REF), mix1 = d * 20.f;
    if (mix1 > 1.f) mix1 = 1.f;
    float g0 = s->g, mix0 = s->mix;
    s->g = g1; s->mix = mix1;
    port_fn port = (*(port_fn **)obj)[0x54 / 4];
    struct bus *b = (struct bus *)fw_bus(ctx, port(obj));
    if ((mix0 > 0.f || mix1 > 0.f) && b && !b->silent) {
        unsigned n = fw_frames(b);
        if (n > M_MAXN) n = M_MAXN;
        float *l, *r;
        fw_stereo(b, &l, &r);
        if (g0 == g1 && mix0 == mix1) {
            for (unsigned i = 0; i < n; i++) {
                l[i] += mix1 * (sat(l[i] * g1) * k1 - l[i]);
                r[i] += mix1 * (sat(r[i] * g1) * k1 - r[i]);
            }
        } else {                    /* Drive moved: ramp across the block (k follows g, so the level stays put) */
            float inv = 1.f / (float)n, dg = (g1 - g0) * inv, dm = (mix1 - mix0) * inv, g = g0, mix = mix0;
            for (unsigned i = 0; i < n; i++) {
                g += dg; mix += dm;
                float k = SAT_REF / sat(g * SAT_REF);
                l[i] += mix * (sat(l[i] * g) * k - l[i]);
                r[i] += mix * (sat(r[i] * g) * k - r[i]);
            }
        }
    }

    if (!next)
        return 1;
    return (*(process_fn **)next)[3](next, ctx);
}
