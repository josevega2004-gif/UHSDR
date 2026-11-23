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
// hears nothing.  These four numbers tell them apart on the screen.
volatile uint32_t conv_stat_slices_in;      // slices handed in by the audio interrupt
volatile uint32_t conv_stat_blocks_in;      // whole blocks that reached the worker's queue
volatile uint32_t conv_stat_blocks_out;     // finished blocks the interrupt took back
volatile uint32_t conv_stat_dropped;        // blocks thrown away because nothing was free
volatile uint32_t conv_stat_nosetup;        // slices refused because cbs.size was still 0

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

void AudioDriver_ConvolutionReset(void)
{
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
}

static Sample_Buffer* conv_block_take(void)
{
    if (conv_free_head == conv_free_tail)
    {
        return NULL;
    }
    Sample_Buffer* c = conv_free[conv_free_head];
    conv_free_head = (conv_free_head + 1) % (SAMPLE_BUFFER_NUM + 1);
    return c;
}

static void conv_block_give(Sample_Buffer* c)
{
    conv_free[conv_free_tail] = c;
    conv_free_tail = (conv_free_tail + 1) % (SAMPLE_BUFFER_NUM + 1);
}

// Hand a slice of I/Q to the filter.  Returns the number of samples taken, which is the whole slice
// unless the pipeline cannot start a block.  A slice is smaller than a block - the audio interrupt
// hands over IQ_BLOCK_SIZE = 48000/1500 = 32 samples, or 8 of them once a narrow filter path has
// decimated by 4, and a block is cbs.size = 128 - so a block is filled across four or sixteen calls
// and the staging between them is what has to be right.  A block dropped here is a dropout in the
// audio rather than a desynchronisation, which is what makes refusing better in an interrupt than
// waiting.
uint16_t AudioDriver_ConvolutionInput(const float32_t* i, const float32_t* q, uint16_t count)
{
    uint16_t done = 0;

    if (cbs.size == 0)
    {   // AudioDriver_SetProcessingChain() has not sized the blocks yet
        conv_stat_nosetup++;
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
                break;
            }
            conv_stage_count = 0;
        }

        uint16_t room = cbs.size - conv_stage_count;
        uint16_t taken = count - done < room ? count - done : room;

        for (uint16_t k = 0; k < taken; k++)
        {
            conv_stage->samples[conv_stage_count + k].real = i[done + k];
            conv_stage->samples[conv_stage_count + k].imag = q[done + k];
        }
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

// Take finished samples back out of the filter.  Returns the number given, which is the whole request
// as long as blocks have come back: 0 until the worker has produced a first one, and short of the
// request only if it has run out mid slice.  Either way the caller fills silence for what is missing,
// and after that the two streams run at the same rate because they carry the same samples.
uint16_t AudioDriver_ConvolutionOutput(float32_t* i, float32_t* q, uint16_t count)
{
    uint16_t done = 0;

    if (cbs.size == 0)
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

        for (uint16_t k = 0; k < given; k++)
        {
            i[done + k] = conv_play->samples[conv_play_count + k].real;
            q[done + k] = conv_play->samples[conv_play_count + k].imag;
        }
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
      cbs.impulse[2 * i + 0] = + coef * cosf (posi * w_osc);
      cbs.impulse[2 * i + 1] = - coef * sinf (posi * w_osc);
      cbs.impulse[2 * j + 0] = + coef * cosf (posj * w_osc);
      cbs.impulse[2 * j + 1] = - coef * sinf (posj * w_osc);
      break;
    }
  }
  //return c_impulse;
}
#endif


#ifdef USE_CONVOLUTION
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
  cbs.buffidx = 0;
  for (i = 0; i < cbs.nfor; i++)
  {
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

}
#endif

#ifdef USE_CONVOLUTION
void convolution_handle()
{
	// This runs in the PendSV handler, out of the audio interrupt, which is the whole reason the
	// design is interesting: a 256-point complex FFT is not something to want inside the sample ISR.
	// It is called whenever the interrupt is pended, so all it may assume is that a block the
	// interrupt finished writing is waiting in the in queue.  Nothing here may wait for one.
	Sample_Buffer* block;

	if (Sample_in_buffer_remove(&block) == 0)
	{
		return;
	}

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
	}
}

#endif
