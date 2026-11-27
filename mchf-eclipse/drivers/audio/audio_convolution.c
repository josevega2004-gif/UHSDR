/*  -*-  mode: c; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4; coding: utf-8  -*-  */
/************************************************************************************
 **                                                                                **
 **                                        UHSDR                                   **
 **               a powerful firmware for STM32 based SDR transceivers             **
 **                                                                                **
 **--------------------------------------------------------------------------------**
 **                                                                                **
 **  Description:   Code for fast convolution filtering, DD4WH 2018_08_19          **
 **  Licence:		GNU GPLv3                                                      **
 ************************************************************************************/

#include "audio_convolution.h"
#include "audio_driver.h"        // Sample_Buffer, the block the queues carry
#include "arm_const_structs.h"    // the arm_cfft_sR_f32_lenNN the geometry below names
#include "profiling.h"            // conv_meas_clock(): the DWT cycle counter, empty header on the host bench
#include <string.h>

// The CMSIS transform instance has to be the one whose length matches FFT_CONVOLUTION_SIZE, which is a
// build-time choice rather than a constant of this file: see the note in audio_convolution.h.  Naming
// it here rather than at each call is what makes `make CONFIGFLAGS='-DUSE_CONVOLUTION
// -DFFT_CONVOLUTION_SIZE=64'` a build rather than an edit.  Guarded, because with the option off the
// symbol is not defined at all and this file is an empty translation unit.
#ifdef USE_CONVOLUTION
#if   FFT_CONVOLUTION_SIZE == 64
#define CONVOLUTION_CFFT_INSTANCE   arm_cfft_sR_f32_len64
#elif FFT_CONVOLUTION_SIZE == 128
#define CONVOLUTION_CFFT_INSTANCE   arm_cfft_sR_f32_len128
#elif FFT_CONVOLUTION_SIZE == 256
#define CONVOLUTION_CFFT_INSTANCE   arm_cfft_sR_f32_len256
#else
#error "FFT_CONVOLUTION_SIZE has to be 64, 128 or 256: those are the float32 CFFT instances this module is given, and a block is half of it.  It is not a sample rate - the IQ rate is IQ_SAMPLE_RATE, 48000, set by the codec clock."
#endif

// All transforms below ask CMSIS for natural order (bitReverseFlag = 1).  Its raw output is a
// mixed-radix digit reversal, not the binary reversal a radix-2 DIF engine produces, so an
// __RBIT-indexed mask or gather is wrong here.

// The transform the minimum phase design works in.  It needs to be several times longer than the
// response, and four times is what the design scratch in ConvolutionBuffers holds for any geometry:
// the response is at most MAX_NO_OF_BLOCKS * SIZE / 2 taps and the scratch is a little over
// 2 * MAX_NO_OF_BLOCKS * SIZE complex points.
#ifndef CONVOLUTION_LINEAR_PHASE
#define CONVOLUTION_MINPHASE_FFT_SIZE   (2 * CONVOLUTION_MAX_NO_OF_BLOCKS * FFT_CONVOLUTION_SIZE)
#if   CONVOLUTION_MINPHASE_FFT_SIZE == 4096
#define CONVOLUTION_MINPHASE_CFFT_INSTANCE   arm_cfft_sR_f32_len4096
#elif CONVOLUTION_MINPHASE_FFT_SIZE == 2048
#define CONVOLUTION_MINPHASE_CFFT_INSTANCE   arm_cfft_sR_f32_len2048
#elif CONVOLUTION_MINPHASE_FFT_SIZE == 1024
#define CONVOLUTION_MINPHASE_CFFT_INSTANCE   arm_cfft_sR_f32_len1024
#else
#error "the minimum phase design needs a CFFT of 2 * CONVOLUTION_MAX_NO_OF_BLOCKS * FFT_CONVOLUTION_SIZE points, and CMSIS has 1024, 2048 and 4096: choose the two so that their product is 512, 1024 or 2048, or define CONVOLUTION_LINEAR_PHASE"
#endif
#endif
#endif

// convolution needs buffers of its own, the audio interrupt is using the
// shared buffers at the same time

#ifdef USE_CONVOLUTION
__IO int32_t Sample_in_head = 0;
__IO int32_t Sample_in_tail = 0;
__IO int32_t Sample_out_head = 0;
__IO int32_t Sample_out_tail = 0;

Sample_Buffer* Sample_in_buffers[SAMPLE_BUFFER_FIFO_SIZE];
Sample_Buffer* Sample_out_buffers[SAMPLE_BUFFER_FIFO_SIZE];

// The queues carry blocks of this module's own storage, and they speak one type.
//
// They used to carry pointers into mmb.nr_audio_buff[], which is the noise reducer's storage: with
// USE_CONVOLUTION and USE_ALTERNATE_NR both defined - which is how every configuration that ever
// named them is built - the convolution path and the NR wrote the same four buffers from two
// different interrupt levels, and nothing in the tree related the two block types either, because
// Sample_in_buffer_add() took an NR_Buffer* while Sample_out_buffer_remove() handed back an
// NR_Buffer* read out of a Sample_Buffer* array.  Each is `comp_float_t samples[256]` only for as
// long as SAMPLE_BUFFER_SIZE and NR_BUFFER_SIZE happen to name the same length.
//
// Owning the blocks settles both at once: the queues cannot be handed to another feature, and
// SAMPLE_BUFFER_SIZE can change without the audio interrupt being given a pointer that runs past
// the end of what it points at.

int Sample_in_buffer_peek(Sample_Buffer** c_ptr)
{
    int ret = 0;

    if (Sample_in_head != Sample_in_tail)
    {
        Sample_Buffer* c = Sample_in_buffers[Sample_in_tail];
        *c_ptr = c;
        ret++;
    }
    return ret;
}


int Sample_in_buffer_remove(Sample_Buffer** c_ptr)
{
    int ret = 0;

    if (Sample_in_head != Sample_in_tail)
    {
        Sample_Buffer* c = Sample_in_buffers[Sample_in_tail];
        Sample_in_tail = (Sample_in_tail + 1) % SAMPLE_BUFFER_FIFO_SIZE;
        *c_ptr = c;
        ret++;
    }
    return ret;
}

/* no room left in the buffer returns 0 */
int Sample_in_buffer_add(Sample_Buffer* c)
{
    int ret = 0;
    int32_t next_head = (Sample_in_head + 1) % SAMPLE_BUFFER_FIFO_SIZE;

    if (next_head != Sample_in_tail)
    {
        /* there is room */
        Sample_in_buffers[Sample_in_head] = c;
        Sample_in_head = next_head;
        ret ++;
    }
    return ret;
}

void Sample_in_buffer_reset()
{
    Sample_in_tail = Sample_in_head;
}

int8_t Sample_in_has_data()
{
    int32_t len = Sample_in_head - Sample_in_tail;
    return len < 0?len+SAMPLE_BUFFER_FIFO_SIZE:len;
}

int32_t Sample_in_has_room()
{
    // FIXME: Since we cannot completely fill the buffer
    // we need to say full 1 element earlier
    return SAMPLE_BUFFER_FIFO_SIZE - 1 - Sample_in_has_data();
}


//*********Out Buffer handling

int Sample_out_buffer_peek(Sample_Buffer** c_ptr)
{
    int ret = 0;

    if (Sample_out_head != Sample_out_tail)
    {
        Sample_Buffer* c = Sample_out_buffers[Sample_out_tail];
        *c_ptr = c;
        ret++;
    }
    return ret;
}


int Sample_out_buffer_remove(Sample_Buffer** c_ptr)
{
    int ret = 0;

    if (Sample_out_head != Sample_out_tail)
    {
        Sample_Buffer* c = Sample_out_buffers[Sample_out_tail];
        Sample_out_tail = (Sample_out_tail + 1) % SAMPLE_BUFFER_FIFO_SIZE;
        *c_ptr = c;
        ret++;
    }
    return ret;
}

/* no room left in the buffer returns 0 */
int Sample_out_buffer_add(Sample_Buffer* c)
{
    int ret = 0;
    int32_t next_head = (Sample_out_head + 1) % SAMPLE_BUFFER_FIFO_SIZE;

    if (next_head != Sample_out_tail)
    {
        /* there is room */
        Sample_out_buffers[Sample_out_head] = c;
        Sample_out_head = next_head;
        ret ++;
    }
    return ret;
}

void Sample_out_buffer_reset()
{
    Sample_out_tail = Sample_out_head;
}

int8_t Sample_out_has_data()
{
    int32_t len = Sample_out_head - Sample_out_tail;
    return len < 0?len+SAMPLE_BUFFER_FIFO_SIZE:len;
}

int32_t Sample_out_has_room()
{
    // FIXME: Since we cannot completely fill the buffer
    // we need to say full 1 element earlier
    return SAMPLE_BUFFER_FIFO_SIZE - 1 - Sample_out_has_data();
}

// ---------------- the blocks the two queues carry, and who owns them ----------------

// Counters for the diagnostic build.  The convolution path can go silent in at least four different
// ways - the chain was never set up so cbs.size is still 0, the free ring is empty so slices are
// dropped, the worker is not running so nothing is queued, or the interrupt is not reading so nothing
// comes back - and from the operator's chair all four look the same: a radio that tunes, displays and
// hears nothing.  These numbers tell them apart on the screen.
volatile uint32_t conv_stat_slices_in;      // slices handed in by the audio interrupt
volatile uint32_t conv_stat_blocks_in;      // whole blocks that reached the worker's queue
volatile uint32_t conv_stat_blocks_out;     // finished blocks the interrupt took back
volatile uint32_t conv_stat_dropped;        // blocks thrown away because nothing was free
volatile uint32_t conv_stat_nosetup;        // slices refused because cbs.size was still 0
volatile uint32_t conv_stat_busyin;         // slices refused while a filter was being designed
volatile uint32_t conv_stat_peak_in;        // loudest |sample| handed in since boot, times 1000
volatile uint32_t conv_stat_peak_out;       // loudest |sample| handed back, times 1000

#ifdef CONVOLUTION_MEASURE
// ---------------- the numbers that decide whether this filter can be switched on ----------------
//
// Everything above says whether the pipeline is moving; none of it says whether moving it is
// affordable.  The three questions a radio has to answer before USE_CONVOLUTION stops being a build
// option and becomes a default are:
//
//   1. How much audio does a filter change eat?  A design runs with the RX processor held off, so a
//      design that takes longer than one IQ interrupt (667 us at 1500 Hz) is not a background cost but
//      a hole in what the operator hears.  conv_meas_gap_* measures the hole directly, in cycles, and
//      conv_meas_busyin_* counts the slices that were refused inside it, which is the same hole
//      counted in units of audio (each slice is 32 samples at 48 kHz or 8 at 12 kHz, and both are one
//      667 us interrupt).
//   2. What does one block of filtering cost, in the worker and in the interrupt?  conv_meas_block_*
//      and conv_meas_isr*_* answer that; they are the per-frame budget the L% readout shows as a duty
//      cycle, but attributed to this filter rather than to the whole handler.
//   3. How close does the stack get to the .bss the filter just grew?  The link says the two regions
//      fit; only running the radio says whether the ISR chain fits between them.  conv_meas_msp_*
//      records the lowest main stack pointer ever seen in each of the three contexts, which compared
//      against _ebss is the margin the linker's arithmetic cannot give.
//
// Cycles, not microseconds: the counter is DWT's free-running CYCCNT, already started at boot by
// profileTimedEventInit() in uhsdr_main.c, and this build does not reset it because the audio load
// readout shares it.  Divide by SystemCoreClock/1000000 to read them (the UI line does).
//
// On the host bench conv_meas_clock() returns 0 and the stack pointer is not sampled, so the timing
// fields stay zero and only the counting fields (design/setup counts, busyin) carry meaning - which is
// what convolution_probe.c section 13 checks, because a counter nobody runs on a host rots.
volatile uint32_t conv_meas_setup_count;     // calls of the setup, cached or not
volatile uint32_t conv_meas_design_count;    // of those, the ones that actually designed masks
volatile uint32_t conv_meas_gap_cyc_last;    // audio-off window of the most recent filter change
volatile uint32_t conv_meas_gap_cyc_max;     // ... and the worst since reset
volatile uint32_t conv_meas_busyin_last;     // slices refused during the most recent design
volatile uint32_t conv_meas_busyin_max;      // ... and the worst since reset
volatile uint32_t conv_meas_minphase_cyc_max;// the cepstrum, i.e. the four long transforms
volatile uint32_t conv_meas_mask_cyc_max;    // the partition loop, i.e. nfor transforms of masks
volatile uint32_t conv_meas_block_cyc_max;   // one worker pass: forward, nfor multiplies, inverse
volatile uint32_t conv_meas_block_cyc_total; // over conv_meas_block_count passes
volatile uint32_t conv_meas_block_count;
volatile uint32_t conv_meas_isrin_cyc_max;   // AudioDriver_ConvolutionInput() inside the audio ISR
volatile uint32_t conv_meas_isrout_cyc_max;  // AudioDriver_ConvolutionOutput() in the same ISR
volatile uint32_t conv_meas_msp_min_isr;     // deepest main stack seen in the audio ISR
volatile uint32_t conv_meas_msp_min_worker;  // ... in the PendSV worker
volatile uint32_t conv_meas_msp_min_design;  // ... in the main loop during a design

// The hole a filter change makes, opened by the main loop and closed by the audio interrupt.  It has
// to span both: emptying the queues and refilling the first block are as much of the silence as the
// design is, and timing only the design reported a hole the operator never heard the end of.
static uint32_t conv_meas_gap_start;        // clock when the current change began
static volatile bool conv_meas_gap_open;    // that window is still being counted

// The worker's average is kept as a sum over a bounded number of passes.  A pass is a few hundred
// thousand cycles, so an unbounded 32-bit sum would wrap in minutes while the count kept climbing and
// the average would walk down towards zero; halving both at this bound keeps the ratio, bounds the
// sum and leaves an average over the last few thousand passes - about ten seconds of receiving.
#define CONV_MEAS_BLOCK_AVG_PASSES  1024u

static inline uint32_t conv_meas_clock(void)
{
#ifdef HOST_BENCH
    return 0;
#else
    return profileCycleCount_get();
#endif
}

// The boot code starts the counter (profileTimedEventInit(), uhsdr_main.c:393) and this build must not
// reset it, because the audio ISR load readout shares it; starting it again is idempotent and covers a
// measurement taken before that line ever ran.
static inline void conv_meas_clock_start(void)
{
#ifndef HOST_BENCH
    profileCycleCount_start();
#endif
}

static volatile bool conv_meas_did_work;   // set by the worker when a pass had a block in it

static inline void conv_meas_max(volatile uint32_t* target, uint32_t cycles)
{
    if (cycles > *target) *target = cycles;
}

// Close the hole the last filter change opened, if it is still open.  Two callers, and that is the
// point: the audio interrupt closes it when the new filter first hands a sample back, which is when
// the operator hears the radio again, and the main loop closes an abandoned one when the next change
// begins.  A change made while the receiver was not running (in TX) leaves the window open, and
// discarding it would report nothing where the operator lost a whole interval.
static void conv_meas_gap_close(uint32_t now)
{
    if (!conv_meas_gap_open)
    {
        return;
    }
    conv_meas_gap_open = false;
    const uint32_t gap = now - conv_meas_gap_start;
    conv_meas_gap_cyc_last = gap;
    conv_meas_max(&conv_meas_gap_cyc_max, gap);
}

// The stack pointer is a register on ARM and does not exist on the bench, so the bench samples 0 and
// the UI line prints "S -" rather than a headroom that would look real.
static inline uint32_t conv_meas_sp(void)
{
#ifdef HOST_BENCH
    return 0;
#else
    return __get_MSP();
#endif
}

static inline void conv_meas_lowest(volatile uint32_t* target, uint32_t sp)
{
    if (sp != 0 && (*target == 0 || sp < *target)) *target = sp;
}

void conv_meas_reset(void)
{
    // A window still open when the counters are zeroed would close against a session that no longer
    // exists, so it is abandoned rather than recorded.
    conv_meas_gap_open = false;
    conv_meas_setup_count = 0;
    conv_meas_design_count = 0;
    conv_meas_gap_cyc_last = 0;
    conv_meas_gap_cyc_max = 0;
    conv_meas_busyin_last = 0;
    conv_meas_busyin_max = 0;
    conv_meas_minphase_cyc_max = 0;
    conv_meas_mask_cyc_max = 0;
    conv_meas_block_cyc_max = 0;
    conv_meas_block_cyc_total = 0;
    conv_meas_block_count = 0;
    conv_meas_isrin_cyc_max = 0;
    conv_meas_isrout_cyc_max = 0;
    // deliberately not the stack minima: those are the answer to "did the radio ever get close", and
    // clearing them on the way past a menu item is how a measurement build ends up reporting a margin
    // that was never measured.
    conv_stat_slices_in = 0;
    conv_stat_blocks_in = 0;
    conv_stat_blocks_out = 0;
    conv_stat_dropped = 0;
    conv_stat_nosetup = 0;
    conv_stat_busyin = 0;
}
#endif

static Sample_Buffer conv_blocks[SAMPLE_BUFFER_NUM];
// The worker writes one whole block into a buffer it is handed back and forth, so the block type has
// to be at least as long as the FFT's idea of a block.  Before the queues owned their buffers this
// relationship existed only as an accident: the blocks were the noise reducer's, whose length and
// the convolution's happened to be the same number.
_Static_assert(SAMPLE_BUFFER_SIZE >= FFT_CONVOLUTION_SIZE / 2,
               "a Sample_Buffer has to hold one block of convolution samples");
// One slot more than there are blocks, so that head == tail means empty and never full: the ring
// genuinely holds all SAMPLE_BUFFER_NUM blocks at the moment a filter path is switched.
static Sample_Buffer* conv_free[SAMPLE_BUFFER_NUM + 1];
static int32_t conv_free_head;
static int32_t conv_free_tail;

// The slice the audio interrupt is writing into the next block, and the slice it is reading out of
// the block the worker has finished.  Both are kept between calls because one block is several
// interrupt periods: cbs.size samples at the rate the block is carried, blockSizeIQ samples per
// period.  That ratio is the whole of the module's latency, and the two of them have to be filled
// and emptied at the same rate or the filtered stream develops a hole in it.
static Sample_Buffer* conv_stage;
static uint16_t conv_stage_count;
static Sample_Buffer* conv_play;
static uint16_t conv_play_count;

// True while a filter is being designed, and the reason the design may use the worker's buffers as its
// working store.  See AudioDriver_ConvolutionSetFilterPath().
static volatile bool conv_design_busy;

// Test seam for the host probe (support/digi/convolution_probe.c, section 12): a function to call from
// the middle of a design, which is what the audio interrupt and the worker it pends do to a design on a
// radio.  A single-threaded host cannot interrupt itself, so the probe names this instead.  It is a null
// pointer in every firmware build and costs two loads per filter change.
void (*conv_test_preempt)(const char* where);

// The three contexts this module runs in are the audio interrupt (Input/Output, from the RX
// processor), the worker that interrupt pends (convolution_handle(), at the lowest exception priority)
// and the main loop (the design).  Where one of the variables below has more than one of them as a
// writer, the update is inside a short critical section, and the reason is spelled out where it is
// taken.  On a host build all three are one thread, so the primitives collapse to nothing and the same
// code runs unguarded - which is what makes the probe's counts comparable with the radio's.
static inline unsigned int conv_irq_save(void)
{
#ifdef __ARM_ARCH
    const unsigned int primask = __get_PRIMASK();
    __disable_irq();
    return primask;
#else
    return 0u;
#endif
}

static inline void conv_irq_restore(unsigned int primask)
{
#ifdef __ARM_ARCH
    __set_PRIMASK(primask);
#else
    (void) primask;
#endif
}

void AudioDriver_ConvolutionReset(void)
{
    // The ring, the two queues and the two part-blocks are one state, and this rebuilds all of it from
    // the main loop while the audio interrupt is live: it has to be atomic, or an interrupt landing in
    // the middle finds a block both free and queued.
    const unsigned int irq = conv_irq_save();
    for (int32_t i = 0; i < SAMPLE_BUFFER_NUM; i++)
    {
        conv_free[i] = &conv_blocks[i];
        for (uint16_t s = 0; s < SAMPLE_BUFFER_SIZE; s++)
        {
            conv_blocks[i].samples[s].real = 0.0f;
            conv_blocks[i].samples[s].imag = 0.0f;
        }
    }
    // the extra slot of the free ring is left unused while every block is free
    conv_free[SAMPLE_BUFFER_NUM] = NULL;
    conv_free_head = 0;
    conv_free_tail = SAMPLE_BUFFER_NUM;

    conv_stage = NULL;
    conv_stage_count = 0;
    conv_play = NULL;
    conv_play_count = 0;

    Sample_in_buffer_reset();
    Sample_out_buffer_reset();
    conv_irq_restore(irq);
}

// Both ends of the free ring have more than one of the three contexts as a writer, so both are a
// read-modify-write on a variable that someone else can change underneath:
//
//   conv_free_head  taken by the audio interrupt, and rebuilt by the main loop in Reset()
//   conv_free_tail  given by the audio interrupt (a block read out, a block that could not be queued)
//                   and by the worker in convolution_handle() when the output queue is full
//
// Losing one of those updates either strands a block outside the pool for the rest of the session -
// and a pool of four that leaks runs dry, which is a radio with no audio whose counters say nothing has
// been dropped - or puts one block in the pool twice, handing the same buffer to the interrupt and the
// worker at once.  These sections are two loads and two stores, which is nothing at the rates this
// module runs at, and they cannot delay the audio interrupt, which is the higher of the two and the one
// being protected.
static Sample_Buffer* conv_block_take(void)
{
    const unsigned int irq = conv_irq_save();
    Sample_Buffer* c = NULL;
    if (conv_free_head != conv_free_tail)
    {
        c = conv_free[conv_free_head];
        conv_free_head = (conv_free_head + 1) % (SAMPLE_BUFFER_NUM + 1);
    }
    conv_irq_restore(irq);
    return c;
}

static void conv_block_give(Sample_Buffer* c)
{
    const unsigned int irq = conv_irq_save();
    conv_free[conv_free_tail] = c;
    conv_free_tail = (conv_free_tail + 1) % (SAMPLE_BUFFER_NUM + 1);
    conv_irq_restore(irq);
}

// Hand a slice of I/Q to the filter.  Returns the number of samples taken, which is the whole slice
// unless the pipeline cannot start a block.  A slice is smaller than a block - the audio interrupt
// hands over IQ_BLOCK_SIZE = 48000/1500 = 32 samples, or 8 of them once a narrow filter path has
// decimated by 4, and a block is cbs.size = 128 - so a block is filled across four or sixteen calls
// and the staging between them is what has to be right.  A block dropped here is a dropout in the
// audio rather than a desynchronisation, which is what makes refusing better in an interrupt than
// waiting.
// The measuring build wraps the two entry points rather than putting a clock read at every return:
// the refusal paths are short and the work is the long part, and the shape of the code the radio runs
// should not change because a diagnostic was switched on.  With CONVOLUTION_MEASURE off this is the
// plain function, one call and no extra code.
#ifdef CONVOLUTION_MEASURE
static uint16_t conv_input_body(const float32_t* i, const float32_t* q, uint16_t count)
#else
uint16_t AudioDriver_ConvolutionInput(const float32_t* i, const float32_t* q, uint16_t count)
#endif
{
    uint16_t done = 0;

    if (cbs.size == 0)
    {   // AudioDriver_SetProcessingChain() has not sized the blocks yet
        conv_stat_nosetup++;
        return 0;
    }
    if (conv_design_busy)
    {   // A filter is being designed in the worker's buffers.  Taking samples now would queue a block
        // for a worker that is not allowed to run, and the queues are emptied when the design starts
        // anyway, so the honest answer is "not now" and the caller fills silence.
        conv_stat_busyin++;
        return 0;
    }
    conv_stat_slices_in++;

    while (done < count)
    {
        if (conv_stage == NULL)
        {
            conv_stage = conv_block_take();
            if (conv_stage == NULL)
            {
                // Every block is already in the pipeline and none has come back: this slice is lost.
                // It used to be the drop nobody could see, because the only place that counted one was
                // the queue-add below - which cannot fail while a block is free to stage, so a radio
                // that had leaked its pool out of the free ring reported "d0" while it was dying.
                conv_stat_dropped++;
                break;
            }
            conv_stage_count = 0;
        }

        uint16_t room = cbs.size - conv_stage_count;
        uint16_t taken = count - done < room ? count - done : room;

#ifdef CONVOLUTION_LEVEL_STATS
        float32_t m = 0.0f;
#endif
        for (uint16_t k = 0; k < taken; k++)
        {
            const float32_t iv = i[done + k];
            const float32_t qv = q[done + k];
            conv_stage->samples[conv_stage_count + k].real = iv;
            conv_stage->samples[conv_stage_count + k].imag = qv;
#ifdef CONVOLUTION_LEVEL_STATS
            const float32_t ai = iv < 0.0f ? -iv : iv;
            const float32_t aq = qv < 0.0f ? -qv : qv;
            if (ai > m) m = ai;
            if (aq > m) m = aq;
#endif
        }
        // What the blocks *contain* is the one thing the counters could not see: a filter can hand
        // back every block it was given, on time, none dropped, and be silent, because a queue of
        // zeros moves exactly as fast as a queue of sound.  That question is answered, and this is the
        // only part of the counters that costs per-sample arithmetic, so the level tracking is a build
        // choice (see CONVOLUTION_LEVEL_STATS in uhsdr_board_config.h) and the variables read zero
        // when it is not asked for.  The debug line says "P off" rather than "P i0 o0" for that reason:
        // a zero that means "not measured" must not look like a zero that means "no signal".
#ifdef CONVOLUTION_LEVEL_STATS
        {
            // NaN is a possible answer from a broken filter, and this is the line whose whole job is to
            // be readable when something is wrong: casting a NaN to uint32_t is undefined and typically
            // prints as a small plausible number.  A NaN reads as a saturating 999999, which says
            // "that was not a sample"; a real zero level is a valid reading and stays zero.
            const float32_t ml = m * 1000.0f;
            const uint32_t pk = (ml >= 0.0f) ? (ml > 999999.0f ? 999999u : (uint32_t) ml) : 999999u;
            if (pk > conv_stat_peak_in) conv_stat_peak_in = pk;
        }
#endif
        conv_stage_count += taken;
        done += taken;

        if (conv_stage_count == cbs.size)
        {
            // full block: pass it to the worker, which runs in the PendSV handler, and let the caller
            // fill another one in the meantime
            if (Sample_in_buffer_add(conv_stage) == 0)
            {
                conv_block_give(conv_stage);        // the worker is behind; drop this block, cleanly
                conv_stat_dropped++;
            }
            else
            {
                conv_stat_blocks_in++;
            }
            conv_stage = NULL;
            conv_stage_count = 0;
        }
    }
    return done;
}

#ifdef CONVOLUTION_MEASURE
uint16_t AudioDriver_ConvolutionInput(const float32_t* i, const float32_t* q, uint16_t count)
{
    const uint32_t meas_t0 = conv_meas_clock();
    const uint16_t r = conv_input_body(i, q, count);
    conv_meas_max(&conv_meas_isrin_cyc_max, conv_meas_clock() - meas_t0);
    // read after the work, at the depth this call reached: the wrapper frame itself is a few words
    // deeper than anything the audio ISR chain does outside a filter, and that is the region whose
    // margin to _ebss is the question.
    conv_meas_lowest(&conv_meas_msp_min_isr, conv_meas_sp());
    return r;
}
#endif

// Take finished samples back out of the filter.  Returns the number given, which is the whole request
// as long as blocks have come back: 0 until the worker has produced a first one, and short of the
// request only if it has run out mid slice.  Either way the caller fills silence for what is missing,
// and after that the two streams run at the same rate because they carry the same samples.
#ifdef CONVOLUTION_MEASURE
static uint16_t conv_output_body(float32_t* i, float32_t* q, uint16_t count)
#else
uint16_t AudioDriver_ConvolutionOutput(float32_t* i, float32_t* q, uint16_t count)
#endif
{
    uint16_t done = 0;

    if (cbs.size == 0 || conv_design_busy)
    {
        return 0;
    }

    while (done < count)
    {
        if (conv_play == NULL)
        {
            if (Sample_out_buffer_remove(&conv_play) == 0)
            {
                conv_play = NULL;
                break;
            }
            conv_stat_blocks_out++;
            conv_play_count = 0;
        }

        uint16_t room = cbs.size - conv_play_count;
        uint16_t given = count - done < room ? count - done : room;

#ifdef CONVOLUTION_LEVEL_STATS
        float32_t m = 0.0f;
#endif
        for (uint16_t k = 0; k < given; k++)
        {
            const float32_t rv = conv_play->samples[conv_play_count + k].real;
            const float32_t imv = conv_play->samples[conv_play_count + k].imag;
            i[done + k] = rv;
            q[done + k] = imv;
#ifdef CONVOLUTION_LEVEL_STATS
            const float32_t ar = rv < 0.0f ? -rv : rv;
            const float32_t aa = imv < 0.0f ? -imv : imv;
            if (ar > m) m = ar;
            if (aa > m) m = aa;
#endif
        }
#ifdef CONVOLUTION_LEVEL_STATS
        {
            // as on the input side: a NaN must read as 999999, not as zero, while a real zero
            // level stays zero.  See the note in AudioDriver_ConvolutionInput().
            const float32_t ml = m * 1000.0f;
            const uint32_t pk = (ml >= 0.0f) ? (ml > 999999.0f ? 999999u : (uint32_t) ml) : 999999u;
            if (pk > conv_stat_peak_out) conv_stat_peak_out = pk;
        }
#endif
        conv_play_count += given;
        done += given;

        if (conv_play_count == cbs.size)
        {
            conv_block_give(conv_play);             // the block is read out, so it can be filled again
            conv_play = NULL;
        }
    }
    return done;
}

#ifdef CONVOLUTION_MEASURE
uint16_t AudioDriver_ConvolutionOutput(float32_t* i, float32_t* q, uint16_t count)
{
    const uint32_t meas_t0 = conv_meas_clock();
    const uint16_t r = conv_output_body(i, q, count);
    conv_meas_max(&conv_meas_isrout_cyc_max, conv_meas_clock() - meas_t0);
    conv_meas_lowest(&conv_meas_msp_min_isr, conv_meas_sp());
    // The first sample the new filter hands back is the end of the hole: the queue flush, the design
    // and the refill of the first block are all in it, and only this side can see the last of the
    // three.  One flag test while no change is in flight.
    if (conv_meas_gap_open && r > 0)
    {
        conv_meas_gap_close(conv_meas_clock());
    }
    return r;
}
#endif
#endif

#ifdef USE_CONVOLUTION

static ConvolutionBuffers cob;
ConvolutionBuffersShared cbs;

void AudioDriver_CalcConvolutionFilterCoeffs (int N, float32_t f_low, float32_t f_high, float32_t samplerate, int wintype, int rtype, float32_t scale)
{
	/****************************************************************
	 *  Partitioned Convolution code adapted from wdsp library
	 *  (c) by Warren Pratt under GNU GPLv3
	 ****************************************************************/

	// TODO: how can I define an array with a function ???
	//float32_t *c_impulse = (float32_t *) malloc0 (N * sizeof (complex));
	//float32_t c_impulse[CONVOLUTION_MAX_NO_OF_COEFFS * 2];
	float32_t ft = (f_high - f_low) / (2.0 * samplerate);
	float32_t ft_rad = 2.0 * PI * ft;
	float32_t w_osc = PI * (f_high + f_low) / samplerate;
  int i, j;
  float32_t m = 0.5 * (float32_t)(N - 1);
  float32_t delta = PI / m;
  float32_t cosphi;
  float32_t posi, posj;
  float32_t sinc, window, coef;

  // The two cases in the loop below are the whole table.  An unrecognised wintype used to leave
  // `window` holding whatever the previous iteration had left in it, so the coefficients - and the
  // masks built from them, and the audio filtered by those masks - came out as a scaled copy of
  // stale memory rather than as a filter.  Every caller in the tree means Blackman-Harris 4-term,
  // so that is what an unknown value means now.
  if (wintype != 0 && wintype != 1)
  {
    wintype = 0;
  }

  if (N & 1)
  {
    switch (rtype)
    {
    case 0:
      cbs.impulse[N >> 1] = scale * 2.0 * ft;
      break;
    case 1:
      cbs.impulse[N - 1] = scale * 2.0 * ft;
      cbs.impulse[  N  ] = 0.0;
      break;
    }
  }
  for (i = (N + 1) / 2, j = N / 2 - 1; i < N; i++, j--)
  {
    posi = (float32_t)i - m;
    posj = (float32_t)j - m;
    sinc = sinf (ft_rad * posi) / (PI * posi);
    switch (wintype)
    {
    case 0: // Blackman-Harris 4-term
      cosphi = cosf (delta * i);
      window  =             + 0.21747
          + cosphi *  ( - 0.45325
          + cosphi *  ( + 0.28256
          + cosphi *  ( - 0.04672 )));
      break;
    case 1: // Blackman-Harris 7-term
      cosphi = cosf (delta * i);
      window  =       + 6.3964424114390378e-02
          + cosphi *  ( - 2.3993864599352804e-01
          + cosphi *  ( + 3.5015956323820469e-01
          + cosphi *  ( - 2.4774111897080783e-01
          + cosphi *  ( + 8.5438256055858031e-02
          + cosphi *  ( - 1.2320203369293225e-02
          + cosphi *  ( + 4.3778825791773474e-04 ))))));
      break;
    }
    coef = scale * sinc * window;
    switch (rtype)
    {
    case 0:
      cbs.impulse[i] = + coef * cosf (posi * w_osc);
      cbs.impulse[j] = + coef * cosf (posj * w_osc);
      break;
    case 1:
      // e^{+j w n}: a band given as positive frequencies passes positive frequencies of I + jQ, which
      // is the side the receiver's i+q sum selects for USB.  The masks here are a forward transform of
      // these taps; with the minus sign this was carried over with, every band came out mirrored
      // about zero and USB received the lower sideband.
      cbs.impulse[2 * i + 0] = + coef * cosf (posi * w_osc);
      cbs.impulse[2 * i + 1] = + coef * sinf (posi * w_osc);
      cbs.impulse[2 * j + 0] = + coef * cosf (posj * w_osc);
      cbs.impulse[2 * j + 1] = + coef * sinf (posj * w_osc);
      break;
    }
  }
  //return c_impulse;
}
#endif


#ifdef USE_CONVOLUTION
#ifndef CONVOLUTION_LINEAR_PHASE
// How far below the passband the minimum phase design keeps the response.  Below this the stop band
// is treated as flat, which is what bounds it: the linear phase taps reach -111 dB, and at -110 dB
// the converted ones measure the same.  Lower buys nothing and costs delay, because the conversion
// has less room to be exact in: its transform is four times the response, in float32.
#define CONVOLUTION_MINPHASE_FLOOR   3.0e-6f

// Turn the taps in cbs.impulse into the minimum phase taps with the same magnitude response.
//
// The designer's windowed sinc is linear phase: symmetric about its middle, so everything it passes
// comes out half its length late - 42.6 ms for 1024 taps at 12 kHz - and that half is most of what a
// steep filter of this kind costs in delay.  Nothing about selecting a band requires it.  Among all
// the filters with one magnitude response the minimum phase one puts its energy as early as it can
// be put, and its delay is set by how narrow the band is and not by how many taps describe it.  The
// price is that the delay is no longer the same at every frequency: it is smallest in the middle of
// the band and rises towards the edges.
//
// This is the standard construction: the logarithm of the magnitude, its cepstrum, the cepstrum
// folded onto positive time, and back.  It runs in place in the design scratch, which is the
// worker's own buffers - so the caller must have emptied the queues first, and must clear the
// worker's history afterwards.
static void convolution_minimum_phase(int nc)
{
    float32_t* const x = cob.design_scratch;
    const int points = CONVOLUTION_MINPHASE_FFT_SIZE;

    for (int k = 0; k < 2 * points; k++)
    {
        x[k] = k < 2 * nc ? cbs.impulse[k] : 0.0f;
    }
    arm_cfft_f32(&CONVOLUTION_MINPHASE_CFFT_INSTANCE, x, 0, 1);

    float32_t peak = 0.0f;
    for (int k = 0; k < points; k++)
    {
        const float32_t power = x[2 * k] * x[2 * k] + x[2 * k + 1] * x[2 * k + 1];
        x[2 * k] = power;
        if (power > peak)
        {
            peak = power;
        }
    }
    if (peak <= 0.0f)
    {
        return;         // no taps at all: nothing to convert, and no logarithm to take
    }
    const float32_t lowest = peak * CONVOLUTION_MINPHASE_FLOOR * CONVOLUTION_MINPHASE_FLOOR;
    for (int k = 0; k < points; k++)
    {
        x[2 * k] = 0.5f * logf(x[2 * k] < lowest ? lowest : x[2 * k]);
        x[2 * k + 1] = 0.0f;
    }
    arm_cfft_f32(&CONVOLUTION_MINPHASE_CFFT_INSTANCE, x, 1, 1);

    // The worst possible moment for a worker pass, and the reason the seam is closed during a design:
    // the half-finished cepstrum is all of x, and x is the worker's fftin, fftout and accum.  Tested
    // from here by section 12 of support/digi/convolution_probe.c.
    if (conv_test_preempt)
    {
        conv_test_preempt("cepstrum inverse transform");
    }

    // fold: twice the positive-time half, none of the negative-time half, the two ends as they are
    for (int k = 1; k < points / 2; k++)
    {
        x[2 * k] *= 2.0f;
        x[2 * k + 1] *= 2.0f;
    }
    for (int k = points / 2 + 1; k < points; k++)
    {
        x[2 * k] = 0.0f;
        x[2 * k + 1] = 0.0f;
    }
    arm_cfft_f32(&CONVOLUTION_MINPHASE_CFFT_INSTANCE, x, 0, 1);

    for (int k = 0; k < points; k++)
    {
        const float32_t magnitude = expf(x[2 * k]);
        const float32_t phase = x[2 * k + 1];
        x[2 * k] = magnitude * cosf(phase);
        x[2 * k + 1] = magnitude * sinf(phase);
    }
    arm_cfft_f32(&CONVOLUTION_MINPHASE_CFFT_INSTANCE, x, 1, 1);

    for (int k = 0; k < 2 * nc; k++)
    {
        cbs.impulse[k] = x[k];
    }
}
#endif

void AudioDriver_SetConvolutionFilter (int nc, float32_t f_low, float32_t f_high, float32_t samplerate, int wintype, float32_t gain)
{
	/****************************************************************
	 *  Partitioned Convolution code adapted from wdsp library
	 *  (c) by Warren Pratt under GNU GPLv3
	 ****************************************************************/

  int i;
  // this calculates the impulse response (=coefficients) of a complex bandpass filter
  // it needs to be complex in order to allow for SSB demodulation
  // this writes the calculated coeffs into the adb.impulse array
  AudioDriver_CalcConvolutionFilterCoeffs (nc, f_low, f_high, samplerate, wintype, 1, gain);
#ifdef CONVOLUTION_MEASURE
  const uint32_t meas_t_minphase = conv_meas_clock();
#endif
#ifndef CONVOLUTION_LINEAR_PHASE
  convolution_minimum_phase(nc);
#endif
#ifdef CONVOLUTION_MEASURE
  conv_meas_max(&conv_meas_minphase_cyc_max, conv_meas_clock() - meas_t_minphase);
  const uint32_t meas_t_masks = conv_meas_clock();
#endif
  cbs.buffidx = 0;
  for (i = 0; i < cbs.nfor; i++)
  {
    // The other window a worker pass would have to land in: this partition's mask is not written yet,
    // and the worker multiplies against every partition.
    if (conv_test_preempt)
    {
        conv_test_preempt("mask partition");
    }
    // I right-justified the impulse response => take output from left side of output buff, discard right side
    // Be careful about flipping an asymmetrical impulse response.
    // DD4WH: unsure how to interpret that, maybe like this:
    // maskgen is of size: 2 * FFT_size * sizeof(complex)
    // maskgen: left half is filled with zeros!? (starts to be filled from maskgen[2 * FFT_size], which is the centre of the array)
    // right half of maskgen: is filled with the relevant part of the impulse response
    // next round takes the next part of the impulse response
    // ???the right half of impulse is not being used (discarded), because 2 * FFT_size * (nfor-1) is maximum of pointer
    for(int idx = 0; idx < cbs.size * 2; idx++)
    {
    	cbs.maskgen[idx] = 0;
    	cbs.maskgen[idx + cbs.size * 2] = cbs.impulse[idx + i * cbs.size * 2];
    }
    //memcpy (&(a->maskgen[2 * a->size]), &(impulse[2 * a->size * i]), a->size * sizeof(complex));

    // do FFT
    arm_cfft_f32(&CONVOLUTION_CFFT_INSTANCE, cbs.maskgen, 0, 1);
    // take input from maskgen and put output into fmask
    for(int idx = 0; idx < cbs.size * 4; idx++)
    {
    	cob.fmask[i][idx] = cbs.maskgen[idx];
    }

    //fftw_execute (a->maskplan[i]);
  }
  // after the loop is finished,  fmask[nfor][FFT_size * 2] is filled with the FFT outputs of the partitioned filter response
#ifdef CONVOLUTION_MEASURE
  conv_meas_max(&conv_meas_mask_cyc_max, conv_meas_clock() - meas_t_masks);
#endif

}

// Size the partitions and design the masks for one filter path of the radio.  This is the whole of
// the setup, and it lives here rather than in AudioDriver_SetProcessingChain() so that the host
// probe measures the arguments the radio uses: when the probe typed its own copy of this, the copy
// had different band edges and a different gain, and neither difference could show.
//
//   width_hz, offset_hz   FilterInfo[].width and FilterPathDescriptor.offset of the path.  The offset
//                         is the centre of the path's audio band, 0 meaning half the width, and the
//                         path's own IIR - which still runs after this filter - is centred there.
//   lsb                   the band below zero instead of above it
//   samplerate            the rate the blocks are handed over at
void AudioDriver_ConvolutionSetFilterPath(float32_t width_hz, float32_t offset_hz, bool lsb, float32_t samplerate)
{
    // the band the masks in cob.fmask were last made for
    static bool designed = false;
    static float32_t designed_low, designed_high, designed_rate;

    // First, and before anything else: the design borrows the worker's buffers as its working store, so
    // the seam that feeds them is closed for the whole of it.
    //
    // This is the condition that makes the aliasing legal, and until now it was held by nobody here.
    // Emptying the queues is not what holds it, because the audio interrupt fills them again on the next
    // slice; what held it was AudioDriver_SetProcessingChain() running with ads.af_disabled set, which
    // keeps the interrupt out of the whole RX processor - true today, invisible from this file, and one
    // call site away from untrue.  A worker pass in the middle of a design is not a glitch either: on
    // this build one pass between the cepstrum's inverse transform and the mask loop leaves all 4096
    // mask floats NaN, and the cache below then holds them until the operator changes filter width - a
    // radio that is deaf until you tune, with block counters that never noticed.
    //
    // Setting the flag is sufficient on its own, and that is the point of doing it here rather than
    // relying on the caller: a design runs in the main loop, and thread mode cannot preempt a worker
    // pass that has already begun, so by the time this function runs none is in flight and none can
    // start until the flag is cleared.  af_disabled still matters for the post-demod stages; it is no
    // longer the only thing standing between a filter change and a NaN filter.
    conv_design_busy = true;
#ifdef CONVOLUTION_MEASURE
    // The window opens here, before the queues are emptied, and is closed by the audio interrupt when
    // the first block filtered by the new masks comes back.  It has to span both: the flush costs
    // about one block of buffering even when the design is instant, and the earlier version timed
    // only the design - a hole whose end the operator heard but the field never reported.
    //
    // PRIMASK is held while a stale window is closed and the new one opened, so the interrupt cannot
    // half-replace either.  The seam is already held by the line above, so nothing can close the new
    // window early: Output() hands back no samples until the design is finished.
    conv_meas_clock_start();
    const unsigned int meas_irq = conv_irq_save();
    conv_meas_gap_close(conv_meas_clock());     // a window left open, e.g. a change made in TX
    conv_meas_gap_start = conv_meas_clock();
    conv_meas_gap_open = true;
    conv_irq_restore(meas_irq);

    const uint32_t meas_busyin_t0 = conv_stat_busyin;
    conv_meas_setup_count++;
    conv_meas_lowest(&conv_meas_msp_min_design, conv_meas_sp());
#endif
    AudioDriver_ConvolutionReset();

    cbs.size = FFT_CONVOLUTION_SIZE / 2;
    // The partitions must fit the arrays: nfor = nc / size, and fmask[]/fftout[] have
    // CONVOLUTION_MAX_NO_OF_BLOCKS of them, so the longest response the block size allows is
    // MAX_NO_OF_BLOCKS * size taps.  Clamped rather than asserted, because this runs on whatever
    // filter path the user last used and a rejected build would only hide it.
    cbs.nc = CONVOLUTION_MAX_NO_OF_COEFFS;
    if (cbs.nc / cbs.size > CONVOLUTION_MAX_NO_OF_BLOCKS)
    {
        cbs.nc = cbs.size * CONVOLUTION_MAX_NO_OF_BLOCKS;
    }
    cbs.nfor = cbs.nc / cbs.size;
    cbs.buffidx = 0;

    const float32_t centre = offset_hz > 0.0f ? offset_hz : width_hz / 2.0f;
    float32_t lo = centre - width_hz / 2.0f;
    const float32_t hi = centre + width_hz / 2.0f;
    if (lo < CONVOLUTION_LOW_CUT_HZ)
    {
        lo = CONVOLUTION_LOW_CUT_HZ;
    }

    // low edge first on either side of zero: handed over the other way round the designer's
    // bandwidth is negative and every tap changes sign
    const float32_t f_low = lsb ? -hi : lo;
    const float32_t f_high = lsb ? -lo : hi;

    // The processing chain is set up again for many reasons that leave the band alone - every step
    // of a DSP setting in the menu is one - and a design is a few thousand logarithms and sines, so
    // the masks are kept when they are already the ones asked for.
    if (!designed || f_low != designed_low || f_high != designed_high || samplerate != designed_rate)
    {
        AudioDriver_SetConvolutionFilter(cbs.nc, f_low, f_high, samplerate, 0, CONVOLUTION_PASSBAND_GAIN);
        designed = true;
        designed_low = f_low;
        designed_high = f_high;
        designed_rate = samplerate;
#ifdef CONVOLUTION_MEASURE
        conv_meas_design_count++;
#endif
    }

    // the worker's history: it held the audio of the filter before, or the design's working
    memset(cob.fftin, 0, sizeof(cob.fftin));
    memset(cob.fftout, 0, sizeof(cob.fftout));

    // Open the seam again last: the history is cleared before the filter can be handed a sample, so the
    // first block out of a new filter is filtered by the new masks and by nothing that came before them.
    conv_design_busy = false;
#ifdef CONVOLUTION_MEASURE
    // Frames refused by the seam.  With the current call site this is zero: the design runs inside
    // AudioDriver_SetProcessingChain()'s af_disabled window, so the RX interrupt never offers a slice
    // while conv_design_busy is set.  It counts only if a design is ever run with the RX path unmuted,
    // which conv_design_busy now makes safe.  The refill after the design is in the hole, not here.
    const uint32_t meas_refused = conv_stat_busyin - meas_busyin_t0;
    conv_meas_busyin_last = meas_refused;
    conv_meas_max(&conv_meas_busyin_max, meas_refused);
    conv_meas_lowest(&conv_meas_msp_min_design, conv_meas_sp());
#endif
}
#endif

#ifdef USE_CONVOLUTION
#ifdef CONVOLUTION_MEASURE
static void conv_handle_body(void)
#else
void convolution_handle()
#endif
{
	// This runs in the PendSV handler, out of the audio interrupt, which is the whole reason the
	// design is interesting: a 256-point complex FFT is not something to want inside the sample ISR.
	// It is called whenever the interrupt is pended, so all it may assume is that a block the
	// interrupt finished writing is waiting in the in queue.  Nothing here may wait for one.
	Sample_Buffer* block;

	// A design in progress means the buffers this function works in belong to the design.  Nothing is
	// queued while that is true - AudioDriver_ConvolutionInput() refuses it - but the test is what makes
	// that safe rather than merely true at the moment.
	if (conv_design_busy)
	{
		return;
	}

	if (Sample_in_buffer_remove(&block) == 0)
	{
		return;
	}
#ifdef CONVOLUTION_MEASURE
	conv_meas_did_work = true;
#endif

	// put everything that should happen, when 128 samples are ready, into this function
	/****************************************************************
	 *
	 *  Partitioned Convolution code adapted from wdsp library
	 *
	 *  (c) by Warren Pratt under GNU GPLv3
	 *
	 *  thanks, Warren!
	 *
	 ****************************************************************/


	            // input of this pass: the block the audio interrupt filled, one complex sample per
	            // slot, oldest first
	            // IIIIIIIIIIIIIIII
	            // <- cbs.size   ->
	            //
	            // (the I and Q of one sample sit together in .real/.imag of the same slot)

	            // NEW AUDIO SAMPLES: the two files interleaved
	            // IQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQ
	            // <-      2 * cbs.size          ->
	            //

	            // fill new samples into SECOND HALF of fftin
	            // first half is filled with the old audio samples
	            //				| OLD  |					| NEW |
	            // IQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQ
	            // <-      2 * cbs.size          -><-      2 * cbs.size          ->

	            // FFT of fftin
	            // output of FFT is copied into fftout[buffidx]
	            // IQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQ
	            // <-      2 * cbs.size          -><-      2 * cbs.size          ->
	            // there are as many FFT output buffers as there are convolution blocks

	            // Complex multiply / accumulate with all FFT outputs of nfor last rounds, see below :-) !

	            // inverse FFT of the accumulated complex multiply outputs
	            // IQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQ
	            // <-      2 * cbs.size          -><-      2 * cbs.size          ->
	            // inverse FFT result is in accum

	            // discard first half of inverse FFT output and separate I & Q into separate buffers
	            // XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQIQ
	            // <-      2 * cbs.size          -><-      2 * cbs.size          ->
	            //
	            // i_buffer_convolution
	            // IIIIIIIIIIIIIIII
	            // <- cbs.size   ->
	            // --> this is the filtered audio of I

	            // q_buffer_convolution
	            // QQQQQQQQQQQQQQQQ
	            // <- cbs.size   ->
	            // this is the filtered audio of Q

	 /*
	  * partitioned block overlap-and-save algorithm
	  */
	            	int fft_conv_size = cbs.size * 2;
	                int i, j, k;
	                // copy input buffer to right half of FFT input buffer
	                // left half already contains data from last round
	                for(int idx=0; idx < cbs.size; idx++)
	                {
	                	cob.fftin[fft_conv_size + 2 * idx + 0] = block->samples[idx].real;
	                  	cob.fftin[fft_conv_size + 2 * idx + 1] = block->samples[idx].imag;
	                }

	                // fftin --> one input buffer
	                // fftout[nfor] --> changing output buffers !
	                // FFT performed on fftin inplace
	                arm_cfft_f32(&CONVOLUTION_CFFT_INSTANCE, cob.fftin, 0, 1);
	                // copy output from fftin into current fftout buffer
	                for (int idx = 0; idx < fft_conv_size * 2; idx++)
	                {
	                	cob.fftout[cbs.buffidx][idx] = cob.fftin[idx];
	                }
	                // The transform above is in place, so fftin now holds a spectrum and its time
	                // samples are gone.  The next block needs these ones as the low half of its
	                // window, and the only copy left is the block that has not been written over
	                // yet, so the history is restored from there here - not after the inverse
	                // transform, which is where this code used to do it and where fftin has been a
	                // spectrum for two steps by then.  wdsp, which this was adapted from, does not
	                // have the problem because its plan writes the transform to a separate buffer.
	                for(int idx = 0; idx < cbs.size; idx++)
	                {
	                	cob.fftin[2 * idx + 0] = block->samples[idx].real;
	                	cob.fftin[2 * idx + 1] = block->samples[idx].imag;
	                }
	                //fftw_execute (a->pcfor[a->buffidx]);

	                k = cbs.buffidx;
	                // fill the array accum with zeros
	                for (int idx = 0; idx < fft_conv_size * 2; idx++)
	                {
	                	cob.accum[idx] = 0;
	                }
	                //memset (a->accum, 0, 2 * a->size * sizeof (complex));


	                for (j = 0; j < cbs.nfor; j++)
	                {
	                  for (i = 0; i < fft_conv_size; i++)
	                  {
	                    // this sums up the complex multiply results for real and imaginary components
	                    // stored in accum
	                    cob.accum[2 * i + 0] += cob.fftout[k][2 * i + 0] * cob.fmask[j][2 * i + 0]
				                              - cob.fftout[k][2 * i + 1] * cob.fmask[j][2 * i + 1];
	                    cob.accum[2 * i + 1] += cob.fftout[k][2 * i + 0] * cob.fmask[j][2 * i + 1]
											  + cob.fftout[k][2 * i + 1] * cob.fmask[j][2 * i + 0];
	                  }
	                  // k points to the next relevant fftout result
	                  // it must flip over when 0 is reached
	                  k = k -1;
	                  if(k < 0)
	                  {
	                	  k = cbs.nfor - 1;
	                  }
	                  //k = (k + a->idxmask) & a->idxmask;
	                }
	                cbs.buffidx +=1;
	                if(cbs.buffidx >= cbs.nfor)
	                {
	                	cbs.buffidx = 0;
	                }
	                // a->buffidx = (a->buffidx + 1) & a->idxmask;
	                // inverse FFT
	                // input: accum
	                // audio is in right half of the output buffer accum
	                arm_cfft_f32(&CONVOLUTION_CFFT_INSTANCE, cob.accum, 1, 1);
	                // fftw_execute (a->crev);
	                // the input history was already restored from the block above, before the
	                // inverse transform; taking it from fftin here would take the spectrum instead
	                //memcpy (a->fftin, &(a->fftin[2 * a->size]), a->size * sizeof(complex));
	                // Take the LEFT half of the inverse FFT output, and discard the right one.
	                // This is what the note above the mask generator has said all along - "I
	                // right-justified the impulse response => take output from left side of output
	                // buff, discard right side" - with DD4WH's own "unsure how to interpret that"
	                // next to it, and the code did the opposite: it kept the right half.  The two
	                // halves of an overlap-save block are the circular part and the clean part, so
	                // choosing the wrong one is not a delay, it is a filter that is not the filter
	                // that was designed.  Measured (support/digi/convolution_probe.c), with the
	                // passband at +270..+2700 designed against 12 kHz:
	                //
	                //   right half:  opposite sideband leaks at -12 dB, and the output differs from
	                //                  a plain complex FIR over the same taps by 0.97 (relative)
	                //   left half :  opposite sideband -140 dB, and 3e-07 from the same FIR
	                //
	                // which is also the difference between a 12 dB sideband score and the 88 dB the
	                // 199-tap Hilbert pair already gives the receiver today.
	                for(int idx = 0; idx < cbs.size; idx++)
	                {
	                	block->samples[idx].real = cob.accum[2 * idx + 0];
	                	block->samples[idx].imag = cob.accum[2 * idx + 1];
	                }
	                // these buffers now contain the bandpass filtered audio
	                // which already has suppressed opposite sideband (if cutoff-frequencies have been set correctly for the BP filter)
	                // so no further "demodulation" is necessary for SSB / CW

	////////////////////////////////////////////////////////////////////////////////////////////////////////////



	/* ************************************************
	 *  AGC
	 * ************************************************/
	            // AGC: deliberately not here.  The AGC the radio runs today is driven from the audio
	            // interrupt on the blocks of adb.a_buffer, with its rate and its DC removal set up
	            // next to the filter path, so a second copy running on I and Q before demodulation
	            // would leave the receiver with two AGCs fighting over one gain.  This function hands
	            // the filtered block back and lets the part of the chain that is maintained do the
	            // rest - which is also the part the sketch below the convolution never wrote.
	            //
	            // The old line was `AudioDriver_RxAgcWdsp(cbs.size, cob.i_buffer_convolution,
	            // cob.q_buffer_convolution)`: no function of that name has existed in the tree since
	            // 2019, which is the same year the rest of this branch stopped compiling.

	if (Sample_out_buffer_add(block) == 0)
	{
		conv_block_give(block);        // the interrupt is not reading: this block is late, not lost
		// The interrupt increments the same counter when it cannot stage a block, and this runs below
		// it: without the lock an increment here can be preempted halfway and lost, and this is the
		// counter the test procedure requires to stay at zero.
		const unsigned int irq = conv_irq_save();
		conv_stat_dropped++;           // and it is gone: the audio it carried never reaches the speaker
		conv_irq_restore(irq);
	}
}

#ifdef CONVOLUTION_MEASURE
void convolution_handle()
{
	const uint32_t meas_t0 = conv_meas_clock();
	conv_meas_did_work = false;
	conv_handle_body();
	const uint32_t meas_cyc = conv_meas_clock() - meas_t0;
	// The worst-case figure counts every call, empty ones included, because an empty call is what a
	// radio that is not being pended costs; the average is taken over the passes that filtered a block,
	// which is the number to compare against 667 us of interrupt interval.
	conv_meas_max(&conv_meas_block_cyc_max, meas_cyc);
	if (conv_meas_did_work)
	{
		conv_meas_block_cyc_total += meas_cyc;
		conv_meas_block_count++;
		// Halved together at the bound, so the ratio survives and the sum cannot wrap.  See
		// CONV_MEAS_BLOCK_AVG_PASSES.
		if (conv_meas_block_count >= CONV_MEAS_BLOCK_AVG_PASSES)
		{
			conv_meas_block_cyc_total /= 2;
			conv_meas_block_count /= 2;
		}
	}
	conv_meas_lowest(&conv_meas_msp_min_worker, conv_meas_sp());
}
#endif

#endif
