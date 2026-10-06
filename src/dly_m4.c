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
    fw_def(table, SFX_CHO_WIDTH, KNOB, "Width:", 0, 1000, "chorus_width");
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
    uint32_t tpage;                 /* the track screen's page (seen by ts_tp_setup) */
    uint16_t _rows[4];              /* (unused) */
    uint32_t page3;                 /* the track screen shows our sends (between it and the sidechain screen) */
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
}

/* the panel order: knobs fill columns of 2, encoders take 4 at a time */
static const int16_t SFX_IDS[] = {
    SFX_CHO_MODE, 0, SFX_CHO_RATE, 500, SFX_CHO_DEPTH, 500, SFX_CHO_WIDTH, 500, SFX_CHO_LEVEL, 1000, SFX_CHO_FX1, 0, SFX_CHO_FX2, 0,
    SFX_CHO_ON, 0,
    SFX_DRV_DRIVE, 500, SFX_DRV_TONE, 600, SFX_DRV_LEVEL, 500, SFX_DRV_ON, 0, SFX_DRV_FX1, 0, SFX_DRV_FX2, 0,
    SFX_D2_TIME, 700, SFX_D2_SYNC, 8, SFX_D2_FB, 400, SFX_D2_TONE, 600, SFX_D2_LEVEL, 700, SFX_D2_FX1, 0, SFX_D2_FX2, 0,
    SFX_D2_PING, 0, SFX_D2_BEAT, 0, SFX_D2_ON, 0,
};
static int sfx_of(unsigned id)      /* which FX an id belongs to, 0 for none */
{
    if ((id >= SFX_CHO_ON && id <= SFX_CHO_LEVEL) || id == SFX_CHO_FX2 || id == SFX_CHO_RATE || id == SFX_CHO_DEPTH || id == SFX_CHO_WIDTH) return 1;
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
 * The track screen (page app+0xcfb70, views 5 and 6 = its two halves, FUN_08133884 picks one, FUN_081339b0 fills
 * its knobs for the selected track) has Vol, Pan, Gain on the first half, FX1, FX2, OUT3, OUT4 (and CUE) on the
 * second. The track button goes track screen -> sidechain screen (view 0x16); we put a turn between: the second
 * half again, with FX3, FX4, FX5 (our sends) in place of FX1, FX2, OUT3, and OUT4 and CUE hidden.
 */
void ts_set_add(void *set, unsigned id, int value)    /* the channel set's last add (bl @0x08120888, id 0x173) */
{
    fw_add(set, id, value);
    fw_add(set, SFX_TS_CHO, 0);
    fw_add(set, SFX_TS_DRV, 0);
    fw_add(set, SFX_TS_D2, 0);
}

static uint32_t ts_on(void)
{
    su_mode();                      /* backup SRAM on */
    return SU->magic == SU_MAGIC && SU->page3 == 1;
}
static uint32_t ts_armed(void)
{
    su_mode();
    return SU->magic == SU_MAGIC && SU->page3 == 2;
}
static void ts_set(uint32_t on)
{
    su_mode();                      /* backup SRAM on */
    if (SU->magic != SU_MAGIC)
        su_write(0);
    PWR_CR1_M4 |= 1u << 8;
    SU->page3 = on;
}

/* the track button (button 1): from the track screen (views 5, 6) to the sidechain screen (@0x08124758), from
 * anywhere else back to the track screen (@0x0812474c, its last half) */
void ts_track_next(void *app, unsigned view, int a, int b)
{
    if (ts_on()) {
        ts_set(0);
        fw_view(app, view, a, b);   /* from our sends on to the sidechain screen */
    } else {
        ts_set(2);                  /* armed: the track screen's setup turns it on */
        fw_view(app, 6, a, b);      /* the track screen's second half, as our sends */
    }
}
void ts_track_back(void *app, unsigned view, int a, int b)
{
    ts_set(0);
    fw_view(app, view, a, b);
}

#define TP_SUB(p) (*(int *)((uint8_t *)(p) + 0xebbc))   /* the track screen's half: 0 first, 1 second */
#define TP_KNOB(p, off) ((uint8_t *)(p) + (off))
#define TP_FX1 0xcf08               /* knobs of the second half: encoder 1 */
#define TP_FX2 0xd2d8               /* encoder 3 */
#define TP_OUT3 0xda78              /* encoder 2 */
#define TP_OUT4 0xde48              /* encoder 4 */
#define TP_CUE 0xd6a8
#define TP_CUE_BTN 0xe814
#define FW_STATE ((void *)0x30018010)                  /* the params (the literal FUN_081339b0 uses) */
typedef void (*tp_setup_fn)(void *page, int sub, int a, int b);
typedef void (*tp_fill_fn)(void *page, void *slot);
typedef void (*getp_fn)(void *state, uint16_t *slot, uint16_t *id, int *value);
typedef void (*info_fn)(void *state, uint16_t *slot, unsigned id, uint8_t *info);
typedef void (*knob_bind_fn)(void *w, unsigned id, int value, int force);
typedef void (*knob_info_fn)(void *w, uint8_t *info);
typedef void (*show_fn)(void *w, int on);
typedef void (*turn_fn)(void *w, int delta);
#define fw_tp_setup ((tp_setup_fn)0x08133885)
#define fw_tp_fill ((tp_fill_fn)0x081339b1)
#define fw_getp ((getp_fn)0x08122581)
#define fw_info ((info_fn)0x08124959)
#define fw_knob_bind ((knob_bind_fn)0x08128bd1)
#define fw_knob_info ((knob_info_fn)0x08128fc9)
#define fw_knob_name ((show_fn)0x081288fd)  /* redraws a knob's name from its text (FUN_081339b0 after OUT3/OUT4's) */
#define fw_turn ((turn_fn)0x08128d9d)

/* hide (1) or show (0) a widget: its vtable slot 0x2c, as the FX panel hides the knobs it has no param for */
static void ts_vis(void *page, unsigned off, int hide)
{
    uint8_t *w = TP_KNOB(page, off);
    ((void (*)(void *, int))(*(uint32_t **)w)[0x2c / 4])(w, hide);
}
/* on our sends only FX3, FX4, FX5 show: Vol, Pan, Gain, CUE (and its button), OUT4 hidden */
static const uint16_t TP_OTHERS[] = { 0xc398, 0xc768, 0xcb38, TP_CUE, TP_OUT4, TP_CUE_BTN };
static void ts_hide(void *page, int hide)
{
    for (unsigned i = 0; i < sizeof TP_OTHERS / sizeof TP_OTHERS[0]; i++)
        ts_vis(page, TP_OTHERS[i], hide);
}

/* bl FUN_08133884 @0x08135720 (event 0x8c: the track screen shown, sub = its half): the first half ends our sends */
void ts_tp_setup(void *page, int sub, int a, int b)
{
    /* our sends only when ts_track_next asked for them (armed, 2); any other showing is the stock half. (The view
     * switch only queues event 0x8c, so this runs after it.) */
    uint32_t armed = ts_armed();
    ts_set(sub == 1 && armed);
    SU->tpage = (uint32_t)page;
    fw_tp_setup(page, sub, a, b);
    ts_hide(page, sub == 1 && ts_on());     /* (the stock halves show them all again) */
}

static void bind(void *page, unsigned off, uint16_t slot, unsigned id)
{
    uint16_t sl = slot, pid = (uint16_t)id;
    int v = 0;
    uint32_t info[8];               /* as FUN_081339b0 sets it: [0] = 1, [2] = 0, [3] = 1, [4] = 0 */
    for (int i = 0; i < 8; i++)
        ((volatile uint32_t *)info)[i] = 0;
    ((uint8_t *)info)[0] = 1;
    ((uint8_t *)info)[3] = 1;
    fw_getp(FW_STATE, &sl, &pid, &v);
    fw_knob_bind(TP_KNOB(page, off), id, v, 0);
    sl = slot;
    fw_info(FW_STATE, &sl, id, (uint8_t *)info);
    fw_knob_info(TP_KNOB(page, off), (uint8_t *)info);
}

/* the three bl FUN_081339b0 in the track screen's handler (@0x081342d8, @0x081342f4, @0x08134316): fill as stock,
 * then on our sends rebind the second half's knobs */
void ts_tp_fill(void *page, void *arg)
{
    fw_tp_fill(page, arg);
    if (TP_SUB(page) != 1 || !ts_on())
        return;
    uint16_t slot = *(uint16_t *)((uint8_t *)arg + 4);
    if (slot >= 12)
        return;
    bind(page, TP_FX1, slot, SFX_TS_CHO);
    bind(page, TP_FX2, slot, SFX_TS_DRV);
    bind(page, TP_OUT3, slot, SFX_TS_D2);
    /* OUT3's name is its own text (+0xf8: "OUT3" or "OUT3&4", set after the bind), not the param's */
    char *name = (char *)TP_KNOB(page, TP_OUT3) + 0xf8;
    name[0] = 'F'; name[1] = 'X'; name[2] = '5'; name[3] = 0;
    TP_KNOB(page, TP_OUT3)[0x1f7] = 0;
    fw_knob_name(TP_KNOB(page, TP_OUT3), 1);
    ts_hide(page, 1);
}

/* bl FUN_08128d9c @0x08134360 (an encoder turned on the track screen): the fourth does nothing on our sends */
void ts_tp_turn(uint8_t *w, int delta)
{
    if (ts_on() && SU->tpage && w == TP_KNOB(SU->tpage, TP_OUT4))
        return;
    fw_turn(w, delta);
}
