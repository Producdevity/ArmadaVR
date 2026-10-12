#include <assert.h>
#include <stdint.h>

uint64_t ssc_test_ticks(void)
{
    uint64_t ticks, frequency;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    __asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(ticks) : : "memory");
    assert(frequency);
    uint64_t value = (__uint128_t)ticks * 19200000 / frequency;
    assert(value >= 38400);
    return value - 38400;
}
