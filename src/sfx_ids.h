/* Send FX (chorus, drive, delay 2): param ids, shared by the M7 engine (src/sfx_m7.c) and the M4 panel (src/dly_m4.c).
 * Undefined in both cores' param tables and handled by no stock node. They live in the reverb slot's param set, so
 * the M4 sends them with slot 0x15; the M7 copies every message to the master queue (0xc), where src/sfx_m7.c reads
 * them, and the stock reverb setter ignores ids outside 0x13d..0x15c. */
#define SFX_CHO_ON    0x44
#define SFX_CHO_FX1   0x45          /* how much of its output goes into FX1 (the delay) */
#define SFX_CHO_MODE  0x46          /* a list: 0 = I, 1 = II, 2 = I+II */
#define SFX_CHO_LEVEL 0x47
#define SFX_DRV_ON    0x48
#define SFX_DRV_FX1   0x49
#define SFX_DRV_DRIVE 0x4c
#define SFX_DRV_TONE  0x4d
#define SFX_DRV_LEVEL 0x4e
#define SFX_D2_ON     0x4f
#define SFX_D2_FX1    0x50
#define SFX_D2_TIME   0x51          /* 0..1000: 10 ms * 200^(v / 1000), so 10 ms .. 2 s */
#define SFX_D2_FB     0x52
#define SFX_D2_TONE   0x53
#define SFX_D2_PING   0x54
#define SFX_D2_LEVEL  0x55
#define SFX_CHO_FX2   0x56          /* how much of its output goes into FX2 (the reverb) */
#define SFX_DRV_FX2   0x57
#define SFX_D2_FX2    0x58
#define SFX_CHO_RATE  0x5a          /* 500 = the mode's own LFO rate; x0.25 .. x4 */
#define SFX_CHO_DEPTH 0x5b          /* 500 = the mode's own depth; 0 .. x2 */
#define SFX_D2_BEAT   0x5c          /* Beat Sync: Time follows the tempo (SFX_D2_SYNC) */
#define SFX_D2_SYNC   0x5d          /* the stock delay's 12 note values (1/64 .. 1 bar), its names list too */
/* Track sends (one per channel, in each channel's set, slots 0..11): how much of the channel goes into each FX */
#define SFX_TS_CHO    0x5e
#define SFX_TS_DRV    0x5f
#define SFX_TS_D2     0x60
#define SFX_D2_REV    0x62          /* the chance (0..1000 = 0..100 %) that a repeat plays backwards */
#define SFX_CHO_WIDTH 0x61          /* stereo width of its output: 0 = mono, 500 = as the Juno (opposite LFOs), 1000 = x2 */
