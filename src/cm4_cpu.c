/*
 * CPU meter, M4 side: a thin vertical bar at the top right of the screen, right of the H:MM:SS clock.
 *
 * The UI renders each layer into an off-screen surface, then FUN_081385dc(surface, layer) blits it into that LTDC
 * layer's back buffer (descriptor from FUN_081013d2) and flips with FUN_08101384(display, layer). We replace the flip
 * call @0x0813861a: draw the bar into the back buffer, then flip as stock does.
 *
 * Back buffer descriptor: +0 pixels, +4 u16 width (stride), +6 u16 height, +8 u8 format, +10 u16 bytes per pixel.
 * Memory rows run top-down. The clock label is narrowed by 6 px (patch in patches/cpu.py) to make room.
 * The bar fills from the bottom with the average load: green, amber above 70 %, red above 90 %.
 * The peak shows as one white row. No data from the M7 yet: nothing is drawn.
 *
 * While the settings page is open and the master compressor is on, the same bar shows the compressor instead
 * (src/mst_m7.c publishes it): its detector level against the threshold, from 30 dB below (bottom) to 12 dB above
 * (top). Green up to the threshold, red past it; the threshold row is white while the level is below it.
 */
#include "cpu_shared.h"
#include "mst_shared.h"

#define FN(addr) ((addr) | 1u)
typedef void (*flip_fn)(void *display, int layer, int a2, int a3);
typedef uint8_t *(*backbuf_fn)(void *display, int layer);
#define fw_flip    ((flip_fn)FN(0x08101384))
#define fw_backbuf ((backbuf_fn)FN(0x081013d2))

#define BAR_X  314          /* columns 314..316 */
#define BAR_W  3
#define BAR_Y0 3            /* rows 3..16, top to bottom */
#define BAR_H  14

static uint32_t rgb(unsigned r, unsigned g, unsigned b) { return r << 16 | g << 8 | b; }

static void put(uint8_t *p, unsigned fmt, uint32_t c)
{
    unsigned r = c >> 16 & 0xff, g = c >> 8 & 0xff, b = c & 0xff;
    switch (fmt) {
    case 1: *(uint32_t *)p = 0xff000000u | c; break;                                       /* ARGB8888 */
    case 2: p[0] = (uint8_t)b; p[1] = (uint8_t)g; p[2] = (uint8_t)r; break;               /* RGB888 */
    case 3: *(uint16_t *)p = (uint16_t)(0x8000u | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3); break; /* ARGB1555 */
    case 4: *(uint16_t *)p = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3); break;  /* RGB565 */
    }
}

/* Is the settings page (the list page showing object 0xc, where the compressor lives) on screen? The app object is
 * *(void **)0x3000e63c; its page manager's current page is at +0x2b4, the list page instance at +0x1e9e8, and the
 * page's object at +0xa3e0. */
static int in_ram(uint32_t a) { return (a >= 0x24000000u && a < 0x24080000u) || (a >= 0x30000000u && a < 0x30048000u) ||
                                       (a >= 0xc0000000u && a < 0xc4000000u) || (a >= 0x20000000u && a < 0x20020000u); }
static int comp_page(void)
{
    uint32_t app = *(volatile uint32_t *)0x3000e63cu;
    if (!in_ram(app) || !in_ram(app + 0x1e9e8u + 0xa3e2u))
        return 0;
    uint32_t page = *(volatile uint32_t *)(app + 0x2b4u);
    return page == app + 0x1e9e8u && *(volatile uint16_t *)(page + 0xa3e0u) == 0xc;
}

static void draw(const uint8_t *d)
{
    if (!d)
        return;
    uint8_t *px = *(uint8_t *const *)d;
    unsigned w = *(const uint16_t *)(d + 4), h = *(const uint16_t *)(d + 6);
    unsigned fmt = d[8], bpp = *(const uint16_t *)(d + 10);
    if (w != 320 || h != 240 || fmt < 1 || fmt > 4 || (bpp != 2 && bpp != 3 && bpp != 4))
        return;
    uint32_t a = (uint32_t)px;
    if (!((a >= 0xc0000000u && a < 0xc4000000u) || (a >= 0x24000000u && a < 0x24080000u) ||
          (a >= 0x30000000u && a < 0x30048000u)))
        return;

    bkp_enable();
    if (comp_page()) {
        volatile struct mst_meter *m = MST_METER;
        if (m->magic == MST_MAGIC && m->on) {
            int over = m->over;                                    /* 0.1 dB above the threshold */
            int lit = (over + 300) * BAR_H / 420;                  /* rows lit from the bottom */
            int thr = 300 * BAR_H / 420;                           /* the threshold's row from the bottom */
            for (int k = 0; k < BAR_H; k++) {
                unsigned row = BAR_Y0 + BAR_H - 1u - (unsigned)k;
                uint32_t c = k < lit ? (k >= thr ? rgb(255, 40, 40) : rgb(0, 200, 80))
                                     : k == thr ? rgb(255, 255, 255) : rgb(48, 48, 48);
                uint8_t *p = px + (row * w + BAR_X) * bpp;
                for (unsigned x = 0; x < BAR_W; x++, p += bpp)
                    put(p, fmt, c);
            }
            return;
        }
    }
    volatile struct cpu_shared *s = CPU_SHARED;
    if (s->magic != CPU_MAGIC)
        return;
    unsigned avg = s->avg, peak = s->peak;
    if (avg > 1000u) avg = 1000u;
    if (peak > 1000u) peak = 1000u;

    unsigned fill = (avg * BAR_H + 500u) / 1000u;                  /* rows lit from the bottom */
    unsigned prow = (peak * BAR_H + 500u) / 1000u;                 /* peak marker, rows from the bottom */
    uint32_t on = avg > 900u ? rgb(255, 40, 40) : avg > 700u ? rgb(255, 176, 0) : rgb(0, 200, 80);
    uint32_t off = rgb(48, 48, 48), mark = rgb(255, 255, 255);
    for (unsigned k = 0; k < BAR_H; k++) {                         /* k = 0 is the bottom row */
        unsigned row = BAR_Y0 + BAR_H - 1u - k;
        uint32_t c = k < fill ? on : off;
        if (prow && k == prow - 1u && prow > fill)
            c = mark;
        uint8_t *p = px + (row * w + BAR_X) * bpp;
        for (unsigned x = 0; x < BAR_W; x++, p += bpp)
            put(p, fmt, c);
    }
}

/* Replaces bl FUN_08101384 @0x0813861a (r0 = display, r1 = layer). */
void cpu_present(void *display, int layer, int a2, int a3)
{
    draw(fw_backbuf(display, layer));
    fw_flip(display, layer, a2, a3);
}
