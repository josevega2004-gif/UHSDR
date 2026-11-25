/* A complex FFT for the host bench, shaped like the CMSIS one the convolution filter calls.
 *
 * support/digi/shim/arm_math.h deliberately contains no DSP: the bench's policy is that a firmware
 * source which needs a real arm_* call simply stops compiling there, so it stays honest about which
 * sources are self-contained.  The partitioned convolution filter needs one real transform - two, in
 * fact, of 256 complex points per block - and the point of measuring it is worth making that policy
 * explicit rather than quietly broken:
 *
 *   - this file is a stand-in for arm_cfft_f32() and nothing else; the code under test is the shipped
 *     drivers/audio/audio_convolution.c, compiled in as it is;
 *   - it is not trusted.  convolution_probe.c proves the whole pipeline a second time against a
 *     plain time-domain complex FIR that contains no FFT at all, so if this transform were wrong the
 *     two would disagree and the run would fail;
 *   - it counts its own arithmetic, which is how the probe reports measured operations per audio
 *     sample instead of quoting a formula.
 *
 * CMSIS conventions, which the shipped code depends on: p1 is 2*fftLen floats as re,im pairs;
 * ifftFlag=1 is the inverse *including* the 1/fftLen normalisation; bitReverseFlag=1 asks for the
 * output in natural order.  A radix-2 iterative transform with a decimation-in-time stage order
 * matches all three.
 */

#include <stdint.h>
#include <math.h>
#include "arm_const_structs.h"

// real multiplies and adds executed by arm_cfft_f32 calls, so the probe can bill them
unsigned long conv_probe_fft_ops = 0;

#define TWO_PI (-6.283185307179586476925286766559)

static void bit_reverse(float* d, int n)
{
    for (int i = 1, j = 0; i < n; i++)
    {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
        {
            j ^= bit;
        }
        j |= bit;
        if (i < j)
        {
            float t = d[2 * i];       d[2 * i] = d[2 * j];       d[2 * j] = t;
            t = d[2 * i + 1];         d[2 * i + 1] = d[2 * j + 1]; d[2 * j + 1] = t;
        }
    }
}

static void transform(float* d, int n, int inverse)
{
    bit_reverse(d, n);

    for (int len = 2; len <= n; len <<= 1)
    {
        double ang = TWO_PI / len * (inverse ? -1.0 : 1.0);
        double wreal = cos(ang), wimag = sin(ang);

        for (int i = 0; i < n; i += len)
        {
            // The rotation is kept in double and only its products are rounded to float.  Kept in float
            // it is rounded once per step, 2048 times over in a 4096 point stage, where CMSIS reads
            // each rotation from a table - and the minimum phase design, whose stop band is 110 dB
            // down, is the one caller that would see the difference.
            double cur_real = 1.0, cur_imag = 0.0;

            for (int k = 0; k < len / 2; k++)
            {
                float* a = &d[2 * (i + k)];
                float* b = &d[2 * (i + k + len / 2)];

                float br = b[0], bi = b[1];
                // t = w * b, then (a, b) = (a + t, a - t).  Both halves have to use t, not b: an
                // earlier draft of this file added the unrotated b to a, which left a tone in the
                // right bin and ruined everything else, so the round trip below is not decoration.
                float tr = (float) (br * cur_real - bi * cur_imag);
                float ti = (float) (br * cur_imag + bi * cur_real);
                conv_probe_fft_ops += 6;       // four multiplies and two adds for the rotation
                b[0] = a[0] - tr;
                b[1] = a[1] - ti;
                a[0] += tr;
                a[1] += ti;
                conv_probe_fft_ops += 4;

                double nr = cur_real * wreal - cur_imag * wimag;
                double ni = cur_real * wimag + cur_imag * wreal;
                cur_real = nr;
                cur_imag = ni;
                conv_probe_fft_ops += 6;
            }
        }
    }

    if (inverse)
    {
        double scale = 1.0 / n;
        for (int i = 0; i < 2 * n; i++)
        {
            d[i] = (float) (d[i] * scale);
            conv_probe_fft_ops += 1;
        }
    }
}

const arm_cfft_instance_f32 arm_cfft_sR_f32_len64  = { 64,  NULL, NULL, 0 };
const arm_cfft_instance_f32 arm_cfft_sR_f32_len128 = { 128, NULL, NULL, 0 };
const arm_cfft_instance_f32 arm_cfft_sR_f32_len256 = { 256, NULL, NULL, 0 };
const arm_cfft_instance_f32 arm_cfft_sR_f32_len512 = { 512, NULL, NULL, 0 };
// the minimum phase design's transform: four times the response, so 1024, 2048 or 4096 points
const arm_cfft_instance_f32 arm_cfft_sR_f32_len1024 = { 1024, NULL, NULL, 0 };
const arm_cfft_instance_f32 arm_cfft_sR_f32_len2048 = { 2048, NULL, NULL, 0 };
const arm_cfft_instance_f32 arm_cfft_sR_f32_len4096 = { 4096, NULL, NULL, 0 };

void arm_cfft_f32(const arm_cfft_instance_f32* S, float32_t* p1, uint8_t ifftFlag,
                  uint8_t bitReverseFlag)
{
    (void) bitReverseFlag;      // natural order is what this produces, and what the caller asks for
    transform(p1, S->fftLen, ifftFlag != 0);
}
