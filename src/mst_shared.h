/* Master compressor meter, shared between the cores: the M7 compressor wrapper (src/mst_m7.c) writes it, the CPU
 * meter on the M4 (src/cm4_cpu.c) draws it while the settings page is open. Backup SRAM, after the CPU meter's. */
#include <stdint.h>

struct mst_meter {
    uint32_t magic;
    int16_t over;              /* unused (0) */
    uint16_t gr;               /* most gain reduction of the last report, 0.1 dB */
    uint8_t on;                /* compressor on */
    uint8_t _p[3];
    uint32_t seq;
};
#define MST_METER ((volatile struct mst_meter *)0x38800f40u)   /* one 32-byte cache line */
#define MST_MAGIC 0x524d4f43u                                   /* "COMR" */
