/* Shared between the two cores: the M7 writes the audio load, the M4 reads it to draw the meter.
 * Lives in the 4 KB backup SRAM (0x38800000), which the stock firmware never touches. Both cores can reach it. */
#include <stdint.h>

#define RCC_AHB4ENR (*(volatile uint32_t *)0x580244e0u)   /* this core's view; bit 28 = BKPRAMEN */
#define PWR_CR1     (*(volatile uint32_t *)0x58024800u)   /* bit 8 = DBP, backup-domain write access */

struct cpu_shared {
    uint32_t magic;
    uint16_t avg, peak;        /* per mille of the audio block period, last report */
    uint32_t seq;              /* bumped on every report */
};
#define CPU_SHARED ((volatile struct cpu_shared *)0x38800f00u)   /* one 32-byte cache line to itself */
#define CPU_MAGIC  0x43505542u                                    /* "BUPC" */

static inline void bkp_enable(void)
{
    RCC_AHB4ENR |= 1u << 28;
    (void)RCC_AHB4ENR;
}
