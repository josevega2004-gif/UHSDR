/* Numerically controlled oscillator used in place of the firmware table DDS.
 * Same 32 bit phase accumulator and the same full scale int16 output, sine computed instead of
 * looked up. The bench measures the frequency of the tone this produces before it grades any
 * decoder on it, so a wrong step calculation cannot hide as a decoder failure. */
#include <math.h>
#include <stdint.h>
#include "softdds.h"

void softdds_setFreqDDS(soft_dds_t* dds, float32_t freq, uint32_t samp_rate, uint8_t smooth)
{
    (void) smooth;
    if (samp_rate == 0) samp_rate = 1;
    dds->step = (uint32_t) ((double) freq * 4294967296.0 / (double) samp_rate);
    dds->acc = 0;
}

int16_t softdds_nextSample(soft_dds_t* dds)
{
    dds->acc += dds->step;
    float phase = (float) ((double) dds->acc * (2.0 * M_PI / 4294967296.0));
    long v = lroundf(32767.0f * sinf(phase));
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return (int16_t) v;
}
