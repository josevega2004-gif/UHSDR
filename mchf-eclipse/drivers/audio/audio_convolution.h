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
// proof: a block is FFT_CONVOLUTION_SIZE / 2 samples, and one block at the rate the blocks carry is
// what the filter makes the audio wait (256 at the decimated 12 kHz: 10.7 ms; 64 there: 2.7 ms).  The
// response cannot be longer than CONVOLUTION_MAX_NO_OF_BLOCKS blocks, which is what sets how steep it
// can be (8 blocks of 32 samples is 256 taps: 144 Hz from -6 to -60 dB at 12 kHz, where 8 blocks of
// 128 is 34 Hz).  The taps are minimum phase, so their own delay is small and does not grow with
// their number - about 1 ms in the middle of an SSB filter, 4 to 7 ms for a CW one - unless
// CONVOLUTION_LINEAR_PHASE is defined, in which case it is half the response on top of the block.
// Whatever is chosen, the audio interrupt must keep up with one block per block-time, and the CMSIS
// transforms used have to exist: see CONVOLUTION_CFFT_INSTANCE in audio_convolution.c.
#ifndef FFT_CONVOLUTION_SIZE
#define FFT_CONVOLUTION_SIZE 256
#endif
#ifndef CONVOLUTION_MAX_NO_OF_BLOCKS
#define CONVOLUTION_MAX_NO_OF_BLOCKS 8
#endif
// The tap count the partitions can hold, derived rather than chosen: AudioDriver_ConvolutionSetFilterPath()
// clamps the response to MAX_NO_OF_BLOCKS blocks of a block anyway, so a larger array here was 8 kB of
// .bss at the default geometry and 12 kB at 128 and 64 - on a chip with 128 kB of it - that no filter
// this build can design would ever reach.  Overriding it upwards with an explicit -D still works, and
// the clamp is what makes that safe.
#ifndef CONVOLUTION_MAX_NO_OF_COEFFS
#define CONVOLUTION_MAX_NO_OF_COEFFS (CONVOLUTION_MAX_NO_OF_BLOCKS * FFT_CONVOLUTION_SIZE / 2)
#endif
#endif

// The band the filter passes is the filter path's own, with two constants the path table does not
// carry.
//
// The low edge: the "LPF" paths of the table start at 0 Hz, and a band that reaches zero lets through
// the DC of the I/Q pair and the first few tens of Hz of the other sideband.  The Hilbert pair this
// replaces is 4 dB down at 50 Hz, so that is where this one starts as well.
//
// The gain: the designer's windowed sinc has a passband gain of one.  The i+q / i-q sum this replaces
// has a gain of two on every table the radio carries, at every width, so two is what keeps the AGC,
// the squelch and the audio level where they are when the option is switched on.
#define CONVOLUTION_LOW_CUT_HZ      50.0f
#define CONVOLUTION_PASSBAND_GAIN   2.0f

#ifdef USE_CONVOLUTION

typedef struct
{
    // for convolution filtering: the block itself lives in the queues (see conv_blocks[] in
    // audio_convolution.c), so what is kept here is only the machinery the partitions need.
    // ifftout and a_buffer are gone because nothing ever wrote or read them, and
    // i/q_buffer_convolution are gone because the queues are now the only way a block reaches the
    // worker - holding a copy here was how the interrupt and the worker came to disagree about
    // which block was being filtered.
    //
    // The same memory is also the scratch the filter design works in, as one flat array.  That is only
    // safe because a design closes the seam before it starts: see conv_design_busy in
    // audio_convolution.c, and section 12 of the probe, which runs a worker pass from the middle of a
    // design and requires the masks to come out identical to an undisturbed one.  Aliasing costs no
    // RAM, but it is not free of a condition - and the condition is now held by this module rather than
    // by whatever the caller happens to have muted.
    union
    {
        struct
        {
            float32_t		fmask[CONVOLUTION_MAX_NO_OF_BLOCKS][FFT_CONVOLUTION_SIZE * 2];
            float32_t		fftin[FFT_CONVOLUTION_SIZE * 2];
            float32_t		fftout[CONVOLUTION_MAX_NO_OF_BLOCKS][FFT_CONVOLUTION_SIZE * 2];
            float32_t		accum[FFT_CONVOLUTION_SIZE * 2];
        };
        float32_t			design_scratch[(2 * CONVOLUTION_MAX_NO_OF_BLOCKS + 2) * FFT_CONVOLUTION_SIZE * 2];
    };
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

// The four counters the diagnostic build prints on the debug overlay, and why they are there: the
// convolution path can end up with no audio in four distinguishable ways and the operator cannot tell
// them apart by ear.  See audio_convolution.c.
extern volatile uint32_t conv_stat_slices_in;
extern volatile uint32_t conv_stat_blocks_in;
extern volatile uint32_t conv_stat_blocks_out;
extern volatile uint32_t conv_stat_dropped;
extern volatile uint32_t conv_stat_nosetup;
extern volatile uint32_t conv_stat_busyin;      // slices refused while a filter was being designed
extern volatile uint32_t conv_stat_peak_in;      // loudest |sample| in, x1000, since boot
extern volatile uint32_t conv_stat_peak_out;     // loudest |sample| back, x1000, since boot

// Test seam, a null pointer in every firmware build: a function the host probe installs so that it can
// run a worker pass from the middle of a filter design.  That is what the audio interrupt and the worker
// it pends do to a design on a radio, and a single-threaded test cannot otherwise arrange it.  Called
// from convolution_minimum_phase() and from the mask loop in AudioDriver_SetConvolutionFilter().
extern void (*conv_test_preempt)(const char* where);

#ifdef CONVOLUTION_MEASURE
// What the filter costs on the hardware, in DWT cycles (divide by SystemCoreClock/1000000 to read
// microseconds) and in main stack pointer values.  The three questions these answer - how much audio a
// filter change eats, what a block costs in the worker and in the interrupt, and how close the stack
// comes to the .bss the filter grew - are in audio_convolution.c above the definitions, with the
// thresholds they are judged against.  A measurement build is not a shipping build: the counters are
// cheap, but the numbers only mean something if whoever reads them also has a radio to listen to.
extern volatile uint32_t conv_meas_setup_count;     // calls of the setup, cached or not
extern volatile uint32_t conv_meas_design_count;    // of those, the ones that actually designed masks
extern volatile uint32_t conv_meas_gap_cyc_last;    // the whole hole of the most recent filter change: queue
                                                    // flush, design and the refill of the first block
extern volatile uint32_t conv_meas_gap_cyc_max;     // ... and the worst since reset
extern volatile uint32_t conv_meas_busyin_last;     // slices refused during the most recent design
extern volatile uint32_t conv_meas_busyin_max;      // ... and the worst since reset
extern volatile uint32_t conv_meas_minphase_cyc_max;// the cepstrum, i.e. the four long transforms
extern volatile uint32_t conv_meas_mask_cyc_max;    // the partition loop, i.e. nfor transforms of masks
extern volatile uint32_t conv_meas_block_cyc_max;   // one worker pass, worst call including empty ones
extern volatile uint32_t conv_meas_block_cyc_total; // over conv_meas_block_count passes (both halved at a
                                                    // bound, so the average is over recent passes) that worked
extern volatile uint32_t conv_meas_block_count;
extern volatile uint32_t conv_meas_isrin_cyc_max;   // AudioDriver_ConvolutionInput() in the audio ISR
extern volatile uint32_t conv_meas_isrout_cyc_max;  // AudioDriver_ConvolutionOutput() in the same ISR
extern volatile uint32_t conv_meas_msp_min_isr;     // deepest main stack seen in the audio ISR
extern volatile uint32_t conv_meas_msp_min_worker;  // ... in the PendSV worker
extern volatile uint32_t conv_meas_msp_min_design;  // ... in the main loop during a design

// Zeroes the timing and counting fields, not the stack minima.  Called from a debugger, or from the UI
// if a build wants the maxima to mean "since this menu item was last touched".
void conv_meas_reset(void);
#endif

void AudioDriver_CalcConvolutionFilterCoeffs (int N, float32_t f_low, float32_t f_high, float32_t samplerate, int wintype, int rtype, float32_t scale);
void AudioDriver_SetConvolutionFilter (int nc, float32_t f_low, float32_t f_high, float32_t samplerate, int wintype, float32_t gain);
void AudioDriver_ConvolutionSetFilterPath(float32_t width_hz, float32_t offset_hz, bool lsb, float32_t samplerate);
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
