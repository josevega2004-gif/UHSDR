/*  -*-  mode: c; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4; coding: utf-8  -*-  */
/************************************************************************************
 **                                                                                **
 **                                        UHSDR                                   **
 **               a powerful firmware for STM32 based SDR transceivers             **
 **                                                                                **
 **--------------------------------------------------------------------------------**
 **                                                                                **
 **  Description:   Code for fast convolution filtering                            **
 **  Licence:		GNU GPLv3                                                      **
 ************************************************************************************/

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __AUDIO_CONVOLUTION_H
#define __AUDIO_CONVOLUTION_H

// arm_math.h for float32_t and nothing else: the convolution filter is a function of one array of
// complex samples in and one array out, which is what makes it measurable on a host without a
// radio.  This header used to pull in the codec, the USB audio interface, radio management and the
// audio driver as well, and to declare DECIMATE_RX_I/Q a second time next to audio_filter.h:32 -
// for the use inside this module's old copy of the RX interrupt, which no longer exists.
#include "arm_math.h"
#include "uhsdr_board.h"


#define SAMPLE_BUFFER_NUM 				4
#define SAMPLE_BUFFER_FIFO_SIZE 		(SAMPLE_BUFFER_NUM + 1)
// SAMPLE_BUFFER_SIZE is in audio_driver.h, next to the Sample_Buffer typedef that uses it


#ifdef USE_CONVOLUTION
// The geometry of the partitioned filter, all three of them overridable on the command line, because
// they trade against each other and the right point to trade at is a listening decision rather than a
// proof: a block is FFT_CONVOLUTION_SIZE / 2 samples, so that number divided by the rate the blocks
// carry *is* the filter's latency (256 at the decimated 12 kHz: 10.7 ms; 64 there: 2.7 ms), and the
// response cannot be longer than CONVOLUTION_MAX_NO_OF_BLOCKS blocks, which is what sets how steep it
// can be (8 blocks of 32 samples is 256 taps: 21 ms of impulse, 47 Hz of transition at 12 kHz).
// Whatever is chosen, the audio interrupt must keep up with one block per block-time, and the CMSIS
// transform used has to exist: see CONVOLUTION_CFFT_INSTANCE in audio_convolution.c.
#ifndef FFT_CONVOLUTION_SIZE
#define FFT_CONVOLUTION_SIZE 256
#endif
#ifndef CONVOLUTION_MAX_NO_OF_BLOCKS
#define CONVOLUTION_MAX_NO_OF_BLOCKS 8
#endif
#ifndef CONVOLUTION_MAX_NO_OF_COEFFS
#define CONVOLUTION_MAX_NO_OF_COEFFS 2048
#endif
#endif

#ifdef USE_CONVOLUTION

typedef struct
{
    // for convolution filtering: the block itself lives in the queues (see conv_blocks[] in
    // audio_convolution.c), so what is kept here is only the machinery the partitions need.
    // ifftout and a_buffer are gone because nothing ever wrote or read them, and
    // i/q_buffer_convolution are gone because the queues are now the only way a block reaches the
    // worker - holding a copy here was how the interrupt and the worker came to disagree about
    // which block was being filtered.
    float32_t				fmask[CONVOLUTION_MAX_NO_OF_BLOCKS][FFT_CONVOLUTION_SIZE * 2];
    float32_t				fftin[FFT_CONVOLUTION_SIZE * 2];
    float32_t				fftout[CONVOLUTION_MAX_NO_OF_BLOCKS][FFT_CONVOLUTION_SIZE * 2];
    float32_t				accum[FFT_CONVOLUTION_SIZE * 2];
} ConvolutionBuffers;


typedef struct
{
    // for convolution filtering
    int						nc; // no. of coefficients
    int 					size; // no. of input samples
    int						nfor; // no. of blocks in the convolution
    int						buffidx; // buffer pointer
    int						DF; // decimation factor
    float32_t				impulse[CONVOLUTION_MAX_NO_OF_COEFFS * 2]; // impulse response has real and imaginary components
    float32_t				maskgen[FFT_CONVOLUTION_SIZE * 2];
} ConvolutionBuffersShared;

extern ConvolutionBuffersShared cbs;

void AudioDriver_CalcConvolutionFilterCoeffs (int N, float32_t f_low, float32_t f_high, float32_t samplerate, int wintype, int rtype, float32_t scale);
void AudioDriver_SetConvolutionFilter (int nc, float32_t f_low, float32_t f_high, float32_t samplerate, int wintype, float32_t gain);
void convolution_handle(void);

// The audio interrupt and the PendSV worker exchange whole blocks through these.  A slice is handed
// in at the rate the audio interrupt runs, and the finished slice is read back at the same rate one
// block later: the caller must keep both sides at the same rate, and must not hold either of them
// waiting, because Input and Output are called from the interrupt.
void AudioDriver_ConvolutionReset(void);
uint16_t AudioDriver_ConvolutionInput(const float32_t* i, const float32_t* q, uint16_t count);
uint16_t AudioDriver_ConvolutionOutput(float32_t* i, float32_t* q, uint16_t count);

#endif

#endif
