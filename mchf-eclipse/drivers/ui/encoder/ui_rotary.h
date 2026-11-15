/*  -*-  mode: c; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4; coding: utf-8  -*-  */
/************************************************************************************
**                                                                                 **
**                               mcHF QRP Transceiver                              **
**                             K Atanassov - M0NKA 2014                            **
**                                                                                 **
**---------------------------------------------------------------------------------**
**                                                                                 **
**  File name:                                                                     **
**  Description:                                                                   **
**  Last Modified:                                                                 **
**  Licence:		GNU GPLv3                                                      **
************************************************************************************/

#ifndef __UI_ROTARY_H
#define __UI_ROTARY_H

// Some encoders have detend and generate x
// ammount of pulses per click
#define USE_DETENTED_ENCODERS
#define USE_DETENTED_VALUE	4

// Protective band on top and bottom scale
// dependent on encoder quality and debounce
// capacitors
#define ENCODER_FLICKR_BAND	4

// Divider to create non linearity
#define ENCODER_LOG_D	1

// Audio Gain public structure

typedef struct EncoderSelection
{
    // pot values
    //
    uint16_t value_old;			// previous value
    uint16_t value_new;			// most current value
    uint8_t	de_detent;			// sw de-detent flag
    TIM_TypeDef* tim;

} EncoderSelection;

//left only for reference:
/*
typedef struct EncoderSelection
{
    // pot values
    //
    ulong	value_old;			// previous value
    ulong	value_new;			// most current value
    uchar	de_detent;			// sw de-detent flag
    TIM_TypeDef* tim;

} EncoderSelection;
*/

void UiRotaryFreqEncoderInit(void);

void UiRotaryEncoderOneInit(void);
void UiRotaryEncoderTwoInit(void);
void UiRotaryEncoderThreeInit(void);
enum EncoderId
{
    ENC1 = 0,
    ENC2,
    ENC3,
    ENCFREQ,
    ENC_MAX // this needs to be the last entry
};

int UiDriverEncoderRead(const uint32_t encId);

// Encoder diagnostics, see the debug overlay: the raw phase pin states and how often each
// phase changed since the last sampling, plus what the hardware timer decoder made of it.
// A phase that never changes while the other one and the count do, is the failing encoder.
typedef struct
{
    uint8_t  state_ab;          // current levels of phase A (bit 1) and phase B (bit 0)
    uint16_t changes_a;         // phase A edge counter, saturates at 999
    uint16_t changes_b;         // phase B edge counter, saturates at 999
    int16_t  count;             // net steps the timer decoder produced, saturates at +/-999
} UiRotaryDiag_t;

/**
 * @brief sample the encoder phase pins and the timer counters once, for the debug overlay
 * @param diag receives one entry per encoder, may be NULL for an encoder
 * @param max number of entries in diag
 */
void UiRotary_DiagSample(UiRotaryDiag_t* diag, uint8_t max);

#endif
