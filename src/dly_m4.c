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
static const char *const cho_mode_names[3] = { "I", "II", "I+II" };
#define SYNC_NAMES ((const char *const *)0x0814ea24)    /* the stock delay's 0x34 list: 1/64 .. 1 bar (12) */
#include "sfx_ids.h"
static void sfx_reset(void);

void dly_defs(void *table, unsigned id, unsigned type, const char *label, int min, int max, const char *key)
{
    fw_def(table, id, type, label, min, max, key);
    fw_def(table, 0x3a, KNOB, "Drift:", 0, 1000, "dlyflutter");   /* key kept from when it was Flutter */
    fw_def(table, 0x3d, KNOB, "Send:", 0, 1000, "dlysend");
    fw_def(table, 0x4a, INT, "Pitch:", -12, 12, "dlypitch");
    fw_def(table, 0x4b, TOGGLE, "Pitch:", 0, 1, "dlypitchon");
    fw_def(table, 0x43, KNOB, "Saturate:", 0, 1000, "mstdrive");     /* master saturator (patches/master.py) */
    fw_def_list(table, 0x59, "USB Out:", usb_out_names, 2, "usbout"); /* USB audio out mode (patches/usb.py) */
    /* send FX (patches/sfx.py) */
    fw_def(table, SFX_CHO_ON, TOGGLE, "On:", 0, 1, "chorus_on");
    fw_def(table, SFX_CHO_FX1, KNOB, "FX1 Send:", 0, 1000, "chorus_fx1");
    fw_def(table, SFX_CHO_FX2, KNOB, "FX2 Send:", 0, 1000, "chorus_fx2");
    fw_def_list(table, SFX_CHO_MODE, "Mode:", cho_mode_names, 3, "chorus_mode");
    fw_def(table, SFX_CHO_RATE, KNOB, "Rate:", 0, 1000, "chorus_rate");
    fw_def(table, SFX_CHO_DEPTH, KNOB, "Depth:", 0, 1000, "chorus_depth");
    fw_def(table, SFX_CHO_LEVEL, KNOB, "Level:", 0, 1000, "chorus_level");
    fw_def(table, SFX_DRV_ON, TOGGLE, "On:", 0, 1, "drive_on");
    fw_def(table, SFX_DRV_FX1, KNOB, "FX1 Send:", 0, 1000, "drive_fx1");
    fw_def(table, SFX_DRV_FX2, KNOB, "FX2 Send:", 0, 1000, "drive_fx2");
    fw_def(table, SFX_DRV_DRIVE, KNOB, "Drive:", 0, 1000, "drive_drive");
    fw_def(table, SFX_DRV_TONE, KNOB, "Tone:", 0, 1000, "drive_tone");
    fw_def(table, SFX_DRV_LEVEL, KNOB, "Level:", 0, 1000, "drive_level");
    fw_def(table, SFX_D2_ON, TOGGLE, "On:", 0, 1, "delay2_on");
    fw_def(table, SFX_D2_FX1, KNOB, "FX1 Send:", 0, 1000, "delay2_fx1");
    fw_def(table, SFX_D2_FX2, KNOB, "FX2 Send:", 0, 1000, "delay2_fx2");
    fw_def(table, SFX_D2_TIME, KNOB, "Time:", 0, 1000, "delay2_time");
    fw_def(table, SFX_D2_FB, KNOB, "Feedback:", 0, 1000, "delay2_fb");
    fw_def(table, SFX_D2_TONE, KNOB, "Tone:", 0, 1000, "delay2_tone");
    fw_def(table, SFX_D2_PING, TOGGLE, "Ping:", 0, 1, "delay2_ping");
    fw_def(table, SFX_D2_LEVEL, KNOB, "Level:", 0, 1000, "delay2_level");
    fw_def(table, SFX_D2_BEAT, TOGGLE, "Beat Sync:", 0, 1, "delay2_beat");
    fw_def_list(table, SFX_D2_SYNC, "Time:", SYNC_NAMES, 12, "delay2_sync");
    /* track sends, per channel (type 8 0..1000 like FX1 0xda) */
    fw_def(table, SFX_TS_CHO, KNOB, "FX3:", 0, 1000, "chorus_send");
    fw_def(table, SFX_TS_DRV, KNOB, "FX4:", 0, 1000, "drive_send");
    fw_def(table, SFX_TS_D2, KNOB, "FX5:", 0, 1000, "delay2_send");
    sfx_reset();                    /* at boot the FX button starts from the reverb itself */
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

/*
 * Send FX panels. The FX button (dispatcher FUN_081244ac, button 4) cycles views 0xe (FX1 delay) -> 0x10 (FX2 reverb)
 * -> 0x13 -> 0xe; views 0xe/0x10 show the FX panel for slot 0x14/0x15. We give the reverb view three more turns:
 * Chorus, Drive and Delay 2 are the reverb panel showing other params of the reverb slot's set, picked by the mode
 * below. Their params are in that set (added after the reverb's own), so edits are kept, saved with the project and
 * sent to the M7 like the reverb's; the panel's list (FUN_081227f0) is filtered by mode.
 */
struct sfx_ui {
    uint32_t magic;
    uint32_t mode;                  /* the FX button: 0 reverb, 1 chorus, 2 drive, 3 delay 2 */
    uint16_t rows[6];               /* the track screen's second page's param per row (stock 0x0814e280, or ours) */
    uint32_t page3;                 /* the track screen's second page shows our sends (its third page) */
};
#define SU ((volatile struct sfx_ui *)0x38800fe0u)    /* own 32-byte line, M4 only */
#define SU_MAGIC 0x49555846u
#define RCC_AHB4ENR_M4 (*(volatile uint32_t *)0x580244e0u)
#define PWR_CR1_M4 (*(volatile uint32_t *)0x58024800u)

static void su_write(uint32_t mode)
{
    RCC_AHB4ENR_M4 |= 1u << 28;
    (void)RCC_AHB4ENR_M4;
    PWR_CR1_M4 |= 1u << 8;
    SU->mode = mode;
    if (SU->magic != SU_MAGIC)
        SU->page3 = 0;
    SU->magic = SU_MAGIC;
}
static uint32_t su_mode(void)
{
    RCC_AHB4ENR_M4 |= 1u << 28;
    (void)RCC_AHB4ENR_M4;
    return SU->magic == SU_MAGIC && SU->mode <= 3 ? SU->mode : 0;
}
static void sfx_reset(void)
{
    su_write(0);
    SU->page3 = 0;
    for (int i = 0; i < 6; i++)
        SU->rows[i] = ((const uint16_t *)0x0814e280)[i];
}

/* the panel order: knobs fill columns of 2, encoders take 4 at a time */
static const int16_t SFX_IDS[] = {
    SFX_CHO_MODE, 0, SFX_CHO_RATE, 500, SFX_CHO_DEPTH, 500, SFX_CHO_LEVEL, 1000, SFX_CHO_FX1, 0, SFX_CHO_FX2, 0,
    SFX_CHO_ON, 0,
    SFX_DRV_DRIVE, 500, SFX_DRV_TONE, 600, SFX_DRV_LEVEL, 500, SFX_DRV_ON, 0, SFX_DRV_FX1, 0, SFX_DRV_FX2, 0,
    SFX_D2_TIME, 700, SFX_D2_SYNC, 8, SFX_D2_FB, 400, SFX_D2_TONE, 600, SFX_D2_LEVEL, 700, SFX_D2_FX1, 0, SFX_D2_FX2, 0,
    SFX_D2_PING, 0, SFX_D2_BEAT, 0, SFX_D2_ON, 0,
};
static int sfx_of(unsigned id)      /* which FX an id belongs to, 0 for none */
{
    if ((id >= SFX_CHO_ON && id <= SFX_CHO_LEVEL) || id == SFX_CHO_FX2 || id == SFX_CHO_RATE || id == SFX_CHO_DEPTH) return 1;
    if (id == SFX_DRV_ON || id == SFX_DRV_FX1 || (id >= SFX_DRV_DRIVE && id <= SFX_DRV_LEVEL) || id == SFX_DRV_FX2) return 2;
    if ((id >= SFX_D2_ON && id <= SFX_D2_LEVEL) || id == SFX_D2_FX2 || id == SFX_D2_BEAT || id == SFX_D2_SYNC) return 3;
    return 0;
}

/* After the reverb set's own adds (the hall table loop's exit @0x08120c34 comes here through sfx_rv_tail). */
void sfx_rv_add(void *set)
{
    for (unsigned i = 0; i < sizeof SFX_IDS / sizeof SFX_IDS[0]; i += 2)
        fw_add(set, (uint16_t)SFX_IDS[i], SFX_IDS[i + 1]);
}
__attribute__((naked)) void sfx_rv_tail(void)
{
    __asm volatile(
        "mov r0, r4\n"              /* the set; r4 and r5 survive the call, as the common tail needs */
        "bl sfx_rv_add\n"
        "movw r3, #0x088d\n"
        "movt r3, #0x0812\n"
        "bx r3\n");                 /* the common tail of FUN_0812060c @0x0812088c */
}

/* bl FUN_081227f0 @0x0812bcca (the panel's list): only the current FX's params on the reverb panel */
typedef int (*list_fn)(void *app, uint16_t *slot, uint32_t *out);
#define fw_list ((list_fn)0x081227f1)
int sfx_list(void *app, uint16_t *slot, uint32_t *out)
{
    int ret = fw_list(app, slot, out);
    if (*slot != 0x15)
        return ret;
    int mode = (int)su_mode();
    uint32_t n = out[0], k = 0;     /* entries of 12 bytes from out + 4: u16 id, ..., value at +8 */
    unsigned hide = SFX_D2_SYNC;    /* Delay 2 shows Time, or with Beat Sync on the note value, in its place */
    for (uint32_t i = 0; i < n; i++)
        if (*(uint16_t *)(out + 1 + 3 * i) == SFX_D2_BEAT && out[1 + 3 * i + 2])
            hide = SFX_D2_TIME;
    for (uint32_t i = 0; i < n; i++) {
        unsigned id = *(uint16_t *)(out + 1 + 3 * i);
        if (sfx_of(id) != mode || id == hide)
            continue;
        if (k != i)
            for (int w = 0; w < 3; w++)
                out[1 + 3 * k + w] = out[1 + 3 * i + w];
        k++;
    }
    out[0] = k;
    return ret;
}

/* bl FUN_081427e2 @0x0812bc90 / @0x0812bc9c: the reverb panel's titles "FX2" and "Reverb" */
typedef void (*text_fn)(void *w, const char *s);
#define fw_text ((text_fn)0x081427e3)
void sfx_title(void *w, const char *s)
{
    static const char *const t[4] = { 0, "FX3", "FX4", "FX5" };
    uint32_t m = su_mode();
    fw_text(w, m ? t[m] : s);
}
void sfx_name(void *w, const char *s)
{
    static const char *const t[4] = { 0, "Chorus", "Drive", "Delay 2" };
    uint32_t m = su_mode();
    fw_text(w, m ? t[m] : s);
}

/* The FX button's calls FUN_08123158(app, view, 0, 0): from the delay to the reverb (@0x081247d8), and from the
 * reverb on (@0x081247e4, to view 0x13): the reverb view comes back three times, as Chorus, Drive and Delay 2. */
typedef void (*view_fn)(void *app, unsigned view, int a, int b);
#define fw_view ((view_fn)0x08123159)
void sfx_to_reverb(void *app, unsigned view, int a, int b)
{
    su_write(0);
    fw_view(app, view, a, b);
}
void sfx_from_reverb(void *app, unsigned view, int a, int b)
{
    uint32_t m = su_mode();
    if (m < 3) {
        su_write(m + 1);
        fw_view(app, 0x10, a, b);
    } else {
        su_write(0);
        fw_view(app, view, a, b);
    }
}

/*
 * Track sends. Each channel's set (FUN_0812060c case 1) gets three more params, so they are kept, saved with the
 * project and sent to the M7 with the channel's slot like FX1/FX2 (src/sfx_m7.c sums them).
 *
 * The track screen (views 2 and 3, page app+0x558) shows one row's param for every channel; view 2 has Vol, Gain,
 * Solo, Mute, Rec, view 3 Pan, FX1, FX2, CUE, OUT3, OUT4 (FUN_0812f604 sets the labels, FUN_0812f398 picks a row from
 * the table at 0x0814e280 for view 3). The track button shows our page between the track and sidechain screens:
 * view 3 with rows FX3, FX4, FX5 (our sends), from a table in backup SRAM that the literal @0x0812f51c now points to.
 */
void ts_set_add(void *set, unsigned id, int value)    /* the channel set's last add (bl @0x08120888, id 0x173) */
{
    fw_add(set, id, value);
    fw_add(set, SFX_TS_CHO, 0);
    fw_add(set, SFX_TS_DRV, 0);
    fw_add(set, SFX_TS_D2, 0);
}

static uint32_t ts_page3(void)
{
    su_mode();                      /* backup SRAM on */
    return SU->magic == SU_MAGIC && SU->page3 == 1;
}
static void ts_set_page3(uint32_t on)
{
    su_mode();
    if (SU->magic != SU_MAGIC)
        su_write(0);
    SU->page3 = on;
}

/* The mixer button (button 0, dispatcher event 0xf9) shows the stock pages only: going to the second page (view 3)
 * from the first (@0x0812471e) or from another screen (@0x08124712, the last mixer view) clears our flag. */
void ts_mixer_view(void *app, unsigned view, int a, int b)
{
    ts_set_page3(0);
    fw_view(app, view, a, b);
}

/* The track button (button 1): from the track screen (views 5, 6) it goes to the sidechain screen (view 0x16,
 * @0x08124758), and from anywhere else back to the track screen (@0x0812474c). Our page goes between the track and
 * sidechain screens: the mixer's second page (view 3) with our rows. */
#define APP_VIEW(app) (*((uint8_t *)(app) + 0x73d2))
void ts_track_to_sends(void *app, unsigned view, int a, int b)
{
    (void)view;
    ts_set_page3(1);
    fw_view(app, 3, a, b);
}
void ts_track_back(void *app, unsigned view, int a, int b)
{
    if (APP_VIEW(app) == 3 && ts_page3()) {
        ts_set_page3(0);
        fw_view(app, 0x16, a, b);   /* on from our page to the sidechain screen */
    } else
        fw_view(app, view, a, b);
}

#define TS_SUB(p) (*(int *)((uint8_t *)(p) + 0xd26c))          /* the page shown: 0 = view 2, 1 = view 3 */
#define TS_ROW(p, sub) (*(int *)((uint8_t *)(p) + (0x349c + (sub)) * 4))   /* the row picked on each */
typedef void (*page_fn)(void *page, int sub);
typedef void (*label_fn)(void *w, const char *s);
#define fw_page ((page_fn)0x0812f605)
#define fw_row ((page_fn)0x0812f399)
#define fw_label ((label_fn)0x08139725)
static const uint16_t TS_LABEL[6] = { 0xc084, 0xc124, 0xc1c4, 0xc264, 0xc304, 0xc3a4 };  /* and each + 0x3c0 */

/* both bl FUN_0812f604 (@0x081356a8 on event 0x7f, @0x0812f8ec on a redraw) */
void ts_page(void *page, int sub)
{
    static const uint16_t ours[6] = { SFX_TS_CHO, SFX_TS_DRV, SFX_TS_D2, 0, 0, 0 };
    static const char *const names[6] = { "FX3", "FX4", "FX5", "", "", "" };
    int on = ts_page3() && sub == 1;     /* (ts_page3 first: it switches backup SRAM on) */
    for (int i = 0; i < 6; i++)
        SU->rows[i] = on ? ours[i] : ((const uint16_t *)0x0814e280)[i];
    if (on && (unsigned)TS_ROW(page, 1) > 2)
        TS_ROW(page, 1) = 0;
    fw_page(page, sub);
    if (on) {
        for (int i = 0; i < 6; i++) {
            fw_label((uint8_t *)page + TS_LABEL[i], names[i]);
            fw_label((uint8_t *)page + TS_LABEL[i] + 0x3c0, names[i]);
        }
        fw_row(page, TS_ROW(page, 1));      /* again: the highlight follows the labels */
    }
}

/* bl FUN_0812f398 @0x0812f590 (a row picked): our page has three */
void ts_row(void *page, int row)
{
    if (TS_SUB(page) == 1 && ts_page3() && (unsigned)row > 2)
        return;
    fw_row(page, row);
}
