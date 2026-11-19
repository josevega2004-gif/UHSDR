/* Host stand-in for the CMSIS DSP header.
 * hardware/uhsdr_types.h includes arm_math.h only to get its sample types, so this file
 * provides those types and nothing else. If a firmware source ever needs a real arm_* call
 * this bench stops compiling, which is the intended failure: it keeps the host build honest
 * about which sources are genuinely self-contained.
 */
#ifndef DIGI_BENCH_ARM_MATH_H
#define DIGI_BENCH_ARM_MATH_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <math.h>

typedef float float32_t;
typedef double float64_t;
typedef int8_t q7_t;
typedef int16_t q15_t;
typedef int32_t q31_t;
typedef int64_t q63_t;

// audio_driver.h and audio_filter.h name these filter instances in declarations and in the filter
// table's pointers. Nothing in the host build instantiates or runs one - the modems here do their
// own filtering - so the shapes only have to be complete types, not the CMSIS layouts.
typedef struct { uint8_t numStages; float32_t* pState; const float32_t* pCoeffs; }
        arm_biquad_casd_df1_inst_f32;
typedef struct { uint8_t M; uint16_t numTaps; const float32_t* pCoeffs; float32_t* pState; }
        arm_fir_instance_f32;
typedef struct { uint8_t M; uint16_t numTaps; const float32_t* pCoeffs; float32_t* pState; }
        arm_fir_decimate_instance_f32;
typedef struct { uint8_t L; uint16_t phaseLength; const float32_t* pCoeffs; float32_t* pState; }
        arm_fir_interpolate_instance_f32;
typedef struct { uint8_t numStages; const float32_t* pkCoeffs; const float32_t* pvCoeffs;
                 float32_t* pState; }
        arm_iir_lattice_instance_f32;

// misc/uhsdr_math.c (the AGC's log10 approximation) takes its extremes with these two. They are
// the real definitions, not stand-ins: max and min of a float array are the same code anywhere,
// and the four argument form with the index output is the one this firmware's CMSIS version has.
static inline void arm_max_f32(const float32_t* pSrc, uint32_t blockSize, float32_t* pResult,
                               uint32_t* pIndex)
{
    float32_t m = pSrc[0];
    uint32_t at = 0;
    for (uint32_t i = 1; i < blockSize; i++) if (pSrc[i] > m) { m = pSrc[i]; at = i; }
    *pResult = m;
    if (pIndex) *pIndex = at;
}

static inline void arm_min_f32(const float32_t* pSrc, uint32_t blockSize, float32_t* pResult,
                               uint32_t* pIndex)
{
    float32_t m = pSrc[0];
    uint32_t at = 0;
    for (uint32_t i = 1; i < blockSize; i++) if (pSrc[i] < m) { m = pSrc[i]; at = i; }
    *pResult = m;
    if (pIndex) *pIndex = at;
}

// the target's C library has pow10f and glibc does not; the AGC turns its dB settings into linear
// gain with it
static inline float32_t pow10f(float32_t x)
{
    return powf(10.0f, x);
}

// the storage qualifier the audio driver's packed sample structs use; CMSIS brings it in on the
// target, a host build has to say what it means
#ifndef __packed
#define __packed __attribute__((__packed__))
#endif

#ifndef PI
#define PI 3.14159265358979f
#endif

#endif
