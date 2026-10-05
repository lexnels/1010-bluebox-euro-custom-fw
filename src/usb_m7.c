/*
 * USB audio out mode, M7 side: "Multichannel" (stock, 18 channels to the computer) or "Master only" (2 channels: the
 * master L/R, exactly what goes to the main outs).
 *
 * The USB device (ST USBD, UAC2, high speed, OTG_HS, no DMA) runs on the M7. Its config descriptor lives in RAM
 * (0x240000a8, copied from .data at boot; GetConfigDescriptor returns that pointer), so Master only edits it in place:
 * the device->host input terminal and its streaming interface say 2 channels (front L/R) and EP 0x82 a 48-byte max
 * packet. The descriptor is only read while enumerating, so a mode change soft-disconnects (OTG DCTL.SDIS), edits it and
 * reconnects; the computer sees the bluebox unplugged and plugged back in. The serial number gets an "M" in Master only
 * so the computer keeps the two kinds apart.
 *
 * The audio loop packs each 32-frame block as 18 channels x 24 bit (54-byte frames; master L/R are channels 13/14,
 * bytes 36..41) and writes it to the IN ring (bl FUN_08066750 @0x0804115a); the class DataIn (FUN_08065db0) sends
 * 6 +-1 frames per microframe from the ring. In Master only, usb_push keeps bytes 36..41 of each frame, and usb_datain
 * does what the stock DataIn does with 6-byte frames. The three fixed 324-byte (6-frame) sends become 36 bytes.
 *
 * The current mode is the descriptor itself (byte 0x24000100 == 2), so a reset always starts as stock. The wanted mode
 * comes from the settings (id 0x59) via src/mst_m7.c and src/usb_shared.h.
 */
#include "cpu_shared.h"
#include "usb_shared.h"

#define FN(addr) ((addr) | 1u)
typedef int (*ring_write_fn)(void *ring, uint8_t *src, int bytes);
typedef uint8_t (*class_fn)(void *pdev, uint8_t arg);
typedef int (*tx_fn)(void *pdev, uint8_t ep, uint8_t *buf, uint32_t len);
typedef void (*watchdog_fn)(uint32_t a, uint32_t b, uint32_t c, uint32_t d);
typedef uint8_t *(*str_fn)(uint32_t speed, uint16_t *len);
typedef uint32_t (*tick_fn)(void);
#define fw_ring_write ((ring_write_fn)FN(0x08066750))
#define fw_datain     ((class_fn)FN(0x08065db0))
#define fw_init       ((class_fn)FN(0x080658c0))
#define fw_tx         ((tx_fn)FN(0x08066f48))
#define fw_watchdog   ((watchdog_fn)FN(0x080655dc))
#define fw_serial     ((str_fn)FN(0x08066fec))
#define fw_tick       ((tick_fn)FN(0x0805e188))

struct ring {                       /* the IN ring, *(0x24000200) */
    uint8_t *buf;
    int32_t size, frame, _c;
    int32_t wr, rd;
    uint32_t alt, streaming;
};
#define RING      (*(struct ring *volatile *)0x24000200u)
#define TXBUF     (*(uint8_t *volatile *)0x2400bd00u)
#define RATE      (*(volatile uint32_t *)0x240001f8u)
#define ADJ       (*(volatile int32_t *)0x2400bc94u)     /* +-1 frame, set by DataOut from the host's OUT packets */
#define IN_FED    (*(volatile uint32_t *)0x2400bd44u)    /* EP 0x82 watchdog count, cleared by DataIn */
#define STARTED   (*(volatile uint32_t *)0x2400b9b4u)
#define WD_STALL  (*(volatile uint32_t *)0x2400b9a0u)    /* the stock host-loss watchdog's state */
#define WD_LAST   (*(volatile uint32_t *)0x2400b9a4u)
#define WD_SOFS   (*(volatile uint32_t *)0x2400b9a8u)
#define WD_SEEN   (*(volatile uint32_t *)0x2400b998u)
#define DCTL      (*(volatile uint32_t *)0x40040804u)    /* OTG_HS device control, bit 1 = soft disconnect */
#define DESC      ((volatile uint8_t *)0x240000a8u)      /* config descriptor (326 bytes) */

#define FRAME18 54
#define FRAME2 6
#define MASTER_AT 36                /* channels 13/14 */
#define RING2 0x480                 /* 192 frames: the same 4 ms as the stock ring */

static int master(void) { return DESC[0x58] == 2; }

/* descriptor fields (offsets into the config descriptor), unaligned, so byte by byte */
static void put32(unsigned at, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        DESC[at + i] = (uint8_t)(v >> (8 * i));
}
static void apply(int m)
{
    DESC[0x58] = m ? 2 : 18;        /* input terminal 4: bNrChannels, bmChannelConfig (FL | FR) */
    put32(0x59, m ? 3 : 0);
    DESC[0xc7] = m ? 2 : 18;        /* AS general, interface 2 alt 1 */
    put32(0xc8, m ? 3 : 0);
    DESC[0xd7] = m ? 48 : 0x80;     /* EP 0x82 wMaxPacketSize: 48 / 384 */
    DESC[0xd8] = m ? 0 : 1;
}

/* Replaces bl FUN_08066750 @0x0804115a: (ring, block, bytes) from the audio loop. */
int usb_push(struct ring *r, uint8_t *src, int bytes)
{
    if (master()) {
        int f = bytes / FRAME18;
        for (int i = 0; i < f; i++) {
            const uint8_t *s = src + i * FRAME18 + MASTER_AT;
            uint8_t *d = src + i * FRAME2;
            for (int k = 0; k < FRAME2; k++)
                d[k] = s[k];
        }
        bytes = f * FRAME2;
    }
    if (r->wr >= r->size || r->wr < 0)
        r->wr = 0;
    return fw_ring_write(r, src, bytes);
}

/* Class DataIn (slot 0x08076c40). Master only, EP 0x82: the stock routine with 6-byte frames. */
uint8_t usb_datain(void *pdev, uint8_t ep)
{
    if (!master() || (ep & 0x7f) != 2)
        return fw_datain(pdev, ep);
    IN_FED = 0;
    int want = 6 * FRAME2 + ADJ * FRAME2;
    ADJ = 0;
    uint8_t *tx = TXBUF;
    struct ring *r = RING;
    if (RATE != 48000) {
        for (int i = 0; i < want; i++)
            tx[i] = 0;
        fw_tx(pdev, 0x82, tx, (uint32_t)want);
        if (r->streaming)
            r->streaming = 0;
        return 0;
    }
    int size = r->size, rd = r->rd;
    if (rd < 0 || rd >= size)
        rd = 0;
    int avail = r->wr - rd;
    if (avail < 0)
        avail += size;
    int n = want < avail ? want : avail;
    for (int i = 0; i < n; i++) {
        tx[i] = r->buf[rd];
        if (++rd == size)
            rd = 0;
    }
    r->rd = rd;
    fw_tx(pdev, 0x82, tx, (uint32_t)n);
    if (r->streaming != 1)
        r->streaming = 1;
    return 0;
}

/* The three fixed 6-frame sends on EP 0x82 (SET_INTERFACE @0x08065ab4, iso IN incomplete @0x08065d1a, SOF watchdog
 * @0x08066640): 6 frames of 2 channels in Master only. */
int usb_tx82(void *pdev, uint8_t ep, uint8_t *buf, uint32_t len)
{
    return fw_tx(pdev, ep, buf, master() ? 6 * FRAME2 : len);
}

/* Class Init (slot 0x08076c2c), on SET_CONFIGURATION: Master only shrinks the IN ring to 6-byte frames. */
uint8_t usb_init(void *pdev, uint8_t cfg)
{
    uint8_t ret = fw_init(pdev, cfg);
    if (master()) {
        struct ring *r = RING;
        r->size = RING2;
        r->frame = FRAME2;
        r->wr = 0;
        r->rd = 0;
    }
    return ret;
}

/* Serial string (descriptor callback slot 0x08076c88): "M" appended in Master only. */
uint8_t *usb_serial(uint32_t speed, uint16_t *len)
{
    uint8_t *s = fw_serial(speed, len);
    if (master() && s && *len >= 2 && *len < 120) {
        s[*len] = 'M';
        s[*len + 1] = 0;
        *len = (uint16_t)(*len + 2);
        s[0] = (uint8_t)*len;
    }
    return s;
}

/* Replaces bl FUN_080655dc @0x08041166, the stock host-loss watchdog the audio loop calls every other block. When the
 * setting differs from the descriptor: soft-disconnect, after 20 ms switch, at 250 ms reconnect. The stock watchdog
 * sits out meanwhile (it would restart USB on the missing SOFs) and starts fresh. */
void usb_tick(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    bkp_enable();
    PWR_CR1 |= 1u << 8;
    volatile struct usb_state *u = usb_state();
    uint32_t now = fw_tick();
    switch (u->phase) {
    case 1:
        if (now - u->t0 >= 20u) {
            apply((int)u->want);
            u->phase = 2;
        }
        return;
    case 2:
        if (now - u->t0 >= 250u) {
            WD_STALL = 0;
            WD_SEEN = 0;
            WD_LAST = WD_SOFS;
            DCTL &= ~2u;
            u->phase = 0;
        }
        return;
    default:
        u->phase = 0;
        if ((int)u->want != master()) {
            if (!STARTED) {
                apply((int)u->want);
            } else {
                DCTL |= 2u;
                u->t0 = now;
                u->phase = 1;
                return;
            }
        }
        fw_watchdog(a, b, c, d);
    }
}
