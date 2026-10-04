/*
 * Lush hall, M7 side: adds a 16th reverb style that runs src/hall_dsp.h instead of the stock reverb engine.
 * bluebox eurorack firmware (BLUEEURO 3).
 *
 * Stock reverb object (0x984 bytes, vtable 0x0806ada0, slot 3 = process FUN_08048de8(obj, ctx)):
 *   +0x08 next node (process chains to next->vtable[3](next, ctx))
 *   +0x18 event queue id, +0x54 delay memory (1 MB, 0x40000 floats), +0x68 sample rate, +0x6e bypass (param 0x15c)
 *   +0x980 "echo style presets back to the UI" flag
 * The process starts by draining its parameter events (type 0x39, id at +0x0c, int value at +0x10) into the setter
 * FUN_080451ec(value, obj, id). Style is id 0x155, stock values 0..14.
 *
 * Two hooks:
 *   hall_set     replaces that setter call (bl @0x08048e56). Remembers every reverb knob, and swallows style 15 so
 *                the stock engine never indexes past its 15 presets.
 *   hall_process replaces vtable slot 3. Runs the stock engine unless style 15 is selected; then drains the events
 *                itself and runs the hall in the stock engine's (now idle) delay memory.
 * The stock engine and the hall share that memory, so it is zeroed whenever it changes hands: the stock engine then
 * restarts from silence with its own state otherwise untouched (it did not run in between). Zeroing 1 MB at once
 * would overrun an audio block, so it is done in slices over ~30 blocks, with the reverb silent meanwhile.
 */
#include "cpu_shared.h"
#include "hall_dsp.h"

#define FN(addr) ((addr) | 1u)
typedef unsigned (*process_fn)(uint8_t *obj, void *ctx);
typedef void (*set_fn)(float value, uint8_t *obj, unsigned id);
typedef void *(*bus_fn)(void *ctx, unsigned port);
typedef unsigned (*frames_fn)(void *bus);
typedef void *(*queue_fn)(void *ctx, unsigned id);
typedef int (*event_fn)(void *queue, unsigned index, void *event);
typedef void (*stereo_fn)(void *bus, float **l, float **r);
typedef void (*echo_fn)(uint8_t *obj, void *ctx);
typedef unsigned (*port_fn)(uint8_t *obj);

#define fw_process ((process_fn)FN(0x08048de8))
#define fw_set     ((set_fn)FN(0x080451ec))
#define fw_bus     ((bus_fn)FN(0x08053cd4))
#define fw_frames  ((frames_fn)FN(0x0804d4c0))
#define fw_queue   ((queue_fn)FN(0x08053c8c))
#define fw_next_ev ((event_fn)FN(0x0804e938))
#define fw_stereo  ((stereo_fn)FN(0x0804d598))
#define fw_echo    ((echo_fn)FN(0x0804467c))

#define HALL_STYLE 15
#define P_FIRST 0x13d
#define P_COUNT 32               /* reverb params 0x13d..0x15c */
#define P_UNSET (-0x7fffffff)

struct fw_ev {
    uint8_t type, _p0[11];
    uint16_t id, _p1;
    int32_t value;
    uint32_t _p2;
};

/* per reverb object, in backup SRAM (survives while the stock engine owns the delay memory) */
struct hall_slot {
    uint8_t *obj;
    uint8_t active;              /* style 15 selected */
    uint8_t owner;               /* 1 = hall owns the delay memory */
    uint8_t clearing;            /* zeroing the delay memory before it changes hands */
    uint8_t _p;
    uint32_t clear_pos;          /* floats zeroed so far */
    int32_t raw[P_COUNT];
};
#define HALL_SLOTS 4
struct hall_shared {
    uint32_t magic;
    struct hall_slot slot[HALL_SLOTS];
};
#define HS ((volatile struct hall_shared *)0x38800000u)   /* 0x38800000..0x38800233; the CPU meter uses 0xf00.. */
#define HS_MAGIC 0x4c534856u

static struct hall_slot *slot_for(uint8_t *obj)
{
    bkp_enable();
    PWR_CR1 |= 1u << 8;
    struct hall_shared *hs = (struct hall_shared *)HS;
    if (hs->magic != HS_MAGIC) {
        for (int s = 0; s < HALL_SLOTS; s++) {
            hs->slot[s].obj = 0;
            hs->slot[s].active = hs->slot[s].owner = hs->slot[s].clearing = 0;
            for (int i = 0; i < P_COUNT; i++)
                hs->slot[s].raw[i] = P_UNSET;
        }
        hs->magic = HS_MAGIC;
    }
    for (int s = 0; s < HALL_SLOTS; s++)
        if (hs->slot[s].obj == obj)
            return &hs->slot[s];
    for (int s = 0; s < HALL_SLOTS; s++)
        if (!hs->slot[s].obj) {
            hs->slot[s].obj = obj;
            return &hs->slot[s];
        }
    return 0;                    /* more reverbs than slots: leave the extras stock */
}

static float raw(const struct hall_slot *sl, unsigned id, float def)
{
    int32_t v = sl->raw[id - P_FIRST];
    return v == P_UNSET ? def : (float)v;
}

static void knobs_from(const struct hall_slot *sl, struct hall_knobs *k)
{
    k->predelay_s = raw(sl, 0x13d, 200.f) * 1e-4f;      /* 0..9990, 0.1 ms */
    k->diffusion  = raw(sl, 0x13e, 700.f) * 1e-3f;
    k->er_time_s  = raw(sl, 0x13f, 400.f) * 1e-4f;      /* 0..899, 0.1 ms */
    k->er_db      = raw(sl, 0x140, -12000.f) * 1e-3f;   /* -36000..0, mdB */
    k->density    = raw(sl, 0x141, 500.f) * 1e-3f;      /* "Feedback" */
    k->size       = raw(sl, 0x143, 700.f) * 1e-3f;
    k->decay      = raw(sl, 0x144, 500.f) * 1e-3f;
    k->level_db   = raw(sl, 0x145, 0.f) * 1e-3f;        /* "Level", mdB */
    k->spread     = raw(sl, 0x146, 0.f) * 1e-3f;        /* -1000..1000 */
    k->hicut      = raw(sl, 0x148, 600.f) * 1e-3f;
    k->lowcut     = raw(sl, 0x14a, 100.f) * 1e-3f;
    k->bass       = raw(sl, 0x14c, 500.f) * 1e-3f;      /* "Resonance" */
    k->mod_rate   = raw(sl, 0x14d, 350.f) * 1e-3f;
    k->mod_depth  = raw(sl, 0x14e, 400.f) * 1e-3f;
    k->freeze     = raw(sl, 0x14f, 0.f) >= 1.f;
}

#define MEM_FLOATS 0x40000u        /* the stock engine's delay memory, all of it */
#define CLEAR_SLICE 8192u          /* floats zeroed per block while it changes hands */

/* Replaces bl FUN_080451ec @0x08048e56 (s0 = value, r0 = obj, r1 = id); hall_process calls it too. */
void hall_set(float value, uint8_t *obj, unsigned id)
{
    int style = (int)value;
    if (id == 0x155 && (style < 0 || style > HALL_STYLE))
        return;                               /* not a style: ignore, as the stock engine would index past its presets */
    struct hall_slot *sl = slot_for(obj);
    if (sl) {
        if (id >= P_FIRST && id < P_FIRST + P_COUNT)
            sl->raw[id - P_FIRST] = (int32_t)value;
        if (id == 0x155)
            sl->active = style == HALL_STYLE;
    }
    if (id == 0x155 && style == HALL_STYLE)
        return;                               /* the stock engine keeps its previous style */
    fw_set(value, obj, id);
}

static unsigned chain(uint8_t *obj, void *ctx)
{
    uint8_t *next = *(uint8_t **)(obj + 0x08);
    if (!next)
        return 1;
    process_fn p = (*(process_fn **)next)[3];
    return p(next, ctx);
}

/* The stock process's preamble, with hall_set in place of the setter. Returns the frame count. */
static unsigned drain(uint8_t *obj, void *ctx, float **l, float **r)
{
    port_fn port = (*(port_fn **)obj)[0x54 / 4];
    void *bus = fw_bus(ctx, port(obj));
    unsigned n = fw_frames(bus);
    void *q = fw_queue(ctx, *(uint32_t *)(obj + 0x18));
    struct fw_ev ev;
    for (unsigned i = 0; fw_next_ev(q, i, &ev); i++) {
        if (ev.type != 0x39)
            continue;
        hall_set((float)ev.value, obj, ev.id);
        if (ev.id == 0x155 && obj[0x980])
            fw_echo(obj, ctx);
    }
    fw_stereo(bus, l, r);
    return n;
}

/* Replaces vtable slot 0x0806adac (process). */
unsigned hall_process(uint8_t *obj, void *ctx)
{
    struct hall_slot *sl = slot_for(obj);
    if (!sl)
        return fw_process(obj, ctx);
    uint8_t want = sl->active && !obj[0x6e];
    if (!sl->clearing && sl->owner == want && !want)
        return fw_process(obj, ctx);          /* its event loop calls hall_set, so knobs keep being tracked */

    float *l, *r;
    unsigned n = drain(obj, ctx, &l, &r);
    want = sl->active && !obj[0x6e];
    float *mem = *(float **)(obj + 0x54);
    struct hall *h = (struct hall *)mem;

    if (sl->clearing || sl->owner != want) {
        /* changing hands: stay silent and zero one slice per block */
        if (!sl->clearing) {
            sl->clearing = 1;
            sl->clear_pos = 0;
        }
        for (unsigned i = 0; i < n; i++)
            l[i] = r[i] = 0.f;
        uint32_t end = sl->clear_pos + CLEAR_SLICE;
        for (uint32_t i = sl->clear_pos; i < end; i++)
            mem[i] = 0.f;
        sl->clear_pos = end;
        if (end >= MEM_FLOATS) {
            sl->clearing = 0;
            sl->owner = want;
            if (want) {
                uint32_t sr = *(uint32_t *)(obj + 0x68);
                hall_init(h, (sr >= 8000u && sr <= 192000u) ? (float)sr : 48000.f);
            }
        }
        return chain(obj, ctx);
    }

    if (h->magic != HALL_MAGIC) {             /* memory reused behind our back (firmware restart): start over */
        sl->owner = 0;
        for (unsigned i = 0; i < n; i++)
            l[i] = r[i] = 0.f;
        return chain(obj, ctx);
    }
    struct hall_knobs k;
    knobs_from(sl, &k);
    hall_process_block(h, mem, &k, l, r, n);
    return chain(obj, ctx);
}
