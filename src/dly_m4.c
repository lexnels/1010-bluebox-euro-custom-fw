/*
 * FX1 delay additions, M4 side: defines the delay's new params in the M4 param table (labels, ranges and the keys
 * projects and presets save them under). The M7 side is src/dly_m7.h.
 *
 * The table (FUN_08135e34) is an array indexed by id with a slot for every id below 0x20b; each definition is a call
 * FUN_08135e08(table, id, type, label, min, max, key). dly_defs replaces the last one (bl @0x0813714a, id 0x117) and
 * adds ours after it. Ids 0x3a, 0x3d, 0x43, 0x4a, 0x4b are undefined in both cores' tables and used nowhere else.
 */
typedef void (*def_fn)(void *table, unsigned id, unsigned type, const char *label, int min, int max, const char *key);
#define fw_def ((def_fn)0x08135e09)
#define KNOB 8                      /* the type of the stock 0..1000 delay knobs (Delay, Feedback, Cutoff, Width) */
#define TOGGLE 4                    /* the type of the stock buttons (Beat Sync, Filt, Ping, Quad) */

void dly_defs(void *table, unsigned id, unsigned type, const char *label, int min, int max, const char *key)
{
    fw_def(table, id, type, label, min, max, key);
    fw_def(table, 0x3a, KNOB, "Flutter:", 0, 1000, "dlyflutter");
    fw_def(table, 0x3d, KNOB, "Send:", 0, 1000, "dlysend");
    fw_def(table, 0x43, KNOB, "Resonance:", 0, 1000, "dlyreso");
    fw_def(table, 0x4a, KNOB, "Pitch:", -12, 12, "dlypitch");
    fw_def(table, 0x4b, TOGGLE, "Pitch:", 0, 1, "dlypitchon");
}

/*
 * The FX panel (FUN_0812bc60 populate, FUN_0812c110 layout, FUN_0812bbfc page select) draws 10 widgets in compact
 * mode, 5 columns of 2, column-major, and pages the encoders 4 widgets (2 columns) at a time. The delay has 13, so
 * the panel slides: the window of 5 columns moves just far enough to show the encoder page's two columns. Each
 * widget slot i has a knob and a toggle; populate shows the one its param needs and hides the rest, and with the
 * stock "hide past 10" check patched out it does so for all 16. We wrap every call to the three functions, then place
 * and show/hide the widgets ourselves, the same way layout does. With 10 or fewer widgets the window never moves
 * and the result is the stock layout.
 */
#include <stdint.h>
typedef void (*vfn_int)(void *w, int v);
typedef void (*vfn_rect)(void *w, int *rect);
typedef void (*populate_fn)(uint8_t *panel, short *slot);
typedef void (*page_fn)(uint8_t *panel, int page, int a2, int a3);
typedef void (*layout_fn)(uint8_t *panel);
#define fw_populate ((populate_fn)0x0812bc61)
#define fw_page ((page_fn)0x0812bbfd)
#define fw_layout ((layout_fn)0x0812c111)
#define P_COMPACT_OFF 0xc398        /* u8: 0 = compact (the only mode stock uses) */
#define P_PAGE 0xc3a0               /* int: encoder page */
#define KNOB_W(p, i) ((uint8_t *)(p) + 0xc3b0 + (i) * 0x3d0)
#define TOG_W(p, i) ((uint8_t *)(p) + 0x100b0 + (i) * 0x1d4)
#define W_HIDDEN 0x30
#define W_DIRTY 0x3d

static int kind(uint8_t *p, int i)  /* 1 knob, 2 toggle, 0 unused (or hidden by populate) */
{
    return !TOG_W(p, i)[W_HIDDEN] ? 2 : !KNOB_W(p, i)[W_HIDDEN] ? 1 : 0;
}

static int last_used(const uint8_t *kinds)
{
    int last = -1;
    for (int i = 0; i < 16; i++)
        if (kinds[i]) last = i;
    return last;
}

/* The kinds as populate left them, kept in backup SRAM (0x38800ec0, free between the delay state and the CPU meter;
 * the M7 enables writes to it at boot), one record per panel object. If the write didn't stick, we fall back to
 * what is visible now and never slide, which is the stock panel. */
struct kinds_rec { uint32_t magic, panel; uint8_t k[16]; };
#define KREC ((volatile struct kinds_rec *)0x38800ec0)
#define KMAGIC 0x50414e4cu
static volatile struct kinds_rec *rec(uint8_t *p, int make)
{
    for (int i = 0; i < 2; i++)
        if (KREC[i].magic == KMAGIC && KREC[i].panel == (uint32_t)p) return &KREC[i];
    if (!make) return 0;
    volatile struct kinds_rec *r = KREC[0].magic == KMAGIC && KREC[1].magic != KMAGIC ? &KREC[1] : &KREC[0];
    r->magic = KMAGIC;
    r->panel = (uint32_t)p;
    return r->magic == KMAGIC && r->panel == (uint32_t)p ? r : 0;
}
static void mark(uint8_t *p)
{
    volatile struct kinds_rec *r = rec(p, 1);
    if (r)
        for (int i = 0; i < 16; i++) r->k[i] = (uint8_t)kind(p, i);
}
static int kinds_of(uint8_t *p, uint8_t *kinds)    /* 1 if they are the stored ones */
{
    volatile struct kinds_rec *r = rec(p, 0);
    for (int i = 0; i < 16; i++) kinds[i] = r ? r->k[i] : (uint8_t)kind(p, i);
    return r != 0;
}

static void slide(uint8_t *p)
{
    if (p[P_COMPACT_OFF])
        return;
    uint8_t kinds[16];
    int stored = kinds_of(p, kinds);
    int ncol = last_used(kinds) / 2 + 1, s = 0;
    if (ncol > 5 && stored) {
        int end = 2 * *(int *)(p + P_PAGE) + 2;
        if (end > ncol) end = ncol;
        s = end - 5;
        if (s < 0) s = 0;
    }
    int x0 = *(int *)(p + 4), y0 = *(int *)(p + 8);
    for (int i = 0; i < 16; i++) {
        int col = i >> 1, in = col >= s && col < s + 5;
        uint8_t *kw = KNOB_W(p, i), *tw = TOG_W(p, i);
        if (in) {
            int r[4] = { x0 + 0x30 * (col - s + 1), y0 + 0x2d - 0x2c * (i & 1), 0x30, 0x2c };
            (*(vfn_rect **)kw)[0x20 / 4](kw, r);
            int r2[4] = { r[0], r[1], r[2], r[3] };
            (*(vfn_rect **)tw)[0x20 / 4](tw, r2);
        }
        (*(vfn_int **)kw)[0x2c / 4](kw, !(in && kinds[i] == 1));
        (*(vfn_int **)tw)[0x2c / 4](tw, !(in && kinds[i] == 2));
        kw[W_DIRTY] = 1;
        tw[W_DIRTY] = 1;
    }
    p[W_DIRTY] = 1;
}

/* the three bl FUN_0812bc60 (populate) */
void dly_populate(uint8_t *p, short *slot)
{
    int ours = (uint16_t)(*slot - 0x14) < 2;
    fw_populate(p, slot);
    if (ours) {
        mark(p);
        slide(p);
    }
}

/* the five bl FUN_0812bbfc (page select): wrap past the last used widget, then slide */
void dly_page(uint8_t *p, int page, int a2, int a3)
{
    if (!p[P_COMPACT_OFF]) {
        uint8_t kinds[16];
        kinds_of(p, kinds);
        if (page < 0 || page * 4 > last_used(kinds)) page = 0;
    }
    fw_page(p, page, a2, a3);
    slide(p);
}

/* the bl FUN_0812c110 (layout) */
void dly_layout(uint8_t *p)
{
    fw_layout(p);
    slide(p);
}
