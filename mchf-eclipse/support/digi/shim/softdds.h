/* Host stand-in for drivers/audio/softdds/softdds.h: same structure and the same prototypes for
 * the two calls the RTTY modulator makes. The firmware reads a quarter sine table; here the value
 * is computed, which keeps the bench free of dds_table.h. softdds_host.c documents the one place
 * where the two differ in detail but not in frequency or amplitude. */
#ifndef DIGI_BENCH_SOFTDDS_H
#define DIGI_BENCH_SOFTDDS_H
#include <stdint.h>
#include "arm_math.h"

typedef struct
{
    uint32_t acc;
    uint32_t step;
} soft_dds_t;

void    softdds_setFreqDDS(soft_dds_t* dds, float32_t freq, uint32_t samp_rate, uint8_t smooth);
int16_t softdds_nextSample(soft_dds_t* dds);
#endif
