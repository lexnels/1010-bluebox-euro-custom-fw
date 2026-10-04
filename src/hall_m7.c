/*
 * Extra reverb styles, M7 side: styles 15.. run our own engines instead of the stock reverb engine.
 * bluebox eurorack firmware (BLUEEURO 3).
 *
 *   15 Lush Hall  src/hall_dsp.h      (8-line modulated FDN, this project, MIT)
 *   16 MVerb      src/rev_mverb.h     (Martin Eastwood, GPL-3: see that file)
 *   17 Squall     src/rev_squall.h    (Clouds reverb, Emilie Gillet, MIT; Squall voicing)
 *   18 Freeverb   src/rev_freeverb.h  (Jezar, public domain)
 *
 * Stock reverb object (0x984 bytes, vtable 0x0806ada0, slot 3 = process FUN_08048de8(obj, ctx)):
 *   +0x08 next node (process chains to next->vtable[3](next, ctx))
 *   +0x18 event queue id, +0x54 delay memory (1 MB, 0x40000 floats), +0x68 sample rate, +0x6e bypass (param 0x15c)
 *   +0x980 "echo style presets back to the UI" flag
 * The process starts by draining its parameter events (type 0x39, id at +0x0c, int value at +0x10) into the setter
 * FUN_080451ec(value, obj, id). Style is id 0x155, stock values 0..14.
 *
 * Two hooks:
 *   hall_set     replaces that setter call (bl @0x08048e56). Remembers every reverb knob, and swallows styles 15+ so
 *                the stock engine never indexes past its 15 presets.
 *   hall_process replaces vtable slot 3. Runs the stock engine unless one of our styles is selected; then drains the
 *                events itself and runs that engine in the stock engine's (now idle) delay memory.
 * The engines share that memory, so it is zeroed whenever it changes hands: the stock engine then
 * restarts from silence with its own state otherwise untouched (it did not run in between). Zeroing 1 MB at once
 * would overrun an audio block, so it is done in slices over ~30 blocks, with the reverb silent meanwhile.
 */
#include "cpu_shared.h"
#include "hall_dsp.h"
#include "rev_common.h"
#include "rev_freeverb.h"
#include "rev_squall.h"
#include "rev_mverb.h"

#define FN(addr) ((addr) | 1u)
typedef unsigned (*process_fn)(uint8_t *obj, void *ctx);
typedef void (*set_fn)(float value, uint8_t *obj, unsigned id);
typedef void *(*bus_fn)(void *ctx, unsigned port);
typedef unsigned (*frames_fn)(void *bus);
typedef void *(*queue_fn)(void *ctx, unsigned id);
typedef int (*event_fn)(void *queue, unsigned index, void *event);
typedef void (*stereo_fn)(void *bus, float **l, float **r);
typedef void (*echo_fn)(uint8_t *obj, void *ctx);
typedef void (*push_fn)(void *queue, const void *msg);
typedef unsigned (*port_fn)(uint8_t *obj);

#define fw_process ((process_fn)FN(0x08048de8))
#define fw_set     ((set_fn)FN(0x080451ec))
#define fw_bus     ((bus_fn)FN(0x08053cd4))
#define fw_frames  ((frames_fn)FN(0x0804d4c0))
#define fw_queue   ((queue_fn)FN(0x08053c8c))
#define fw_next_ev ((event_fn)FN(0x0804e938))
#define fw_stereo  ((stereo_fn)FN(0x0804d598))
#define fw_echo    ((echo_fn)FN(0x0804467c))
#define fw_push    ((push_fn)FN(0x0804e968))      /* (queue, 24-byte message): the M7 -> M4 queue fw_echo uses */

#define FIRST_STYLE 15             /* stock styles are 0..14 */
enum { ENG_NONE, ENG_HALL, ENG_MVERB, ENG_SQUALL, ENG_FREEVERB, ENG_COUNT };
#define LAST_STYLE (FIRST_STYLE + ENG_COUNT - 2)
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
    uint8_t active;              /* engine of the selected style (ENG_NONE = a stock style) */
    uint8_t owner;               /* engine that owns the delay memory (ENG_NONE = the stock engine) */
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

static float sample_rate(const uint8_t *obj)
{
    uint32_t sr = *(const uint32_t *)(obj + 0x68);
    return (sr >= 8000u && sr <= 192000u) ? (float)sr : 48000.f;
}

/* Panel values for our styles. Time and Level follow the stock meaning (0..2000, 1000 = default); Diffusion and Spread
 * are read back from the stock object, where the stock setter keeps them (diffamt x 0.001 at +0x78, spread x 0.1 at
 * +0xa0, checked by running the setter under Unicorn), so they always match what the panel shows, whether a stock
 * style set them or the knob did. The panel doesn't show the other params (patches/panel.py), so they keep whatever a
 * stock preset left in them; our styles use fixed values for those. */
static float obj_f(const uint8_t *obj, unsigned ofs, float lo, float hi, float def)
{
    float v = *(const float *)(obj + ofs);
    return v >= lo && v <= hi ? v : def;       /* also catches NaN */
}

static void rev_knobs_from(const struct hall_slot *sl, struct rev_knobs *k)
{
    k->time       = raw(sl, 0x159, 1000.f) * 1e-3f;
    k->level      = raw(sl, 0x15a, 1000.f) * 1e-3f;
    k->predelay_s = raw(sl, 0x13d, 200.f) * 1e-4f;      /* 0..9990, 0.1 ms */
    k->hicut      = raw(sl, 0x148, 1000.f) * 1e-3f;
    k->lowcut     = raw(sl, 0x14a, 0.f) * 1e-3f;
    k->freeze     = raw(sl, 0x14f, 0.f) >= 1.f;
    k->spread     = obj_f(sl->obj, 0xa0, -100.f, 100.f, 0.f) * 1e-2f;   /* -1..1 */
    k->diffusion  = obj_f(sl->obj, 0x78, 0.f, 1.f, 0.8f);
    k->size       = 0.8f;
    k->feedback   = 0.5f;
    k->mod_rate   = 0.4f;
    k->mod_depth  = 0.5f;
    k->er_level   = 0.5f;                              /* MVerb: 3/4 tank, 1/4 early reflections */
}

static void hall_knobs_from(const struct hall_slot *sl, struct hall_knobs *k)
{
    struct rev_knobs r;
    rev_knobs_from(sl, &r);
    k->decay      = (0.415f + 3.f * r.time) * (1.f / 6.6438562f);   /* RT60 = 0.4 s x 8^t: 0.4 s, 3.2 s, 25.6 s */
    k->level      = r.level;
    k->predelay_s = r.predelay_s;
    k->hicut      = r.hicut;
    k->lowcut     = r.lowcut;
    k->freeze     = r.freeze;
    k->size       = r.size;
    k->diffusion  = r.diffusion;
    k->density    = r.feedback;                          /* "Feedback": diffusion inside the tail */
    k->spread     = r.spread;
    k->mod_rate   = r.mod_rate;
    k->mod_depth  = r.mod_depth;
    k->er_time_s  = 0.025f;
    k->er_db      = -36.f;                               /* early reflections off */
    k->bass       = 0.6f;                                /* bass decay multiplier */
}

#define MEM_FLOATS 0x40000u        /* the stock engine's delay memory, all of it */
_Static_assert(HALL_MEM_FLOATS <= MEM_FLOATS, "Lush Hall memory");
_Static_assert(sizeof(struct fv_state) <= FV_STATE_FLOATS * 4 && sizeof(struct sq_state) <= SQ_STATE_FLOATS * 4 &&
               sizeof(struct mv_state) <= MV_STATE_FLOATS * 4 && sizeof(struct rev_wrap) <= RW_HEAD_FLOATS * 4,
               "engine state headers");
_Static_assert(RW_ENGINE_OFS + FV_MEM_FLOATS <= MEM_FLOATS, "Freeverb memory");
_Static_assert(RW_ENGINE_OFS + SQ_MEM_FLOATS <= MEM_FLOATS, "Squall memory");
_Static_assert(RW_ENGINE_OFS + MV_STATE_FLOATS + 79168u <= MEM_FLOATS, "MVerb memory");   /* mv_mem_floats() */
#define CLEAR_SLICE 8192u          /* floats zeroed per block while it changes hands */

/* Replaces bl FUN_080451ec @0x08048e56 (s0 = value, r0 = obj, r1 = id); hall_process calls it too. */
void hall_set(float value, uint8_t *obj, unsigned id)
{
    int style = (int)value;
    if (id == 0x155 && (style < 0 || style > LAST_STYLE))
        return;                               /* not a style: ignore, as the stock engine would index past its presets */
    struct hall_slot *sl = slot_for(obj);
    if (sl) {
        if (id >= P_FIRST && id < P_FIRST + P_COUNT)
            sl->raw[id - P_FIRST] = (int32_t)value;
        if (id == 0x155)
            sl->active = style >= FIRST_STYLE ? (uint8_t)(ENG_HALL + style - FIRST_STYLE) : ENG_NONE;
    }
    if (id == 0x155 && style >= FIRST_STYLE)
        return;                               /* the stock engine keeps its previous style */
    fw_set(value, obj, id);
}

static void echo_one(uint8_t *obj, void *ctx, unsigned id, float v)
{
    uint32_t m[6] = { 0x39, 0, *(uint32_t *)(obj + 0x18), id, (uint32_t)(int32_t)(v + (v >= 0.f ? 0.5f : -0.5f)), 0 };
    fw_push((uint8_t *)ctx + 0x1529c, m);
}

/* Replaces bl FUN_0804467c @0x08048e6e: after a style change the M7 tells the M4 the values the style set, so the
 * panel shows them. Stock sends Low Cut, HI C and Pre Delay; this adds Diffusion and Spread. */
void hall_echo(uint8_t *obj, void *ctx)
{
    fw_echo(obj, ctx);
    echo_one(obj, ctx, 0x13e, *(float *)(obj + 0x78) * 1000.f);
    echo_one(obj, ctx, 0x146, *(float *)(obj + 0xa0) * 10.f);
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
            hall_echo(obj, ctx);
    }
    fw_stereo(bus, l, r);
    return n;
}

static void engine_init(int eng, float *mem, float fs)
{
    if (eng == ENG_HALL) {
        hall_init((struct hall *)mem, fs);
        return;
    }
    rw_init((struct rev_wrap *)mem, (uint32_t)eng, fs);
    float *em = mem + RW_ENGINE_OFS;
    if (eng == ENG_MVERB) mv_init(em, fs);
    else if (eng == ENG_SQUALL) sq_init(em, fs);
    else fv_init(em, fs);
}

static int engine_ok(int eng, const float *mem)
{
    if (eng == ENG_HALL)
        return ((const struct hall *)mem)->magic == HALL_MAGIC;
    const struct rev_wrap *rw = (const struct rev_wrap *)mem;
    return rw->magic == 0x56455257u && rw->engine == (uint32_t)eng;
}

static void engine_run(int eng, const struct hall_slot *sl, float *mem, float *l, float *r, unsigned n)
{
    if (eng == ENG_HALL) {
        struct hall_knobs k;
        hall_knobs_from(sl, &k);
        hall_process_block((struct hall *)mem, mem, &k, l, r, n);
        return;
    }
    struct rev_knobs k;
    rev_knobs_from(sl, &k);
    struct rev_wrap *rw = (struct rev_wrap *)mem;
    float *em = mem + RW_ENGINE_OFS;
    if (eng == ENG_MVERB) {
        float pre = k.predelay_s;
        k.predelay_s = 0.f;                   /* MVerb pre-delays the tank only, inside the engine */
        rw_pre(rw, mem, &k, l, r, n);
        k.predelay_s = pre;
        mv_process(em, &k, l, r, n);
    } else {
        rw_pre(rw, mem, &k, l, r, n);
        if (eng == ENG_SQUALL) sq_process(em, &k, l, r, n);
        else fv_process(em, &k, l, r, n);
    }
    rw_post(rw, &k, l, r, n);
}

/* Replaces vtable slot 0x0806adac (process). */
unsigned hall_process(uint8_t *obj, void *ctx)
{
    struct hall_slot *sl = slot_for(obj);
    if (!sl)
        return fw_process(obj, ctx);
    uint8_t want = obj[0x6e] ? ENG_NONE : sl->active;
    if (!sl->clearing && sl->owner == want && !want)
        return fw_process(obj, ctx);          /* its event loop calls hall_set, so knobs keep being tracked */

    float *l, *r;
    unsigned n = drain(obj, ctx, &l, &r);
    want = obj[0x6e] ? ENG_NONE : sl->active;
    float *mem = *(float **)(obj + 0x54);

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
            if (want)
                engine_init(want, mem, sample_rate(obj));
        }
        return chain(obj, ctx);
    }

    if (!engine_ok(want, mem)) {              /* memory reused behind our back (firmware restart): start over */
        sl->owner = 0;
        for (unsigned i = 0; i < n; i++)
            l[i] = r[i] = 0.f;
        return chain(obj, ctx);
    }
    engine_run(want, sl, mem, l, r, n);
    return chain(obj, ctx);
}
