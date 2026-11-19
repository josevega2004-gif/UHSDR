/*  -*-  mode: c; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4; coding: utf-8  -*-  */
/************************************************************************************
 **                                                                                **
 **                                        UHSDR                                   **
 **               a powerful firmware for STM32 based SDR transceivers             **
 **                                                                                **
 **--------------------------------------------------------------------------------**
 **                                                                                **
 **  Description:   Code for automatic gain control based on the WDSP Lib          **
 **  Licence:		GNU GPLv3                                                      **
 ************************************************************************************/

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __AUDIO_AGC_H
#define __AUDIO_AGC_H

#include "uhsdr_board_config.h"
#include "uhsdr_types.h"

// these are the control parameter
// for external runtime configuration
// of the AGC
typedef struct
{
    uint8_t mode;
    uint8_t slope;
    uint8_t hang_enable;
    int     thresh;
    int     hang_thresh;
    int hang_time;
    uint8_t action;
    uint8_t switch_mode;
    uint8_t hang_action;
    int tau_decay[6];
    int tau_hang_decay;
} agc_wdsp_params_t;

// The factory defaults of the WDSP AGC.  Both the configuration table in ui_configuration.c and the
// menu items that offer "restore default" use these, so that a menu reset and a configuration reset
// cannot present the operator with two different defaults for the same parameter any more.
#define AGC_WDSP_MODE_DEFAULT               2       // agcMED
#define AGC_WDSP_SLOPE_DEFAULT              70      // dB
#define AGC_WDSP_HANG_ENABLE_DEFAULT        0
#define AGC_WDSP_THRESH_DEFAULT             20      // dB, the knee the AGC starts acting at
#define AGC_WDSP_HANG_THRESH_DEFAULT        45      // dB
#define AGC_WDSP_HANG_TIME_DEFAULT          500     // ms
#define AGC_WDSP_TAU_HANG_DECAY_DEFAULT     500     // ms
// the decay time constant belonging to each AGC mode, index is agc_wdsp_conf.mode
#define AGC_WDSP_TAU_DECAY_0_DEFAULT        4000    // very LONG
#define AGC_WDSP_TAU_DECAY_1_DEFAULT        2000    // LONG
#define AGC_WDSP_TAU_DECAY_2_DEFAULT        500     // SLOW
#define AGC_WDSP_TAU_DECAY_3_DEFAULT        250     // MED
#define AGC_WDSP_TAU_DECAY_4_DEFAULT        50      // FAST
#define AGC_WDSP_TAU_DECAY_5_DEFAULT        1       // AGC OFF


extern agc_wdsp_params_t agc_wdsp_conf;

#ifdef HOST_BENCH
// the host bench starts every case from a cold AGC; the radio build has no need to, since it
// re-initialises on a decimation change and never runs two signals through one AGC in a row
void AudioAgc_ResetAgcWdsp(void);
#endif
void AudioAgc_RunAgcWdsp(int16_t blockSize, float32_t (*agcbuffer)[AUDIO_BLOCK_SIZE], const bool use_stereo );
void AudioAgc_SetupAgcWdsp(float32_t sample_rate, bool remove_dc);
void AudioAgc_AgcWdsp_Init(void);


#endif
