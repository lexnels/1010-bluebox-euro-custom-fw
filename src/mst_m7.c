/*
 * Master bus, M7 side: a meter for the master compressor and a saturator after it.
 *
 * The master compressor (graph node 0x0a, vtable 0x0806acb0, queue/slot 0xc, on the master bus) is the only one in
 * the firmware. Its process FUN_08042a90 (vtable slot 3, 0x0806acbc) is replaced by mst_process, which
 *   - picks up Drive (id 0x43, 0..1000) from the compressor's queue (the stock loop ignores unknown ids);
 *   - runs the stock compressor alone (next node cleared for the call, so it doesn't chain);
 *   - publishes, every ~20 ms, how far its detector went past the threshold and the most gain reduction, for the
 *     M4 to draw (src/mst_shared.h);
 *   - saturates the master bus (Drive > 0), then runs the next node (master level, meters, click, output).
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

struct fw_ev { uint8_t type, _p0[11]; uint16_t id, _p1; int32_t value; uint32_t _p2; };
struct bus { uint32_t cap, frames; float *l, *r; uint8_t silent, stereo; };

struct mst_state {
    uint32_t magic;
    int32_t drive;
    float g;                        /* saturator curve amount a now (0 = off) */
    float over, gr;                 /* worst since the last report, log2 units */
    uint32_t blocks;
};
#define MS ((volatile struct mst_state *)0x38800f60u)
#define MS_MAGIC 0x5453414du

static float log2f_(float x)        /* x > 0, ~1e-4 */
{
    union { float f; uint32_t u; } v = { x };
    float e = (float)(int32_t)((v.u >> 23) & 0xff) - 127.f;
    v.u = (v.u & 0x007fffffu) | 0x3f800000u;     /* 1 <= m < 2 */
    float m = v.f - 1.f;
    return e + m * (1.4425449f + m * (-0.7181452f + m * (0.4575485f + m * (-0.2779042f + m * (0.1217970f + m * -0.0258411f)))));
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
    float over = s->over * 60.206f, gr = s->gr * 60.206f;    /* log2 units -> 0.1 dB */
    if (over < -1200.f) over = -1200.f;
    if (over > 1200.f) over = 1200.f;
    if (gr < 0.f) gr = 0.f;
    if (gr > 1200.f) gr = 1200.f;
    m->over = (int16_t)over;
    m->gr = (uint16_t)gr;
    m->on = (uint8_t)on;
    m->seq = m->seq + 1;
    m->magic = MST_MAGIC;
    __asm volatile("dsb" ::: "memory");
    SCB_DCCMVAC = (uint32_t)m;
    __asm volatile("dsb\n isb" ::: "memory");
    s->over = -100.f;
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
        s->g = 0.f;
        s->over = -100.f;
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
        float env = *(float *)(obj + 0xbc), env_r = *(float *)(obj + 0xd4);
        if (env_r > env) env = env_r;
        float over = (env > 1e-9f ? log2f_(env) : -30.f) - *(float *)(obj + 0xf4);
        float g = *(float *)(obj + 0xc0), g_r = *(float *)(obj + 0xd8);
        if (g_r < g) g = g_r;
        float gr = *(float *)(obj + 0xfc) - g;
        if (over > s->over) s->over = over;
        if (gr > s->gr) s->gr = gr;
    }
    if (++s->blocks >= REPORT_BLOCKS)
        report(s, on);

    /* saturator: y = sat(a x) / sat(a), a = 8 Drive^2. Full scale stays full scale; below it the curve lifts and
     * rounds the signal more as Drive goes up (quiet parts up to +18 dB louder at full Drive: denser, louder). At
     * small Drive it is nearly a straight line, so turning it up from 0 doesn't jump. */
    float d = (float)s->drive * 1e-3f;
    if (d < 0.f) d = 0.f;
    if (d > 1.f) d = 1.f;
    float a1 = 8.f * d * d, a0 = s->g;
    s->g = a1;
    port_fn port = (*(port_fn **)obj)[0x54 / 4];
    struct bus *b = (struct bus *)fw_bus(ctx, port(obj));
    if ((a0 > 1e-3f || a1 > 1e-3f) && b && !b->silent) {
        unsigned n = fw_frames(b);
        if (n > M_MAXN) n = M_MAXN;
        float *l, *r;
        fw_stereo(b, &l, &r);
        if (a0 < 1e-3f) a0 = 1e-3f;
        if (a1 < 1e-3f) a1 = 1e-3f;
        if (a0 == a1) {
            float k = 1.f / sat(a1);
            for (unsigned i = 0; i < n; i++) {
                l[i] = sat(l[i] * a1) * k;
                r[i] = sat(r[i] * a1) * k;
            }
        } else {                    /* Drive moved: ramp a across the block, keeping full scale at full scale */
            float da = (a1 - a0) / (float)n, aa = a0;
            for (unsigned i = 0; i < n; i++, aa += da) {
                float k = 1.f / sat(aa);
                l[i] = sat(l[i] * aa) * k;
                r[i] = sat(r[i] * aa) * k;
            }
        }
    }

    if (!next)
        return 1;
    return (*(process_fn **)next)[3](next, ctx);
}
