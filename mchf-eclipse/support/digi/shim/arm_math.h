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

#ifndef PI
#define PI 3.14159265358979f
#endif

#endif
