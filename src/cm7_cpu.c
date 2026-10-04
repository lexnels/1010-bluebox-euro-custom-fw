/*
 * CPU meter, M7 side. bluebox eurorack firmware (BLUEEURO 3).
 *
 * The audio callback FUN_0804c778 already profiles itself: it calls FUN_0804c020(engine) on entry and
 * FUN_0804c040(engine) on exit. The exit call appends {period, busy} (DWT cycles: end-to-end since the previous block,
 * and since this block's entry) to a ring at engine+0x3e58 (count at +0x3e5c, 0x800 entries). We replace that exit
 * call, let it record as usual, read back the entry it just wrote, and every ~0.25 s publish the average and the worst
 * block's load (per mille) to backup SRAM for the M4 to draw.
 */
#include "cpu_shared.h"

#define FN(addr) ((addr) | 1u)
typedef void (*prof_end_fn)(uint8_t *engine);
#define fw_prof_end ((prof_end_fn)FN(0x0804c040))

#define PROF_RING  0x3e58
#define PROF_COUNT 0x3e5c
#define SCB_DCCMVAC (*(volatile uint32_t *)0xe000ef68u)   /* clean D-cache line by address */

#define REPORT_CYCLES 120000000u                           /* ~0.25 s at 480 MHz */

struct cpu_acc {
    uint32_t magic;
    uint32_t sum_busy, sum_period;
    uint16_t peak;
};
#define ACC ((volatile struct cpu_acc *)0x38800f20u)
#define ACC_MAGIC 0x41435055u

static void publish(unsigned avg, unsigned peak)
{
    PWR_CR1 |= 1u << 8;
    volatile struct cpu_shared *s = CPU_SHARED;
    s->avg = (uint16_t)avg;
    s->peak = (uint16_t)peak;
    s->seq = s->seq + 1;
    s->magic = CPU_MAGIC;
    __asm volatile("dsb" ::: "memory");
    SCB_DCCMVAC = (uint32_t)s;          /* push the line out so the M4 sees it */
    __asm volatile("dsb\n isb" ::: "memory");
}

/* Replaces bl FUN_0804c040 @0x0804ca10 (r0 = engine). */
void cpu_prof_end(uint8_t *engine)
{
    fw_prof_end(engine);

    bkp_enable();
    if (ACC->magic != ACC_MAGIC) {
        PWR_CR1 |= 1u << 8;
        ACC->sum_busy = ACC->sum_period = 0;
        ACC->peak = 0;
        ACC->magic = ACC_MAGIC;
        return;
    }
    const uint32_t *ring = *(uint32_t *const *)(engine + PROF_RING);
    uint32_t count = *(const uint32_t *)(engine + PROF_COUNT);
    if (!ring || !count)
        return;
    const uint32_t *e = ring + ((count - 1u) & 0x7ffu) * 2u;
    uint32_t period = e[0], busy = e[1];
    if (period < 1000u || period > 50000000u || busy > period)
        return;                          /* first block after boot, or a stall: not a real block */

    PWR_CR1 |= 1u << 8;
    unsigned load = busy / (period / 1000u + 1u);
    if (load > ACC->peak)
        ACC->peak = (uint16_t)load;
    ACC->sum_busy += busy;
    ACC->sum_period += period;
    if (ACC->sum_period >= REPORT_CYCLES) {
        unsigned avg = ACC->sum_busy / (ACC->sum_period / 1000u + 1u);
        publish(avg > 1000u ? 1000u : avg, ACC->peak > 1000u ? 1000u : ACC->peak);
        ACC->sum_busy = ACC->sum_period = 0;
        ACC->peak = 0;
    }
}
