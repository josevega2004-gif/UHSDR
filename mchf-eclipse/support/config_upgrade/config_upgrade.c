/* generated from the repository headers by gen_config_upgrade.py, do not edit */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define FIRST_UNUSED 416u
#define PREVIOUS_FIRST_UNUSED 383u
#define SAVE_RECORDS_ENTRY_COUNT 1

/* ---------------------------------------------------------------------------------------------
   The address translation of the previous layout, compiled from ui_configuration.c itself and
   compared with the two recorded maps below. A wrong offset in one of its branches corrupts
   every radio that is migrated, and nothing else in the tree would notice. */
#define CONFIG_LAYOUT_OLD_NO_VALUE     0xffff               
#define CONFIG_LAYOUT_NEW_BAND_SLOTS   (MAX_BANDS)          
#define CONFIG_LAYOUT_OLD_BAND_SLOTS   18                   
#define CONFIG_LAYOUT_OLD_VFO_BASE     183                  
#define CONFIG_LAYOUT_OLD_FULL_BASE    115                  
#define BAND_MODE_10 9
#define BAND_MODE_11 8
#define BAND_MODE_160 13
#define BAND_MODE_2200 11
#define BAND_MODE_6 10
#define BAND_MODE_630 12
#define BAND_MODE_8 14
#define BAND_MODE_GEN 15
#define EEPROM_BAND0_5W 97
#define EEPROM_BAND0_FULL 112
#define EEPROM_BAND0_MODE_A 179
#define EEPROM_BAND13_5W 110
#define EEPROM_BAND13_FULL 125
#define EEPROM_BAND_REGION 374
#define MAX_BANDS 15

static uint16_t UiConfiguration_OldBandSlot(uint16_t new_slot)
{
    switch (new_slot)
    {
    case BAND_MODE_10:   return 8;      
    case BAND_MODE_6:    return 9;      
    case BAND_MODE_2200: return 14;
    case BAND_MODE_630:  return 15;
    case BAND_MODE_160:  return 16;
    case BAND_MODE_GEN:  return 17;                         
    case BAND_MODE_11:   return CONFIG_LAYOUT_OLD_NO_VALUE; 
    case BAND_MODE_8:    return CONFIG_LAYOUT_OLD_NO_VALUE; 
    default:             return new_slot;                   
    }
}

static uint16_t UiConfiguration_OldLayoutAddress(uint16_t id)
{
    
    if (id >= EEPROM_BAND0_5W && id <= EEPROM_BAND13_5W)
    {
        uint16_t slot = UiConfiguration_OldBandSlot(id - EEPROM_BAND0_5W);
        return slot == CONFIG_LAYOUT_OLD_NO_VALUE ? slot : (uint16_t)(EEPROM_BAND0_5W + slot);
    }
    if (id >= EEPROM_BAND0_FULL && id <= EEPROM_BAND13_FULL)
    {
        uint16_t slot = UiConfiguration_OldBandSlot(id - EEPROM_BAND0_FULL);
        return slot == CONFIG_LAYOUT_OLD_NO_VALUE ? slot : (uint16_t)(CONFIG_LAYOUT_OLD_FULL_BASE + slot);
    }

    
    if (id >= EEPROM_BAND0_MODE_A && id < EEPROM_BAND0_MODE_A + 6 * CONFIG_LAYOUT_NEW_BAND_SLOTS)
    {
        uint16_t block = (id - EEPROM_BAND0_MODE_A) / CONFIG_LAYOUT_NEW_BAND_SLOTS;
        uint16_t slot = UiConfiguration_OldBandSlot((id - EEPROM_BAND0_MODE_A) % CONFIG_LAYOUT_NEW_BAND_SLOTS);
        if (slot == CONFIG_LAYOUT_OLD_NO_VALUE)
        {
            return CONFIG_LAYOUT_OLD_NO_VALUE;
        }
        return (uint16_t)(CONFIG_LAYOUT_OLD_VFO_BASE + block * CONFIG_LAYOUT_OLD_BAND_SLOTS + slot);
    }

    
    if (id < EEPROM_BAND0_5W)
    {
        return id;                                          
    }
    if (id < 127)
    {
        return id;                                          
    }
    if (id < 160)
    {
        return id + 6;                                      
    }
    if (id < EEPROM_BAND0_MODE_A)
    {
        return id + 4;                                      
    }
    if (id < 295)
    {
        return id + 22;                                     
    }
    if (id < 299)
    {
        return id + 32;
    }
    if (id <= EEPROM_BAND_REGION)
    {
        return id + 57;                                     
    }
    return CONFIG_LAYOUT_OLD_NO_VALUE;
}

typedef enum { OLD_NONE, OLD_ONE, OLD_BLOCK } old_layout_kind_t;
typedef struct { uint16_t id, lo, hi; old_layout_kind_t kind; const char* owner; } old_layout_t;
static const old_layout_t old_layout[] =
{
    { 321, 378, 378, OLD_ONE, "EEPROM_AGC_WDSP_HANG" },
    { 384, 0, 0, OLD_NONE, "EEPROM_AGC_WDSP_HANG_THRESH" },
    { 383, 0, 0, OLD_NONE, "EEPROM_AGC_WDSP_HANG_TIME" },
    { 318, 375, 375, OLD_ONE, "EEPROM_AGC_WDSP_MODE" },
    { 320, 377, 377, OLD_ONE, "EEPROM_AGC_WDSP_SLOPE" },
    { 323, 380, 380, OLD_ONE, "EEPROM_AGC_WDSP_TAU_DECAY_0" },
    { 326, 383, 383, OLD_ONE, "EEPROM_AGC_WDSP_TAU_DECAY_1" },
    { 327, 384, 384, OLD_ONE, "EEPROM_AGC_WDSP_TAU_DECAY_2" },
    { 328, 385, 385, OLD_ONE, "EEPROM_AGC_WDSP_TAU_DECAY_3" },
    { 329, 386, 386, OLD_ONE, "EEPROM_AGC_WDSP_TAU_DECAY_4" },
    { 385, 0, 0, OLD_NONE, "EEPROM_AGC_WDSP_TAU_DECAY_5" },
    { 324, 381, 381, OLD_ONE, "EEPROM_AGC_WDSP_TAU_HANG_DECAY" },
    { 319, 376, 376, OLD_ONE, "EEPROM_AGC_WDSP_THRESH" },
    { 151, 157, 157, OLD_ONE, "EEPROM_ALC_DECAY_TIME" },
    { 152, 158, 158, OLD_ONE, "EEPROM_ALC_POSTFILT_TX_GAIN" },
    { 377, 0, 0, OLD_NONE, "EEPROM_AM_CARRIER_LEVEL" },
    { 146, 152, 152, OLD_ONE, "EEPROM_AM_MODE_DISABLE" },
    { 380, 0, 0, OLD_NONE, "EEPROM_AM_PEP_DIM" },
    { 379, 0, 0, OLD_NONE, "EEPROM_AM_PEP_HOLD" },
    { 381, 0, 0, OLD_NONE, "EEPROM_AM_PEP_TARGET" },
    { 382, 0, 0, OLD_NONE, "EEPROM_AM_PEP_WARN" },
    { 378, 0, 0, OLD_NONE, "EEPROM_AM_TX_SIDEBAND" },
    { 8, 8, 8, OLD_ONE, "EEPROM_AUDIO_GAIN" },
    { 1, 1, 1, OLD_ONE, "EEPROM_BAND_MODE (written outside the table)" },
    { 374, 431, 431, OLD_ONE, "EEPROM_BAND_REGION" },
    { 303, 360, 360, OLD_ONE, "EEPROM_BASS_GAIN" },
    { 287, 309, 309, OLD_ONE, "EEPROM_BEEP_LOUDNESS" },
    { 293, 315, 315, OLD_ONE, "EEPROM_CAT_XLAT" },
    { 376, 0, 0, OLD_NONE, "EEPROM_CONFIG_LAYOUT_VERSION (written outside the table)" },
    { 354, 411, 411, OLD_ONE, "EEPROM_CW_DECODER_BLOCKSIZE" },
    { 347, 404, 404, OLD_ONE, "EEPROM_CW_DECODER_ENABLE" },
    { 373, 430, 430, OLD_ONE, "EEPROM_CW_DECODER_FLAGS" },
    { 353, 410, 410, OLD_ONE, "EEPROM_CW_DECODER_THRESH" },
    { 14, 14, 14, OLD_ONE, "EEPROM_CW_KEYER_MODE" },
    { 13, 13, 13, OLD_ONE, "EEPROM_CW_KEYER_SPEED" },
    { 331, 388, 388, OLD_ONE, "EEPROM_CW_KEYER_WEIGHT" },
    { 169, 173, 173, OLD_ONE, "EEPROM_CW_OFFSET_MODE" },
    { 89, 89, 89, OLD_ONE, "EEPROM_CW_RX_DELAY" },
    { 15, 15, 15, OLD_ONE, "EEPROM_CW_SIDETONE_GAIN" },
    { 322, 379, 379, OLD_ONE, "EEPROM_DBM_CALIBRATE" },
    { 375, 0, 0, OLD_NONE, "EEPROM_DETECTOR_COUPLING_COEFF_11M" },
    { 96, 96, 96, OLD_ONE, "EEPROM_DETECTOR_COUPLING_COEFF_15M" },
    { 289, 311, 311, OLD_ONE, "EEPROM_DETECTOR_COUPLING_COEFF_160M" },
    { 95, 95, 95, OLD_ONE, "EEPROM_DETECTOR_COUPLING_COEFF_20M" },
    { 91, 91, 91, OLD_ONE, "EEPROM_DETECTOR_COUPLING_COEFF_2200M" },
    { 94, 94, 94, OLD_ONE, "EEPROM_DETECTOR_COUPLING_COEFF_40M" },
    { 92, 92, 92, OLD_ONE, "EEPROM_DETECTOR_COUPLING_COEFF_630M" },
    { 290, 312, 312, OLD_ONE, "EEPROM_DETECTOR_COUPLING_COEFF_6M" },
    { 93, 93, 93, OLD_ONE, "EEPROM_DETECTOR_COUPLING_COEFF_80M" },
    { 332, 389, 389, OLD_ONE, "EEPROM_DIGI_MODE_CONF" },
    { 302, 359, 359, OLD_ONE, "EEPROM_DISPLAY_DBM" },
    { 154, 160, 160, OLD_ONE, "EEPROM_DSP_MODE" },
    { 351, 408, 408, OLD_ONE, "EEPROM_DSP_MODE_MASK" },
    { 161, 165, 165, OLD_ONE, "EEPROM_DSP_NOTCH_CONV_RATE" },
    { 160, 164, 164, OLD_ONE, "EEPROM_DSP_NOTCH_DECOR_BUFLEN" },
    { 272, 294, 294, OLD_ONE, "EEPROM_DSP_NOTCH_FFT_NUMTAPS" },
    { 155, 161, 161, OLD_ONE, "EEPROM_DSP_NR_STRENGTH" },
    { 352, 409, 409, OLD_ONE, "EEPROM_ENABLE_PTT_RTS" },
    { 370, 427, 427, OLD_ONE, "EEPROM_EXPFLAGS1" },
    { 277, 299, 299, OLD_ONE, "EEPROM_FILTER_DISP_COLOUR" },
    { 166, 170, 170, OLD_ONE, "EEPROM_FLAGS1" },
    { 276, 298, 298, OLD_ONE, "EEPROM_FLAGS2" },
    { 282, 304, 304, OLD_ONE, "EEPROM_FM_SQUELCH_SETTING" },
    { 285, 307, 307, OLD_ONE, "EEPROM_FM_SUBAUDIBLE_TONE_DET" },
    { 280, 302, 302, OLD_ONE, "EEPROM_FM_SUBAUDIBLE_TONE_GEN" },
    { 281, 303, 303, OLD_ONE, "EEPROM_FM_TONE_BURST_MODE" },
    { 75, 75, 75, OLD_ONE, "EEPROM_FREQ_CAL" },
    { 170, 174, 174, OLD_ONE, "EEPROM_FREQ_CONV_MODE" },
    { 2, 2, 2, OLD_ONE, "EEPROM_FREQ_HIGH (written outside the table)" },
    { 3, 3, 3, OLD_ONE, "EEPROM_FREQ_LOW (written outside the table)" },
    { 4, 4, 4, OLD_ONE, "EEPROM_FREQ_STEP" },
    { 349, 406, 406, OLD_ONE, "EEPROM_Freq_Display_Font" },
    { 312, 369, 369, OLD_ONE, "EEPROM_I2C1_SPEED" },
    { 313, 370, 370, OLD_ONE, "EEPROM_I2C2_SPEED" },
    { 316, 373, 373, OLD_ONE, "EEPROM_IQ_AUTO_CORRECTION" },
    { 286, 308, 308, OLD_ONE, "EEPROM_KEYBOARD_BEEP_FREQ" },
    { 173, 177, 177, OLD_ONE, "EEPROM_LCD_BLANKING_CONFIG" },
    { 315, 372, 372, OLD_ONE, "EEPROM_LINEOUT_GAIN" },
    { 79, 79, 79, OLD_ONE, "EEPROM_LINE_GAIN" },
    { 330, 387, 387, OLD_ONE, "EEPROM_LOW_POWER_CONFIG" },
    { 171, 175, 175, OLD_ONE, "EEPROM_LSB_USB_AUTO_SELECT" },
    { 299, 356, 356, OLD_ONE, "EEPROM_MANUAL_NOTCH" },
    { 300, 357, 357, OLD_ONE, "EEPROM_MANUAL_PEAK" },
    { 10, 10, 10, OLD_ONE, "EEPROM_MAX_VOLUME" },
    { 296, 328, 328, OLD_ONE, "EEPROM_METER_COLOUR_DOWN" },
    { 295, 327, 327, OLD_ONE, "EEPROM_METER_COLOUR_UP" },
    { 150, 156, 156, OLD_ONE, "EEPROM_METER_MODE" },
    { 16, 16, 16, OLD_ONE, "EEPROM_MIC_BOOST" },
    { 78, 78, 78, OLD_ONE, "EEPROM_MIC_GAIN" },
    { 76, 76, 76, OLD_ONE, "EEPROM_NB_SETTING" },
    { 350, 407, 407, OLD_ONE, "EEPROM_NUMBER_OF_ENTRIES" },
    { 88, 88, 88, OLD_ONE, "EEPROM_PADDLE_REVERSE" },
    { 7, 7, 7, OLD_ONE, "EEPROM_PA_BIAS" },
    { 147, 153, 153, OLD_ONE, "EEPROM_PA_CW_BIAS" },
    { 389, 0, 0, OLD_NONE, "EEPROM_PSK_SPEED_IDX" },
    { 386, 0, 0, OLD_NONE, "EEPROM_PWR_METER_DISP" },
    { 325, 382, 382, OLD_ONE, "EEPROM_RTC_CALIB" },
    { 387, 0, 0, OLD_NONE, "EEPROM_RTTY_SHIFT_IDX" },
    { 388, 0, 0, OLD_NONE, "EEPROM_RTTY_SPEED_IDX" },
    { 9, 9, 9, OLD_ONE, "EEPROM_RX_CODEC_GAIN" },
    { 135, 141, 141, OLD_ONE, "EEPROM_RX_IQ_10M_GAIN_BALANCE" },
    { 136, 142, 142, OLD_ONE, "EEPROM_RX_IQ_10M_PHASE_BALANCE" },
    { 19, 19, 19, OLD_ONE, "EEPROM_RX_IQ_80M_GAIN_BALANCE" },
    { 20, 20, 20, OLD_ONE, "EEPROM_RX_IQ_80M_PHASE_BALANCE" },
    { 314, 371, 371, OLD_ONE, "EEPROM_SAM_FADE_LEVELER" },
    { 311, 368, 368, OLD_ONE, "EEPROM_SAM_PLL_BANDWIDTH" },
    { 309, 366, 366, OLD_ONE, "EEPROM_SAM_PLL_LOCKING_RANGE" },
    { 310, 367, 367, OLD_ONE, "EEPROM_SAM_PLL_STEP_RESPONSE" },
    { 137, 143, 143, OLD_ONE, "EEPROM_SENSOR_NULL" },
    { 80, 80, 80, OLD_ONE, "EEPROM_SIDETONE_FREQ" },
    { 355, 412, 412, OLD_ONE, "EEPROM_SMETER_ALPHAS" },
    { 149, 155, 155, OLD_ONE, "EEPROM_SPECTRUM_AGC_RATE" },
    { 90, 90, 90, OLD_ONE, "EEPROM_SPECTRUM_CENTRE_LINE_COLOUR" },
    { 148, 154, 154, OLD_ONE, "EEPROM_SPECTRUM_DB_DIV" },
    { 82, 82, 82, OLD_ONE, "EEPROM_SPECTRUM_FILTER" },
    { 86, 86, 86, OLD_ONE, "EEPROM_SPECTRUM_GRID_COLOUR" },
    { 143, 149, 149, OLD_ONE, "EEPROM_SPECTRUM_MAGNIFY" },
    { 87, 87, 87, OLD_ONE, "EEPROM_SPECTRUM_SCALE_COLOUR" },
    { 273, 295, 295, OLD_ONE, "EEPROM_SPECTRUM_SIZE" },
    { 85, 85, 85, OLD_ONE, "EEPROM_SPECTRUM_TRACE_COLOUR" },
    { 81, 81, 81, OLD_ONE, "EEPROM_SPEC_SCOPE_SPEED" },
    { 153, 159, 159, OLD_ONE, "EEPROM_STEP_SIZE_CONFIG" },
    { 348, 405, 405, OLD_ONE, "EEPROM_Scope_Graticule_Ypos" },
    { 333, 390, 390, OLD_ONE, "EEPROM_Scope_TRACE_HL_BW" },
    { 334, 391, 391, OLD_ONE, "EEPROM_Scope_TRACE_HL_BW_BGR" },
    { 6, 6, 6, OLD_ONE, "EEPROM_TCXO_STATE" },
    { 304, 361, 361, OLD_ONE, "EEPROM_TREBLE_GAIN" },
    { 335, 392, 392, OLD_ONE, "EEPROM_TScal0_High" },
    { 336, 393, 393, OLD_ONE, "EEPROM_TScal0_High" },
    { 337, 394, 394, OLD_ONE, "EEPROM_TScal1_High" },
    { 338, 395, 395, OLD_ONE, "EEPROM_TScal1_High" },
    { 339, 396, 396, OLD_ONE, "EEPROM_TScal2_High" },
    { 340, 397, 397, OLD_ONE, "EEPROM_TScal2_High" },
    { 341, 398, 398, OLD_ONE, "EEPROM_TScal3_High" },
    { 342, 399, 399, OLD_ONE, "EEPROM_TScal3_High" },
    { 343, 400, 400, OLD_ONE, "EEPROM_TScal4_High" },
    { 344, 401, 401, OLD_ONE, "EEPROM_TScal4_High" },
    { 345, 402, 402, OLD_ONE, "EEPROM_TScal5_High" },
    { 346, 403, 403, OLD_ONE, "EEPROM_TScal5_High" },
    { 291, 313, 313, OLD_ONE, "EEPROM_TUNE_POWER_LEVEL" },
    { 275, 297, 297, OLD_ONE, "EEPROM_TXRX_SWITCH_AUDIO_MUTE_DELAY" },
    { 163, 167, 167, OLD_ONE, "EEPROM_TX_AUDIO_COMPRESS" },
    { 5, 5, 5, OLD_ONE, "EEPROM_TX_AUDIO_SRC" },
    { 307, 364, 364, OLD_ONE, "EEPROM_TX_BASS_GAIN" },
    { 165, 169, 169, OLD_ONE, "EEPROM_TX_DISABLE" },
    { 306, 363, 363, OLD_ONE, "EEPROM_TX_FILTER" },
    { 133, 139, 139, OLD_ONE, "EEPROM_TX_IQ_10M_GAIN_BALANCE" },
    { 278, 300, 300, OLD_ONE, "EEPROM_TX_IQ_10M_GAIN_BALANCE_TRANS_OFF" },
    { 134, 140, 140, OLD_ONE, "EEPROM_TX_IQ_10M_PHASE_BALANCE" },
    { 279, 301, 301, OLD_ONE, "EEPROM_TX_IQ_10M_PHASE_BALANCE_TRANS_OFF" },
    { 361, 418, 418, OLD_ONE, "EEPROM_TX_IQ_10M_UP_GAIN_BALANCE" },
    { 367, 424, 424, OLD_ONE, "EEPROM_TX_IQ_10M_UP_GAIN_BALANCE_TRANS_OFF" },
    { 362, 419, 419, OLD_ONE, "EEPROM_TX_IQ_10M_UP_PHASE_BALANCE" },
    { 368, 425, 425, OLD_ONE, "EEPROM_TX_IQ_10M_UP_PHASE_BALANCE_TRANS_OFF" },
    { 359, 416, 416, OLD_ONE, "EEPROM_TX_IQ_15M_GAIN_BALANCE" },
    { 365, 422, 422, OLD_ONE, "EEPROM_TX_IQ_15M_GAIN_BALANCE_TRANS_OFF" },
    { 360, 417, 417, OLD_ONE, "EEPROM_TX_IQ_15M_PHASE_BALANCE" },
    { 366, 423, 423, OLD_ONE, "EEPROM_TX_IQ_15M_PHASE_BALANCE_TRANS_OFF" },
    { 357, 414, 414, OLD_ONE, "EEPROM_TX_IQ_20M_GAIN_BALANCE" },
    { 363, 420, 420, OLD_ONE, "EEPROM_TX_IQ_20M_GAIN_BALANCE_TRANS_OFF" },
    { 358, 415, 415, OLD_ONE, "EEPROM_TX_IQ_20M_PHASE_BALANCE" },
    { 364, 421, 421, OLD_ONE, "EEPROM_TX_IQ_20M_PHASE_BALANCE_TRANS_OFF" },
    { 17, 17, 17, OLD_ONE, "EEPROM_TX_IQ_80M_GAIN_BALANCE" },
    { 164, 168, 168, OLD_ONE, "EEPROM_TX_IQ_80M_GAIN_BALANCE_TRANS_OFF" },
    { 18, 18, 18, OLD_ONE, "EEPROM_TX_IQ_80M_PHASE_BALANCE" },
    { 301, 358, 358, OLD_ONE, "EEPROM_TX_IQ_80M_PHASE_BALANCE_TRANS_OFF" },
    { 12, 12, 12, OLD_ONE, "EEPROM_TX_POWER_LEVEL" },
    { 308, 365, 365, OLD_ONE, "EEPROM_TX_TREBLE_GAIN" },
    { 172, 176, 176, OLD_ONE, "EEPROM_VERSION_MAJOR" },
    { 288, 310, 310, OLD_ONE, "EEPROM_VERSION_MINOR" },
    { 167, 171, 171, OLD_ONE, "EEPROM_VERSION_RELEASE" },
    { 141, 147, 147, OLD_ONE, "EEPROM_VFO_MEM_MODE" },
    { 174, 178, 178, OLD_ONE, "EEPROM_VOLTMETER_CALIBRATE" },
    { 369, 426, 426, OLD_ONE, "EEPROM_VSWR_PROTECTION_THRESHOLD" },
    { 175, 179, 179, OLD_ONE, "EEPROM_WATERFALL_COLOR_SCHEME" },
    { 178, 182, 182, OLD_ONE, "EEPROM_WATERFALL_CONTRAST" },
    { 269, 291, 291, OLD_ONE, "EEPROM_WATERFALL_SPEED" },
    { 176, 180, 180, OLD_ONE, "EEPROM_WATERFALL_VERTICAL_STEP_SIZE" },
    { 139, 145, 145, OLD_ONE, "EEPROM_XVERTER_DISP" },
    { 140, 146, 146, OLD_ONE, "EEPROM_XVERTER_OFFSET_HIGH (written outside the table)" },
    { 142, 148, 148, OLD_ONE, "EEPROM_XVERTER_OFFSET_LOW (written outside the table)" },
    { 371, 428, 428, OLD_ONE, "EEPROM_XVERTER_OFFSET_TX_HIGH (written outside the table)" },
    { 372, 429, 429, OLD_ONE, "EEPROM_XVERTER_OFFSET_TX_LOW (written outside the table)" },
    { 97, 97, 97, OLD_ONE, "EEPROM_BAND0_5W" },
    { 98, 98, 98, OLD_ONE, "EEPROM_BAND1_5W" },
    { 99, 99, 99, OLD_ONE, "EEPROM_BAND2_5W" },
    { 100, 100, 100, OLD_ONE, "EEPROM_BAND3_5W" },
    { 101, 101, 101, OLD_ONE, "EEPROM_BAND4_5W" },
    { 102, 102, 102, OLD_ONE, "EEPROM_BAND5_5W" },
    { 103, 103, 103, OLD_ONE, "EEPROM_BAND6_5W" },
    { 104, 104, 104, OLD_ONE, "EEPROM_BAND7_5W" },
    { 105, 0, 0, OLD_NONE, "EEPROM_BAND8_5W" },
    { 106, 105, 105, OLD_ONE, "EEPROM_BAND9_5W" },
    { 107, 106, 106, OLD_ONE, "EEPROM_BAND10_5W" },
    { 108, 111, 111, OLD_ONE, "EEPROM_BAND11_5W" },
    { 109, 112, 112, OLD_ONE, "EEPROM_BAND12_5W" },
    { 110, 113, 113, OLD_ONE, "EEPROM_BAND13_5W" },
    { 194, 201, 201, OLD_ONE, "EEPROM_BAND0_FREQ_HIGH_A (15 band slots)" },
    { 195, 202, 202, OLD_ONE, "EEPROM_BAND0_FREQ_HIGH_A (15 band slots)" },
    { 196, 203, 203, OLD_ONE, "EEPROM_BAND0_FREQ_HIGH_A (15 band slots)" },
    { 197, 204, 204, OLD_ONE, "EEPROM_BAND0_FREQ_HIGH_A (15 band slots)" },
    { 198, 205, 205, OLD_ONE, "EEPROM_BAND0_FREQ_HIGH_A (15 band slots)" },
    { 199, 206, 206, OLD_ONE, "EEPROM_BAND0_FREQ_HIGH_A (15 band slots)" },
    { 200, 207, 207, OLD_ONE, "EEPROM_BAND0_FREQ_HIGH_A (15 band slots)" },
    { 201, 208, 208, OLD_ONE, "EEPROM_BAND0_FREQ_HIGH_A (15 band slots)" },
    { 202, 0, 0, OLD_NONE, "EEPROM_BAND0_FREQ_HIGH_A (15 band slots)" },
    { 203, 209, 209, OLD_ONE, "EEPROM_BAND0_FREQ_HIGH_A (15 band slots)" },
    { 204, 210, 210, OLD_ONE, "EEPROM_BAND0_FREQ_HIGH_A (15 band slots)" },
    { 205, 215, 215, OLD_ONE, "EEPROM_BAND0_FREQ_HIGH_A (15 band slots)" },
    { 206, 216, 216, OLD_ONE, "EEPROM_BAND0_FREQ_HIGH_A (15 band slots)" },
    { 207, 217, 217, OLD_ONE, "EEPROM_BAND0_FREQ_HIGH_A (15 band slots)" },
    { 208, 0, 0, OLD_NONE, "EEPROM_BAND0_FREQ_HIGH_A (15 band slots)" },
    { 239, 255, 255, OLD_ONE, "EEPROM_BAND0_FREQ_HIGH_B (15 band slots)" },
    { 240, 256, 256, OLD_ONE, "EEPROM_BAND0_FREQ_HIGH_B (15 band slots)" },
    { 241, 257, 257, OLD_ONE, "EEPROM_BAND0_FREQ_HIGH_B (15 band slots)" },
    { 242, 258, 258, OLD_ONE, "EEPROM_BAND0_FREQ_HIGH_B (15 band slots)" },
    { 243, 259, 259, OLD_ONE, "EEPROM_BAND0_FREQ_HIGH_B (15 band slots)" },
    { 244, 260, 260, OLD_ONE, "EEPROM_BAND0_FREQ_HIGH_B (15 band slots)" },
    { 245, 261, 261, OLD_ONE, "EEPROM_BAND0_FREQ_HIGH_B (15 band slots)" },
    { 246, 262, 262, OLD_ONE, "EEPROM_BAND0_FREQ_HIGH_B (15 band slots)" },
    { 247, 0, 0, OLD_NONE, "EEPROM_BAND0_FREQ_HIGH_B (15 band slots)" },
    { 248, 263, 263, OLD_ONE, "EEPROM_BAND0_FREQ_HIGH_B (15 band slots)" },
    { 249, 264, 264, OLD_ONE, "EEPROM_BAND0_FREQ_HIGH_B (15 band slots)" },
    { 250, 269, 269, OLD_ONE, "EEPROM_BAND0_FREQ_HIGH_B (15 band slots)" },
    { 251, 270, 270, OLD_ONE, "EEPROM_BAND0_FREQ_HIGH_B (15 band slots)" },
    { 252, 271, 271, OLD_ONE, "EEPROM_BAND0_FREQ_HIGH_B (15 band slots)" },
    { 253, 0, 0, OLD_NONE, "EEPROM_BAND0_FREQ_HIGH_B (15 band slots)" },
    { 209, 219, 219, OLD_ONE, "EEPROM_BAND0_FREQ_LOW_A (15 band slots)" },
    { 210, 220, 220, OLD_ONE, "EEPROM_BAND0_FREQ_LOW_A (15 band slots)" },
    { 211, 221, 221, OLD_ONE, "EEPROM_BAND0_FREQ_LOW_A (15 band slots)" },
    { 212, 222, 222, OLD_ONE, "EEPROM_BAND0_FREQ_LOW_A (15 band slots)" },
    { 213, 223, 223, OLD_ONE, "EEPROM_BAND0_FREQ_LOW_A (15 band slots)" },
    { 214, 224, 224, OLD_ONE, "EEPROM_BAND0_FREQ_LOW_A (15 band slots)" },
    { 215, 225, 225, OLD_ONE, "EEPROM_BAND0_FREQ_LOW_A (15 band slots)" },
    { 216, 226, 226, OLD_ONE, "EEPROM_BAND0_FREQ_LOW_A (15 band slots)" },
    { 217, 0, 0, OLD_NONE, "EEPROM_BAND0_FREQ_LOW_A (15 band slots)" },
    { 218, 227, 227, OLD_ONE, "EEPROM_BAND0_FREQ_LOW_A (15 band slots)" },
    { 219, 228, 228, OLD_ONE, "EEPROM_BAND0_FREQ_LOW_A (15 band slots)" },
    { 220, 233, 233, OLD_ONE, "EEPROM_BAND0_FREQ_LOW_A (15 band slots)" },
    { 221, 234, 234, OLD_ONE, "EEPROM_BAND0_FREQ_LOW_A (15 band slots)" },
    { 222, 235, 235, OLD_ONE, "EEPROM_BAND0_FREQ_LOW_A (15 band slots)" },
    { 223, 0, 0, OLD_NONE, "EEPROM_BAND0_FREQ_LOW_A (15 band slots)" },
    { 254, 273, 273, OLD_ONE, "EEPROM_BAND0_FREQ_LOW_B (15 band slots)" },
    { 255, 274, 274, OLD_ONE, "EEPROM_BAND0_FREQ_LOW_B (15 band slots)" },
    { 256, 275, 275, OLD_ONE, "EEPROM_BAND0_FREQ_LOW_B (15 band slots)" },
    { 257, 276, 276, OLD_ONE, "EEPROM_BAND0_FREQ_LOW_B (15 band slots)" },
    { 258, 277, 277, OLD_ONE, "EEPROM_BAND0_FREQ_LOW_B (15 band slots)" },
    { 259, 278, 278, OLD_ONE, "EEPROM_BAND0_FREQ_LOW_B (15 band slots)" },
    { 260, 279, 279, OLD_ONE, "EEPROM_BAND0_FREQ_LOW_B (15 band slots)" },
    { 261, 280, 280, OLD_ONE, "EEPROM_BAND0_FREQ_LOW_B (15 band slots)" },
    { 262, 0, 0, OLD_NONE, "EEPROM_BAND0_FREQ_LOW_B (15 band slots)" },
    { 263, 281, 281, OLD_ONE, "EEPROM_BAND0_FREQ_LOW_B (15 band slots)" },
    { 264, 282, 282, OLD_ONE, "EEPROM_BAND0_FREQ_LOW_B (15 band slots)" },
    { 265, 287, 287, OLD_ONE, "EEPROM_BAND0_FREQ_LOW_B (15 band slots)" },
    { 266, 288, 288, OLD_ONE, "EEPROM_BAND0_FREQ_LOW_B (15 band slots)" },
    { 267, 289, 289, OLD_ONE, "EEPROM_BAND0_FREQ_LOW_B (15 band slots)" },
    { 268, 0, 0, OLD_NONE, "EEPROM_BAND0_FREQ_LOW_B (15 band slots)" },
    { 112, 115, 115, OLD_ONE, "EEPROM_BAND0_FULL" },
    { 113, 116, 116, OLD_ONE, "EEPROM_BAND1_FULL" },
    { 114, 117, 117, OLD_ONE, "EEPROM_BAND2_FULL" },
    { 115, 118, 118, OLD_ONE, "EEPROM_BAND3_FULL" },
    { 116, 119, 119, OLD_ONE, "EEPROM_BAND4_FULL" },
    { 117, 120, 120, OLD_ONE, "EEPROM_BAND5_FULL" },
    { 118, 121, 121, OLD_ONE, "EEPROM_BAND6_FULL" },
    { 119, 122, 122, OLD_ONE, "EEPROM_BAND7_FULL" },
    { 120, 0, 0, OLD_NONE, "EEPROM_BAND8_FULL" },
    { 121, 123, 123, OLD_ONE, "EEPROM_BAND9_FULL" },
    { 122, 124, 124, OLD_ONE, "EEPROM_BAND10_FULL" },
    { 123, 129, 129, OLD_ONE, "EEPROM_BAND11_FULL" },
    { 124, 130, 130, OLD_ONE, "EEPROM_BAND12_FULL" },
    { 125, 131, 131, OLD_ONE, "EEPROM_BAND13_FULL" },
    { 179, 183, 183, OLD_ONE, "EEPROM_BAND0_MODE_A (15 band slots)" },
    { 180, 184, 184, OLD_ONE, "EEPROM_BAND0_MODE_A (15 band slots)" },
    { 181, 185, 185, OLD_ONE, "EEPROM_BAND0_MODE_A (15 band slots)" },
    { 182, 186, 186, OLD_ONE, "EEPROM_BAND0_MODE_A (15 band slots)" },
    { 183, 187, 187, OLD_ONE, "EEPROM_BAND0_MODE_A (15 band slots)" },
    { 184, 188, 188, OLD_ONE, "EEPROM_BAND0_MODE_A (15 band slots)" },
    { 185, 189, 189, OLD_ONE, "EEPROM_BAND0_MODE_A (15 band slots)" },
    { 186, 190, 190, OLD_ONE, "EEPROM_BAND0_MODE_A (15 band slots)" },
    { 187, 0, 0, OLD_NONE, "EEPROM_BAND0_MODE_A (15 band slots)" },
    { 188, 191, 191, OLD_ONE, "EEPROM_BAND0_MODE_A (15 band slots)" },
    { 189, 192, 192, OLD_ONE, "EEPROM_BAND0_MODE_A (15 band slots)" },
    { 190, 197, 197, OLD_ONE, "EEPROM_BAND0_MODE_A (15 band slots)" },
    { 191, 198, 198, OLD_ONE, "EEPROM_BAND0_MODE_A (15 band slots)" },
    { 192, 199, 199, OLD_ONE, "EEPROM_BAND0_MODE_A (15 band slots)" },
    { 193, 0, 0, OLD_NONE, "EEPROM_BAND0_MODE_A (15 band slots)" },
    { 224, 237, 237, OLD_ONE, "EEPROM_BAND0_MODE_B (15 band slots)" },
    { 225, 238, 238, OLD_ONE, "EEPROM_BAND0_MODE_B (15 band slots)" },
    { 226, 239, 239, OLD_ONE, "EEPROM_BAND0_MODE_B (15 band slots)" },
    { 227, 240, 240, OLD_ONE, "EEPROM_BAND0_MODE_B (15 band slots)" },
    { 228, 241, 241, OLD_ONE, "EEPROM_BAND0_MODE_B (15 band slots)" },
    { 229, 242, 242, OLD_ONE, "EEPROM_BAND0_MODE_B (15 band slots)" },
    { 230, 243, 243, OLD_ONE, "EEPROM_BAND0_MODE_B (15 band slots)" },
    { 231, 244, 244, OLD_ONE, "EEPROM_BAND0_MODE_B (15 band slots)" },
    { 232, 0, 0, OLD_NONE, "EEPROM_BAND0_MODE_B (15 band slots)" },
    { 233, 245, 245, OLD_ONE, "EEPROM_BAND0_MODE_B (15 band slots)" },
    { 234, 246, 246, OLD_ONE, "EEPROM_BAND0_MODE_B (15 band slots)" },
    { 235, 251, 251, OLD_ONE, "EEPROM_BAND0_MODE_B (15 band slots)" },
    { 236, 252, 252, OLD_ONE, "EEPROM_BAND0_MODE_B (15 band slots)" },
    { 237, 253, 253, OLD_ONE, "EEPROM_BAND0_MODE_B (15 band slots)" },
    { 238, 0, 0, OLD_NONE, "EEPROM_BAND0_MODE_B (15 band slots)" },
    { 391, 329, 353, OLD_BLOCK, "filter path memories (5 modes x 5 slots)" },
    { 392, 329, 353, OLD_BLOCK, "filter path memories (5 modes x 5 slots)" },
    { 393, 329, 353, OLD_BLOCK, "filter path memories (5 modes x 5 slots)" },
    { 394, 329, 353, OLD_BLOCK, "filter path memories (5 modes x 5 slots)" },
    { 395, 329, 353, OLD_BLOCK, "filter path memories (5 modes x 5 slots)" },
    { 396, 329, 353, OLD_BLOCK, "filter path memories (5 modes x 5 slots)" },
    { 397, 329, 353, OLD_BLOCK, "filter path memories (5 modes x 5 slots)" },
    { 398, 329, 353, OLD_BLOCK, "filter path memories (5 modes x 5 slots)" },
    { 399, 329, 353, OLD_BLOCK, "filter path memories (5 modes x 5 slots)" },
    { 400, 329, 353, OLD_BLOCK, "filter path memories (5 modes x 5 slots)" },
    { 401, 329, 353, OLD_BLOCK, "filter path memories (5 modes x 5 slots)" },
    { 402, 329, 353, OLD_BLOCK, "filter path memories (5 modes x 5 slots)" },
    { 403, 329, 353, OLD_BLOCK, "filter path memories (5 modes x 5 slots)" },
    { 404, 329, 353, OLD_BLOCK, "filter path memories (5 modes x 5 slots)" },
    { 405, 329, 353, OLD_BLOCK, "filter path memories (5 modes x 5 slots)" },
    { 406, 329, 353, OLD_BLOCK, "filter path memories (5 modes x 5 slots)" },
    { 407, 329, 353, OLD_BLOCK, "filter path memories (5 modes x 5 slots)" },
    { 408, 329, 353, OLD_BLOCK, "filter path memories (5 modes x 5 slots)" },
    { 409, 329, 353, OLD_BLOCK, "filter path memories (5 modes x 5 slots)" },
    { 410, 329, 353, OLD_BLOCK, "filter path memories (5 modes x 5 slots)" },
    { 411, 329, 353, OLD_BLOCK, "filter path memories (5 modes x 5 slots)" },
    { 412, 329, 353, OLD_BLOCK, "filter path memories (5 modes x 5 slots)" },
    { 413, 329, 353, OLD_BLOCK, "filter path memories (5 modes x 5 slots)" },
    { 414, 329, 353, OLD_BLOCK, "filter path memories (5 modes x 5 slots)" },
    { 415, 329, 353, OLD_BLOCK, "filter path memories (5 modes x 5 slots)" },
};
static const unsigned old_layout_count = sizeof(old_layout)/sizeof(old_layout[0]);

static const char* old_layout_expectation(const old_layout_t* e)
{
    static char txt[32];
    switch (e->kind)
    {
    case OLD_NONE:  return "that the value did not exist";
    case OLD_ONE:   snprintf(txt, sizeof(txt), "word %u", e->lo); return txt;
    default:        snprintf(txt, sizeof(txt), "a word in %u..%u", e->lo, e->hi); return txt;
    }
}

static int check_old_layout_translation(void)
{
    static uint16_t seen[1024];
    unsigned bad = 0, mapped = 0;
    memset(seen, 0, sizeof(seen));
    for (unsigned i = 0; i < old_layout_count; i++)
    {
        const old_layout_t* e = &old_layout[i];
        const uint16_t got = UiConfiguration_OldLayoutAddress(e->id);
        int ok;
        switch (e->kind)
        {
        case OLD_NONE:  ok = (got == CONFIG_LAYOUT_OLD_NO_VALUE); break;
        case OLD_ONE:   ok = (got == e->lo); break;
        default:        ok = (got == CONFIG_LAYOUT_OLD_NO_VALUE) ||
                            (got >= e->lo && got <= e->hi); break;
        }
        if (got != CONFIG_LAYOUT_OLD_NO_VALUE)
        {
            mapped++;
            if (got < sizeof(seen)/sizeof(seen[0]))
            {
                if (seen[got])
                {
                    ok = 0;         /* two values translated onto the same word */
                }
                seen[got] = 1;
            }
        }
        if (!ok)
        {
            bad++;
            if (bad <= 10)
            {
                printf("CHECK FAILED: %s (id %u) translates to ", e->owner, e->id);
                if (got == CONFIG_LAYOUT_OLD_NO_VALUE)
                {
                    printf("no previous address, the two records say %s\n", old_layout_expectation(e));
                }
                else
                {
                    printf("%u, the two records say %s\n", got, old_layout_expectation(e));
                }
            }
        }
    }
    if (bad == 0)
    {
        printf("previous layout translation: %u stored values, %u of them land on a word the "
               "recorded previous map gives them, none twice\n\n", old_layout_count, mapped);
    }
    return bad ? 1 : 0;
}

typedef enum { KIND_PERCENT, KIND_SIDEBAND, KIND_HALF_SECONDS, KIND_ON_OFF, KIND_DB } kind_t;
typedef struct { const char* name; const char* idname; uint16_t id; int16_t def, min, max; kind_t kind; } entry_t;
static const entry_t entries[] = {
    { "carrier level", "EEPROM_AM_CARRIER_LEVEL", 377u, 100, 50, 125, KIND_PERCENT },
    { "AM TX sideband", "EEPROM_AM_TX_SIDEBAND", 378u, 0, 0, 2, KIND_SIDEBAND },
    { "meter hold", "EEPROM_AM_PEP_HOLD", 379u, 4, 0, 20, KIND_HALF_SECONDS },
    { "meter dim", "EEPROM_AM_PEP_DIM", 380u, 1, 0, 1, KIND_ON_OFF },
    { "PEP target", "EEPROM_AM_PEP_TARGET", 381u, 12, 4, 40, KIND_DB },
    { "warn delay", "EEPROM_AM_PEP_WARN", 382u, 6, 0, 20, KIND_HALF_SECONDS },
};
static const unsigned entry_count = sizeof(entries)/sizeof(entries[0]);

typedef enum { STORE_I2C, STORE_FLASH } store_t;
typedef struct { uint16_t value[1024]; bool present[1024]; } image_t;

/* a value an older firmware never stored is simply erased in the image */
static void erase(image_t* img, uint16_t id)
{
    img->value[id] = 0xFFFF;
    img->present[id] = false;
}

/* mirror of UiReadSettingEEPROM_UInt8(): a failed read, an out of range value or a requested
   default loads the default, everything else is used as stored.
   Note: the firmware truncates the stored 16 bit value to uint8 BEFORE the range check, this
   mirror checks the 16 bit value. The two agree as long as the maximum stays below 255, the
   firmware default value is always in range and the erased value is 0xFFFF */
static uint8_t read_uint8(const image_t* img, const entry_t* e, bool load_default, store_t store)
{
    if (store == STORE_FLASH && !img->present[e->id])
    {
        return (uint8_t)e->def;         /* Flash_ReadVariable() reports a value that was never stored */
    }
    const uint16_t value = img->value[e->id];
    if ((value < e->min) || (value > e->max) || load_default)
    {
        return (uint8_t)e->def;
    }
    return (uint8_t)value;
}

/* mirror of UiConfiguration_NeedsDefaultIndex(): values above the stored entry count did not
   exist in the firmware that stored the configuration, on flash the count is not used */
static int32_t needs_default_index(uint16_t stored_count, store_t store)
{
    return (store == STORE_I2C) ? (int32_t)stored_count : INT32_MAX;
}

static const char* describe(const entry_t* e, uint8_t v)
{
    static char txt[24];
    switch (e->kind)
    {
    case KIND_HALF_SECONDS: snprintf(txt, sizeof(txt), v ? "%u.%u s" : "OFF", v / 2, (v % 2) * 5); break;
    case KIND_ON_OFF:       snprintf(txt, sizeof(txt), "%s", v ? "ON" : "OFF"); break;
    case KIND_DB:           snprintf(txt, sizeof(txt), "-%u.%u dB", v / 2, (v % 2) * 5); break;
    case KIND_PERCENT:      snprintf(txt, sizeof(txt), "%u%%", v); break;
    default:                snprintf(txt, sizeof(txt), "%s", v == 0 ? "BOTH" : (v == 1 ? "LSB" : "USB")); break;
    }
    return txt;
}

static void run(const char* title, const image_t* img, uint16_t stored_count, store_t store, bool load_defaults)
{
    const int32_t force_above = needs_default_index(stored_count, store);
    printf("%s\n  stored entry count %u, %s%s\n", title, stored_count,
           store == STORE_I2C ? "serial EEPROM / RAM cache" : "flash",
           load_defaults ? ", factory defaults requested" : "");
    for (unsigned i = 0; i < entry_count; i++)
    {
        const entry_t* e = &entries[i];
        const bool forced = (e->id > force_above);
        const bool present = img->present[e->id];
        const uint16_t raw = img->value[e->id];
        const uint8_t v = read_uint8(img, e, forced || load_defaults, store);
        char stored[16];
        if (present && raw != 0xFFFF) snprintf(stored, sizeof(stored), "%u", raw);
        else                          snprintf(stored, sizeof(stored), "erased");
        const char* why =
            (e->id > force_above)          ? "not in the stored configuration" :
            load_defaults                  ? "defaults requested" :
            (store == STORE_FLASH && !present) ? "never written, read fails" :
            (!present || raw == 0xFFFF)    ? "never written, out of range" :
            (raw < e->min || raw > e->max) ? "stored value out of range" : "loaded from storage";
        printf("    %-16s id %3u  stored %-8s -> %3u  %-8s %s\n",
               e->name, e->id, stored, v, describe(e, v), why);
    }
    printf("\n");
}

/* ---------------------------------------------------------------------------------------------
   the WDSP AGC parameters, parsed from the ConfigEntry table, the AGC menu items and
   agc_wdsp_params_t */
typedef enum { ST_UINT8, ST_INT32_16 } agc_stype_t;
typedef struct
{
    const char* name;
    const char* idname;
    uint16_t id;
    agc_stype_t stype;
    int32_t def, min, max;
    int32_t menu_min, menu_max, menu_def;     /* what the AGC menu offers, menu_max < 0 if not editable */
} agc_entry_t;
static const agc_entry_t agc_entries[] =
{
    { "mode", "EEPROM_AGC_WDSP_MODE", 318, ST_UINT8, 2, 0, 5, 0, 5, 2 },
    { "thresh", "EEPROM_AGC_WDSP_THRESH", 319, ST_INT32_16, 20, -20, 120, -20, 120, 20 },
    { "slope", "EEPROM_AGC_WDSP_SLOPE", 320, ST_UINT8, 70, 0, 200, 0, 200, 70 },
    { "hang_enable", "EEPROM_AGC_WDSP_HANG", 321, ST_UINT8, 0, 0, 1, 0, 1, 0 },
    { "tau_decay[0]", "EEPROM_AGC_WDSP_TAU_DECAY_0", 323, ST_INT32_16, 4000, 50, 5000, 50, 5000, 1000 },
    { "tau_hang_decay", "EEPROM_AGC_WDSP_TAU_HANG_DECAY", 324, ST_INT32_16, 500, 100, 5000, 100, 5000, 500 },
    { "tau_decay[1]", "EEPROM_AGC_WDSP_TAU_DECAY_1", 326, ST_INT32_16, 2000, 50, 5000, 50, 5000, 1000 },
    { "tau_decay[2]", "EEPROM_AGC_WDSP_TAU_DECAY_2", 327, ST_INT32_16, 500, 50, 5000, 50, 5000, 1000 },
    { "tau_decay[3]", "EEPROM_AGC_WDSP_TAU_DECAY_3", 328, ST_INT32_16, 250, 50, 5000, 50, 5000, 1000 },
    { "tau_decay[4]", "EEPROM_AGC_WDSP_TAU_DECAY_4", 329, ST_INT32_16, 50, 50, 5000, 50, 5000, 1000 },
    { "hang_time", "EEPROM_AGC_WDSP_HANG_TIME", 383, ST_INT32_16, 500, 10, 5000, 10, 5000, 500 },
    { "hang_thresh", "EEPROM_AGC_WDSP_HANG_THRESH", 384, ST_INT32_16, 45, -20, 120, -20, 120, 45 },
    { "tau_decay[5]", "EEPROM_AGC_WDSP_TAU_DECAY_5", 385, ST_INT32_16, 1, 1, 5000, 50, 5000, 1000 },
};
static const unsigned agc_entry_count = sizeof(agc_entries)/sizeof(agc_entries[0]);

/* what the write path of UiConfiguration_WriteConfigEntryData() leaves in the 16 bit slot */
static uint16_t agc_store_bits(const agc_entry_t* e, int32_t v)
{
    return e->stype == ST_UINT8 ? (uint16_t)(uint8_t)v : (uint16_t)(int16_t)v;
}

/* what the read path makes out of that slot: both UiReadSettingEEPROM_UInt8() and
   UiReadSettingEEPROM_Int32_16() convert to the type of the ram variable BEFORE the range check */
static int32_t agc_load_bits(const agc_entry_t* e, uint16_t raw)
{
    return e->stype == ST_UINT8 ? (int32_t)(uint8_t)raw : (int32_t)(int16_t)raw;
}

static void agc_save(image_t* img, const agc_entry_t* e, int32_t v)
{
    img->value[e->id] = agc_store_bits(e, v);
    img->present[e->id] = true;
}

static int32_t agc_read(const image_t* img, const agc_entry_t* e, bool load_default, store_t store)
{
    if (store == STORE_FLASH && !img->present[e->id])
    {
        return e->def;                        /* a flash slot that was never written fails to read */
    }
    const int32_t v = agc_load_bits(e, img->value[e->id]);
    if ((v < e->min) || (v > e->max) || load_default)
    {
        return e->def;
    }
    return v;
}

static const char* agc_reason(const image_t* img, const agc_entry_t* e, int32_t force_above, store_t store)
{
    if (e->id > force_above)         return "not in the stored configuration";
    if (store == STORE_FLASH && !img->present[e->id]) return "never written, read fails";
    if (!img->present[e->id])        return "never written";
    const int32_t v = agc_load_bits(e, img->value[e->id]);
    if (v < e->min || v > e->max)    return "stored value out of range";
    return "loaded from storage";
}

static void run_agc(const char* title, const image_t* img, uint16_t stored_count, store_t store)
{
    const int32_t force_above = needs_default_index(stored_count, store);
    printf("%s\n  stored entry count %u, %s\n", title, stored_count,
           store == STORE_I2C ? "serial EEPROM / RAM cache" : "flash");
    for (unsigned i = 0; i < agc_entry_count; i++)
    {
        const agc_entry_t* e = &agc_entries[i];
        const int32_t v = agc_read(img, e, e->id > force_above, store);
        char stored[16];
        if (img->present[e->id]) snprintf(stored, sizeof(stored), "%d", (int)img->value[e->id]);
        else                     snprintf(stored, sizeof(stored), "erased");
        printf("    %-18s id %3u  stored %-8s -> %6d  %s\n", e->name, e->id, stored,
               (int)v, agc_reason(img, e, force_above, store));
    }
    printf("\n");
}

/* every value the AGC menu can produce has to survive a save/load cycle, on the serial EEPROM
   as well as on the flash of the boards that have no external EEPROM */
static int check_upgrade_persistence(void)
{
    int rc = 0;
    for (unsigned i = 0; i < agc_entry_count; i++)
    {
        const agc_entry_t* e = &agc_entries[i];
        if (e->id < PREVIOUS_FIRST_UNUSED)
        {
            continue;                 /* the older firmware knew this one already */
        }
        const int32_t want = e->menu_max >= 0 ? e->menu_max : e->max;
        if (want < e->min || want > e->max || agc_load_bits(e, agc_store_bits(e, want)) != want)
        {
            continue;
        }
        image_t img;
        memset(&img, 0, sizeof(img));
        for (unsigned k = 0; k < sizeof(img.value)/sizeof(img.value[0]); k++)
        {
            img.value[k] = 0xFFFF;
        }
        uint16_t count = PREVIOUS_FIRST_UNUSED;

        /* first boot after the upgrade: the id did not exist, the default is expected */
        if (agc_read(&img, e, e->id > needs_default_index(count, STORE_I2C), STORE_I2C) != e->def)
        {
            printf("CHECK FAILED: %s: a value unknown to the older firmware does not come up as default\n", e->idname);
            rc = 1;
        }

        /* the operator sets it, the firmware saves */
        agc_save(&img, e, want);
        if (SAVE_RECORDS_ENTRY_COUNT)
        {
            count = FIRST_UNUSED;
        }

        /* the next boot has to find it */
        const int32_t got = agc_read(&img, e, e->id > needs_default_index(count, STORE_I2C),
                                      STORE_I2C);
        if (got != want)
        {
            printf("CHECK FAILED: %s (id %u) set after an upgrade is lost on the next boot\n", e->idname, e->id);
            printf("           the stored entry count stayed %u while EEPROM_FIRST_UNUSED is %u,\n",
                   (unsigned)count, FIRST_UNUSED);
            printf("           so the load resets the value back to its default.\n");
            printf("           UiConfiguration_WriteConfigEntries() has to assign\n");
            printf("           number_of_entries_cur_fw = EEPROM_FIRST_UNUSED before saving.\n");
            rc = 1;
        }
    }
    return rc;
}

/* ---------------------------------------------------------------------------------------------
   the EEPROM address map of this build, replayed in the order in which the firmware writes it.
   UiConfiguration_SaveEepromValues() writes the whole ConfigEntry table first and the multi word
   blocks afterwards, so a word that a block covers never keeps the value of the row that claims
   it. That is what destroyed the I2C bus speeds, the WDSP AGC threshold and the receive IQ
   autocorrection: the filter path memories owned their words. Every row has to still own its
   words once the save order above ran through.
*/
typedef struct { uint16_t word; const char* idname; } map_row_t;
static const map_row_t map_rows_rt[] = {
    { 4, "EEPROM_FREQ_STEP" },
    { 5, "EEPROM_TX_AUDIO_SRC" },
    { 6, "EEPROM_TCXO_STATE" },
    { 7, "EEPROM_PA_BIAS" },
    { 8, "EEPROM_AUDIO_GAIN" },
    { 9, "EEPROM_RX_CODEC_GAIN" },
    { 10, "EEPROM_MAX_VOLUME" },
    { 12, "EEPROM_TX_POWER_LEVEL" },
    { 13, "EEPROM_CW_KEYER_SPEED" },
    { 14, "EEPROM_CW_KEYER_MODE" },
    { 15, "EEPROM_CW_SIDETONE_GAIN" },
    { 16, "EEPROM_MIC_BOOST" },
    { 17, "EEPROM_TX_IQ_80M_GAIN_BALANCE" },
    { 18, "EEPROM_TX_IQ_80M_PHASE_BALANCE" },
    { 19, "EEPROM_RX_IQ_80M_GAIN_BALANCE" },
    { 20, "EEPROM_RX_IQ_80M_PHASE_BALANCE" },
    { 75, "EEPROM_FREQ_CAL" },
    { 76, "EEPROM_NB_SETTING" },
    { 78, "EEPROM_MIC_GAIN" },
    { 79, "EEPROM_LINE_GAIN" },
    { 80, "EEPROM_SIDETONE_FREQ" },
    { 81, "EEPROM_SPEC_SCOPE_SPEED" },
    { 82, "EEPROM_SPECTRUM_FILTER" },
    { 85, "EEPROM_SPECTRUM_TRACE_COLOUR" },
    { 86, "EEPROM_SPECTRUM_GRID_COLOUR" },
    { 87, "EEPROM_SPECTRUM_SCALE_COLOUR" },
    { 88, "EEPROM_PADDLE_REVERSE" },
    { 89, "EEPROM_CW_RX_DELAY" },
    { 90, "EEPROM_SPECTRUM_CENTRE_LINE_COLOUR" },
    { 91, "EEPROM_DETECTOR_COUPLING_COEFF_2200M" },
    { 92, "EEPROM_DETECTOR_COUPLING_COEFF_630M" },
    { 93, "EEPROM_DETECTOR_COUPLING_COEFF_80M" },
    { 94, "EEPROM_DETECTOR_COUPLING_COEFF_40M" },
    { 95, "EEPROM_DETECTOR_COUPLING_COEFF_20M" },
    { 96, "EEPROM_DETECTOR_COUPLING_COEFF_15M" },
    { 97, "EEPROM_BAND0_5W" },
    { 98, "EEPROM_BAND1_5W" },
    { 99, "EEPROM_BAND2_5W" },
    { 100, "EEPROM_BAND3_5W" },
    { 101, "EEPROM_BAND4_5W" },
    { 102, "EEPROM_BAND5_5W" },
    { 103, "EEPROM_BAND6_5W" },
    { 104, "EEPROM_BAND7_5W" },
    { 105, "EEPROM_BAND8_5W" },
    { 106, "EEPROM_BAND9_5W" },
    { 107, "EEPROM_BAND10_5W" },
    { 108, "EEPROM_BAND11_5W" },
    { 109, "EEPROM_BAND12_5W" },
    { 110, "EEPROM_BAND13_5W" },
    { 112, "EEPROM_BAND0_FULL" },
    { 113, "EEPROM_BAND1_FULL" },
    { 114, "EEPROM_BAND2_FULL" },
    { 115, "EEPROM_BAND3_FULL" },
    { 116, "EEPROM_BAND4_FULL" },
    { 117, "EEPROM_BAND5_FULL" },
    { 118, "EEPROM_BAND6_FULL" },
    { 119, "EEPROM_BAND7_FULL" },
    { 120, "EEPROM_BAND8_FULL" },
    { 121, "EEPROM_BAND9_FULL" },
    { 122, "EEPROM_BAND10_FULL" },
    { 123, "EEPROM_BAND11_FULL" },
    { 124, "EEPROM_BAND12_FULL" },
    { 125, "EEPROM_BAND13_FULL" },
    { 133, "EEPROM_TX_IQ_10M_GAIN_BALANCE" },
    { 134, "EEPROM_TX_IQ_10M_PHASE_BALANCE" },
    { 135, "EEPROM_RX_IQ_10M_GAIN_BALANCE" },
    { 136, "EEPROM_RX_IQ_10M_PHASE_BALANCE" },
    { 137, "EEPROM_SENSOR_NULL" },
    { 139, "EEPROM_XVERTER_DISP" },
    { 141, "EEPROM_VFO_MEM_MODE" },
    { 143, "EEPROM_SPECTRUM_MAGNIFY" },
    { 146, "EEPROM_AM_MODE_DISABLE" },
    { 147, "EEPROM_PA_CW_BIAS" },
    { 148, "EEPROM_SPECTRUM_DB_DIV" },
    { 149, "EEPROM_SPECTRUM_AGC_RATE" },
    { 150, "EEPROM_METER_MODE" },
    { 151, "EEPROM_ALC_DECAY_TIME" },
    { 152, "EEPROM_ALC_POSTFILT_TX_GAIN" },
    { 153, "EEPROM_STEP_SIZE_CONFIG" },
    { 154, "EEPROM_DSP_MODE" },
    { 155, "EEPROM_DSP_NR_STRENGTH" },
    { 160, "EEPROM_DSP_NOTCH_DECOR_BUFLEN" },
    { 161, "EEPROM_DSP_NOTCH_CONV_RATE" },
    { 163, "EEPROM_TX_AUDIO_COMPRESS" },
    { 164, "EEPROM_TX_IQ_80M_GAIN_BALANCE_TRANS_OFF" },
    { 165, "EEPROM_TX_DISABLE" },
    { 166, "EEPROM_FLAGS1" },
    { 167, "EEPROM_VERSION_RELEASE" },
    { 169, "EEPROM_CW_OFFSET_MODE" },
    { 170, "EEPROM_FREQ_CONV_MODE" },
    { 171, "EEPROM_LSB_USB_AUTO_SELECT" },
    { 172, "EEPROM_VERSION_MAJOR" },
    { 173, "EEPROM_LCD_BLANKING_CONFIG" },
    { 174, "EEPROM_VOLTMETER_CALIBRATE" },
    { 175, "EEPROM_WATERFALL_COLOR_SCHEME" },
    { 176, "EEPROM_WATERFALL_VERTICAL_STEP_SIZE" },
    { 178, "EEPROM_WATERFALL_CONTRAST" },
    { 269, "EEPROM_WATERFALL_SPEED" },
    { 272, "EEPROM_DSP_NOTCH_FFT_NUMTAPS" },
    { 273, "EEPROM_SPECTRUM_SIZE" },
    { 275, "EEPROM_TXRX_SWITCH_AUDIO_MUTE_DELAY" },
    { 276, "EEPROM_FLAGS2" },
    { 277, "EEPROM_FILTER_DISP_COLOUR" },
    { 278, "EEPROM_TX_IQ_10M_GAIN_BALANCE_TRANS_OFF" },
    { 279, "EEPROM_TX_IQ_10M_PHASE_BALANCE_TRANS_OFF" },
    { 280, "EEPROM_FM_SUBAUDIBLE_TONE_GEN" },
    { 281, "EEPROM_FM_TONE_BURST_MODE" },
    { 282, "EEPROM_FM_SQUELCH_SETTING" },
    { 285, "EEPROM_FM_SUBAUDIBLE_TONE_DET" },
    { 286, "EEPROM_KEYBOARD_BEEP_FREQ" },
    { 287, "EEPROM_BEEP_LOUDNESS" },
    { 288, "EEPROM_VERSION_MINOR" },
    { 289, "EEPROM_DETECTOR_COUPLING_COEFF_160M" },
    { 290, "EEPROM_DETECTOR_COUPLING_COEFF_6M" },
    { 291, "EEPROM_TUNE_POWER_LEVEL" },
    { 293, "EEPROM_CAT_XLAT" },
    { 295, "EEPROM_METER_COLOUR_UP" },
    { 296, "EEPROM_METER_COLOUR_DOWN" },
    { 299, "EEPROM_MANUAL_NOTCH" },
    { 300, "EEPROM_MANUAL_PEAK" },
    { 301, "EEPROM_TX_IQ_80M_PHASE_BALANCE_TRANS_OFF" },
    { 302, "EEPROM_DISPLAY_DBM" },
    { 303, "EEPROM_BASS_GAIN" },
    { 304, "EEPROM_TREBLE_GAIN" },
    { 306, "EEPROM_TX_FILTER" },
    { 307, "EEPROM_TX_BASS_GAIN" },
    { 308, "EEPROM_TX_TREBLE_GAIN" },
    { 309, "EEPROM_SAM_PLL_LOCKING_RANGE" },
    { 310, "EEPROM_SAM_PLL_STEP_RESPONSE" },
    { 311, "EEPROM_SAM_PLL_BANDWIDTH" },
    { 312, "EEPROM_I2C1_SPEED" },
    { 313, "EEPROM_I2C2_SPEED" },
    { 314, "EEPROM_SAM_FADE_LEVELER" },
    { 315, "EEPROM_LINEOUT_GAIN" },
    { 316, "EEPROM_IQ_AUTO_CORRECTION" },
    { 318, "EEPROM_AGC_WDSP_MODE" },
    { 319, "EEPROM_AGC_WDSP_THRESH" },
    { 320, "EEPROM_AGC_WDSP_SLOPE" },
    { 321, "EEPROM_AGC_WDSP_HANG" },
    { 322, "EEPROM_DBM_CALIBRATE" },
    { 323, "EEPROM_AGC_WDSP_TAU_DECAY_0" },
    { 324, "EEPROM_AGC_WDSP_TAU_HANG_DECAY" },
    { 325, "EEPROM_RTC_CALIB" },
    { 326, "EEPROM_AGC_WDSP_TAU_DECAY_1" },
    { 327, "EEPROM_AGC_WDSP_TAU_DECAY_2" },
    { 328, "EEPROM_AGC_WDSP_TAU_DECAY_3" },
    { 329, "EEPROM_AGC_WDSP_TAU_DECAY_4" },
    { 330, "EEPROM_LOW_POWER_CONFIG" },
    { 331, "EEPROM_CW_KEYER_WEIGHT" },
    { 332, "EEPROM_DIGI_MODE_CONF" },
    { 333, "EEPROM_Scope_TRACE_HL_BW" },
    { 334, "EEPROM_Scope_TRACE_HL_BW_BGR" },
    { 335, "EEPROM_TScal0_High" },
    { 336, "EEPROM_TScal0_High" },
    { 337, "EEPROM_TScal1_High" },
    { 338, "EEPROM_TScal1_High" },
    { 339, "EEPROM_TScal2_High" },
    { 340, "EEPROM_TScal2_High" },
    { 341, "EEPROM_TScal3_High" },
    { 342, "EEPROM_TScal3_High" },
    { 343, "EEPROM_TScal4_High" },
    { 344, "EEPROM_TScal4_High" },
    { 345, "EEPROM_TScal5_High" },
    { 346, "EEPROM_TScal5_High" },
    { 347, "EEPROM_CW_DECODER_ENABLE" },
    { 348, "EEPROM_Scope_Graticule_Ypos" },
    { 349, "EEPROM_Freq_Display_Font" },
    { 350, "EEPROM_NUMBER_OF_ENTRIES" },
    { 351, "EEPROM_DSP_MODE_MASK" },
    { 352, "EEPROM_ENABLE_PTT_RTS" },
    { 353, "EEPROM_CW_DECODER_THRESH" },
    { 354, "EEPROM_CW_DECODER_BLOCKSIZE" },
    { 355, "EEPROM_SMETER_ALPHAS" },
    { 357, "EEPROM_TX_IQ_20M_GAIN_BALANCE" },
    { 358, "EEPROM_TX_IQ_20M_PHASE_BALANCE" },
    { 359, "EEPROM_TX_IQ_15M_GAIN_BALANCE" },
    { 360, "EEPROM_TX_IQ_15M_PHASE_BALANCE" },
    { 361, "EEPROM_TX_IQ_10M_UP_GAIN_BALANCE" },
    { 362, "EEPROM_TX_IQ_10M_UP_PHASE_BALANCE" },
    { 363, "EEPROM_TX_IQ_20M_GAIN_BALANCE_TRANS_OFF" },
    { 364, "EEPROM_TX_IQ_20M_PHASE_BALANCE_TRANS_OFF" },
    { 365, "EEPROM_TX_IQ_15M_GAIN_BALANCE_TRANS_OFF" },
    { 366, "EEPROM_TX_IQ_15M_PHASE_BALANCE_TRANS_OFF" },
    { 367, "EEPROM_TX_IQ_10M_UP_GAIN_BALANCE_TRANS_OFF" },
    { 368, "EEPROM_TX_IQ_10M_UP_PHASE_BALANCE_TRANS_OFF" },
    { 369, "EEPROM_VSWR_PROTECTION_THRESHOLD" },
    { 370, "EEPROM_EXPFLAGS1" },
    { 373, "EEPROM_CW_DECODER_FLAGS" },
    { 374, "EEPROM_BAND_REGION" },
    { 375, "EEPROM_DETECTOR_COUPLING_COEFF_11M" },
    { 377, "EEPROM_AM_CARRIER_LEVEL" },
    { 378, "EEPROM_AM_TX_SIDEBAND" },
    { 379, "EEPROM_AM_PEP_HOLD" },
    { 380, "EEPROM_AM_PEP_DIM" },
    { 381, "EEPROM_AM_PEP_TARGET" },
    { 382, "EEPROM_AM_PEP_WARN" },
    { 383, "EEPROM_AGC_WDSP_HANG_TIME" },
    { 384, "EEPROM_AGC_WDSP_HANG_THRESH" },
    { 385, "EEPROM_AGC_WDSP_TAU_DECAY_5" },
    { 386, "EEPROM_PWR_METER_DISP" },
    { 387, "EEPROM_RTTY_SHIFT_IDX" },
    { 388, "EEPROM_RTTY_SPEED_IDX" },
    { 389, "EEPROM_PSK_SPEED_IDX" },
};
static const unsigned map_row_count = sizeof(map_rows_rt)/sizeof(map_rows_rt[0]);

typedef struct { uint16_t base; uint16_t words; const char* name; } map_block_t;
static const map_block_t map_blocks_rt[] = {
    { 391, 25, "filter path memories (5 modes x 5 slots)" },
    { 179, 15, "EEPROM_BAND0_MODE_A (15 band slots)" },
    { 194, 15, "EEPROM_BAND0_FREQ_HIGH_A (15 band slots)" },
    { 209, 15, "EEPROM_BAND0_FREQ_LOW_A (15 band slots)" },
    { 224, 15, "EEPROM_BAND0_MODE_B (15 band slots)" },
    { 239, 15, "EEPROM_BAND0_FREQ_HIGH_B (15 band slots)" },
    { 254, 15, "EEPROM_BAND0_FREQ_LOW_B (15 band slots)" },
};
static const unsigned map_block_count = sizeof(map_blocks_rt)/sizeof(map_blocks_rt[0]);

static int check_save_order(void)
{
    int owner[1024];        /* >= 0: index into map_rows_rt, < 0: -(block + 1) */
    for (unsigned i = 0; i < 1024; i++) owner[i] = -1000;
    for (unsigned i = 0; i < map_row_count; i++) owner[map_rows_rt[i].word] = (int)i;
    for (unsigned b = 0; b < map_block_count; b++)
    {
        for (unsigned k = 0; k < map_blocks_rt[b].words; k++)
        {
            owner[map_blocks_rt[b].base + k] = -((int)b + 1);
        }
    }
    int rc = 0;
    int shown = 0;
    for (unsigned i = 0; i < map_row_count; i++)
    {
        const int o = owner[map_rows_rt[i].word];
        if (o == (int)i)
        {
            continue;
        }
        if (shown++ < 10)
        {
            printf("CHECK FAILED: after a save the row %s no longer owns word %u, %s wrote it "
                   "last, the setting is destroyed on every power off\n",
                   map_rows_rt[i].idname, map_rows_rt[i].word,
                   o < 0 ? map_blocks_rt[-o - 1].name : "another table row");
        }
        rc = 1;
    }
    if (rc == 0)
    {
        printf("save order: %d table words and %d multi word blocks, every table row still owns "
               "its words after the save\n\n", map_row_count, map_block_count);
    }
    return rc;
}

/* ---------------------------------------------------------------------------------------------
   the filter path memories: which base a stored configuration is read from.

   UiReadSettingEEPROM_Filter() decides it, and the case that matters is a configuration of the
   previous layout: words 391..415 then held other ids, so the "has this block ever been written"
   probe finds data in every old configuration and cannot be asked first.
*/
#define FILTER_MAP_BASE   391u
#define FILTER_MAP_WORDS  25u
#define FILTER_MAP_LEGACY 297u

/* mirror of UiReadSettingEEPROM_Filter(): the previous layout is handled before the probe, a
   factory reset reads the current block with defaults, otherwise the probe decides */
static uint16_t filter_map_base(const image_t* img, bool migrate, bool load_default)
{
    if (migrate)
    {
        return FILTER_MAP_LEGACY;
    }
    if (load_default)
    {
        return FILTER_MAP_BASE;
    }
    for (unsigned k = 0; k < FILTER_MAP_WORDS; k++)
    {
        if (img->present[FILTER_MAP_BASE + k] && img->value[FILTER_MAP_BASE + k] != 0xFFFF)
        {
            return FILTER_MAP_BASE;
        }
    }
    return FILTER_MAP_LEGACY;
}

static int check_filter_map_migration(void)
{
    image_t previous, moved, current;
    int rc = 0;

    /* a configuration of the previous layout: its filter memories sat at the legacy base, and
       the words of the current block held the ids that now live at 334..358, so they are written
       there too - which is exactly why the probe cannot tell the two layouts apart */
    memset(&previous, 0, sizeof(previous));
    for (unsigned i = 0; i < sizeof(previous.value)/sizeof(previous.value[0]); i++)
    {
        previous.value[i] = 0xFFFF;
    }
    for (unsigned k = 0; k < FILTER_MAP_WORDS; k++)
    {
        previous.present[FILTER_MAP_LEGACY + k] = true;
        previous.value[FILTER_MAP_LEGACY + k] = (uint16_t)(1 + k);   /* the operator memories */
        previous.present[FILTER_MAP_BASE + k] = true;
        previous.value[FILTER_MAP_BASE + k] = (uint16_t)(40 + k);    /* another setting of that layout */
    }

    /* the layout just before this one: block never written, so the probe has to say so */
    moved = previous;
    for (unsigned k = 0; k < FILTER_MAP_WORDS; k++)
    {
        erase(&moved, (uint16_t)(FILTER_MAP_BASE + k));
    }

    /* the current layout: the block holds the operator memories */
    current = previous;
    for (unsigned k = 0; k < FILTER_MAP_WORDS; k++)
    {
        erase(&current, (uint16_t)(FILTER_MAP_LEGACY + k));
        current.value[FILTER_MAP_BASE + k] = (uint16_t)(60 + k);
    }

    const struct { const char* what; const image_t* img; bool migrate; bool load_default; uint16_t want; } cases[] =
    {
        { "configuration of the previous layout",    &previous, true,  false, FILTER_MAP_LEGACY },
        { "layout before this one, block unwritten", &moved,    false, false, FILTER_MAP_LEGACY },
        { "current layout, block written",           &current,  false, false, FILTER_MAP_BASE },
        { "factory reset",                           &current,  false, true,  FILTER_MAP_BASE },
    };

    for (unsigned c = 0; c < sizeof(cases)/sizeof(cases[0]); c++)
    {
        const uint16_t got = filter_map_base(cases[c].img, cases[c].migrate, cases[c].load_default);
        if (got != cases[c].want)
        {
            printf("CHECK FAILED: %s reads the filter path memories from %u, expected %u\n",
                   cases[c].what, got, cases[c].want);
            rc = 1;
        }
    }

    /* and the previous layout has to deliver the operator memories, not the values of the ids
       that took those words later */
    const uint16_t base = filter_map_base(&previous, true, false);
    for (unsigned k = 0; k < FILTER_MAP_WORDS; k++)
    {
        if (previous.value[base + k] != (uint16_t)(1 + k))
        {
            printf("CHECK FAILED: previous layout filter memory %u reads %u from word %u, expected %u\n",
                   k, previous.value[base + k], base + k, 1 + k);
            rc = 1;
            break;
        }
    }

    if (rc == 0)
    {
        printf("filter path memories: previous layout from %u, unwritten block from %u, current from %u, "
               "factory reset from %u\n\n",
               filter_map_base(&previous, true, false), filter_map_base(&moved, false, false),
               filter_map_base(&current, false, false), filter_map_base(&current, false, true));
    }
    return rc;
}

/* ---------------------------------------------------------------------------------------------
   the migration has to finish: once a migrated radio saves, the block sits at the current base,
   the probe finds it and the next boot reads it like any other configuration. A save that writes
   the legacy base, or nothing, leaves the radio taking the legacy branch on every boot.
*/
static void save_filter_memories(image_t* img, const uint8_t* mem)
{
    /* mirror of UiWriteSettingEEPROM_Filter(): every memory goes to the current block */
    for (unsigned k = 0; k < FILTER_MAP_WORDS; k++)
    {
        img->present[FILTER_MAP_BASE + k] = true;
        img->value[FILTER_MAP_BASE + k] = (uint16_t)mem[k];
    }
}

static void load_filter_memories(const image_t* img, bool migrate, uint8_t* mem)
{
    const uint16_t base = filter_map_base(img, migrate, false);
    for (unsigned k = 0; k < FILTER_MAP_WORDS; k++)
    {
        mem[k] = (uint8_t)img->value[base + k];
    }
}

static int check_filter_map_save(void)
{
    image_t before, after;
    uint8_t mem[FILTER_MAP_WORDS];
    int rc = 0;

    /* a migrated radio: the memories sit at the legacy base and the current block was never
       written, which is the state the save has to move it out of */
    memset(&before, 0, sizeof(before));
    for (unsigned i = 0; i < sizeof(before.value)/sizeof(before.value[0]); i++)
    {
        before.value[i] = 0xFFFF;
    }
    for (unsigned k = 0; k < FILTER_MAP_WORDS; k++)
    {
        before.present[FILTER_MAP_LEGACY + k] = true;
        before.value[FILTER_MAP_LEGACY + k] = (uint16_t)(1 + k);   /* the operator memories */
    }
    if (filter_map_base(&before, false, false) != FILTER_MAP_LEGACY)
    {
        printf("CHECK FAILED: a migrated configuration that has not been saved yet is already "
               "read from the current block, so the save below proves nothing\n");
        rc = 1;
    }

    /* the boot that migrates reads the legacy memories, the save puts them at the current base */
    load_filter_memories(&before, true, mem);
    after = before;
    save_filter_memories(&after, mem);
    if (filter_map_base(&after, false, false) != FILTER_MAP_BASE)
    {
        printf("CHECK FAILED: after a migrated configuration is saved the next boot still reads "
               "the filter path memories from %u instead of %u\n",
               filter_map_base(&after, false, false), FILTER_MAP_BASE);
        return 1;
    }
    load_filter_memories(&after, false, mem);
    for (unsigned k = 0; k < FILTER_MAP_WORDS; k++)
    {
        if (mem[k] != (uint8_t)(1 + k))
        {
            printf("CHECK FAILED: after saving a migrated configuration filter memory %u reads %u, "
                   "expected %u\n", k, mem[k], 1 + k);
            return 1;
        }
    }

    if (rc == 0)
    {
        printf("filter path memories: a migrated configuration is saved at %u..%u and the next boot "
               "reads it from there\n\n", FILTER_MAP_BASE, FILTER_MAP_BASE + FILTER_MAP_WORDS - 1);
    }
    return rc;
}

/* ---------------------------------------------------------------------------------------------
   one round trip per row of the configuration table. The read rules are those of the firmware:
   the stored word goes through the type before the range check, which is where a Uint8 row
   loses its high byte and an Int32_16 row gets its sign back
*/
typedef enum { R_UINT8, R_UINT8X2, R_UINT16, R_UINT32_16, R_INT16, R_INT32_16, R_INT32 } rtype_t;
typedef struct { const char* idname; uint16_t id; rtype_t type; uint16_t words; int32_t def, min, max, outside; } rrow_t;

#define R_NO_OUTSIDE INT32_MIN
static const rrow_t rrows[] =
{
    { "EEPROM_SPEC_SCOPE_SPEED", 81u, R_UINT8, 1u, 5, 0, 30, 31 },
    { "EEPROM_FREQ_STEP", 4u, R_UINT32_16, 1u, 3, 0, 9, 10 },
    { "EEPROM_TX_AUDIO_SRC", 5u, R_UINT8, 1u, 0, 0, 4, 5 },
    { "EEPROM_TCXO_STATE", 6u, R_UINT8, 1u, 1, 0, 31, 32 },
    { "EEPROM_AUDIO_GAIN", 8u, R_UINT8, 1u, 16, 0, 30, 31 },
    { "EEPROM_RX_CODEC_GAIN", 9u, R_UINT8, 1u, 9, 0, 9, 10 },
    { "EEPROM_NB_SETTING", 76u, R_UINT8, 1u, 0, 0, 15, 16 },
    { "EEPROM_TX_POWER_LEVEL", 12u, R_UINT8, 1u, 2, 0, 5, 6 },
    { "EEPROM_CW_KEYER_SPEED", 13u, R_UINT8, 1u, 20, 5, 48, 49 },
    { "EEPROM_CW_KEYER_MODE", 14u, R_UINT8, 1u, 0, 0, 3, 4 },
    { "EEPROM_CW_KEYER_WEIGHT", 331u, R_UINT8, 1u, 100, 50, 150, 151 },
    { "EEPROM_CW_SIDETONE_GAIN", 15u, R_UINT8, 1u, 5, 0, 15, 16 },
    { "EEPROM_FREQ_CAL", 75u, R_INT32_16, 1u, 0, -1499, 1499, 1500 },
    { "EEPROM_AGC_WDSP_MODE", 318u, R_UINT8, 1u, 2, 0, 5, 6 },
    { "EEPROM_AGC_WDSP_HANG", 321u, R_UINT8, 1u, 0, 0, 1, 2 },
    { "EEPROM_AGC_WDSP_THRESH", 319u, R_INT32_16, 1u, 20, -20, 120, 121 },
    { "EEPROM_AGC_WDSP_SLOPE", 320u, R_UINT8, 1u, 70, 0, 200, 201 },
    { "EEPROM_AGC_WDSP_TAU_DECAY_0", 323u, R_INT32_16, 1u, 4000, 50, 5000, 5001 },
    { "EEPROM_AGC_WDSP_TAU_DECAY_1", 326u, R_INT32_16, 1u, 2000, 50, 5000, 5001 },
    { "EEPROM_AGC_WDSP_TAU_DECAY_2", 327u, R_INT32_16, 1u, 500, 50, 5000, 5001 },
    { "EEPROM_AGC_WDSP_TAU_DECAY_3", 328u, R_INT32_16, 1u, 250, 50, 5000, 5001 },
    { "EEPROM_AGC_WDSP_TAU_DECAY_4", 329u, R_INT32_16, 1u, 50, 50, 5000, 5001 },
    { "EEPROM_AGC_WDSP_TAU_DECAY_5", 385u, R_INT32_16, 1u, 1, 1, 5000, 5001 },
    { "EEPROM_AGC_WDSP_HANG_TIME", 383u, R_INT32_16, 1u, 500, 10, 5000, 5001 },
    { "EEPROM_AGC_WDSP_HANG_THRESH", 384u, R_INT32_16, 1u, 45, -20, 120, 121 },
    { "EEPROM_AGC_WDSP_TAU_HANG_DECAY", 324u, R_INT32_16, 1u, 500, 100, 5000, 5001 },
    { "EEPROM_MIC_BOOST", 16u, R_INT32_16, 1u, 0, 0, 20, 21 },
    { "EEPROM_AM_CARRIER_LEVEL", 377u, R_UINT8, 1u, 100, 50, 125, 126 },
    { "EEPROM_AM_TX_SIDEBAND", 378u, R_UINT8, 1u, 0, 0, 2, 3 },
    { "EEPROM_AM_PEP_HOLD", 379u, R_UINT8, 1u, 4, 0, 20, 21 },
    { "EEPROM_AM_PEP_DIM", 380u, R_UINT8, 1u, 1, 0, 1, 2 },
    { "EEPROM_AM_PEP_TARGET", 381u, R_UINT8, 1u, 12, 4, 40, 41 },
    { "EEPROM_AM_PEP_WARN", 382u, R_UINT8, 1u, 6, 0, 20, 21 },
    { "EEPROM_MIC_GAIN", 78u, R_UINT8, 1u, 15, 2, 99, 100 },
    { "EEPROM_LINE_GAIN", 79u, R_UINT8, 1u, 12, 3, 31, 32 },
    { "EEPROM_SIDETONE_FREQ", 80u, R_UINT32_16, 1u, 750, 400, 10000, 10001 },
    { "EEPROM_SPECTRUM_FILTER", 82u, R_UINT8, 1u, 4, 1, 20, 21 },
    { "EEPROM_METER_COLOUR_UP", 295u, R_UINT8, 1u, 2, 0, 19, 20 },
    { "EEPROM_METER_COLOUR_DOWN", 296u, R_UINT8, 1u, 8, 0, 19, 20 },
    { "EEPROM_SPECTRUM_TRACE_COLOUR", 85u, R_UINT8, 1u, 0, 0, 19, 20 },
    { "EEPROM_SPECTRUM_GRID_COLOUR", 86u, R_UINT8, 1u, 16, 0, 19, 20 },
    { "EEPROM_SPECTRUM_CENTRE_LINE_COLOUR", 90u, R_UINT8, 1u, 16, 0, 19, 20 },
    { "EEPROM_SPECTRUM_SCALE_COLOUR", 87u, R_UINT8, 1u, 16, 0, 19, 20 },
    { "EEPROM_PADDLE_REVERSE", 88u, R_UINT8, 1u, 0, 0, 1, 2 },
    { "EEPROM_CW_RX_DELAY", 89u, R_UINT8, 1u, 8, 0, 50, 51 },
    { "EEPROM_MAX_VOLUME", 10u, R_UINT8, 1u, 16, 8, 30, 31 },
    { "EEPROM_PA_BIAS", 7u, R_UINT8, 1u, 0, 0, 115, 116 },
    { "EEPROM_PA_CW_BIAS", 147u, R_UINT8, 1u, 0, 0, 115, 116 },
    { "EEPROM_IQ_AUTO_CORRECTION", 316u, R_UINT8, 1u, 1, 0, 1, 2 },
    { "EEPROM_TX_IQ_80M_GAIN_BALANCE", 17u, R_INT32_16, 1u, -128, -128, 127, 128 },
    { "EEPROM_TX_IQ_80M_PHASE_BALANCE", 18u, R_INT32_16, 1u, -128, -128, 127, 128 },
    { "EEPROM_TX_IQ_80M_GAIN_BALANCE_TRANS_OFF", 164u, R_INT32_16, 1u, -128, -128, 127, 128 },
    { "EEPROM_TX_IQ_80M_PHASE_BALANCE_TRANS_OFF", 301u, R_INT32_16, 1u, -128, -128, 127, 128 },
    { "EEPROM_TX_IQ_20M_GAIN_BALANCE", 357u, R_INT32_16, 1u, -128, -128, 127, 128 },
    { "EEPROM_TX_IQ_20M_PHASE_BALANCE", 358u, R_INT32_16, 1u, -128, -128, 127, 128 },
    { "EEPROM_TX_IQ_20M_GAIN_BALANCE_TRANS_OFF", 363u, R_INT32_16, 1u, -128, -128, 127, 128 },
    { "EEPROM_TX_IQ_20M_PHASE_BALANCE_TRANS_OFF", 364u, R_INT32_16, 1u, -128, -128, 127, 128 },
    { "EEPROM_TX_IQ_15M_GAIN_BALANCE", 359u, R_INT32_16, 1u, -128, -128, 127, 128 },
    { "EEPROM_TX_IQ_15M_PHASE_BALANCE", 360u, R_INT32_16, 1u, -128, -128, 127, 128 },
    { "EEPROM_TX_IQ_15M_GAIN_BALANCE_TRANS_OFF", 365u, R_INT32_16, 1u, -128, -128, 127, 128 },
    { "EEPROM_TX_IQ_15M_PHASE_BALANCE_TRANS_OFF", 366u, R_INT32_16, 1u, -128, -128, 127, 128 },
    { "EEPROM_TX_IQ_10M_GAIN_BALANCE", 133u, R_INT32_16, 1u, -128, -128, 127, 128 },
    { "EEPROM_TX_IQ_10M_PHASE_BALANCE", 134u, R_INT32_16, 1u, -128, -128, 127, 128 },
    { "EEPROM_TX_IQ_10M_GAIN_BALANCE_TRANS_OFF", 278u, R_INT32_16, 1u, -128, -128, 127, 128 },
    { "EEPROM_TX_IQ_10M_PHASE_BALANCE_TRANS_OFF", 279u, R_INT32_16, 1u, -128, -128, 127, 128 },
    { "EEPROM_TX_IQ_10M_UP_GAIN_BALANCE", 361u, R_INT32_16, 1u, -128, -128, 127, 128 },
    { "EEPROM_TX_IQ_10M_UP_PHASE_BALANCE", 362u, R_INT32_16, 1u, -128, -128, 127, 128 },
    { "EEPROM_TX_IQ_10M_UP_GAIN_BALANCE_TRANS_OFF", 367u, R_INT32_16, 1u, -128, -128, 127, 128 },
    { "EEPROM_TX_IQ_10M_UP_PHASE_BALANCE_TRANS_OFF", 368u, R_INT32_16, 1u, -128, -128, 127, 128 },
    { "EEPROM_RX_IQ_80M_GAIN_BALANCE", 19u, R_INT32_16, 1u, -128, -128, 127, 128 },
    { "EEPROM_RX_IQ_80M_PHASE_BALANCE", 20u, R_INT32_16, 1u, -128, -128, 127, 128 },
    { "EEPROM_RX_IQ_10M_GAIN_BALANCE", 135u, R_INT32_16, 1u, -128, -128, 127, 128 },
    { "EEPROM_RX_IQ_10M_PHASE_BALANCE", 136u, R_INT32_16, 1u, -128, -128, 127, 128 },
    { "EEPROM_SENSOR_NULL", 137u, R_UINT8, 1u, 100, 75, 150, 151 },
    { "EEPROM_PWR_METER_DISP", 386u, R_UINT8, 1u, 0, 0, 1, 2 },
    { "EEPROM_XVERTER_DISP", 139u, R_UINT32_16, 1u, 0, 0, 10, 11 },
    { "EEPROM_SPECTRUM_MAGNIFY", 143u, R_UINT8, 1u, 0, 0, 5, 6 },
    { "EEPROM_AM_MODE_DISABLE", 146u, R_UINT16, 1u, 1, 0, 7, 8 },
    { "EEPROM_SPECTRUM_DB_DIV", 148u, R_UINT8, 1u, 3, 1, 8, 9 },
    { "EEPROM_SPECTRUM_AGC_RATE", 149u, R_UINT8, 1u, 25, 1, 50, 51 },
    { "EEPROM_METER_MODE", 150u, R_UINT8, 1u, 0, 0, 3, 4 },
    { "EEPROM_ALC_DECAY_TIME", 151u, R_UINT32_16, 1u, 10, 0, 20, 21 },
    { "EEPROM_ALC_POSTFILT_TX_GAIN", 152u, R_UINT32_16, 1u, 1, 1, 25, 26 },
    { "EEPROM_STEP_SIZE_CONFIG", 153u, R_UINT16, 1u, 0, 0, 255, 256 },
    { "EEPROM_DSP_MODE", 154u, R_UINT8, 1u, 0, 0, 255, R_NO_OUTSIDE },
    { "EEPROM_DSP_NR_STRENGTH", 155u, R_UINT8, 1u, 160, 0, 200, 201 },
    { "EEPROM_DSP_NOTCH_DECOR_BUFLEN", 160u, R_UINT8, 1u, 128, 128, 128, 129 },
    { "EEPROM_DSP_NOTCH_FFT_NUMTAPS", 272u, R_UINT8, 1u, 64, 16, 64, 65 },
    { "EEPROM_DSP_NOTCH_CONV_RATE", 161u, R_UINT8, 1u, 10, 0, 40, 41 },
    { "EEPROM_TX_AUDIO_COMPRESS", 163u, R_INT16, 1u, 2, -1, 13, 14 },
    { "EEPROM_TX_DISABLE", 165u, R_UINT8, 1u, 0, 0, 1, 2 },
    { "EEPROM_FLAGS1", 166u, R_UINT16, 1u, 384, 0, 65535, R_NO_OUTSIDE },
    { "EEPROM_FLAGS2", 276u, R_UINT16, 1u, 72, 0, 65535, R_NO_OUTSIDE },
    { "EEPROM_VERSION_MINOR", 288u, R_UINT16, 1u, 0, 0, 255, 256 },
    { "EEPROM_VERSION_RELEASE", 167u, R_UINT16, 1u, 0, 0, 255, 256 },
    { "EEPROM_VERSION_MAJOR", 172u, R_UINT16, 1u, 0, 0, 255, 256 },
    { "EEPROM_CW_OFFSET_MODE", 169u, R_UINT8, 1u, 0, 0, 8, 9 },
    { "EEPROM_FREQ_CONV_MODE", 170u, R_INT32_16, 1u, 4, 0, 4, 5 },
    { "EEPROM_LSB_USB_AUTO_SELECT", 171u, R_UINT8, 1u, 2, 0, 2, 3 },
    { "EEPROM_LCD_BLANKING_CONFIG", 173u, R_UINT8, 1u, 0, 0, 255, R_NO_OUTSIDE },
    { "EEPROM_VFO_MEM_MODE", 141u, R_UINT32_16, 1u, 0, 0, 255, 256 },
    { "EEPROM_DETECTOR_COUPLING_COEFF_2200M", 91u, R_UINT8, 1u, 100, 50, 150, 151 },
    { "EEPROM_DETECTOR_COUPLING_COEFF_630M", 92u, R_UINT8, 1u, 100, 50, 150, 151 },
    { "EEPROM_DETECTOR_COUPLING_COEFF_160M", 289u, R_UINT8, 1u, 100, 50, 150, 151 },
    { "EEPROM_DETECTOR_COUPLING_COEFF_80M", 93u, R_UINT8, 1u, 100, 50, 150, 151 },
    { "EEPROM_DETECTOR_COUPLING_COEFF_40M", 94u, R_UINT8, 1u, 100, 50, 150, 151 },
    { "EEPROM_DETECTOR_COUPLING_COEFF_20M", 95u, R_UINT8, 1u, 100, 50, 150, 151 },
    { "EEPROM_DETECTOR_COUPLING_COEFF_15M", 96u, R_UINT8, 1u, 100, 50, 150, 151 },
    { "EEPROM_DETECTOR_COUPLING_COEFF_6M", 290u, R_UINT8, 1u, 100, 50, 150, 151 },
    { "EEPROM_DETECTOR_COUPLING_COEFF_11M", 375u, R_UINT8, 1u, 100, 50, 150, 151 },
    { "EEPROM_VOLTMETER_CALIBRATE", 174u, R_UINT32_16, 1u, 100, 0, 200, 201 },
    { "EEPROM_LOW_POWER_CONFIG", 330u, R_UINT8, 1u, 60, 0, 254, 255 },
    { "EEPROM_WATERFALL_COLOR_SCHEME", 175u, R_UINT8, 1u, 0, 0, 4, 5 },
    { "EEPROM_WATERFALL_VERTICAL_STEP_SIZE", 176u, R_UINT8, 1u, 2, 1, 5, 6 },
    { "EEPROM_SPECTRUM_SIZE", 273u, R_UINT8, 1u, 0, 0, 1, 2 },
    { "EEPROM_WATERFALL_CONTRAST", 178u, R_UINT32_16, 1u, 120, 10, 225, 226 },
    { "EEPROM_WATERFALL_SPEED", 269u, R_UINT8, 1u, 10, 0, 30, 31 },
    { "EEPROM_TXRX_SWITCH_AUDIO_MUTE_DELAY", 275u, R_UINT8, 1u, 1, 0, 25, 26 },
    { "EEPROM_FILTER_DISP_COLOUR", 277u, R_UINT8, 1u, 0, 0, 19, 20 },
    { "EEPROM_FM_SUBAUDIBLE_TONE_GEN", 280u, R_UINT32_16, 1u, 0, 0, 56, 57 },
    { "EEPROM_FM_SUBAUDIBLE_TONE_DET", 285u, R_UINT32_16, 1u, 0, 0, 56, 57 },
    { "EEPROM_FM_TONE_BURST_MODE", 281u, R_UINT8, 1u, 0, 0, 2, 3 },
    { "EEPROM_FM_SQUELCH_SETTING", 282u, R_UINT8, 1u, 12, 0, 20, 21 },
    { "EEPROM_KEYBOARD_BEEP_FREQ", 286u, R_UINT32_16, 1u, 1000, 200, 3000, 3001 },
    { "EEPROM_BEEP_LOUDNESS", 287u, R_UINT8, 1u, 10, 0, 21, 22 },
    { "EEPROM_TUNE_POWER_LEVEL", 291u, R_UINT8, 1u, 5, 0, 5, 6 },
    { "EEPROM_CAT_XLAT", 293u, R_UINT8, 1u, 1, 0, 1, 2 },
    { "EEPROM_MANUAL_NOTCH", 299u, R_UINT32_16, 1u, 800, 200, 5000, 5001 },
    { "EEPROM_MANUAL_PEAK", 300u, R_UINT32_16, 1u, 750, 200, 5000, 5001 },
    { "EEPROM_DISPLAY_DBM", 302u, R_UINT8, 1u, 0, 0, 2, 3 },
    { "EEPROM_DBM_CALIBRATE", 322u, R_INT32_16, 1u, 0, -100, 100, 101 },
    { "EEPROM_DIGI_MODE_CONF", 332u, R_UINT8, 1u, 1, 1, 2, 3 },
    { "EEPROM_RTTY_SHIFT_IDX", 387u, R_UINT32_16, 1u, 1, 0, 5, 6 },
    { "EEPROM_RTTY_SPEED_IDX", 388u, R_UINT32_16, 1u, 0, 0, 1, 2 },
    { "EEPROM_PSK_SPEED_IDX", 389u, R_UINT32_16, 1u, 0, 0, 2, 3 },
    { "EEPROM_BASS_GAIN", 303u, R_INT32_16, 1u, 2, -20, 20, 21 },
    { "EEPROM_TREBLE_GAIN", 304u, R_INT32_16, 1u, 0, -20, 20, 21 },
    { "EEPROM_TX_FILTER", 306u, R_UINT8, 1u, 0, 0, 3, 4 },
    { "EEPROM_TX_BASS_GAIN", 307u, R_INT32_16, 1u, 4, -20, 6, 7 },
    { "EEPROM_TX_TREBLE_GAIN", 308u, R_INT32_16, 1u, 4, -20, 6, 7 },
    { "EEPROM_SAM_PLL_LOCKING_RANGE", 309u, R_INT32_16, 1u, 2500, 50, 8000, 8001 },
    { "EEPROM_SAM_PLL_STEP_RESPONSE", 310u, R_INT32_16, 1u, 65, 1, 100, 101 },
    { "EEPROM_SAM_PLL_BANDWIDTH", 311u, R_INT32_16, 1u, 250, 15, 1000, 1001 },
    { "EEPROM_I2C1_SPEED", 312u, R_INT32_16, 1u, 5, 1, 20, 21 },
    { "EEPROM_I2C2_SPEED", 313u, R_INT32_16, 1u, 5, 1, 20, 21 },
    { "EEPROM_SAM_FADE_LEVELER", 314u, R_UINT8, 1u, 1, 0, 1, 2 },
    { "EEPROM_LINEOUT_GAIN", 315u, R_UINT8, 1u, 73, 1, 80, 81 },
    { "EEPROM_RTC_CALIB", 325u, R_INT16, 1u, 0, -487, 488, 489 },
    { "EEPROM_CW_DECODER_ENABLE", 347u, R_UINT8, 1u, 1, 0, 1, 2 },
    { "EEPROM_Scope_Graticule_Ypos", 348u, R_UINT16, 1u, 0, 0, 480, 481 },
    { "EEPROM_Freq_Display_Font", 349u, R_UINT8, 1u, 0, 0, 1, 2 },
    { "EEPROM_BAND0_5W", 97u, R_UINT8, 1u, 0, 0, 0, 1 },
    { "EEPROM_BAND1_5W", 98u, R_UINT8, 1u, 0, 0, 0, 1 },
    { "EEPROM_BAND2_5W", 99u, R_UINT8, 1u, 0, 0, 0, 1 },
    { "EEPROM_BAND3_5W", 100u, R_UINT8, 1u, 0, 0, 0, 1 },
    { "EEPROM_BAND4_5W", 101u, R_UINT8, 1u, 0, 0, 0, 1 },
    { "EEPROM_BAND5_5W", 102u, R_UINT8, 1u, 0, 0, 0, 1 },
    { "EEPROM_BAND6_5W", 103u, R_UINT8, 1u, 0, 0, 0, 1 },
    { "EEPROM_BAND7_5W", 104u, R_UINT8, 1u, 0, 0, 0, 1 },
    { "EEPROM_BAND8_5W", 105u, R_UINT8, 1u, 0, 0, 0, 1 },
    { "EEPROM_BAND9_5W", 106u, R_UINT8, 1u, 0, 0, 0, 1 },
    { "EEPROM_BAND10_5W", 107u, R_UINT8, 1u, 0, 0, 0, 1 },
    { "EEPROM_BAND11_5W", 108u, R_UINT8, 1u, 0, 0, 0, 1 },
    { "EEPROM_BAND12_5W", 109u, R_UINT8, 1u, 0, 0, 0, 1 },
    { "EEPROM_BAND13_5W", 110u, R_UINT8, 1u, 0, 0, 0, 1 },
    { "EEPROM_BAND0_FULL", 112u, R_UINT8, 1u, 0, 0, 0, 1 },
    { "EEPROM_BAND1_FULL", 113u, R_UINT8, 1u, 0, 0, 0, 1 },
    { "EEPROM_BAND2_FULL", 114u, R_UINT8, 1u, 0, 0, 0, 1 },
    { "EEPROM_BAND3_FULL", 115u, R_UINT8, 1u, 0, 0, 0, 1 },
    { "EEPROM_BAND4_FULL", 116u, R_UINT8, 1u, 0, 0, 0, 1 },
    { "EEPROM_BAND5_FULL", 117u, R_UINT8, 1u, 0, 0, 0, 1 },
    { "EEPROM_BAND6_FULL", 118u, R_UINT8, 1u, 0, 0, 0, 1 },
    { "EEPROM_BAND7_FULL", 119u, R_UINT8, 1u, 0, 0, 0, 1 },
    { "EEPROM_BAND8_FULL", 120u, R_UINT8, 1u, 0, 0, 0, 1 },
    { "EEPROM_BAND9_FULL", 121u, R_UINT8, 1u, 0, 0, 0, 1 },
    { "EEPROM_BAND10_FULL", 122u, R_UINT8, 1u, 0, 0, 0, 1 },
    { "EEPROM_BAND11_FULL", 123u, R_UINT8, 1u, 0, 0, 0, 1 },
    { "EEPROM_BAND12_FULL", 124u, R_UINT8, 1u, 0, 0, 0, 1 },
    { "EEPROM_BAND13_FULL", 125u, R_UINT8, 1u, 0, 0, 0, 1 },
    { "EEPROM_Scope_TRACE_HL_BW", 333u, R_UINT8, 1u, 0, 0, 19, 20 },
    { "EEPROM_Scope_TRACE_HL_BW_BGR", 334u, R_UINT8, 1u, 20, 0, 100, 101 },
    { "EEPROM_TScal0_High", 335u, R_INT32, 2u, 72816, -2147483648, 2147483647, R_NO_OUTSIDE },
    { "EEPROM_TScal1_High", 337u, R_INT32, 2u, -5, -2147483648, 2147483647, R_NO_OUTSIDE },
    { "EEPROM_TScal2_High", 339u, R_INT32, 2u, -1615424, -2147483648, 2147483647, R_NO_OUTSIDE },
    { "EEPROM_TScal3_High", 341u, R_INT32, 2u, -1, -2147483648, 2147483647, R_NO_OUTSIDE },
    { "EEPROM_TScal4_High", 343u, R_INT32, 2u, 74886, -2147483648, 2147483647, R_NO_OUTSIDE },
    { "EEPROM_TScal5_High", 345u, R_INT32, 2u, -1630326, -2147483648, 2147483647, R_NO_OUTSIDE },
    { "EEPROM_NUMBER_OF_ENTRIES", 350u, R_UINT16, 1u, 416, 416, 416, 417 },
    { "EEPROM_DSP_MODE_MASK", 351u, R_UINT16, 1u, 63, 1, 63, 64 },
    { "EEPROM_ENABLE_PTT_RTS", 352u, R_UINT8, 1u, 0, 0, 1, 2 },
    { "EEPROM_CW_DECODER_THRESH", 353u, R_UINT32_16, 1u, 32000, 1000, 50000, 50001 },
    { "EEPROM_CW_DECODER_BLOCKSIZE", 354u, R_INT32_16, 1u, 88, 8, 128, 129 },
    { "EEPROM_SMETER_ALPHAS", 355u, R_UINT8X2, 1u, 12805, 257, 25700, R_NO_OUTSIDE },
    { "EEPROM_VSWR_PROTECTION_THRESHOLD", 369u, R_UINT8, 1u, 1, 1, 10, 11 },
    { "EEPROM_EXPFLAGS1", 370u, R_UINT16, 1u, 0, 0, 65535, R_NO_OUTSIDE },
    { "EEPROM_CW_DECODER_FLAGS", 373u, R_UINT16, 1u, 49, 0, 32767, 32768 },
    { "EEPROM_BAND_REGION", 374u, R_UINT8, 1u, 0, 0, 127, 128 },
};
static const unsigned rrow_count = sizeof(rrows)/sizeof(rrows[0]);

static void blank_image(image_t* img)
{
    memset(img, 0, sizeof(*img));
    for (unsigned i = 0; i < sizeof(img->value)/sizeof(img->value[0]); i++)
    {
        img->value[i] = 0xFFFF;
    }
}

/* mirror of UiWriteSettingEEPROM_*(): an Int32 row takes two words, high first */
static void row_store(image_t* img, const rrow_t* r, int32_t value)
{
    if (r->type == R_INT32)
    {
        img->present[r->id] = true;
        img->value[r->id] = (uint16_t)(((uint32_t)value) >> 16);      /* high word at the id */
        img->present[r->id + 1] = true;
        img->value[r->id + 1] = (uint16_t)(((uint32_t)value) & 0xFFFFu);  /* low word follows it */
        return;
    }
    img->present[r->id] = true;
    img->value[r->id] = (uint16_t)value;
}

/* mirror of UiReadSettingEEPROM_UInt8/UInt16/UInt32_16/Int16/Int32_16/Int32 */
static int32_t row_read(const image_t* img, const rrow_t* r, bool load_default)
{
    if (!img->present[r->id] || (r->type == R_INT32 && !img->present[r->id + 1]))
    {
        return r->def;                       /* a read that fails loads the default */
    }
    const uint16_t raw = img->value[r->id];
    int32_t v;
    if (r->type == R_UINT8X2)
    {
        /* UiReadSettingEEPROM_UInt8x2() repairs the two bytes separately and never range checks
           the whole word, so the other half of a repaired word has to survive */
        uint16_t hi = (raw >> 8) & 0xFF, lo = raw & 0xFF;
        if (load_default || hi < ((r->min >> 8) & 0xFF) || hi > ((r->max >> 8) & 0xFF))
        {
            hi = (r->def >> 8) & 0xFF;
        }
        if (load_default || lo < (r->min & 0xFF) || lo > (r->max & 0xFF))
        {
            lo = r->def & 0xFF;
        }
        return (int32_t)((hi << 8) | lo);
    }
    switch (r->type)
    {
    case R_UINT8:
        v = raw & 0xFF;                      /* the uint8_t pointer truncates before the check */
        break;
    case R_UINT16:
    case R_UINT32_16:
        v = raw;
        break;
    case R_INT16:
    case R_INT32_16:
        v = (int16_t)raw;                    /* the firmware reads the word as a signed 16 bit */
        break;
    default:                                 /* R_INT32: high word, then low word */
        v = (int32_t)(((uint32_t)raw << 16) | img->value[r->id + 1]);
        break;
    }
    return (v < r->min || v > r->max || load_default) ? r->def : v;
}

static int check_row_roundtrip(void)
{
    static const char* const probe_name[3] = { "minimum", "default", "maximum" };
    int rc = 0;
    unsigned probes = 0;

    for (unsigned i = 0; i < rrow_count; i++)
    {
        const rrow_t* r = &rrows[i];
        const int32_t values[3] = { r->min, r->def, r->max };
        image_t img;

        for (unsigned p = 0; p < 3; p++)
        {
            blank_image(&img);
            row_store(&img, r, values[p]);
            const int32_t got = row_read(&img, r, false);
            if (got != values[p])
            {
                printf("CHECK FAILED: %s (id %u) stored its %s %d and read back %d\n",
                       r->idname, r->id, probe_name[p], values[p], got);
                rc = 1;
            }
            if (row_read(&img, r, true) != r->def)
            {
                printf("CHECK FAILED: %s (id %u) does not load its default %d when it is asked to\n",
                       r->idname, r->id, r->def);
                rc = 1;
            }
            probes += 2;
        }

        if (r->outside != R_NO_OUTSIDE)
        {
            blank_image(&img);
            row_store(&img, r, r->outside);
            const int32_t got = row_read(&img, r, false);
            if (got != r->def)
            {
                printf("CHECK FAILED: %s (id %u) keeps the stored %d, which is outside %d..%d, "
                       "instead of loading the default %d\n",
                       r->idname, r->id, r->outside, r->min, r->max, r->def);
                rc = 1;
            }
            probes++;
        }

        if (r->type == R_UINT8X2)
        {
            const int32_t hi_min = (r->min >> 8) & 0xFF, hi_max = (r->max >> 8) & 0xFF;
            const int32_t lo_min = r->min & 0xFF, lo_max = r->max & 0xFF;
            const int32_t hi_def = (r->def >> 8) & 0xFF, lo_def = r->def & 0xFF;

            /* one byte out of range is repaired on its own, the other byte has to survive */
            if (hi_max < 0xFF)
            {
                const int32_t word = ((hi_max + 1) << 8) | lo_min;
                blank_image(&img);
                img.present[r->id] = true;
                img.value[r->id] = (uint16_t)word;
                const int32_t got = row_read(&img, r, false);
                const int32_t want = (hi_def << 8) | lo_min;
                if (got != want)
                {
                    printf("CHECK FAILED: %s (id %u) reads 0x%04X as 0x%04X, the high byte is "
                           "repaired on its own, so it has to read 0x%04X\n",
                           r->idname, r->id, word, got, want);
                    rc = 1;
                }
                probes++;
            }
            if (lo_max < 0xFF)
            {
                const int32_t word = (hi_min << 8) | (lo_max + 1);
                blank_image(&img);
                img.present[r->id] = true;
                img.value[r->id] = (uint16_t)word;
                const int32_t got = row_read(&img, r, false);
                const int32_t want = (hi_min << 8) | lo_def;
                if (got != want)
                {
                    printf("CHECK FAILED: %s (id %u) reads 0x%04X as 0x%04X, the low byte is "
                           "repaired on its own, so it has to read 0x%04X\n",
                           r->idname, r->id, word, got, want);
                    rc = 1;
                }
                probes++;
            }

            /* an erased word has to leave both bytes inside their range */
            blank_image(&img);
            row_store(&img, r, -1);
            img.value[r->id] = 0xFFFF;
            {
                const int32_t erased = row_read(&img, r, false);
                if (((erased >> 8) & 0xFF) < hi_min || ((erased >> 8) & 0xFF) > hi_max ||
                    (erased & 0xFF) < lo_min || (erased & 0xFF) > lo_max)
                {
                    printf("CHECK FAILED: %s (id %u) reads an erased word as 0x%04X, outside "
                           "%02X..%02X (high byte) or %02X..%02X (low byte)\n",
                           r->idname, r->id, erased, hi_min, hi_max, lo_min, lo_max);
                    rc = 1;
                }
                probes++;
            }
        }
        else
        {
            /* an erased word must never come back as a value outside the range */
            blank_image(&img);
            row_store(&img, r, -1);
            img.value[r->id] = 0xFFFF;
            if (r->type == R_INT32)
            {
                img.value[r->id + 1] = 0xFFFF;
            }
            {
                const int32_t erased = row_read(&img, r, false);
                if (erased < r->min || erased > r->max)
                {
                    printf("CHECK FAILED: %s (id %u) reads an erased word as %d, outside %d..%d\n",
                           r->idname, r->id, erased, r->min, r->max);
                    rc = 1;
                }
                probes++;
            }
        }

        /* the truncation case: a Uint8 row whose range fits a byte accepts 0x0100 | value */
        if (r->type == R_UINT8 && r->min >= 0 && r->max <= 0xFF)
        {
            const int32_t v = r->max;
            const uint16_t word = (uint16_t)(0x0100 | (v & 0xFF));
            blank_image(&img);
            img.present[r->id] = true;
            img.value[r->id] = word;
            {
                const int32_t got = row_read(&img, r, false);
                if (got != v)
                {
                    printf("CHECK FAILED: %s (id %u) reads 0x%04X as %d, a Uint8 setting keeps "
                           "only the low byte, so it has to read %d\n", r->idname, r->id, word, got, v);
                    rc = 1;
                }
            }
            probes++;
        }
    }

    if (rc == 0)
    {
        printf("round trip: %u probes over %u stored rows, every value comes back the way it was "
               "written, and a value outside the range falls back to the default\n\n",
               probes, rrow_count);
    }
    return rc;
}

/* ---------------------------------------------------------------------------------------------
   the migration lifecycle: one save turns a previous layout configuration into a current one,
   and the marker that save writes stops the next boot from translating it once more */
#define MARKER_WORD 376u
#define LAYOUT_MAGIC 0x5AA5u

/* mirror of UiConfiguration_LayoutMigrationRequired(): only the current magic means the words
   were stored in this layout already */
static bool needs_migration(const image_t* img)
{
    return !(img->present[MARKER_WORD] && img->value[MARKER_WORD] == LAYOUT_MAGIC);
}

/* the save of a migrated configuration: every value read through the translation is written
   at its current word, and the marker is written last */
static void save_as_current(image_t* img)
{
    const image_t before = *img;
    for (unsigned w = 0; w < FIRST_UNUSED; w++)
    {
        const uint16_t old = UiConfiguration_OldLayoutAddress((uint16_t)w);
        if (old != CONFIG_LAYOUT_OLD_NO_VALUE && old < 1024)
        {
            img->value[w] = before.value[old];
            img->present[w] = before.present[old];
        }
    }
    img->value[MARKER_WORD] = LAYOUT_MAGIC;
    img->present[MARKER_WORD] = true;
}

static int check_migration_lifecycle(void)
{
    unsigned probed = 0;
    for (unsigned i = 0; i < old_layout_count; i++)
    {
        const old_layout_t* e = &old_layout[i];
        image_t img;
        if (e->kind != OLD_ONE)
        {
            continue;           /* a block word needs its band, the translation check has it */
        }
        blank_image(&img);
        img.value[e->lo] = 0x1234;
        img.present[e->lo] = true;
        if (!needs_migration(&img))
        {
            printf("CHECK FAILED: a configuration without the layout marker is not migrated, "
                   "its words are read as if they were current\n");
            return 1;
        }
        save_as_current(&img);
        if (needs_migration(&img))
        {
            printf("CHECK FAILED: the save did not mark the configuration as current, so the "
                   "next boot translates %s (id %u) again\n", e->owner, e->id);
            return 1;
        }
        if (!img.present[e->id] || img.value[e->id] != 0x1234)
        {
            printf("CHECK FAILED: %s (id %u) was stored at word %u in the previous layout and "
                   "did not arrive at its current word after the save\n",
                   e->owner, e->id, e->lo);
            return 1;
        }
        probed++;
    }
    printf("migration lifecycle: %u values stored in the previous layout are read at their "
           "current word after one save, and the marker that save writes stops the next boot "
           "from translating them again\n\n", probed);
    return 0;
}

int main(void)
{
    image_t img;
    memset(&img, 0, sizeof(img));
    for (unsigned i = 0; i < sizeof(img.value)/sizeof(img.value[0]); i++)
    {
        img.value[i] = 0xFFFF;          /* erased storage reads as 0xFFFF */
    }
    img.present[377] = true; img.value[377] = 80;   /* EEPROM_AM_CARRIER_LEVEL */
    img.present[378] = true; img.value[378] = 2;   /* EEPROM_AM_TX_SIDEBAND */
    img.present[379] = true; img.value[379] = 10;   /* EEPROM_AM_PEP_HOLD */
    img.present[380] = true; img.value[380] = 0;   /* EEPROM_AM_PEP_DIM */
    img.present[381] = true; img.value[381] = 8;   /* EEPROM_AM_PEP_TARGET */
    img.present[382] = true; img.value[382] = 2;   /* EEPROM_AM_PEP_WARN */

    image_t before_meter      = img;   /* stored by the build without the meter settings */
    erase(&before_meter, 379);   /* am_carrier_pep_hold */
    erase(&before_meter, 380);   /* am_carrier_pep_dim */
    erase(&before_meter, 381);   /* am_carrier_pep_target */
    erase(&before_meter, 382);   /* am_carrier_pep_warn */
    image_t before_warn       = img;   /* stored by the build without the warning delay */
    erase(&before_warn, 382);   /* am_carrier_pep_warn */
    image_t upstream          = before_meter;  /* no AM additions at all */
    erase(&upstream, 377);   /* am_carrier_level */
    erase(&upstream, 378);   /* am_tx_sideband */

    run("early build of the current layout, none of the AM additions present", &upstream, 375u, STORE_I2C, false);
    run("fork build before the meter settings", &before_meter, 379, STORE_I2C, false);
    run("fork build before the warning delay", &before_warn, 382, STORE_I2C, false);
    run("current build, everything stored", &img, FIRST_UNUSED, STORE_I2C, false);
    run("current build on flash, warning delay never written", &before_warn, FIRST_UNUSED, STORE_FLASH, false);
    run("factory reset of the current build", &img, FIRST_UNUSED, STORE_I2C, true);

    /* ---------------------------------------------------------------------------------------------
       the AGC values */
    image_t agc_all;
    memset(&agc_all, 0, sizeof(agc_all));
    for (unsigned i = 0; i < sizeof(agc_all.value)/sizeof(agc_all.value[0]); i++)
    {
        agc_all.value[i] = 0xFFFF;
    }
    for (unsigned i = 0; i < agc_entry_count; i++)
    {
        const agc_entry_t* e = &agc_entries[i];
        /* a stored value that differs from the default, so that the check below can tell the
           two apart: the highest value of the range the menu offers */
        agc_save(&agc_all, e, e->menu_max >= 0 ? e->menu_max : e->max);
    }
    image_t agc_before_hang_values = agc_all;
    erase(&agc_before_hang_values, 383);   /* hang_time */
    erase(&agc_before_hang_values, 384);   /* hang_thresh */
    erase(&agc_before_hang_values, 385);   /* tau_decay[5] */
    run_agc("AGC, configuration stored before the hang and OFF mode values existed",
            &agc_before_hang_values, 383u, STORE_I2C);
    run_agc("AGC, current build, everything stored", &agc_all, FIRST_UNUSED, STORE_I2C);
    run_agc("AGC, current build on flash", &agc_all, FIRST_UNUSED, STORE_FLASH);

    /* the self checks which make this program fail, so that CI catches a broken configuration */
    int rc = 0;

    rc |= check_row_roundtrip();
    rc |= check_upgrade_persistence();
    rc |= check_save_order();
    rc |= check_filter_map_migration();
    rc |= check_filter_map_save();
    rc |= check_old_layout_translation();
    rc |= check_migration_lifecycle();
    if (entry_count == 0)
    {
        printf("CHECK FAILED: no entries parsed, the generator matched nothing\n");
        rc = 1;
    }
    for (unsigned i = 0; i < entry_count; i++)
    {
        const entry_t* e = &entries[i];
        if (e->def < e->min || e->def > e->max)
        {
            printf("CHECK FAILED: %s default %d is outside its range %d..%d\n", e->idname, e->def, e->min, e->max);
            rc = 1;
        }
        if (e->id >= FIRST_UNUSED)
        {
            printf("CHECK FAILED: %s id %u is not below EEPROM_FIRST_UNUSED %u\n", e->idname, e->id, FIRST_UNUSED);
            rc = 1;
        }
    }
    if (rc == 0) printf("all self checks passed\n");
    return rc;
}
