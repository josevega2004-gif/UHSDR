/*  -*-  mode: c; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4; coding: utf-8  -*-  */
/************************************************************************************
 **                                                                                 **
 **                               UHSDR FIRMWARE                                    **
 **                                                                                 **
 **---------------------------------------------------------------------------------**
 **  Licence:        GNU GPLv3, see LICENSE.md                                      **
 ************************************************************************************/

// Common
#include <assert.h>
#include "uhsdr_board.h"
#include "profiling.h"

#include <stdio.h>
#include <math.h>
#include <limits.h>

#include "softdds.h"
#include "rtty.h"
#include "ui_driver.h" // only necessary because of UiDriver_TextMsgPutChar
#include "radio_management.h" // only necessary because of RadioManagement_Request_TxOff
#include "uhsdr_digi_buffer.h"
#include <string.h>


// character tables borrowed from fldigi / rtty.cxx
static const char RTTYLetters[] = {
    '\0',   'E',    '\n',   'A',    ' ',    'S',    'I',    'U',
    '\r',   'D',    'R',    'J',    'N',    'F',    'C',    'K',
    'T',    'Z',    'L',    'W',    'H',    'Y',    'P',    'Q',
    'O',    'B',    'G',    ' ',    'M',    'X',    'V',    ' '
};

static const char RTTYSymbols[32] = {
    '\0',   '3',    '\n',   '-',    ' ',    '\a',   '8',    '7',
    '\r',   '$',    '4',    '\'',   ',',    '!',    ':',    '(',
    '5',    '"',    ')',    '2',    '#',    '6',    '0',    '1',
    '9',    '?',    '&',    ' ',    '.',    '/',    ';',    ' '
};


// bits 0-4 -> baudot, bit 5 1 == LETTER, 0 == NUMBER/FIGURE
const uint8_t Ascii2Baudot[128] =
{
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0b001011, //	BEL	N
		0,
		0,
		0b000010, //	\n	NL
		0,
		0,
		0b001000, //	\r	NL
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0b100100, // 	 	NL
		0b001101, //	!   N
		0b010001, //	"   N
		0b010100, //	#   N
		0b001001, //    $   N
		0, 		  //	%
		0b011010, //	&   N
		0b000101, //	'	N
		0b001111, //	(	N
		0b010010, //	)	N
		0, 		  //	*
		0, 		  //	+	
		0b001100, //	,	N
		0b000011, //	-	N
		0b011100, //	.	N
		0b011101, //	/	N
		0b010110, //	0	N
		0b010111, //	1	N
		0b010011, //	2	N
		0b000001, //	3	N
		0b001010, //	4	N
		0b010000, //	5	N
		0b010101, //	6	N
		0b000111, //	7	N
		0b000110, //	8	N
		0b011000, //	9	N
		0b001110, //	:	N
		0b011110, //	;   N
		0, 		  //	<
		0,        //	=
		0,        //	>
		0b011001, //	?	N
		0,        //	@
		0b100011, //	A	L
		0b111001, //	B	L
		0b101110, //	C	L
		0b101001, //	D	L
		0b100001, //	E	L
		0b101101, //	F	L
		0b111010, //	G	L
		0b110100, //	H	L
		0b100110, //	I	L
		0b101011, //	J	L
		0b101111, //	K	L
		0b110010, //	L	L
		0b111100, //	M	L
		0b101100, //	N	L
		0b111000, //	O	L
		0b110110, //	P	L
		0b110111, //	Q	L
		0b101010, //	R	L
		0b100101, //	S	L
		0b110000, //	T	L
		0b100111, //	U	L
		0b111110, //	V	L
		0b110011, //	W	L
		0b111101, //	X	L
		0b110101, //	Y	L
		0b110001, //	Z	L
		0,
		0,
		0,
		0,
		0,
		0,
		0b100011, //	A	L
		0b111001, //	B	L
		0b101110, //	C	L
		0b101001, //	D	L
		0b100001, //	E	L
		0b101101, //	F	L
		0b111010, //	G	L
		0b110100, //	H	L
		0b100110, //	I	L
		0b101011, //	J	L
		0b101111, //	K	L
		0b110010, //	L	L
		0b111100, //	M	L
		0b101100, //	N	L
		0b111000, //	O	L
		0b110110, //	P	L
		0b110111, //	Q	L
		0b101010, //	R	L
		0b100101, //	S	L
		0b110000, //	T	L
		0b100111, //	U	L
		0b111110, //	V	L
		0b110011, //	W	L
		0b111101, //	X	L
		0b110101, //	Y	L
		0b110001, //	Z	L
		0,
		0,
		0,
		0,
		0,
};

#define RTTY_SYMBOL_CODE (0b11011)
#define RTTY_LETTER_CODE (0b11111)

// RTTY Experiment based on code from the DSP Tutorial at http://dp.nonoo.hu/projects/ham-dsp-tutorial/18-rtty-decoder-using-iir-filters/
// Used with permission from Norbert Varga, HA2NON under GPLv3 license

/*
 * Experimental Code
 */
#ifdef USE_RTTY_PROCESSOR

const rtty_speed_item_t rtty_speeds[RTTY_SPEED_NUM] =
{
		{ .id =RTTY_SPEED_45, .value = 45.45, .label = "45" },
		{ .id =RTTY_SPEED_50, .value = 50, .label = "50"  },
};

const rtty_shift_item_t rtty_shifts[RTTY_SHIFT_NUM] =
{
		{ RTTY_SHIFT_85, 85, " 85" },
		{ RTTY_SHIFT_170, 170, "170" },
		{ RTTY_SHIFT_200, 200, "200" },
		{ RTTY_SHIFT_425, 425, "425" },
		{ RTTY_SHIFT_450, 450, "450" },
		{ RTTY_SHIFT_850, 850, "850" },
};

rtty_ctrl_t rtty_ctrl_config =
{
		.shift_idx = RTTY_SHIFT_170,
		.speed_idx = RTTY_SPEED_45,
		.stopbits_idx = RTTY_STOP_1_5
};



typedef struct
{
	float32_t gain;
	float32_t coeffs[4];
	uint16_t freq; // center freq
} rtty_bpf_config_t;

typedef struct
{
	float32_t gain;
	float32_t coeffs[2];
} rtty_lpf_config_t;

typedef struct
{
	float32_t xv[5];
	float32_t yv[5];
} rtty_bpf_data_t;

typedef struct
{
	float32_t xv[3];
	float32_t yv[3];
} rtty_lpf_data_t;



static float32_t RttyDecoder_bandPassFreq(float32_t sampleIn, const rtty_bpf_config_t* coeffs, rtty_bpf_data_t* data) {
	data->xv[0] = data->xv[1]; data->xv[1] = data->xv[2]; data->xv[2] = data->xv[3]; data->xv[3] = data->xv[4];
	data->xv[4] = sampleIn / coeffs->gain; // gain at centre
	data->yv[0] = data->yv[1]; data->yv[1] = data->yv[2]; data->yv[2] = data->yv[3]; data->yv[3] = data->yv[4];
	data->yv[4] = (data->xv[0] + data->xv[4]) - 2 * data->xv[2]
															 + (coeffs->coeffs[0] * data->yv[0]) + (coeffs->coeffs[1] * data->yv[1])
															 + (coeffs->coeffs[2] * data->yv[2]) + (coeffs->coeffs[3] * data->yv[3]);
	return data->yv[4];
}

static float32_t RttyDecoder_lowPass(float32_t sampleIn, const rtty_lpf_config_t* coeffs, rtty_lpf_data_t* data) {
	data->xv[0] = data->xv[1]; data->xv[1] = data->xv[2];
	data->xv[2] = sampleIn / coeffs->gain; // gain at DC
	data->yv[0] = data->yv[1]; data->yv[1] = data->yv[2];
	data->yv[2] = (data->xv[0] + data->xv[2]) + 2 * data->xv[1]
															 + (coeffs->coeffs[0] * data->yv[0]) + (coeffs->coeffs[1] * data->yv[1]);
	return data->yv[2];
}

typedef enum {
	RTTY_RUN_STATE_WAIT_START = 0,
	RTTY_RUN_STATE_BIT,
} rtty_run_state_t;


typedef enum {
	RTTY_MODE_LETTERS = 0,
	RTTY_MODE_SYMBOLS
} rtty_charSetMode_t;




typedef struct {
	rtty_bpf_data_t bpfSpaceData;
	rtty_bpf_data_t bpfMarkData;
	rtty_lpf_data_t lpfData;
	rtty_bpf_config_t *bpfSpaceConfig;
	rtty_bpf_config_t *bpfMarkConfig;
	rtty_lpf_config_t *lpfConfig;

	uint16_t oneBitSampleCount;
	int32_t DPLLOldVal;
	int32_t DPLLBitPhase;

	// the ATC envelopes and the framing search: per transmission, not per program run
	float32_t mark_env, space_env, mark_noise, space_noise;
	bool phaseChanged;
	int16_t wait_for_start_state, wait_for_half;

	uint8_t byteResult;
	uint16_t byteResultp;

	rtty_charSetMode_t charSetMode;

	rtty_run_state_t state;

	soft_dds_t tx_dds[2];

	const rtty_mode_config_t* config_p;

} rtty_decoder_data_t;

rtty_decoder_data_t rttyDecoderData;




#if 0 // 48Khz filters, not needed
// this is for 48ksps sample rate
// for filter designing, see http://www-users.cs.york.ac.uk/~fisher/mkfilter/
// order 2 Butterworth, freqs: 865-965 Hz
rtty_bpf_config_t rtty_bp_48khz_915 =
{
		.gain = 2.356080041e+04,  // gain at centre
		.coeffs = {-0.9816582826, 3.9166274264, -5.8882201843, 3.9530488323 },
		.freq = 915
};

// order 2 Butterworth, freqs: 1035-1135 Hz
rtty_bpf_config_t rtty_bp_48khz_1085 =
{
		.gain = 2.356080365e+04,
		.coeffs = {-0.9816582826, 3.9051693660, -5.8653953990, 3.9414842213 },
		.freq = 1085
};
#endif

// order 2 Butterworth, freq: 50 Hz
rtty_lpf_config_t rtty_lp_48khz_50 =
{
		.gain = 9.381008646e+04,
		.coeffs = {-0.9907866988, 1.9907440595 }
};

// this is for 12ksps sample rate
// for filter designing, see http://www-users.cs.york.ac.uk/~fisher/mkfilter/
// order 2 Butterworth, freqs: 865-965 Hz, centre: 915 Hz
static rtty_bpf_config_t rtty_bp_12khz_915 =
{
		.gain = 1.513364755e+03,
		.coeffs = { -0.9286270861, 3.3584472566, -4.9635817596, 3.4851652468 },
		.freq = 915
};

// order 2 Butterworth, freqs: 1315-1415 Hz, centre 1365Hz
static rtty_bpf_config_t rtty_bp_12khz_1365 =
{
		.gain = 1.513365021e+03,
		.coeffs = { -0.9286270861, 2.8583904592, -4.1263569882, 2.9662407443 },
		.freq = 1365
};
// order 2 Butterworth, freqs: 1035-1135 Hz, centre: 1085Hz
static rtty_bpf_config_t rtty_bp_12khz_1085 =
{
		.gain = 1.513364927e+03,
		.coeffs = { -0.9286270861, 3.1900687349, -4.6666321297, 3.3104336142 },
		.freq = 1085
};
// order 2 Butterworth, freqs: 1065-1165 Hz, centre: 1115Hz
// for 200Hz shift
static rtty_bpf_config_t rtty_bp_12khz_1115 =
{
		.gain = 1.513364944e+03,
		.coeffs = { -0.9286270861, 3.1576917276, -4.6112830458, 3.2768349860 },
		.freq = 1115
};

// for 85Hz shift --> 915 + 85Hz = space = 1000Hz
// 3dB bandwidth 50Hz
// order 2 Butterworth, freqs: 975-1025 Hz, centre: 1000Hz
static rtty_bpf_config_t rtty_bp_12khz_1000 =
{
		.gain = 5.944465261e+03,
		.coeffs = { -0.9636529842, 3.3693752166, -4.9084595657, 3.4323354886 },
		.freq = 1000
};

// for 425Hz shift --> 915 + 425Hz = space = 1340Hz
// 3dB bandwidth 100Hz
// order 2 Butterworth, freqs: 1290 - 1390 Hz, centre: 1340Hz
static rtty_bpf_config_t rtty_bp_12khz_1340 =
{
		.gain = 1.513365017e+03,
		.coeffs = { -0.9286270861, 2.8906128090, -4.1762457779, 2.9996788796 },
		.freq = 1340
};

// for 850Hz shift --> 915 + 850Hz = space = 1765Hz
// 3dB bandwidth 100Hz
// order 2 Butterworth, freqs: 1715 - 1815 Hz, centre: 1765Hz
// The coefficients that were here before this line were designed for 1865 Hz, i.e. 100 Hz above
// the label and above the tone the modulator sends for this shift, so the space channel of an
// 850 Hz-shift transmission was filtered at the wrong frequency. Regenerated with the same method
// as the other filters in this bank (bilinear transform of the order 2 Butterworth prototype at
// 12 kHz, edges 1715/1815, unity gain at 1765 Hz).
static rtty_bpf_config_t rtty_bp_12khz_1765 =
{
		.gain = 1.513365053e+03,
		.coeffs = { -0.9286270861, 2.2802267531, -3.3262458862, 2.3662622716 },
		.freq = 1765
};


static rtty_lpf_config_t rtty_lp_12khz_50 =
{
		.gain = 5.944465310e+03,
		.coeffs = { -0.9636529842, 1.9629800894 }
};

static rtty_mode_config_t  rtty_mode_current_config;

/**
 * Receive space filter of a shift
 *
 * Kept as its own function so that the host bench can measure exactly the filter the demodulator
 * uses, instead of a copy of this mapping (support/digi builds with -DHOST_BENCH).
 *
 * @param shift shift in Hz as stored, unsupported values decode as 170
 * @return the filter the demodulator runs on the space tone
 */
static rtty_bpf_config_t* Rtty_SpaceFilterForShift(uint16_t shift)
{
	switch (shift)
	{
	case 85:
		return &rtty_bp_12khz_1000;
	case 200:
		return &rtty_bp_12khz_1115;
	case 425:
		return &rtty_bp_12khz_1340;
	case 450:
		return &rtty_bp_12khz_1365; // this is space or '0'
	case 850:
		return &rtty_bp_12khz_1765; // this is space or '0'
	case 170:
	default:
		// all unsupported shifts are mapped to 170
		return &rtty_bp_12khz_1085; // this is space or '0'
	}
}


void Rtty_Modem_Init(uint32_t output_sample_rate)
{
	// TODO: pass config as parameter and make it changeable via menu
	rtty_mode_current_config.samplerate = 12000;
	rtty_mode_current_config.shift = rtty_shifts[rtty_ctrl_config.shift_idx].value;
	rtty_mode_current_config.speed = rtty_speeds[rtty_ctrl_config.speed_idx].value;
	rtty_mode_current_config.stopbits = rtty_ctrl_config.stopbits_idx;

	rttyDecoderData.config_p = &rtty_mode_current_config;

	// common config to all supported modes
	rttyDecoderData.oneBitSampleCount = (uint16_t)roundf(rttyDecoderData.config_p->samplerate/rttyDecoderData.config_p->speed);
	rttyDecoderData.charSetMode = RTTY_MODE_LETTERS;
	rttyDecoderData.state = RTTY_RUN_STATE_WAIT_START;

	rttyDecoderData.bpfMarkConfig = &rtty_bp_12khz_915; // this is mark, or '1'
	rttyDecoderData.lpfConfig = &rtty_lp_12khz_50;

	// now we handled the specifics
	rttyDecoderData.bpfSpaceConfig = Rtty_SpaceFilterForShift(rttyDecoderData.config_p->shift);

	// configure DDS for transmission
	softdds_setFreqDDS(&rttyDecoderData.tx_dds[0], rttyDecoderData.bpfSpaceConfig->freq, output_sample_rate, 0);
	softdds_setFreqDDS(&rttyDecoderData.tx_dds[1], rttyDecoderData.bpfMarkConfig->freq, output_sample_rate, 0);

}

// TX symbol clock, as a multiplier on the bit length: 1.0 is the rate the receiver expects, 0.99 is
// a transmitter one percent fast. Only the host bench changes it; the radio build leaves it at 1.0.
static float rtty_tx_bit_scale = 1.0f;

#ifdef HOST_BENCH
/* ---------------------------------------------------------------------------------------------
   Host bench hooks. support/digi builds this file with -DHOST_BENCH, the firmware never defines
   it. They expose the receive filter bank so that the bench can check every filter against the
   frequency its own label claims - the check that was missing when rtty_bp_12khz_1765 turned out
   to be an 1865 Hz design. The loopback decodes through the mark channel, so it did not notice.
* ------------------------------------------------------------------------------------------- */

/**
 * Wipe the receiver state so a transmission starts from nothing.
 *
 * The filter histories, the ATC envelopes, the DPLL and the start bit search are state of the
 * receiver, and without this they are state of the program instead: a case in the host bench would
 * then be scored partly on what the case before it transmitted. Only the bench calls this - in the
 * firmware the state lives as long as the mode does - so asking for a clean receiver here changes
 * nothing about what the radio does.
 */
void Rtty_BenchResetDecoder(void)
{
	memset(&rttyDecoderData, 0, sizeof(rttyDecoderData));
}

unsigned Rtty_BenchFilterCount(void)
{
	return 1 + RTTY_SHIFT_NUM;              // the mark filter plus one space filter per shift
}

uint16_t Rtty_BenchFilterLabel(unsigned idx)
{
	if (idx == 0)
	{
		return rtty_bp_12khz_915.freq;
	}
	return Rtty_SpaceFilterForShift(rtty_shifts[idx - 1].value)->freq;
}

/**
 * Magnitude of one receive filter at a frequency, measured by running its own recursion
 * @param idx 0 = mark, 1..RTTY_SHIFT_NUM = space filter of rtty_shifts[idx - 1]
 * @param hz frequency in the 12 kHz audio domain
 * @return response of the filter to a unit amplitude sine at hz, proportional to |H(hz)|
 */
float Rtty_BenchFilterMagnitude(unsigned idx, float hz)
{
	const rtty_bpf_config_t* cfg = (idx == 0) ? &rtty_bp_12khz_915
	                                          : Rtty_SpaceFilterForShift(rtty_shifts[idx - 1].value);
	rtty_bpf_data_t data = { { 0 }, { 0 } };
	const unsigned settle = 600, measure = 2000;
	const double step = 6.283185307179586 * (double)hz / 12000.0;
	double phase = 0.0, acc = 0.0, wsum = 0.0;

	for (unsigned n = 0; n < settle + measure; n++)
	{
		// a wrapping accumulator: computing the phase as hz * n loses every digit of a float32 by
		// the time n reaches a few thousand, and the bench then measures noise instead of the filter
		const float32_t s = (float32_t)sin(phase);
		phase += step;
		if (phase > 6.283185307179586)
		{
			phase -= 6.283185307179586;
		}
		const float32_t y = RttyDecoder_bandPassFreq(s, cfg, &data);
		if (n >= settle)
		{
			// Hann taper: a plain RMS over the segment ripples with the fractional number of cycles
			// in it, which moved the measured peak by up to 5 Hz. The taper makes the estimate
			// smooth in hz, which is all the peak search needs.
			const double k = (double)(n - settle);
			const double w = 0.5 - 0.5 * cos(6.283185307179586 * k / (double)(measure - 1));
			acc += w * (double)y * (double)y;
			wsum += w;
		}
	}
	return (float)sqrt(acc / wsum);
}

/**
 * Slide both transmit tones by the same number of Hz.
 *
 * That is exactly what a tuning error or a reference oscillator error does to a received FSK
 * pair: mark and space keep the spacing the transmitter sent and move together in the audio,
 * because both are shifted by the same number of Hz at RF. The receiver has no automatic
 * frequency control to follow them - its filters are fixed at 915 Hz and 915 + shift - so this
 * knob is how the host bench asks how much of that error the demodulator survives.
 *
 * @param hz offset in the 12 kHz audio domain, positive = pair moves up
 * @param output_sample_rate the rate Rtty_Modem_Init() was last given
 */
void Rtty_BenchOffsetTx(float hz, uint32_t output_sample_rate)
{
	softdds_setFreqDDS(&rttyDecoderData.tx_dds[0], rttyDecoderData.bpfSpaceConfig->freq + hz, output_sample_rate, 0);
	softdds_setFreqDDS(&rttyDecoderData.tx_dds[1], rttyDecoderData.bpfMarkConfig->freq + hz, output_sample_rate, 0);
}

/**
 * Put the transmitter's space tone at an absolute frequency, leaving mark where it is.
 *
 * Both tones normally follow the receiver's shift setting, which is fine for a loopback but wrong
 * for grading the setting: a receiver set to the wrong shift would send the wrong signal too, and
 * the test could never fail. This lets a signal be sent at one shift while the receiver's space
 * filter sits at another, which is what a station on the air looks like to a radio set wrong.
 *
 * @param hz centre of the space tone in the 12 kHz audio domain
 * @param output_sample_rate the rate Rtty_Modem_Init() was last given
 */
void Rtty_BenchTxSpaceTone(float hz, uint32_t output_sample_rate)
{
	softdds_setFreqDDS(&rttyDecoderData.tx_dds[0], hz, output_sample_rate, 0);
}

/**
 * Stretch or shrink the transmitter's bits, which is what a symbol clock that is off does.
 *
 * The modulator holds a bit for oneBitSampleCount * 4 samples of its 48 kHz output, so a scale
 * below 1 sends shorter bits, i.e. a faster transmitter than the receiver has been told to expect.
 * A receiver set to 50 Bd hearing a 45.45 Bd station is the same knob with scale 45.45/50: the
 * receiver's decimation and DPLL stay on 50 Bd and the bits arrive 10 % long.
 *
 * @param scale bit length multiplier, 1.0 = the receiver's own rate
 */
void Rtty_BenchTxBitScale(float scale)
{
	rtty_tx_bit_scale = scale;
}
#endif

float32_t decayavg(float32_t average, float32_t input, int weight)
{ // adapted from https://github.com/ukhas/dl-fldigi/blob/master/src/include/misc.h
	float32_t retval;
	if (weight <= 1)
	{
		retval = input;
	}
	else
	{
		retval = ( ( input - average ) / (float32_t)weight ) + average ;
	}
	return retval;
}


// this function returns the bit value of the current sample
static int RttyDecoder_demodulator(float32_t sample)
{

	float32_t space_mag = RttyDecoder_bandPassFreq(sample, rttyDecoderData.bpfSpaceConfig, &rttyDecoderData.bpfSpaceData);
	float32_t mark_mag = RttyDecoder_bandPassFreq(sample, rttyDecoderData.bpfMarkConfig, &rttyDecoderData.bpfMarkData);

	float32_t v1 = 0.0;
	// calculating the RMS of the two lines (squaring them)
	space_mag *= space_mag;
	mark_mag *= mark_mag;

    if(rtty_ctrl_config.atc_disable == false)
	{   // RTTY decoding with ATC = automatic threshold correction
		/* Not a bug, and not to be "cleaned up": the mark/space names in the FlDigi ATC formula below
		 * are the opposite of this modem's, so this exchange cancels that. After it, space_mag and
		 * the space envelope carry the 915 Hz MARK filter output and mark_mag and the mark
		 * envelope the space filter output.
		 * A dominant mark tone then drives v1 negative and the function returns 1, a dominant space
		 * tone drives it positive and returns 0 - the same result the non-ATC branch below reaches by
		 * inverting the mark line without any exchange. Dropping this swap would really invert mark
		 * and space; see support/digi/README.md for how both branches were checked against the
		 * modulator, which sends bit 1 at 915 Hz. */
		float32_t helper = space_mag;
		space_mag = mark_mag;
		mark_mag = helper;
		// experiment to implement an ATC (Automatic threshold correction), DD4WH, 2017_08_24
		// everything taken from FlDigi, licensed by GNU GPLv2 or later
		// https://github.com/ukhas/dl-fldigi/blob/master/src/cw_rtty/rtty.cxx
		// calculate envelope of the mark and space signals
		// uses fast attack and slow decay
		rttyDecoderData.mark_env = decayavg (rttyDecoderData.mark_env, mark_mag,
				(mark_mag > rttyDecoderData.mark_env) ? rttyDecoderData.oneBitSampleCount / 4 : rttyDecoderData.oneBitSampleCount * 16);
		rttyDecoderData.space_env = decayavg (rttyDecoderData.space_env, space_mag,
				(space_mag > rttyDecoderData.space_env) ? rttyDecoderData.oneBitSampleCount / 4 : rttyDecoderData.oneBitSampleCount * 16);
		// calculate the noise on the mark and space signals
		rttyDecoderData.mark_noise = decayavg (rttyDecoderData.mark_noise, mark_mag,
				(mark_mag < rttyDecoderData.mark_noise) ? rttyDecoderData.oneBitSampleCount / 4 : rttyDecoderData.oneBitSampleCount * 48);
		rttyDecoderData.space_noise = decayavg (rttyDecoderData.space_noise, space_mag,
				(space_mag < rttyDecoderData.space_noise) ? rttyDecoderData.oneBitSampleCount / 4 : rttyDecoderData.oneBitSampleCount * 48);
		// the noise floor is the lower signal of space and mark noise
		float32_t noise_floor = (rttyDecoderData.space_noise < rttyDecoderData.mark_noise) ? rttyDecoderData.space_noise : rttyDecoderData.mark_noise;

		// Linear ATC, section 3 of www.w7ay.net/site/Technical/ATC
		//		v1 = space_mag - mark_mag - 0.5 * (space_env - mark_env);

		// Compensating for the noise floor by using clipping
		float32_t mclipped = 0.0, sclipped = 0.0;
		mclipped = mark_mag > rttyDecoderData.mark_env ? rttyDecoderData.mark_env : mark_mag;
		sclipped = space_mag > rttyDecoderData.space_env ? rttyDecoderData.space_env : space_mag;
		if (mclipped < noise_floor)
		{
			mclipped = noise_floor;
		}
		if (sclipped < noise_floor)
		{
			sclipped = noise_floor;
		}

		// we could add options for mark-only or space-only decoding
		// however, the current implementation with ATC already works quite well with mark-only/space-only
		/*					switch (progdefaults.rtty_cwi) {
						case 1 : // mark only decode
							rttyDecoderData.space_env = sclipped = noise_floor;
							break;
						case 2: // space only decode
							rttyDecoderData.mark_env = mclipped = noise_floor;
						default : ;
		}
		 */

		// Optimal ATC (Section 6 of of www.w7ay.net/site/Technical/ATC)
		v1  = (mclipped - noise_floor) * (rttyDecoderData.mark_env - noise_floor) -
				(sclipped - noise_floor) * (rttyDecoderData.space_env - noise_floor) -
				0.25 *  ((rttyDecoderData.mark_env - noise_floor) * (rttyDecoderData.mark_env - noise_floor) -
						(rttyDecoderData.space_env - noise_floor) * (rttyDecoderData.space_env - noise_floor));

		v1 = RttyDecoder_lowPass(v1, rttyDecoderData.lpfConfig, &rttyDecoderData.lpfData);

	}
	else
	{   // RTTY without ATC, which works very well too!
		// inverting line 1
		mark_mag *= -1;

		// summing the two lines
		v1 = mark_mag + space_mag;

		// lowpass filtering the summed line
		v1 = RttyDecoder_lowPass(v1, rttyDecoderData.lpfConfig, &rttyDecoderData.lpfData);
	}

	return (v1 > 0)?0:1;
}

// this function returns true once at the half of a bit with the bit's value
static bool RttyDecoder_getBitDPLL(float32_t sample, bool* val_p) {
	bool retval = false;


	if (rttyDecoderData.DPLLBitPhase < rttyDecoderData.oneBitSampleCount)
	{
		*val_p = RttyDecoder_demodulator(sample);

		if (!rttyDecoderData.phaseChanged && *val_p != rttyDecoderData.DPLLOldVal) {
			if (rttyDecoderData.DPLLBitPhase < rttyDecoderData.oneBitSampleCount/2)
			{
				// rttyDecoderData.DPLLBitPhase += rttyDecoderData.oneBitSampleCount/8; // early
				rttyDecoderData.DPLLBitPhase += rttyDecoderData.oneBitSampleCount/32; // early
			}
			else
			{
				//rttyDecoderData.DPLLBitPhase -= rttyDecoderData.oneBitSampleCount/8; // late
				rttyDecoderData.DPLLBitPhase -= rttyDecoderData.oneBitSampleCount/32; // late
			}
			rttyDecoderData.phaseChanged = true;
		}
		rttyDecoderData.DPLLOldVal = *val_p;
		rttyDecoderData.DPLLBitPhase++;
	}

	if (rttyDecoderData.DPLLBitPhase >= rttyDecoderData.oneBitSampleCount)
	{
		rttyDecoderData.DPLLBitPhase -= rttyDecoderData.oneBitSampleCount;
		retval = true;
	}

	return retval;
}

// this function returns only true when the start bit is successfully received
static bool RttyDecoder_waitForStartBit(float32_t sample) {
	bool retval = false;
	int bitResult;

	bitResult = RttyDecoder_demodulator(sample);
	switch (rttyDecoderData.wait_for_start_state)
	{
	case 0:
		// waiting for a falling edge
		if (bitResult != 0)
		{
			rttyDecoderData.wait_for_start_state++;
		}
		break;
	case 1:
		if (bitResult != 1)
		{
			rttyDecoderData.wait_for_start_state++;
		}
		break;
	case 2:
		rttyDecoderData.wait_for_half = rttyDecoderData.oneBitSampleCount/2;
		rttyDecoderData.wait_for_start_state ++;
        /* fall through */ // this is for the compiler, the following comment is for Eclipse
		/* no break */
	case 3:
		rttyDecoderData.wait_for_half--;
		if (rttyDecoderData.wait_for_half == 0)
		{
			retval = (bitResult == 0);
			rttyDecoderData.wait_for_start_state = 0;
		}
		break;
	}
	return retval;
}


void Rtty_Demodulator_ProcessSample(float32_t sample)
{

	switch(rttyDecoderData.state)
	{
	case RTTY_RUN_STATE_WAIT_START: // not synchronized, need to wait for start bit
		if (RttyDecoder_waitForStartBit(sample))
		{
			rttyDecoderData.state = RTTY_RUN_STATE_BIT;
			rttyDecoderData.byteResultp = 1;
			rttyDecoderData.byteResult = 0;
		}
		break;
	case RTTY_RUN_STATE_BIT:
		// reading 7 more bits
		if (rttyDecoderData.byteResultp < 8)
		{
			bool bitResult = false;
			if (RttyDecoder_getBitDPLL(sample, &bitResult))
			{
				switch (rttyDecoderData.byteResultp)
				{
				case 6: // stop bit 1

				case 7: // stop bit 2
				if (bitResult == false)
				{
					// not in sync
					rttyDecoderData.state = RTTY_RUN_STATE_WAIT_START;
				}
				if (rttyDecoderData.config_p->stopbits != RTTY_STOP_2 && rttyDecoderData.byteResultp == 6)
				{
					// we pretend to be at the 7th bit after receiving the first stop bit if we have less than 2 stop bits
					// this omits check for 1.5 bit condition but we should be more or less safe here, may cause
					// a little more unaligned receive but without that shortcut we simply cannot receive these configurations
					// so it is worth it
					rttyDecoderData.byteResultp = 7;
				}

				break;
				default:
					// System.out.print(bitResult);
					rttyDecoderData.byteResult |= (bitResult?1:0) << (rttyDecoderData.byteResultp-1);
				}
				rttyDecoderData.byteResultp++;
			}
		}
		if (rttyDecoderData.byteResultp == 8 && rttyDecoderData.state == RTTY_RUN_STATE_BIT)
		{
			char charResult;

			switch (rttyDecoderData.byteResult) {
			case RTTY_LETTER_CODE:
				rttyDecoderData.charSetMode = RTTY_MODE_LETTERS;
				// System.out.println(" ^L^");
				break;
			case RTTY_SYMBOL_CODE:
				rttyDecoderData.charSetMode = RTTY_MODE_SYMBOLS;
				// System.out.println(" ^F^");
				break;
			default:
				switch (rttyDecoderData.charSetMode)
				{
				case RTTY_MODE_SYMBOLS:
					charResult = RTTYSymbols[rttyDecoderData.byteResult];
					break;
                case RTTY_MODE_LETTERS:
                default:
                    charResult = RTTYLetters[rttyDecoderData.byteResult];
                    break;
				}
				UiDriver_TextMsgPutChar(charResult);
				break;
			}
			rttyDecoderData.state = RTTY_RUN_STATE_WAIT_START;
		}
	}
}

typedef enum
{
	MSK_IDLE = 0,
	MSK_WAIT_FOR_NEG,
	MSK_WAIT_FOR_PLUS,
	MSK_WAIT_FOR_PLUSGRAD, // wait for the values growing towards max
	MSK_WAIT_FOR_MAX, // wait for the value after max, needs to be done one the growing part of the curve
} msk_state_t;

typedef struct
{
	uint8_t char_bit_idx;
	uint16_t char_bits;
	uint32_t char_bit_samples;
	uint16_t last_bit;
	int16_t last_value;
	uint8_t current_bit;
	msk_state_t msk_state;
	rtty_charSetMode_t char_mode;

} rtty_tx_encoder_state_t;

rtty_tx_encoder_state_t  rtty_tx =
{
		.last_bit = 1,
		.last_value = INT16_MIN,
		.char_mode = RTTY_MODE_LETTERS,

};

#define USE_RTTY_MSK
#define RTTY_CODE_MODE_MASK (0b100000)
#define RTTY_CODE_MODE_LETTER (RTTY_CODE_MODE_MASK)
#define RTTY_CODE_MODE_SYMBOL (0)

static void Rtty_BaudotAdd(uint8_t bits)
{
	uint8_t bitCount = rttyDecoderData.config_p->stopbits == RTTY_STOP_1?7:8;

	bits <<= 1; // add stop bit

	if (bitCount == 7)
	{
		bits |= 0b01000000;
	}
	else
	{
		// for 1.5 and 2 we use 2 bits for now
		bits |= 0b11000000;
	}

	rtty_tx.char_bits |= bits << rtty_tx.char_bit_idx;
	// we add more bits towards the MSB if we already have bits

	rtty_tx.char_bit_idx += bitCount;
	// now remember how many bits we added
}

void Rtty_Modulator_Code2Bits(uint8_t baudot_info)
{
	rtty_tx.char_bits = 0;
	rtty_tx.char_bit_idx = 0;

	if ((baudot_info & RTTY_CODE_MODE_MASK) == RTTY_CODE_MODE_LETTER)
	{
		if(rtty_tx.char_mode != RTTY_MODE_LETTERS)
		{
			rtty_tx.char_mode = RTTY_MODE_LETTERS;
			Rtty_BaudotAdd(RTTY_LETTER_CODE);
		}
	}
	else
	{
		if(rtty_tx.char_mode != RTTY_MODE_SYMBOLS)
		{
			rtty_tx.char_mode = RTTY_MODE_SYMBOLS;
			Rtty_BaudotAdd(RTTY_SYMBOL_CODE);
		}
	}
	Rtty_BaudotAdd(baudot_info & ~RTTY_CODE_MODE_MASK);
}

// MUST BE CALLED BEFORE WE START RTTY TX, i.e. in RX Mode !!!
void Rtty_Modulator_StartTX()
{
	rtty_tx.last_bit = 1;
	rtty_tx.last_value = INT16_MIN;
	rtty_tx.char_mode = RTTY_MODE_LETTERS;
	// an over that ended on EOT stopped in the middle of a bit, and its remaining samples used to
	// be handed to the first bit of the next one, shortening it by however far the last one got
	rtty_tx.char_bit_samples = 0;
	Rtty_Modulator_Code2Bits(RTTY_LETTER_CODE);
}

const char rtty_test_string[] = " --- UHSDR FIRMWARE RTTY TX TEST DE DB4PLE --- ";

int16_t Rtty_Modulator_GenSample()
{
	if (rtty_tx.char_bit_samples == 0)
	{
		rtty_tx.char_bit_samples = (uint32_t) roundf((float) (rttyDecoderData.oneBitSampleCount * 4) * rtty_tx_bit_scale);

		rtty_tx.char_bits >>= 1;
		if (rtty_tx.char_bit_idx == 0)
		{
			// load the character and add the stop bits;
			bool bitsFilled = false;

			while ( bitsFilled == false
			        &&  DigiModes_TxBufferHasDataFor(RTTY))
			{

			    uint8_t current_ascii;

			    // as the character might have been removed from the buffer,
			    // we do a final check when removing the character
			    if (DigiModes_TxBufferRemove( &current_ascii, RTTY ))
			    {
			        if (current_ascii == 0x04 ) //EOT
			        {
			            RadioManagement_Request_TxOff();
			        }
			        else
			        {
			            uint8_t current_baudot = Ascii2Baudot[current_ascii & 0x7f];
			            if (current_baudot > 0)
			            { // we have valid baudot code
			                Rtty_Modulator_Code2Bits(current_baudot);
			                bitsFilled = true;
			            }
			        }
			    }
			}

			if (bitsFilled == false)
			{
				// IDLE
				Rtty_Modulator_Code2Bits(RTTY_LETTER_CODE | RTTY_CODE_MODE_LETTER);
				// we ensure that we will switch back to letter mode by making the switch symbol a letter
#if 0
				for (uint8_t idx = 0; idx < sizeof(rtty_test_string); idx++)
				{
					DigiModes_TxBufferPutChar( rtty_test_string[idx] , CW );
				}
#endif
			}
		}
		rtty_tx.char_bit_idx--;

		rtty_tx.current_bit = rtty_tx.char_bits&1;
#ifdef USE_RTTY_MSK
		if (rtty_tx.last_bit != rtty_tx.current_bit)
		{
			if (rtty_tx.last_bit == 1)
			{
				rtty_tx.msk_state = MSK_WAIT_FOR_NEG;
				// WAIT_FOR_NEG
				// WAIT_FOR_ZERO or plus
				rtty_tx.current_bit = rtty_tx.last_bit;
			}
			else
			{
				rtty_tx.msk_state = MSK_WAIT_FOR_PLUSGRAD;
				rtty_tx.current_bit = rtty_tx.last_bit;
				rtty_tx.last_value = INT16_MIN;
				// WAIT_FOR_MAX
			}
		}
#else
		rtty_tx.msk_state = MSK_IDLE;
#endif
	}

	rtty_tx.char_bit_samples--;

	int16_t current_value = softdds_nextSample(&rttyDecoderData.tx_dds[rtty_tx.current_bit]);

	switch(rtty_tx.msk_state)
	{
	case MSK_WAIT_FOR_NEG:
		if (current_value < 0)
		{
			rtty_tx.msk_state = MSK_WAIT_FOR_PLUS;
		}
		break;
	case MSK_WAIT_FOR_PLUS:
		if (current_value >= 0)
		{
			rtty_tx.msk_state = MSK_IDLE;
			rtty_tx.current_bit = rtty_tx.char_bits&1;
			rttyDecoderData.tx_dds[rtty_tx.current_bit].acc = rttyDecoderData.tx_dds[rtty_tx.last_bit].acc;
			rtty_tx.last_bit = rtty_tx.current_bit;
			current_value = softdds_nextSample(&rttyDecoderData.tx_dds[rtty_tx.current_bit]);
		}
		break;
	case MSK_WAIT_FOR_PLUSGRAD:
		if (current_value > rtty_tx.last_value)
		{
			rtty_tx.msk_state = MSK_WAIT_FOR_MAX;
		}
		break;
	case MSK_WAIT_FOR_MAX:
		if (current_value >= rtty_tx.last_value)
		{
			rtty_tx.last_value = current_value;
		}
		else if (current_value < rtty_tx.last_value && rtty_tx.last_value > 0)
		{
			rtty_tx.msk_state = MSK_IDLE;
			rtty_tx.current_bit = rtty_tx.char_bits&1;
			rttyDecoderData.tx_dds[rtty_tx.current_bit].acc = rttyDecoderData.tx_dds[rtty_tx.last_bit].acc;
			rtty_tx.last_bit = rtty_tx.current_bit;
			current_value = softdds_nextSample(&rttyDecoderData.tx_dds[rtty_tx.current_bit]);
		}
		break;
	default:
		break;
	}

	rtty_tx.last_value = current_value;

	return current_value;
}

#endif
