/* USB audio out mode, shared between the master wrapper (src/mst_m7.c, which picks the setting up from the event queue)
 * and the USB code (src/usb_m7.c). Both on the M7. Backup SRAM, after the master meter's state. */
#include <stdint.h>

struct usb_state {
    uint32_t magic;
    uint32_t want;             /* the setting: 0 = Multichannel (18 ch), 1 = Master only (2 ch) */
    uint32_t phase, t0;        /* re-enumeration in progress (src/usb_m7.c) */
};
#define USB_STATE ((volatile struct usb_state *)0x38800f80u)
#define USB_MAGIC 0x4253554du                                   /* "MUSB" */
#define USB_MODE_ID 0x59

static inline volatile struct usb_state *usb_state(void)
{
    volatile struct usb_state *u = USB_STATE;
    if (u->magic != USB_MAGIC) {
        u->want = 0;
        u->phase = 0;
        u->t0 = 0;
        u->magic = USB_MAGIC;
    }
    return u;
}
