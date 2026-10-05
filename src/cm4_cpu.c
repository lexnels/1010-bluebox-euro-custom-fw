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
 * On the settings page, with the master compressor on, the Thresh row gets a blue bar along its bottom edge showing
 * the compressor's gain reduction (src/mst_m7.c publishes it): 0 to 20 dB across the row, ticks every 5 dB.
 * The UI only redraws when something on screen changes, so comp_tick asks for a redraw while the bar would move.
 */
#include "cpu_shared.h"
#include "mst_shared.h"

#define FN(addr) ((addr) | 1u)
typedef void (*flip_fn)(void *display, int layer, int a2, int a3);
typedef uint8_t *(*backbuf_fn)(void *display, int layer);
#define fw_flip    ((flip_fn)FN(0x08101384))
#define fw_backbuf ((backbuf_fn)FN(0x081013d2))
typedef void (*dirty_fn)(uint8_t *pages, uint8_t *flags);
#define fw_dirty   ((dirty_fn)FN(0x0813aa9e))

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

/* The settings list (page + 0x9c8, FUN_081400e2 lays it out): rect at +4 {x, y, w, h}, rows of 0x330 bytes from +0xe8,
 * row count at +0x99e8. Each row: rect at +4 (y-up, 28 px high, 30 px pitch), hidden byte +0x30, param id at +0x32c.
 * Its label sits 4 px above the row's bottom edge, so the bottom 3 px are free for the bar. */
#define GR_FULL 200         /* 0.1 dB at full width */
static void comp_draw(uint8_t *px, unsigned w, unsigned fmt, unsigned bpp)
{
    volatile struct mst_meter *m = MST_METER;
    if (!comp_page() || m->magic != MST_MAGIC || !m->on)
        return;
    uint32_t list = *(volatile uint32_t *)(*(volatile uint32_t *)0x3000e63cu + 0x2b4u) + 0x9c8u;
    const volatile int32_t *lr = (const volatile int32_t *)(list + 4);
    uint32_t n = *(volatile uint32_t *)(list + 0x99e8u);
    if (n > 48u)
        return;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t row = list + 0xe8u + k * 0x330u;
        if (*(volatile uint16_t *)(row + 0x32cu) != 0x132u)
            continue;
        const volatile int32_t *rr = (const volatile int32_t *)(row + 4);
        int x0 = rr[0], y = rr[1], rw = rr[2];
        if (*(volatile uint8_t *)(row + 0x30u) || y < lr[1] || y + 28 > lr[1] + lr[3] || y < 0 || y + 28 > 240 ||
            x0 < 0 || rw <= 0 || x0 + rw > 320)
            return;
        int gr = m->gr;
        if (gr > GR_FULL) gr = GR_FULL;
        int fill = gr * rw / GR_FULL;
        for (int r = 0; r < 3; r++) {
            uint8_t *p = px + ((unsigned)(239 - y - r) * w + (unsigned)x0) * bpp;
            for (int x = 0; x < rw; x++, p += bpp) {
                int tick = x > 0 && (x * 4) % rw < 4;              /* every 5 dB */
                put(p, fmt, x < fill ? rgb(40, 150, 255) : tick ? rgb(80, 100, 150) : rgb(20, 32, 64));
            }
        }
        return;
    }
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
    comp_draw(px, w, fmt, bpp);
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

/* The UI loop FUN_08135180 asks the page manager (app + 0x280) which layers need redrawing (FUN_0813aa9e, bl
 * @0x081351a6, patches/master.py). Its bytes +0x38/+0x39 force layers 0/1, as a page change does. While the bar is
 * showing, set them whenever the gain reduction changed, at most every 3rd M7 report (~15 fps). */
struct comp_tick_state { uint32_t magic, seq; uint16_t gr; };
#define CT ((volatile struct comp_tick_state *)0x38800fc0u)
#define CT_MAGIC 0x4b434954u
void comp_tick(uint8_t *pages, uint8_t *flags)
{
    bkp_enable();
    PWR_CR1 |= 1u << 8;             /* backup SRAM writes (the M7 sets it too) */
    volatile struct mst_meter *m = MST_METER;
    volatile struct comp_tick_state *t = CT;
    if (comp_page() && m->magic == MST_MAGIC && m->on) {
        uint32_t seq = m->seq;
        uint16_t gr = m->gr;
        if (t->magic != CT_MAGIC || seq - t->seq >= 3u) {
            if (t->magic != CT_MAGIC || gr != t->gr) {
                pages[0x38] = 1;
                pages[0x39] = 1;
                t->gr = gr;
            }
            t->seq = seq;
            t->magic = CT_MAGIC;
        }
    }
    fw_dirty(pages, flags);
}

/* Replaces bl FUN_08101384 @0x0813861a (r0 = display, r1 = layer). */
void cpu_present(void *display, int layer, int a2, int a3)
{
    draw(fw_backbuf(display, layer));
    fw_flip(display, layer, a2, a3);
}
