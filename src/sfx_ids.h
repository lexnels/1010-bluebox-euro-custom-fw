/* Send FX (chorus, drive, delay 2): param ids, shared by the M7 engine (src/sfx_m7.c) and the M4 panel (src/dly_m4.c).
 * Undefined in both cores' param tables and handled by no stock node. They live in the reverb slot's param set, so
 * the M4 sends them with slot 0x15; the M7 copies every message to the master queue (0xc), where src/sfx_m7.c reads
 * them, and the stock reverb setter ignores ids outside 0x13d..0x15c. */
#define SFX_CHO_ON    0x44
#define SFX_CHO_SEND  0x45
#define SFX_CHO_MODE  0x46          /* 1 = I, 2 = II, 3 = I+II */
#define SFX_CHO_LEVEL 0x47
#define SFX_DRV_ON    0x48
#define SFX_DRV_SEND  0x49
#define SFX_DRV_DRIVE 0x4c
#define SFX_DRV_TONE  0x4d
#define SFX_DRV_LEVEL 0x4e
#define SFX_D2_ON     0x4f
#define SFX_D2_SEND   0x50
#define SFX_D2_TIME   0x51          /* 0..1000: 10 ms * 200^(v / 1000), so 10 ms .. 2 s */
#define SFX_D2_FB     0x52
#define SFX_D2_TONE   0x53
#define SFX_D2_PING   0x54
#define SFX_D2_LEVEL  0x55
