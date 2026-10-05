/*
 * FX1 delay additions, M4 side: defines the delay's new params in the M4 param table (labels, ranges and the keys
 * projects and presets save them under). The M7 side is src/dly_m7.h.
 *
 * The table (FUN_08135e34) is an array indexed by id with a slot for every id below 0x20b; each definition is a call
 * FUN_08135e08(table, id, type, label, min, max, key). dly_defs replaces the last one (bl @0x0813714a, id 0x117) and
 * adds ours after it. Ids 0x3a, 0x3d, 0x4a, 0x4b, 0x43, 0x59 are undefined in both cores' tables and used nowhere else.
 */
typedef void (*def_fn)(void *table, unsigned id, unsigned type, const char *label, int min, int max, const char *key);
#define fw_def ((def_fn)0x08135e09)
#define KNOB 8                      /* the type of the stock 0..1000 delay knobs (Delay, Feedback, Cutoff, Width) */
#define INT 1                       /* a plain whole number (stock: Channel 1..16, BPM): semitones */
#define TOGGLE 4                    /* the type of the stock buttons (Beat Sync, Filt, Ping, Quad) */

/* A list param (type 5, like the compressor's Mode): FUN_08135db4(table, id, label, names, count, key). */
typedef void (*def_list_fn)(void *table, unsigned id, const char *label, const char *const *names, int count, const char *key);
#define fw_def_list ((def_list_fn)0x08135db5)
static const char *const usb_out_names[2] = { "Multichannel", "Master only" };

void dly_defs(void *table, unsigned id, unsigned type, const char *label, int min, int max, const char *key)
{
    fw_def(table, id, type, label, min, max, key);
    fw_def(table, 0x3a, KNOB, "Drift:", 0, 1000, "dlyflutter");   /* key kept from when it was Flutter */
    fw_def(table, 0x3d, KNOB, "Send:", 0, 1000, "dlysend");
    fw_def(table, 0x4a, INT, "Pitch:", -12, 12, "dlypitch");
    fw_def(table, 0x4b, TOGGLE, "Pitch:", 0, 1, "dlypitchon");
    fw_def(table, 0x43, KNOB, "Saturate:", 0, 1000, "mstdrive");     /* master saturator (patches/master.py) */
    fw_def_list(table, 0x59, "USB Out:", usb_out_names, 2, "usbout"); /* USB audio out mode (patches/usb.py) */
}

/* Master saturator Drive (0x43) in the global set (FUN_0812060c case 2), so edits are kept, saved with the project and
 * re-sent to the M7 on load: replaces the set's last add (bl FUN_081205e2 @0x08120b90, id 0x162) and adds ours after
 * it. The settings page shows it because patches/master.py puts 0x43 at the end of that page's id list. */
typedef void (*add_fn)(void *set, unsigned id, int value);
#define fw_add ((add_fn)0x081205e3)
void mst_set_add(void *set, unsigned id, int value)
{
    fw_add(set, id, value);
    fw_add(set, 0x43, 0);
    fw_add(set, 0x59, 0);           /* USB Out: Multichannel */
}

/*
 * The FX panel (FUN_0812bc60 populate, FUN_0812c110 layout) draws its widgets in compact mode in 5 columns of 2,
 * column-major in list order, from x0 + 48; the 48 px left of that are empty. The delay has 12 (7 knobs, 5 buttons),
 * so we move three: BEAT and PING (8, 9) to the empty column on the left, PITCH and QUAD (10, 11) to column 5 (FILT,
 * 7, is already under Send). Each widget slot i has a knob and a toggle (populate shows the one its param needs); we
 * move both, the same way layout does. Other panels get the stock positions back.
 *
 * Only through setRect (vtable +0x20), never by poking fields: v0.7/0.8 also set byte +0x3d of each widget as a
 * "dirty" flag, which on a button is inside its label's y position (+0x3c, see FUN_0813940c) and threw every
 * button label off screen. Widgets are only moved when they aren't already in place.
 */
#include <stdint.h>
typedef void (*vfn_rect)(void *w, int *rect);
typedef void (*populate_fn)(uint8_t *panel, short *slot);
typedef void (*layout_fn)(uint8_t *panel);
#define fw_populate ((populate_fn)0x0812bc61)
#define fw_layout ((layout_fn)0x0812c111)
#define P_COMPACT_OFF 0xc398        /* u8: 0 = compact (the only mode stock uses) */
#define P_SLOT 0xc3a4               /* u16: the slot populate last showed (0x14 delay, 0x15 reverb) */
#define KNOB_W(p, i) ((uint8_t *)(p) + 0xc3b0 + (i) * 0x3d0)
#define TOG_W(p, i) ((uint8_t *)(p) + 0x100b0 + (i) * 0x1d4)

static const int8_t DLY_COL[4] = { -1, -1, 4, 4 };  /* widgets 8..11 on the delay panel */

static void move(uint8_t *w, int x, int y)
{
    if (*(int *)(w + 4) == x && *(int *)(w + 8) == y)
        return;
    int r[4] = { x, y, 0x30, 0x2c };
    (*(vfn_rect **)w)[0x20 / 4](w, r);
}

static void place(uint8_t *p, int slot)
{
    if (p[P_COMPACT_OFF] || (uint16_t)(slot - 0x14) >= 2)
        return;
    int x0 = *(int *)(p + 4), y0 = *(int *)(p + 8);
    for (int i = 8; i < 12; i++) {
        int col = slot == 0x14 ? DLY_COL[i - 8] : i >> 1;
        int x = x0 + 0x30 * (col + 1), y = y0 + 0x2d - 0x2c * (i & 1);
        move(KNOB_W(p, i), x, y);
        move(TOG_W(p, i), x, y);
    }
}

/* the three bl FUN_0812bc60 (populate) */
void dly_populate(uint8_t *p, short *slot)
{
    place(p, *slot);
    fw_populate(p, slot);
}

/* the bl FUN_0812c110 (layout) */
void dly_layout(uint8_t *p)
{
    fw_layout(p);
    place(p, *(short *)(p + P_SLOT));
}
