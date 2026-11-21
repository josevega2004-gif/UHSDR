/*  -*-  mode: c; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4; coding: utf-8  -*-  */
/************************************************************************************
**                                                                                 **
**                               UHSDR FIRMWARE                                    **
**                                                                                 **
**---------------------------------------------------------------------------------**
**  Licence:		GNU GPLv3, see LICENSE.md                                                      **
************************************************************************************/

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __RTTY_H
#define __RTTY_H

#include "uhsdr_types.h"

typedef enum {
    RTTY_STOP_1 = 0,
    RTTY_STOP_1_5,
    RTTY_STOP_2
} rtty_stop_t;


typedef struct
{
    float32_t speed;
    rtty_stop_t stopbits;
    uint16_t shift;
    float32_t samplerate;
} rtty_mode_config_t;



typedef enum {
    RTTY_SPEED_45,
    RTTY_SPEED_50,
    RTTY_SPEED_NUM
} rtty_speed_t;

typedef enum {
    RTTY_SHIFT_85,
    RTTY_SHIFT_170,
	RTTY_SHIFT_200,
    RTTY_SHIFT_425,
	RTTY_SHIFT_450,
	RTTY_SHIFT_850,
    RTTY_SHIFT_NUM
} rtty_shift_t;

typedef struct
{
    rtty_speed_t id;
    float32_t value;
    char* label;
} rtty_speed_item_t;

// TODO: Probably we should define just a few for the various value types and let
// the id be an uint32_t
typedef struct
{
    rtty_shift_t id;
    uint32_t value;
    char* label;
} rtty_shift_item_t;


extern const rtty_speed_item_t rtty_speeds[RTTY_SPEED_NUM];
extern const rtty_shift_item_t rtty_shifts[RTTY_SHIFT_NUM];
extern float32_t decayavg(float32_t average, float32_t input, int weight);

// TODO: maybe this should be placed in the ui or radio management part
typedef struct
{
    rtty_shift_t shift_idx;
    rtty_speed_t speed_idx;
    rtty_stop_t stopbits_idx;
    bool atc_disable; // should the automatic level control be turned off?
}  rtty_ctrl_t;

extern rtty_ctrl_t rtty_ctrl_config;
void Rtty_Modem_Init(uint32_t output_sample_rate);
void Rtty_Demodulator_ProcessSample(float32_t sample);
int16_t Rtty_Modulator_GenSample(void);

#ifdef HOST_BENCH
// the host bench (support/digi) measures the receive filter bank against the frequencies its
// labels claim, and drives the modulator from a receiver that is off frequency; nothing in the
// radio build defines HOST_BENCH
unsigned Rtty_BenchFilterCount(void);
uint16_t Rtty_BenchFilterLabel(unsigned idx);
float    Rtty_BenchFilterMagnitude(unsigned idx, float hz);
void     Rtty_BenchResetDecoder(void);
void     Rtty_BenchOffsetTx(float hz, uint32_t output_sample_rate);
void     Rtty_BenchTxBitScale(float scale);
void     Rtty_BenchTxSpaceTone(float hz, uint32_t output_sample_rate);

// The decoder's own bit decisions. Reconstructing them from the characters it printed cannot work:
// a character is five bits and the report only carries the letter they add up to, so a wrong letter
// says a bit was wrong but never which one, and the decoder drops characters whose stop bit it did
// not see. These are recorded inside the demodulator loop, where the decision is actually taken.
typedef struct
{
	uint32_t at;      // the ProcessSample call the decision was taken on, from the trace start
	uint8_t  bit;     // what the demodulator read
	uint8_t  slot;    // 1..5 data, 6..7 stop, as the receiver numbers them inside a character
} rtty_bench_bit_t;

#define RTTY_BENCH_TRACE_BITS 8192
extern rtty_bench_bit_t Rtty_BenchTraceBits[RTTY_BENCH_TRACE_BITS];
extern int Rtty_BenchTraceCount;
void     Rtty_BenchTraceStart(void);

// the code the transmitter's own table holds for a character, mode bit stripped: this is what the
// station sent, taken from the firmware's Ascii2Baudot instead of from a copy in the bench
uint8_t  Rtty_BenchTxCodeForAscii(char c);
#endif

#endif
