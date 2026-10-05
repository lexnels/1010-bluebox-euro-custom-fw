/*
 * FX1 delay additions, M4 side: defines the delay's new params in the M4 param table (labels, ranges and the keys
 * projects and presets save them under). The M7 side is src/dly_m7.h.
 *
 * The table (FUN_08135e34) is an array indexed by id with a slot for every id below 0x20b; each definition is a call
 * FUN_08135e08(table, id, type, label, min, max, key). dly_defs replaces the last one (bl @0x0813714a, id 0x117) and
 * adds ours after it. Ids 0x3a, 0x3d, 0x4a, 0x4b are undefined in both cores' tables and used nowhere else.
 */
typedef void (*def_fn)(void *table, unsigned id, unsigned type, const char *label, int min, int max, const char *key);
#define fw_def ((def_fn)0x08135e09)
#define KNOB 8                      /* the type of the stock 0..1000 delay knobs (Delay, Feedback, Cutoff, Width) */
#define INT 1                       /* a plain whole number (stock: Channel 1..16, BPM): semitones */
#define TOGGLE 4                    /* the type of the stock buttons (Beat Sync, Filt, Ping, Quad) */

void dly_defs(void *table, unsigned id, unsigned type, const char *label, int min, int max, const char *key)
{
    fw_def(table, id, type, label, min, max, key);
    fw_def(table, 0x3a, KNOB, "Flutter:", 0, 1000, "dlyflutter");
    fw_def(table, 0x3d, KNOB, "Send:", 0, 1000, "dlysend");
    fw_def(table, 0x4a, INT, "Pitch:", -12, 12, "dlypitch");
    fw_def(table, 0x4b, TOGGLE, "Pitch:", 0, 1, "dlypitchon");
}

/*
 * The FX panel (FUN_0812bc60 populate, FUN_0812c110 layout) draws its widgets in compact mode in 5 columns of 2,
 * column-major in list order, from x0 + 48; the 48 px left of that are empty. The delay has 12 (7 knobs, 5 buttons),
 * so after populate and layout we move four: FILT (7) under Send, BEAT and PING (8, 9) to the empty column on the
 * left, PITCH and QUAD (10, 11) to column 5. Each widget slot i has a knob and a toggle (populate shows the one its
 * param needs); we move both, the same way layout does. Other panels get the stock positions back.
 */
#include <stdint.h>
typedef void (*vfn_rect)(void *w, int *rect);
typedef void (*populate_fn)(uint8_t *panel, short *slot);
typedef void (*layout_fn)(uint8_t *panel);
#define fw_populate ((populate_fn)0x0812bc61)
#define fw_layout ((layout_fn)0x0812c111)
#define P_COMPACT_OFF 0xc398        /* u8: 0 = compact (the only mode stock uses) */
#define P_SLOT 0xc3a4               /* u16: the FX slot shown, 0x14 = delay */
#define KNOB_W(p, i) ((uint8_t *)(p) + 0xc3b0 + (i) * 0x3d0)
#define TOG_W(p, i) ((uint8_t *)(p) + 0x100b0 + (i) * 0x1d4)
#define W_DIRTY 0x3d

static const int8_t DLY_COL[12] = { 0, 0, 1, 1, 2, 2, 3, 3, -1, -1, 4, 4 };

static void place(uint8_t *p)
{
    if (p[P_COMPACT_OFF])
        return;
    int dly = *(uint16_t *)(p + P_SLOT) == 0x14;
    int x0 = *(int *)(p + 4), y0 = *(int *)(p + 8);
    for (int i = 0; i < 12; i++) {
        int col = dly ? DLY_COL[i] : i >> 1;
        int r[4] = { x0 + 0x30 * (col + 1), y0 + 0x2d - 0x2c * (i & 1), 0x30, 0x2c };
        uint8_t *kw = KNOB_W(p, i), *tw = TOG_W(p, i);
        (*(vfn_rect **)kw)[0x20 / 4](kw, r);
        int r2[4] = { r[0], r[1], r[2], r[3] };
        (*(vfn_rect **)tw)[0x20 / 4](tw, r2);
        kw[W_DIRTY] = 1;
        tw[W_DIRTY] = 1;
    }
    p[W_DIRTY] = 1;
}

/* the three bl FUN_0812bc60 (populate) */
void dly_populate(uint8_t *p, short *slot)
{
    fw_populate(p, slot);
    place(p);
}

/* the bl FUN_0812c110 (layout) */
void dly_layout(uint8_t *p)
{
    fw_layout(p);
    place(p);
}
