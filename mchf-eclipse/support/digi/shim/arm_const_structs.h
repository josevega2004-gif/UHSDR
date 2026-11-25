/* Host stand-in for CMSIS arm_const_structs.h: the FFT instances the convolution geometry can name. */
#ifndef CONV_PROBE_ARM_CONST_STRUCTS_H
#define CONV_PROBE_ARM_CONST_STRUCTS_H
#include "arm_math.h"
typedef struct { uint16_t fftLen; const float32_t *pTwiddle; const float32_t *pBitRevTable;
                 uint16_t bitRevLength; } arm_cfft_instance_f32;
extern const arm_cfft_instance_f32 arm_cfft_sR_f32_len64;
extern const arm_cfft_instance_f32 arm_cfft_sR_f32_len128;
extern const arm_cfft_instance_f32 arm_cfft_sR_f32_len256;
extern const arm_cfft_instance_f32 arm_cfft_sR_f32_len1024;
extern const arm_cfft_instance_f32 arm_cfft_sR_f32_len2048;
extern const arm_cfft_instance_f32 arm_cfft_sR_f32_len4096;
void arm_cfft_f32(const arm_cfft_instance_f32 *S, float32_t *p1, uint8_t ifftFlag, uint8_t bitReverseFlag);
#endif
