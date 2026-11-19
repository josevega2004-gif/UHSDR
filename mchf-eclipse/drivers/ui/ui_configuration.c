/*  -*-  mode: c; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4; coding: utf-8  -*-  */
/************************************************************************************
 **                                                                                 **
 **                                        UHSDR                                    **
 **               a powerful firmware for STM32 based SDR transceivers              **
 **                                                                                 **
 **---------------------------------------------------------------------------------**
 **                                                                                 **
 **  File name:                                                                     **
 **  Description:                                                                   **
 **  Last Modified:                                                                 **
 **  Licence:		GNU GPLv3                                                      **
 ************************************************************************************/

#include "ui_configuration.h"
#include "config_storage.h"

#include "ui_driver.h"

#include "audio_driver.h"
#include "audio_agc.h"
#include "rtty.h"
#include "psk.h"
#include "cw_decoder.h"

#include "ui_spectrum.h"
#include "radio_management.h"
#include "audio_management.h"
#include "ui.h" // bandInfo

#include "uhsdr_hw_i2c.h"
#include "uhsdr_rtc.h"

#if (MAX_VAR_ADDR > NB_OF_VAR)
    #error "Too many eeprom variables defined in ui_configuration.h (MAX_VAR_ADDR > NB_OF_VAR ). Please change maximum number of vars in eeprom.h"
#endif

/* The filter path memories are the only multi word configuration value, and the word count they need
 * follows from the mode and slot counts. If either grows, EEPROM_FILTER_PATH_MAP_WORDS and
 * EEPROM_FIRST_UNUSED have to grow with it, otherwise the block silently runs into the next ids. */
typedef char UiConfiguration_FilterMapWordsFit[
    (EEPROM_FILTER_PATH_MAP_WORDS == FILTER_MODE_MAX * FILTER_PATH_MEM_MAX) ? 1 : -1];
typedef char UiConfiguration_FilterMapInsideAddressRange[
    (EEPROM_FILTER_PATH_MAP_END < EEPROM_FIRST_UNUSED) ? 1 : -1];


static int32_t UiConfiguration_CompareConfigBuildVersions(uint major, uint32_t minor, uint32_t release);

// If more EEPROM variables are added, make sure that you add to this table - and the index to it in "eeprom.h"
// and correct MAX_VAR_ADDR in uhsdr_board.h

static uint16_t number_of_entries_cur_fw; // we need this to be able to read config values without modifying anything

// set while a configuration stored in the previous EEPROM layout is read, see UiConfiguration_LoadEepromValues()
static bool config_layout_migration_active = false;

/*
 * Migration of configurations stored with the previous EEPROM layout.
 *
 * The previous layout had 17 bands plus general coverage and used 18 slots per per-band block.
 * The current layout has 14 bands plus general coverage (11m added, 4m/2m/70cm/23cm removed) and
 * uses 15 slots per block, so almost every configuration address moved. Instead of forcing all
 * settings to their defaults, a stored configuration is read once through the address translation
 * below and then written back in the current layout by the next save.
 *
 * Values below were derived by comparing both address maps. Only the band dependent blocks need a
 * real translation, the remaining values moved by a constant offset per section.
 */
#define CONFIG_LAYOUT_OLD_NO_VALUE     0xffff               // value did not exist in the previous layout
#define CONFIG_LAYOUT_NEW_BAND_SLOTS   (MAX_BANDS)          // per band memories per block, general coverage is not stored
#define CONFIG_LAYOUT_OLD_BAND_SLOTS   18                   // bands plus general coverage, previous layout
#define CONFIG_LAYOUT_OLD_VFO_BASE     183                  // EEPROM_BAND0_MODE_A of the previous layout
#define CONFIG_LAYOUT_OLD_FULL_BASE    115                  // EEPROM_BAND0_FULL of the previous layout

#if (EEPROM_CONFIG_LAYOUT_VERSION >= EEPROM_FIRST_UNUSED)
    #error "The configuration layout marker must be inside the configuration address range"
#endif

/**
 * Per-band EEPROM slot of a band in the previous layout
 * @param new_slot band slot in the current layout
 * @return the slot used by the previous layout or CONFIG_LAYOUT_OLD_NO_VALUE if the band did not exist there
 */
static uint16_t UiConfiguration_OldBandSlot(uint16_t new_slot)
{
    switch (new_slot)
    {
    case BAND_MODE_10:   return 8;      // 10m was band memory 8
    case BAND_MODE_6:    return 9;      // 6m was band memory 9
    case BAND_MODE_2200: return 14;
    case BAND_MODE_630:  return 15;
    case BAND_MODE_160:  return 16;
    case BAND_MODE_GEN:  return 17;                         // general coverage was the last slot
    case BAND_MODE_11:   return CONFIG_LAYOUT_OLD_NO_VALUE; // no 11m band memory existed before
    case BAND_MODE_8:    return CONFIG_LAYOUT_OLD_NO_VALUE; // no 8m band memory existed before
    default:             return new_slot;                   // 80m .. 12m kept their band memory
    }
}

/**
 * Translates a band slot stored by the previous layout into the current band numbering
 * @param old_slot band slot as stored by the previous layout
 * @return band slot of the current layout
 */
static uint8_t UiConfiguration_NewBandSlot(uint8_t old_slot)
{
    switch (old_slot)
    {
    case 8:  return BAND_MODE_10;
    case 9:  return BAND_MODE_6;
    case 14: return BAND_MODE_2200;
    case 15: return BAND_MODE_630;
    case 16: return BAND_MODE_160;
    case 10:    // 4m  - no longer supported
    case 11:    // 2m  - no longer supported
    case 12:    // 70cm - no longer supported
    case 13:    // 23cm - no longer supported
    case 17:    // general coverage is not a selectable band memory
        return BAND_MODE_80;
    default:
        return old_slot < MAX_BANDS ? old_slot : BAND_MODE_80;
    }
}

/**
 * Address of a configuration value in the previous EEPROM layout
 * @param id address of the value in the current layout
 * @return address of the value in the previous layout or CONFIG_LAYOUT_OLD_NO_VALUE if it did not exist there
 */
static uint16_t UiConfiguration_OldLayoutAddress(uint16_t id)
{
    // per band output power factors. The 5W table kept its base address, the FULL table lost three slots
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

    // per band mode/frequency memories of VFO A and VFO B: six contiguous blocks
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

    // the remaining values moved by a constant offset per section
    if (id < EEPROM_BAND0_5W)
    {
        return id;                                          // 80m .. 12m power factors and all low addresses
    }
    if (id < 127)
    {
        return id;                                          // reserved slots of the band power tables
    }
    if (id < 160)
    {
        return id + 6;                                      // after the band power tables
    }
    if (id < EEPROM_BAND0_MODE_A)
    {
        return id + 4;                                      // DSP/AGC block
    }
    if (id < 295)
    {
        return id + 22;                                     // after the per band memories
    }
    if (id < 299)
    {
        return id + 32;
    }
    if (id <= EEPROM_BAND_REGION)
    {
        return id + 57;                                     // EEPROM_BAND_REGION was the last address of the previous layout
    }
    return CONFIG_LAYOUT_OLD_NO_VALUE;
}

/**
 * Base address of one of the six per band memory blocks, taking a pending layout migration into account
 * @param new_base base address of the block in the current layout
 * @return base address of the block to read from
 */
static uint16_t UiConfiguration_BandMemBlockBase(uint16_t new_base)
{
    if (!config_layout_migration_active)
    {
        return new_base;
    }
    uint16_t block = (new_base - EEPROM_BAND0_MODE_A) / CONFIG_LAYOUT_NEW_BAND_SLOTS;
    return (uint16_t)(CONFIG_LAYOUT_OLD_VFO_BASE + block * CONFIG_LAYOUT_OLD_BAND_SLOTS);
}

/**
 * Address to read a configuration value from, taking a pending layout migration into account
 * @param id address of the value in the current layout
 * @return address to read the value from
 */
static uint16_t UiConfiguration_LayoutAddress(uint16_t id)
{
    uint16_t old_id = config_layout_migration_active ? UiConfiguration_OldLayoutAddress(id) : id;
    return old_id == CONFIG_LAYOUT_OLD_NO_VALUE ? id : old_id;
}

/**
 * Is the stored configuration still using the previous EEPROM layout?
 * The previous firmware never stored the layout marker, so its absence identifies such a configuration.
 * @return true if the configuration has to be migrated
 */
static bool UiConfiguration_LayoutMigrationRequired(void)
{
    uint16_t layout_version = 0;

    if (ts.configstore_in_use == CONFIGSTORE_IN_USE_ERROR)
    {
        return false;                                       // no usable storage, nothing to migrate
    }
    if (ConfigStorage_ReadVariable(EEPROM_CONFIG_LAYOUT_VERSION, &layout_version) != HAL_OK)
    {
        return true;                                        // flash storage without a stored marker: the previous firmware never wrote one
    }
    // an I2C configuration written by the previous layout has some other value here (the address
    // was EEPROM_AGC_WDSP_THRESH, which only ever holds values up to 120), a fresh EEPROM reads
    // 0xFFFF, neither can be the magic, so only a marker written by the current layout stops this
    return layout_version != CONFIG_LAYOUT_VERSION_MAGIC;
}

#define CONFIG_UINT8x2_COMBINE(a,b) ((((a) << 8) | (b)))

#define UI_C_EEPROM_BAND_5W_PF(bandNo,bandName1,bandName2) { ConfigEntry_UInt8 | Calib_Val, EEPROM_BAND##bandNo##_5W,&ts.pwr_adj[ADJ_REF_PWR][BAND_MODE_##bandName1],TX_POWER_FACTOR_##bandName1##_DEFAULT,0,TX_POWER_FACTOR_MAX },
#define UI_C_EEPROM_BAND_FULL_PF(bandNo,bandName1,bandName2) { ConfigEntry_UInt8 | Calib_Val, EEPROM_BAND##bandNo##_FULL,&ts.pwr_adj[ADJ_FULL_POWER][BAND_MODE_##bandName1],TX_POWER_FACTOR_##bandName1##_DEFAULT,0,TX_POWER_FACTOR_MAX },

// here all simple configuration values are defined
// in order to have a new configuration value being stored in nonvolatile memory
// just add an entry. It is important to specify the addres of a ram location from  where to read/write  the
// respective value. As these configuration descriptors are constant values, the address has to be the address
// of a global (or static variable) the compiler know about.
// It is also important to use the matching typeId otherwise data will not be (re)store in a number of cases
// Some configuration values are currently not in this structure since saving them is more complex. See LoadEeprom/SaveEeprom functions.
// What means: INT DEFAULT PROBLEM: When using the EEPROM as configuration value store, we cannot distinguish the deleted (read never used) state from the normal state.
// This leads in a number of cases to the problem of initializing the parameter with its default value when it is first introduced. Since we simply read the storage location
// from eeprom, we read -1 from the new location and leave it there. Unless -1 is the default value or the range of the int configuration parameter does not include -1
// we will not automatically set the default value, which can be a problem.

const ConfigEntryDescriptor ConfigEntryInfo[] =
{
    // { ConfigEntry_UInt16, EEPROM_ZERO_LOC,&dummy_value16,0xffff,0,0xffff},
    // disable since we don't want to handle the eeprom signature here.

    { ConfigEntry_UInt8, EEPROM_SPEC_SCOPE_SPEED,&ts.scope_speed,SPECTRUM_SCOPE_SPEED_DEFAULT,0,SPECTRUM_SCOPE_SPEED_MAX},
    { ConfigEntry_UInt32_16, EEPROM_FREQ_STEP,&df.selected_idx,3,0,T_STEP_MAX_STEPS-2},
    { ConfigEntry_UInt8, EEPROM_TX_AUDIO_SRC,&ts.tx_audio_source,0,0,TX_AUDIO_MAX_ITEMS},
    { ConfigEntry_UInt8, EEPROM_TCXO_STATE,&df.temp_enabled,TCXO_ON,0,TCXO_MODE_MASK|TCXO_UNIT_MASK}, // we use
    { ConfigEntry_UInt8, EEPROM_AUDIO_GAIN,&ts.rx_gain[RX_AUDIO_SPKR].value,AUDIO_GAIN_DEFAULT,0,AUDIO_GAIN_MAX},
    { ConfigEntry_UInt8, EEPROM_RX_CODEC_GAIN,&ts.rf_codec_gain,DEFAULT_RF_CODEC_GAIN_VAL,0,MAX_RF_CODEC_GAIN_VAL},
    { ConfigEntry_UInt8, EEPROM_NB_SETTING,&ts.dsp.nb_setting,0,0,MAX_NB_SETTING},
    { ConfigEntry_UInt8, EEPROM_TX_POWER_LEVEL,&ts.power_level,PA_LEVEL_DEFAULT,0,PA_LEVEL_TUNE_KEEP_CURRENT},
    { ConfigEntry_UInt8, EEPROM_CW_KEYER_SPEED,&ts.cw_keyer_speed,CW_KEYER_SPEED_DEFAULT,CW_KEYER_SPEED_MIN, CW_KEYER_SPEED_MAX},
    { ConfigEntry_UInt8, EEPROM_CW_KEYER_MODE,&ts.cw_keyer_mode,CW_KEYER_MODE_IAM_B, 0, CW_KEYER_MAX_MODE},
    { ConfigEntry_UInt8, EEPROM_CW_KEYER_WEIGHT,&ts.cw_keyer_weight,CW_KEYER_WEIGHT_DEFAULT, CW_KEYER_WEIGHT_MIN, CW_KEYER_WEIGHT_MAX},
    { ConfigEntry_UInt8, EEPROM_CW_SIDETONE_GAIN,&ts.cw_sidetone_gain,DEFAULT_SIDETONE_GAIN,0, SIDETONE_MAX_GAIN},
    { ConfigEntry_Int32_16 | Calib_Val, EEPROM_FREQ_CAL,&ts.freq_cal,0,MIN_FREQ_CAL,MAX_FREQ_CAL}, // MINOR INT DEFAULT PROBLEM
    { ConfigEntry_UInt8, EEPROM_AGC_WDSP_MODE,&agc_wdsp_conf.mode, AGC_WDSP_MODE_DEFAULT,0,5},
    { ConfigEntry_UInt8, EEPROM_AGC_WDSP_HANG,&agc_wdsp_conf.hang_enable, AGC_WDSP_HANG_ENABLE_DEFAULT,0,1},
    { ConfigEntry_Int32_16, EEPROM_AGC_WDSP_THRESH,&agc_wdsp_conf.thresh, AGC_WDSP_THRESH_DEFAULT,-20,120}, // INT DEFAULT PROBLEM,see above
    { ConfigEntry_UInt8, EEPROM_AGC_WDSP_SLOPE,&agc_wdsp_conf.slope, AGC_WDSP_SLOPE_DEFAULT,0,200},
    { ConfigEntry_Int32_16, EEPROM_AGC_WDSP_TAU_DECAY_0,&agc_wdsp_conf.tau_decay[0], AGC_WDSP_TAU_DECAY_0_DEFAULT,50,5000}, // NO INT DEFAULT PROBLEM
    { ConfigEntry_Int32_16, EEPROM_AGC_WDSP_TAU_DECAY_1,&agc_wdsp_conf.tau_decay[1], AGC_WDSP_TAU_DECAY_1_DEFAULT,50,5000},  // NO INT DEFAULT PROBLEM
    { ConfigEntry_Int32_16, EEPROM_AGC_WDSP_TAU_DECAY_2,&agc_wdsp_conf.tau_decay[2], AGC_WDSP_TAU_DECAY_2_DEFAULT,50,5000},  // NO INT DEFAULT PROBLEM
    { ConfigEntry_Int32_16, EEPROM_AGC_WDSP_TAU_DECAY_3,&agc_wdsp_conf.tau_decay[3], AGC_WDSP_TAU_DECAY_3_DEFAULT,50,5000},  // NO INT DEFAULT PROBLEM
    { ConfigEntry_Int32_16, EEPROM_AGC_WDSP_TAU_DECAY_4,&agc_wdsp_conf.tau_decay[4], AGC_WDSP_TAU_DECAY_4_DEFAULT,50,5000}, // NO INT DEFAULT PROBLEM
    { ConfigEntry_Int32_16, EEPROM_AGC_WDSP_TAU_DECAY_5,&agc_wdsp_conf.tau_decay[5], AGC_WDSP_TAU_DECAY_5_DEFAULT,1,5000}, // AGC OFF mode, NO INT DEFAULT PROBLEM
    { ConfigEntry_Int32_16, EEPROM_AGC_WDSP_HANG_TIME,&agc_wdsp_conf.hang_time, AGC_WDSP_HANG_TIME_DEFAULT,10,5000}, // NO INT DEFAULT PROBLEM
    { ConfigEntry_Int32_16, EEPROM_AGC_WDSP_HANG_THRESH,&agc_wdsp_conf.hang_thresh, AGC_WDSP_HANG_THRESH_DEFAULT,-20,120}, // INT DEFAULT PROBLEM,see above
    { ConfigEntry_Int32_16, EEPROM_AGC_WDSP_TAU_HANG_DECAY,&agc_wdsp_conf.tau_hang_decay, AGC_WDSP_TAU_HANG_DECAY_DEFAULT,100,5000}, // NO INT DEFAULT PROBLEM
    { ConfigEntry_Int32_16, EEPROM_MIC_BOOST,&ts.tx_mic_boost, MIC_BOOST_DEFAULT,MIC_BOOST_MIN,MIC_BOOST_MAX},
    { ConfigEntry_UInt8, EEPROM_AM_CARRIER_LEVEL,&ts.am_carrier_level,AM_CARRIER_LEVEL_DEFAULT,AM_CARRIER_LEVEL_MIN,AM_CARRIER_LEVEL_MAX},
    { ConfigEntry_UInt8, EEPROM_AM_TX_SIDEBAND,&ts.am_tx_sideband,AmSideband_Both,AmSideband_Both,AmSideband_USB},
    { ConfigEntry_UInt8, EEPROM_AM_PEP_HOLD,&ts.am_carrier_pep_hold,AM_PEP_HOLD_DEFAULT,AM_PEP_HOLD_MIN,AM_PEP_HOLD_MAX},
    { ConfigEntry_UInt8, EEPROM_AM_PEP_DIM,&ts.am_carrier_pep_dim,AM_PEP_DIM_DEFAULT,0,1},
    { ConfigEntry_UInt8, EEPROM_AM_PEP_TARGET,&ts.am_carrier_pep_target,AM_PEP_TARGET_DEFAULT,AM_PEP_TARGET_MIN,AM_PEP_TARGET_MAX},
    { ConfigEntry_UInt8, EEPROM_AM_PEP_WARN,&ts.am_carrier_pep_warn,AM_PEP_WARN_DEFAULT,AM_PEP_WARN_MIN,AM_PEP_WARN_MAX},
    { ConfigEntry_UInt8, EEPROM_MIC_GAIN,&ts.tx_gain[TX_AUDIO_MIC], MIC_GAIN_DEFAULT,MIC_GAIN_MIN,MIC_GAIN_MAX},
    { ConfigEntry_UInt8, EEPROM_LINE_GAIN,&ts.tx_gain[TX_AUDIO_LINEIN_L], LINE_GAIN_DEFAULT,LINE_GAIN_MIN,LINE_GAIN_MAX},
    // the range of a stored value has to be the range of the menu item that changes it, otherwise
    // a value the operator can dial in is silently reset to its default at the next start
    { ConfigEntry_UInt32_16, EEPROM_SIDETONE_FREQ,&ts.cw_sidetone_freq,CW_SIDETONE_FREQ_DEFAULT,CW_SIDETONE_FREQ_MIN,CW_SIDETONE_FREQ_MAX * 10},
    { ConfigEntry_UInt8, EEPROM_SPECTRUM_FILTER,&ts.spectrum_filter,SPECTRUM_FILTER_DEFAULT,SPECTRUM_FILTER_MIN,SPECTRUM_FILTER_MAX},
#ifdef OBSOLETE_AGC
    { ConfigEntry_UInt8, EEPROM_AGC_CUSTOM_DECAY,&ts.agc_custom_decay,AGC_CUSTOM_DEFAULT,0,AGC_CUSTOM_MAX},
#endif
    { ConfigEntry_UInt8, EEPROM_METER_COLOUR_UP,&ts.meter_colour_up,SPEC_BLUE, 0, SPEC_MAX_COLOUR},
    { ConfigEntry_UInt8, EEPROM_METER_COLOUR_DOWN,&ts.meter_colour_down,SPEC_CYAN, 0, SPEC_MAX_COLOUR},
    { ConfigEntry_UInt8, EEPROM_SPECTRUM_TRACE_COLOUR,&ts.scope_trace_colour,SPEC_COLOUR_TRACE_DEFAULT, 0, SPEC_MAX_COLOUR},
    { ConfigEntry_UInt8, EEPROM_SPECTRUM_GRID_COLOUR,&ts.scope_grid_colour,SPEC_COLOUR_GRID_DEFAULT, 0, SPEC_MAX_COLOUR},
    { ConfigEntry_UInt8, EEPROM_SPECTRUM_CENTRE_LINE_COLOUR,&ts.spectrum_centre_line_colour,SPEC_COLOUR_GRID_DEFAULT, 0, SPEC_MAX_COLOUR},
    { ConfigEntry_UInt8, EEPROM_SPECTRUM_SCALE_COLOUR,&ts.spectrum_freqscale_colour,SPEC_COLOUR_SCALE_DEFAULT, 0, SPEC_MAX_COLOUR},
    { ConfigEntry_UInt8, EEPROM_PADDLE_REVERSE,&ts.cw_paddle_reverse,0,0,1},
    { ConfigEntry_UInt8, EEPROM_CW_RX_DELAY,&ts.cw_rx_delay,CW_TX2RX_DELAY_DEFAULT,0,CW_RX_DELAY_MAX},
    { ConfigEntry_UInt8, EEPROM_MAX_VOLUME,&ts.rx_gain[RX_AUDIO_SPKR].max,MAX_VOLUME_DEFAULT,MAX_VOLUME_MIN,MAX_VOLUME_MAX},
    { ConfigEntry_UInt8 | Calib_Val, EEPROM_PA_BIAS,&ts.pa_bias,PA_BIAS_DEFAULT,0,PA_BIAS_MAX},
    { ConfigEntry_UInt8 | Calib_Val, EEPROM_PA_CW_BIAS,&ts.pa_cw_bias,PA_BIAS_DEFAULT,0,PA_BIAS_MAX},

    { ConfigEntry_UInt8, EEPROM_IQ_AUTO_CORRECTION,&ts.iq_auto_correction,1, 0, 1},

#define UI_C_TX_IQ_ADJ(bandName) \
    { ConfigEntry_Int32_16 | Calib_Val, EEPROM_TX_IQ_##bandName##_GAIN_BALANCE,&iq_adjust[IQ_##bandName].adj.tx[IQ_TRANS_ON].gain,IQ_BALANCE_OFF, MIN_IQ_GAIN_BALANCE, MAX_IQ_GAIN_BALANCE}, \
    { ConfigEntry_Int32_16 | Calib_Val, EEPROM_TX_IQ_##bandName##_PHASE_BALANCE,&iq_adjust[IQ_##bandName].adj.tx[IQ_TRANS_ON].phase,IQ_BALANCE_OFF, MIN_IQ_PHASE_BALANCE, MAX_IQ_PHASE_BALANCE}, \
    { ConfigEntry_Int32_16 | Calib_Val, EEPROM_TX_IQ_##bandName##_GAIN_BALANCE_TRANS_OFF,&iq_adjust[IQ_##bandName].adj.tx[IQ_TRANS_OFF].gain,IQ_BALANCE_OFF, MIN_IQ_GAIN_BALANCE, MAX_IQ_GAIN_BALANCE}, \
    { ConfigEntry_Int32_16 | Calib_Val, EEPROM_TX_IQ_##bandName##_PHASE_BALANCE_TRANS_OFF,&iq_adjust[IQ_##bandName].adj.tx[IQ_TRANS_OFF].phase,IQ_BALANCE_OFF, MIN_IQ_PHASE_BALANCE, MAX_IQ_PHASE_BALANCE},

    UI_C_TX_IQ_ADJ(80M)
    UI_C_TX_IQ_ADJ(20M)
    UI_C_TX_IQ_ADJ(15M)
    UI_C_TX_IQ_ADJ(10M)
    UI_C_TX_IQ_ADJ(10M_UP)

#define UI_C_RX_IQ_ADJ(bandName) \
    { ConfigEntry_Int32_16 | Calib_Val, EEPROM_RX_IQ_##bandName##_GAIN_BALANCE,&iq_adjust[IQ_##bandName].adj.rx.gain,IQ_BALANCE_OFF, MIN_IQ_GAIN_BALANCE, MAX_IQ_GAIN_BALANCE}, \
    { ConfigEntry_Int32_16 | Calib_Val, EEPROM_RX_IQ_##bandName##_PHASE_BALANCE,&iq_adjust[IQ_##bandName].adj.rx.phase,IQ_BALANCE_OFF, MIN_IQ_PHASE_BALANCE, MAX_IQ_PHASE_BALANCE},

    UI_C_RX_IQ_ADJ(80M)
    UI_C_RX_IQ_ADJ(10M)

    { ConfigEntry_UInt8 | Calib_Val, EEPROM_SENSOR_NULL,&swrm.sensor_null,SWR_CAL_DEFAULT,SWR_CAL_MIN,SWR_CAL_MAX},
    { ConfigEntry_UInt8, EEPROM_PWR_METER_DISP,&swrm.pwr_meter_disp,0,0,1},
    { ConfigEntry_UInt32_16, EEPROM_XVERTER_DISP,&ts.xverter_mode,0,0,XVERTER_MULT_MAX},
    { ConfigEntry_UInt8, EEPROM_SPECTRUM_MAGNIFY,&sd.magnify,MAGNIFY_DEFAULT,MAGNIFY_MIN,MAGNIFY_MAX},
    // { ConfigEntry_UInt8, EEPROM_WIDE_FILT_CW_DISABLE,&ts.filter_cw_wide_disable,1,0,1},
    // { ConfigEntry_UInt8, EEPROM_NARROW_FILT_SSB_DISABLE,&ts.filter_ssb_narrow_disable,1,0,1},
    { ConfigEntry_UInt16, EEPROM_AM_MODE_DISABLE,&ts.demod_mode_disable,1,0,7},
    { ConfigEntry_UInt8, EEPROM_SPECTRUM_DB_DIV,&ts.spectrum_db_scale,DB_DIV_ADJUST_DEFAULT,DB_DIV_ADJUST_MIN, DB_DIV_ADJUST_MAX},
    { ConfigEntry_UInt8, EEPROM_SPECTRUM_AGC_RATE,&ts.spectrum_agc_rate,SPECTRUM_SCOPE_AGC_DEFAULT,SPECTRUM_SCOPE_AGC_MIN, SPECTRUM_SCOPE_AGC_MAX},
    { ConfigEntry_UInt8, EEPROM_METER_MODE,&ts.tx_meter_mode,METER_SWR,0,METER_MAX},
    { ConfigEntry_UInt32_16, EEPROM_ALC_DECAY_TIME,&ts.alc_decay,ALC_DECAY_DEFAULT,0,ALC_DECAY_MAX },
    { ConfigEntry_UInt32_16, EEPROM_ALC_POSTFILT_TX_GAIN,&ts.alc_tx_postfilt_gain,ALC_POSTFILT_GAIN_DEFAULT, ALC_POSTFILT_GAIN_MIN, ALC_POSTFILT_GAIN_MAX},
    { ConfigEntry_UInt16, EEPROM_STEP_SIZE_CONFIG,&ts.freq_step_config,0,0,255},
    { ConfigEntry_UInt8, EEPROM_DSP_MODE,&ts.dsp.active,0,0,255},
	{ ConfigEntry_UInt8, EEPROM_DSP_NR_STRENGTH,&ts.dsp.nr_strength,DSP_NR_STRENGTH_DEFAULT,0, DSP_NR_STRENGTH_MAX},

#ifdef OBSOLETE_NR
	{ ConfigEntry_UInt32_16, EEPROM_DSP_NR_DECOR_BUFLEN,&ts.dsp.nr_delaybuf_len,DSP_NR_BUFLEN_DEFAULT, DSP_NR_BUFLEN_MIN, DSP_NR_BUFLEN_MAX},
    { ConfigEntry_UInt8, EEPROM_DSP_NR_FFT_NUMTAPS,&ts.dsp.nr_numtaps,DSP_NR_NUMTAPS_DEFAULT, DSP_NR_NUMTAPS_MIN, DSP_NOTCH_NUMTAPS_MAX},

	{ ConfigEntry_UInt8, EEPROM_DSP_NOTCH_DECOR_BUFLEN,&ts.dsp.notch_delaybuf_len,DSP_NOTCH_DELAYBUF_DEFAULT,DSP_NOTCH_BUFLEN_MIN,DSP_NOTCH_BUFLEN_MAX},
    { ConfigEntry_UInt8, EEPROM_DSP_NOTCH_FFT_NUMTAPS,&ts.dsp.notch_numtaps,DSP_NOTCH_NUMTAPS_DEFAULT, DSP_NOTCH_NUMTAPS_MIN,DSP_NOTCH_NUMTAPS_MAX},
    { ConfigEntry_UInt8, EEPROM_DSP_NOTCH_CONV_RATE,&ts.dsp.notch_mu,DSP_NOTCH_MU_DEFAULT,0,DSP_NOTCH_MU_MAX},
#endif

#ifdef USE_LMS_AUTONOTCH
	{ ConfigEntry_UInt8, EEPROM_DSP_NOTCH_DECOR_BUFLEN,&ts.dsp.notch_delaybuf_len,DSP_NOTCH_DELAYBUF_DEFAULT,DSP_NOTCH_BUFLEN_MIN,DSP_NOTCH_BUFLEN_MAX},
    { ConfigEntry_UInt8, EEPROM_DSP_NOTCH_FFT_NUMTAPS,&ts.dsp.notch_numtaps,DSP_NOTCH_NUMTAPS_DEFAULT, DSP_NOTCH_NUMTAPS_MIN,DSP_NOTCH_NUMTAPS_MAX},
    { ConfigEntry_UInt8, EEPROM_DSP_NOTCH_CONV_RATE,&ts.dsp.notch_mu,DSP_NOTCH_MU_DEFAULT,0,DSP_NOTCH_MU_MAX},
#endif
	//   { ConfigEntry_UInt8, EEPROM_MAX_RX_GAIN,&ts.max_rf_gain,MAX_RF_GAIN_DEFAULT,0,MAX_RF_GAIN_MAX},
    { ConfigEntry_Int16, EEPROM_TX_AUDIO_COMPRESS,&ts.tx_comp_level,TX_AUDIO_COMPRESSION_DEFAULT,TX_AUDIO_COMPRESSION_MIN,TX_AUDIO_COMPRESSION_MAX}, // NO INT DEFAULT PROBLEM
    { ConfigEntry_UInt8, EEPROM_TX_DISABLE,&ts.tx_disable,0,0,1},
    { ConfigEntry_UInt16, EEPROM_FLAGS1,&ts.flags1,FLAGS1_CONFIG_DEFAULT,0,0xffff},
    { ConfigEntry_UInt16, EEPROM_FLAGS2,&ts.flags2,FLAGS2_CONFIG_DEFAULT,0,0xffff},
    { ConfigEntry_UInt16, EEPROM_VERSION_MINOR,&ts.version_number_minor,0,0,255},
    { ConfigEntry_UInt16, EEPROM_VERSION_RELEASE,&ts.version_number_release,0,0,255},
    { ConfigEntry_UInt16, EEPROM_VERSION_MAJOR,&ts.version_number_major,0,0,255},
//    { ConfigEntry_UInt8, EEPROM_NB_AGC_TIME_CONST,&ts.nb_agc_time_const,NB_AGC_DEFAULT,0,NB_MAX_AGC_SETTING},
    { ConfigEntry_UInt8, EEPROM_CW_OFFSET_MODE,&ts.cw_offset_mode,CW_OFFSET_MODE_DEFAULT,0,CW_OFFSET_NUM-1},
    { ConfigEntry_Int32_16, EEPROM_FREQ_CONV_MODE,&ts.iq_freq_mode,FREQ_IQ_CONV_MODE_DEFAULT,0,FREQ_IQ_CONV_MODE_MAX}, // NO INT DEFAULT PROBLEM
    { ConfigEntry_UInt8, EEPROM_LSB_USB_AUTO_SELECT,&ts.lsb_usb_auto_select,AUTO_LSB_USB_DEFAULT,0,AUTO_LSB_USB_MAX},
    { ConfigEntry_UInt8, EEPROM_LCD_BLANKING_CONFIG,&ts.lcd_backlight_blanking,0,0,255},
    { ConfigEntry_UInt32_16, EEPROM_VFO_MEM_MODE,&ts.vfo_mem_mode,0,0,255},
    { ConfigEntry_UInt8 | Calib_Val, EEPROM_DETECTOR_COUPLING_COEFF_2200M,&swrm.coupling_calc[COUPLING_2200M],SWR_COUPLING_DEFAULT,SWR_COUPLING_MIN,SWR_COUPLING_MAX},
    { ConfigEntry_UInt8 | Calib_Val, EEPROM_DETECTOR_COUPLING_COEFF_630M,&swrm.coupling_calc[COUPLING_630M],SWR_COUPLING_DEFAULT,SWR_COUPLING_MIN,SWR_COUPLING_MAX},
    { ConfigEntry_UInt8 | Calib_Val, EEPROM_DETECTOR_COUPLING_COEFF_160M,&swrm.coupling_calc[COUPLING_160M],SWR_COUPLING_DEFAULT,SWR_COUPLING_MIN,SWR_COUPLING_MAX},
    { ConfigEntry_UInt8 | Calib_Val, EEPROM_DETECTOR_COUPLING_COEFF_80M,&swrm.coupling_calc[COUPLING_80M],SWR_COUPLING_DEFAULT,SWR_COUPLING_MIN,SWR_COUPLING_MAX},
    { ConfigEntry_UInt8 | Calib_Val, EEPROM_DETECTOR_COUPLING_COEFF_40M,&swrm.coupling_calc[COUPLING_40M],SWR_COUPLING_DEFAULT,SWR_COUPLING_MIN,SWR_COUPLING_MAX},
    { ConfigEntry_UInt8 | Calib_Val, EEPROM_DETECTOR_COUPLING_COEFF_20M,&swrm.coupling_calc[COUPLING_20M],SWR_COUPLING_DEFAULT,SWR_COUPLING_MIN,SWR_COUPLING_MAX},
    { ConfigEntry_UInt8 | Calib_Val, EEPROM_DETECTOR_COUPLING_COEFF_15M,&swrm.coupling_calc[COUPLING_15M],SWR_COUPLING_DEFAULT,SWR_COUPLING_MIN,SWR_COUPLING_MAX},
    { ConfigEntry_UInt8 | Calib_Val, EEPROM_DETECTOR_COUPLING_COEFF_6M,&swrm.coupling_calc[COUPLING_6M],SWR_COUPLING_DEFAULT,SWR_COUPLING_MIN,SWR_COUPLING_MAX},
    { ConfigEntry_UInt8 | Calib_Val, EEPROM_DETECTOR_COUPLING_COEFF_11M,&swrm.coupling_calc[COUPLING_11M],SWR_COUPLING_DEFAULT,SWR_COUPLING_MIN,SWR_COUPLING_MAX},
    { ConfigEntry_UInt32_16 | Calib_Val, EEPROM_VOLTMETER_CALIBRATE,&ts.voltmeter_calibrate,POWER_VOLTMETER_CALIBRATE_DEFAULT,POWER_VOLTMETER_CALIBRATE_MIN,POWER_VOLTMETER_CALIBRATE_MAX},
    { ConfigEntry_UInt8 | Calib_Val, EEPROM_LOW_POWER_CONFIG,&ts.low_power_config,LOW_POWER_CONFIG_DEFAULT,LOW_POWER_CONFIG_MIN,LOW_POWER_CONFIG_MAX},
    { ConfigEntry_UInt8, EEPROM_WATERFALL_COLOR_SCHEME,&ts.waterfall.color_scheme,WATERFALL_COLOR_DEFAULT,WATERFALL_COLOR_MIN,WATERFALL_COLOR_MAX},
    { ConfigEntry_UInt8, EEPROM_WATERFALL_VERTICAL_STEP_SIZE,&ts.waterfall.vert_step_size,WATERFALL_STEP_SIZE_DEFAULT,WATERFALL_STEP_SIZE_MIN,WATERFALL_STEP_SIZE_MAX},
    //{ ConfigEntry_Int32_16, EEPROM_WATERFALL_OFFSET,&ts.waterfall.offset,WATERFALL_OFFSET_DEFAULT,WATERFALL_OFFSET_MIN,WATERFALL_OFFSET_MAX},
    { ConfigEntry_UInt8, EEPROM_SPECTRUM_SIZE,&ts.spectrum_size,SPECTRUM_SIZE_DEFAULT,0,SPECTRUM_BIG},
    { ConfigEntry_UInt32_16, EEPROM_WATERFALL_CONTRAST,&ts.waterfall.contrast,WATERFALL_CONTRAST_DEFAULT,WATERFALL_CONTRAST_MIN,WATERFALL_CONTRAST_MAX},
    { ConfigEntry_UInt8, EEPROM_WATERFALL_SPEED,&ts.waterfall.speed,WATERFALL_SPEED_DEFAULT,0,WATERFALL_SPEED_MAX},
    //{ ConfigEntry_UInt8, EEPROM_SPECTRUM_SCOPE_NOSIG_ADJUST,&ts.spectrum_scope_nosig_adjust,SPECTRUM_SCOPE_NOSIG_ADJUST_DEFAULT,SPECTRUM_SCOPE_NOSIG_ADJUST_MIN,SPECTRUM_SCOPE_NOSIG_ADJUST_MAX},
    //{ ConfigEntry_UInt8, EEPROM_WATERFALL_NOSIG_ADJUST,&ts.waterfall.nosig_adjust,SPECTRUM_SCOPE_NOSIG_ADJUST_DEFAULT,WATERFALL_NOSIG_ADJUST_MIN,WATERFALL_NOSIG_ADJUST_MAX},
    //{ ConfigEntry_UInt8, EEPROM_FFT_WINDOW,&ts.fft_window_type,FFT_WINDOW_DEFAULT,0,FFT_WINDOW_MAX},
    { ConfigEntry_UInt8, EEPROM_TXRX_SWITCH_AUDIO_MUTE_DELAY,&ts.txrx_switch_audio_muting_timing,TXRX_SWITCH_AUDIO_MUTE_DELAY_DEFAULT,0,TXRX_SWITCH_AUDIO_MUTE_DELAY_MAX},
    { ConfigEntry_UInt8, EEPROM_FILTER_DISP_COLOUR,&ts.filter_disp_colour,0,0,SPEC_MAX_COLOUR},
    { ConfigEntry_UInt32_16, EEPROM_FM_SUBAUDIBLE_TONE_GEN,&ts.fm_subaudible_tone_gen_select,FM_SUBAUDIBLE_TONE_OFF,0,NUM_SUBAUDIBLE_TONES},
    { ConfigEntry_UInt32_16, EEPROM_FM_SUBAUDIBLE_TONE_DET,&ts.fm_subaudible_tone_det_select,FM_SUBAUDIBLE_TONE_OFF,0,NUM_SUBAUDIBLE_TONES},
    { ConfigEntry_UInt8, EEPROM_FM_TONE_BURST_MODE,&ts.fm_tone_burst_mode,FM_TONE_BURST_OFF,0,FM_TONE_BURST_MAX},
    { ConfigEntry_UInt8, EEPROM_FM_SQUELCH_SETTING,&ts.fm_sql_threshold,FM_SQUELCH_DEFAULT,0,FM_SQUELCH_MAX},
    { ConfigEntry_UInt32_16, EEPROM_KEYBOARD_BEEP_FREQ,&ts.beep_frequency,DEFAULT_BEEP_FREQUENCY,MIN_BEEP_FREQUENCY,MAX_BEEP_FREQUENCY},
    { ConfigEntry_UInt8, EEPROM_BEEP_LOUDNESS,&ts.beep_loudness,DEFAULT_BEEP_LOUDNESS,0,MAX_BEEP_LOUDNESS},
    { ConfigEntry_UInt8, EEPROM_TUNE_POWER_LEVEL,&ts.tune_power_level,PA_LEVEL_TUNE_KEEP_CURRENT,PA_LEVEL_FULL,PA_LEVEL_TUNE_KEEP_CURRENT},
    { ConfigEntry_UInt8, EEPROM_CAT_XLAT,&ts.xlat,1,0,1},
    { ConfigEntry_UInt32_16, EEPROM_MANUAL_NOTCH,&ts.dsp.notch_frequency,800,200,5000},
    { ConfigEntry_UInt32_16, EEPROM_MANUAL_PEAK,&ts.dsp.peak_frequency,750,200,5000},
    { ConfigEntry_UInt8, EEPROM_DISPLAY_DBM,&ts.display_dbm,0,0,2},
    { ConfigEntry_Int32_16 | Calib_Val, EEPROM_DBM_CALIBRATE,&ts.dbm_constant,0,-100,100}, // MINOR INT DEFAULT PROBLEM,see above
//    { ConfigEntry_UInt8, EEPROM_S_METER,&ts.s_meter,0,0,2},
    { ConfigEntry_UInt8, EEPROM_DIGI_MODE_CONF,&ts.digital_mode,DigitalMode_RTTY,DigitalMode_RTTY,DigitalMode_BPSK},
    // the digi mode parameters the encoders change: until now they only existed in the static
    // initializers of rtty.c / psk.c, so every power cycle lost the shift and speed the operator had set
    { ConfigEntry_UInt32_16, EEPROM_RTTY_SHIFT_IDX,&rtty_ctrl_config.shift_idx,RTTY_SHIFT_170,RTTY_SHIFT_85,RTTY_SHIFT_NUM-1},
    { ConfigEntry_UInt32_16, EEPROM_RTTY_SPEED_IDX,&rtty_ctrl_config.speed_idx,RTTY_SPEED_45,RTTY_SPEED_45,RTTY_SPEED_NUM-1},
    { ConfigEntry_UInt32_16, EEPROM_PSK_SPEED_IDX,&psk_ctrl_config.speed_idx,PSK_SPEED_31,PSK_SPEED_31,PSK_SPEED_NUM-1},
	{ ConfigEntry_Int32_16, EEPROM_BASS_GAIN,&ts.dsp.bass_gain,2,-20,20}, // INT DEFAULT PROBLEM,see above
    { ConfigEntry_Int32_16, EEPROM_TREBLE_GAIN,&ts.dsp.treble_gain,0,-20,20},  // MINOR INT DEFAULT PROBLEM,see above
    { ConfigEntry_UInt8, EEPROM_TX_FILTER,&ts.tx_filter,0,0,TX_FILTER_BASS},
	{ ConfigEntry_Int32_16, EEPROM_TX_BASS_GAIN,&ts.dsp.tx_bass_gain,4,-20,6}, // INT DEFAULT PROBLEM,see above
    { ConfigEntry_Int32_16, EEPROM_TX_TREBLE_GAIN,&ts.dsp.tx_treble_gain,4,-20,6}, // INT DEFAULT PROBLEM,see above
    { ConfigEntry_Int32_16, EEPROM_SAM_PLL_LOCKING_RANGE,&ads.pll_fmax_int,2500,50,8000}, // NO INT DEFAULT PROBLEM
    { ConfigEntry_Int32_16, EEPROM_SAM_PLL_STEP_RESPONSE,&ads.zeta_int,65,1,100},  // NO INT DEFAULT PROBLEM
    { ConfigEntry_Int32_16, EEPROM_SAM_PLL_BANDWIDTH,&ads.omegaN_int, 250,15,1000}, // NO INT DEFAULT PROBLEM
    { ConfigEntry_Int32_16, EEPROM_I2C1_SPEED,&ts.i2c_speed[0], I2C1_SPEED_DEFAULT,1,20}, // NO INT DEFAULT PROBLEM
    { ConfigEntry_Int32_16, EEPROM_I2C2_SPEED,&ts.i2c_speed[1], I2C2_SPEED_DEFAULT,1,20}, // NO INT DEFAULT PROBLEM
    { ConfigEntry_UInt8, EEPROM_SAM_FADE_LEVELER,&ads.fade_leveler,1,0,1},
    { ConfigEntry_UInt8, EEPROM_LINEOUT_GAIN,&ts.lineout_gain,LINEOUT_GAIN_DEFAULT,LINEOUT_GAIN_MIN,LINEOUT_GAIN_MAX},
    { ConfigEntry_Int16 | Calib_Val, EEPROM_RTC_CALIB,&ts.rtc_calib,RTC_CALIB_PPM_DEFAULT, RTC_CALIB_PPM_MIN, RTC_CALIB_PPM_MAX},
    { ConfigEntry_UInt8, EEPROM_CW_DECODER_ENABLE,&ts.cw_decoder_enable,1,0,1},
	{ ConfigEntry_UInt16, EEPROM_Scope_Graticule_Ypos,&ts.graticulePowerupYpos,0,0,480},
	{ ConfigEntry_UInt8, EEPROM_Freq_Display_Font,&ts.FreqDisplayFont,0,0,1},


    UI_C_EEPROM_BAND_5W_PF(0,80,m)
    UI_C_EEPROM_BAND_5W_PF(1,60,m)
    UI_C_EEPROM_BAND_5W_PF(2,40,m)
    UI_C_EEPROM_BAND_5W_PF(3,30,m)
    UI_C_EEPROM_BAND_5W_PF(4,20,m)
    UI_C_EEPROM_BAND_5W_PF(5,17,m)
    UI_C_EEPROM_BAND_5W_PF(6,15,m)
    UI_C_EEPROM_BAND_5W_PF(7,12,m)
    UI_C_EEPROM_BAND_5W_PF(8,11,m)
    UI_C_EEPROM_BAND_5W_PF(9,10,m)
    UI_C_EEPROM_BAND_5W_PF(10,6,m)
    UI_C_EEPROM_BAND_5W_PF(11,2200,m)
    UI_C_EEPROM_BAND_5W_PF(12,630,m)
    UI_C_EEPROM_BAND_5W_PF(13,160,m)
    
    UI_C_EEPROM_BAND_FULL_PF(0,80,m)
    UI_C_EEPROM_BAND_FULL_PF(1,60,m)
    UI_C_EEPROM_BAND_FULL_PF(2,40,m)
    UI_C_EEPROM_BAND_FULL_PF(3,30,m)
    UI_C_EEPROM_BAND_FULL_PF(4,20,m)
    UI_C_EEPROM_BAND_FULL_PF(5,17,m)
    UI_C_EEPROM_BAND_FULL_PF(6,15,m)
    UI_C_EEPROM_BAND_FULL_PF(7,12,m)
    UI_C_EEPROM_BAND_FULL_PF(8,11,m)
    UI_C_EEPROM_BAND_FULL_PF(9,10,m)
    UI_C_EEPROM_BAND_FULL_PF(10,6,m)
    UI_C_EEPROM_BAND_FULL_PF(11,2200,m)
    UI_C_EEPROM_BAND_FULL_PF(12,630,m)
    UI_C_EEPROM_BAND_FULL_PF(13,160,m)
	{ ConfigEntry_UInt8, EEPROM_Scope_TRACE_HL_BW,&ts.scope_trace_BW_colour,SPEC_COLOUR_TRACEBW_DEFAULT,0,SPEC_MAX_COLOUR},
	{ ConfigEntry_UInt8, EEPROM_Scope_TRACE_HL_BW_BGR,&ts.scope_backgr_BW_colour,SPEC_COLOUR_BACKGRBW_DEFAULT,0,100},
	{ ConfigEntry_Int32 | Calib_Val, EEPROM_TScal0_High,&mchf_touchscreen.cal[0], 72816, INT32_MIN,INT32_MAX}, // INT DEFAULT PROBLEM,see above
	{ ConfigEntry_Int32 | Calib_Val, EEPROM_TScal1_High,&mchf_touchscreen.cal[1], -5,INT32_MIN,INT32_MAX}, // INT DEFAULT PROBLEM,see above
	{ ConfigEntry_Int32 | Calib_Val, EEPROM_TScal2_High,&mchf_touchscreen.cal[2], -1615424,INT32_MIN,INT32_MAX}, // INT DEFAULT PROBLEM,see above
	{ ConfigEntry_Int32 | Calib_Val, EEPROM_TScal3_High,&mchf_touchscreen.cal[3], -1,INT32_MIN,INT32_MAX},    // INT DEFAULT PROBLEM,see above
	{ ConfigEntry_Int32 | Calib_Val, EEPROM_TScal4_High,&mchf_touchscreen.cal[4], 74886,INT32_MIN,INT32_MAX}, // INT DEFAULT PROBLEM,see above
	{ ConfigEntry_Int32 | Calib_Val, EEPROM_TScal5_High,&mchf_touchscreen.cal[5], -1630326,INT32_MIN,INT32_MAX}, // INT DEFAULT PROBLEM,see above
	{ ConfigEntry_UInt16, EEPROM_NUMBER_OF_ENTRIES,&number_of_entries_cur_fw,EEPROM_FIRST_UNUSED,EEPROM_FIRST_UNUSED,EEPROM_FIRST_UNUSED},
	{ ConfigEntry_UInt16, EEPROM_DSP_MODE_MASK,&ts.dsp.mode_mask,DSP_SWITCH_MODEMASK_ENABLE_DEFAULT,DSP_SWITCH_MODEMASK_ENABLE_DSPOFF,DSP_SWITCH_MODEMASK_ENABLE_MASK},
    { ConfigEntry_UInt8, EEPROM_ENABLE_PTT_RTS,&ts.enable_ptt_rts,0,0,1},
	{ ConfigEntry_UInt32_16, EEPROM_CW_DECODER_THRESH,&cw_decoder_config.thresh,CW_DECODER_THRESH_DEFAULT,CW_DECODER_THRESH_MIN,CW_DECODER_THRESH_MAX}, // NO INT DEFAULT PROBLEM
	{ ConfigEntry_Int32_16, EEPROM_CW_DECODER_BLOCKSIZE,&cw_decoder_config.blocksize,CW_DECODER_BLOCKSIZE_DEFAULT,CW_DECODER_BLOCKSIZE_MIN,CW_DECODER_BLOCKSIZE_MAX}, // NO INT DEFAULT PROBLEM
	{ ConfigEntry_UInt8x2, EEPROM_SMETER_ALPHAS,&sm.config.alphaCombined,CONFIG_UINT8x2_COMBINE(SMETER_ALPHA_ATTACK_DEFAULT, SMETER_ALPHA_DECAY_DEFAULT), CONFIG_UINT8x2_COMBINE(SMETER_ALPHA_MIN, SMETER_ALPHA_MIN), CONFIG_UINT8x2_COMBINE(SMETER_ALPHA_MAX,SMETER_ALPHA_MAX) },
    { ConfigEntry_UInt8, EEPROM_VSWR_PROTECTION_THRESHOLD,&ts.vswr_protection_threshold,1,1,10},
	{ ConfigEntry_UInt16, EEPROM_EXPFLAGS1,&ts.expflags1,EXPFLAGS1_CONFIG_DEFAULT,0,0xffff},
	{ ConfigEntry_UInt16, EEPROM_CW_DECODER_FLAGS,&cw_decoder_config.flags,CW_DECODER_FLAGS_DEFAULT,0,0x7fff},
    { ConfigEntry_UInt8, EEPROM_BAND_REGION, &bandinfo_idx, BAND_INFO_SET_DEFAULT, 0, 0x7f}, // we need to validate the range later as we cannot do it here due to C shortcomings
	
    // the entry below MUST be the last entry, and only at the last position Stop is allowed
    {
        ConfigEntry_Stop
    }
};

// TODO: LINEAR SEARCH IS VERY BAD FOR LARGER SETS, replace with more clever strategy.
// for use case in menu this acceptable, however
const ConfigEntryDescriptor* UiConfiguration_GetEntry(uint16_t id)
{
    int idx;
    const ConfigEntryDescriptor* retval = NULL;
    for (idx = 0; ConfigEntryInfo[idx].typeId != ConfigEntry_Stop; idx++)
    {
        if (ConfigEntryInfo[idx].id == id)
        {
            retval = &ConfigEntryInfo[idx];
            break;
        }
    }
    return retval;
}


/*
static void __attribute__ ((noinline)) UiReadSettingEEPROM_Bool(uint16_t addr, volatile bool* val_ptr, uint16_t default_val, uint16_t min_val, uint16_t max_val ) {
    uint16_t value;
    if(Read_EEPROM(addr, &value) == 0)
    {
        *val_ptr = value;
        if (*val_ptr < min_val || *val_ptr > max_val || load_default) {
            *val_ptr = default_val;
        }
    }
}
*/

static void __attribute__ ((noinline)) UiReadSettingEEPROM_UInt8(uint16_t addr, volatile uint8_t* val_ptr, uint16_t default_val, uint16_t min_val, uint16_t max_val, bool load_default)
{
    uint16_t value;
    if(ConfigStorage_ReadVariable(addr, &value) == 0)
    {
        *val_ptr = value;
        if (*val_ptr < min_val || *val_ptr > max_val || load_default)
        {
            *val_ptr = default_val;
        }
    }
    else
    {
        *val_ptr = default_val;
    }
}


static void __attribute__ ((noinline)) UiReadSettingEEPROM_UInt8x2_Split(uint16_t addr, volatile uint8_t* val_ptr_high, uint8_t default_val_high, uint8_t min_val_high, uint8_t max_val_high,
                                                                                  volatile uint8_t* val_ptr_low, uint8_t default_val_low, uint8_t min_val_low, uint8_t max_val_low, bool load_default)
{
    uint16_t value;
    if(ConfigStorage_ReadVariable(addr, &value) == 0)
    {
        *val_ptr_high = value >> 8;
        *val_ptr_low  = value & 0xff;

        if ((*val_ptr_high  < min_val_high) || (*val_ptr_high  > max_val_high) || load_default)
        {
            *val_ptr_high = default_val_high;
        }

        if ((*val_ptr_low  < min_val_low) || (*val_ptr_low  > max_val_low) || load_default)
        {
            *val_ptr_low = default_val_low;
        }


    }
    else
    {
        *val_ptr_low = default_val_low;
        *val_ptr_high = default_val_high;
    }
}

static void __attribute__ ((noinline)) UiReadSettingEEPROM_UInt8x2(uint16_t addr, volatile uint16_t* val_ptr, uint16_t default_val, uint16_t min_val, uint16_t max_val, bool load_default)
{
    uint16_t value;
    if(ConfigStorage_ReadVariable(addr, &value) == 0)
    {
        *val_ptr = value;

        if ( (((*val_ptr >> 8) & 0xff)  < ((min_val >> 8) & 0xff)) || (((*val_ptr >> 8) & 0xff)  > ((max_val >> 8) & 0xff)) || load_default)
        {
            *val_ptr = (*val_ptr & 0xff) | (default_val & 0xff00);
        }

        if ( ((*val_ptr & 0xff)  < (min_val & 0xff)) || ((*val_ptr & 0xff)  > (max_val & 0xff)) || load_default)
        {
            *val_ptr = (*val_ptr & 0xff00) | (default_val & 0x00ff);
        }
    }
    else
    {
        *val_ptr = default_val;
    }
}

static void __attribute__ ((noinline)) UiReadSettingEEPROM_UInt16(uint16_t addr, volatile uint16_t* val_ptr, uint16_t default_val, uint16_t min_val, uint16_t max_val, bool load_default)
{
    uint16_t value;
    if(ConfigStorage_ReadVariable(addr, &value) == 0)
    {
        *val_ptr = value;
        if (*val_ptr < min_val || *val_ptr > max_val || load_default)
        {
            *val_ptr = default_val;
        }
    }
    else
    {
        *val_ptr = default_val;
    }
}

static void __attribute__ ((noinline)) UiReadSettingEEPROM_Int16(uint16_t addr, volatile int16_t* val_ptr, int16_t default_val, int16_t min_val, int16_t max_val, bool load_default)
{
    int16_t value;
    if(ConfigStorage_ReadVariable(addr, (uint16_t*)&value) == 0)
    {
        *val_ptr = value;
        if (*val_ptr < min_val || *val_ptr > max_val || load_default)
        {
            *val_ptr = default_val;
        }
    }
    else
    {
        *val_ptr = default_val;
    }
}


static void __attribute__ ((noinline)) UiReadSettingEEPROM_UInt32_16(uint16_t addr, volatile uint32_t* val_ptr, uint16_t default_val, uint16_t min_val, uint16_t max_val, bool load_default)
{
    uint16_t value;
    if(ConfigStorage_ReadVariable(addr, &value) == 0)
    {
        *val_ptr = value;
        if (*val_ptr < min_val || *val_ptr > max_val || load_default)
        {
            *val_ptr = default_val;
        }
    }
    else
    {
        *val_ptr = default_val;
    }
}

static void __attribute__ ((noinline)) UiReadSettingEEPROM_Int32_16(uint16_t addr, volatile int32_t* val_ptr, int default_val, int min_val, int max_val, bool load_default)
{
    uint16_t value;
    if(ConfigStorage_ReadVariable(addr, &value) == 0)
    {
        *val_ptr = (int16_t)value;
        if (*val_ptr < min_val || *val_ptr > max_val || load_default)
        {
            *val_ptr = default_val;
        }
    }
    else
    {
        *val_ptr = default_val;
    }
}

static void UiReadSettingEEPROM_UInt32(uint16_t addrH, uint16_t addrL, volatile uint32_t* val_ptr, uint32_t default_val, uint32_t min_val, uint32_t max_val, bool load_default)
{
    uint16_t valueH,valueL;
    if(ConfigStorage_ReadVariable(addrH, &valueH) == 0 && ConfigStorage_ReadVariable(addrL, &valueL) == 0)
    {

        *val_ptr = valueH;
        *val_ptr <<=16;
        *val_ptr |= valueL;

        if (*val_ptr < min_val || *val_ptr > max_val || load_default)
        {
            *val_ptr = default_val;
        }
    }
    else
    {
        *val_ptr = default_val;
    }
}

static uint16_t UiWriteSettingEEPROM_UInt16(uint16_t addr, uint16_t set_val)
{
    return ConfigStorage_WriteVariable(addr, set_val);
}

static uint16_t UiWriteSettingEEPROM_UInt8x2(uint16_t addr, uint16_t set_val)
{
    return ConfigStorage_WriteVariable(addr, set_val);
}


static uint16_t UiWriteSettingEEPROM_Int16(uint16_t addr, int16_t set_val)
{
    return ConfigStorage_WriteVariable(addr, (uint16_t)set_val);
}

static uint16_t UiWriteSettingEEPROM_UInt32(uint16_t addrH, uint16_t addrL, uint32_t set_val)
{
    uint16_t retval = ConfigStorage_WriteVariable(addrH, (uint16_t)(set_val >> 16));

    if (retval == HAL_OK)
    {
        retval = ConfigStorage_WriteVariable(addrL, (uint16_t)(set_val));
    }
    return retval;
}

static uint32_t UiConfiguration_LimitFrequency(BandInfo_c* bandInfo, const uint32_t freq, bool set_to_default)
{
    uint32_t retval = freq;
    bool do_reset = false;

#ifndef USE_MEMORY_MODE
        // Load default for this band
    const uint32_t resetval = bandInfo->tune + DEFAULT_FREQ_OFFSET;
#else
    const uint32_t resetval = DEFAULT_MEMORY_FREQ;
#endif


    // this code handles the migration of stored frequency settings from the older approach to/from the newer
    // approach. We will have to introduce the newer approach with firmware 2.11.0 in order not to cause
    // issues when using older firmware with this migration code already built in.

    // stored configuration is older than 2.11.0, dial frequencies are stored as multiples of 4 * dial frequency
    // we have to scale them down with our old TUNE_MULT
    if (UiConfiguration_CompareConfigBuildVersions(2,11,00) == 1)
    {
        retval /= 4;
    }

    if(set_to_default)
    {
        do_reset = true;
    }
    else
    {
        if((ts.flags2 & FLAGS2_FREQ_MEM_LIMIT_RELAX) == 0)
        {
#ifndef USE_MEMORY_MODE
            do_reset = RadioManagement_FreqIsInBand(bandInfo,retval) == false;       // xxxx relax memory-save frequency restrictions and is it within the allowed range?
#else
            do_reset = RadioManagement_FreqIsInEnabledBand(retval) == false;
#endif
        }
    }
    return do_reset == true? resetval : retval;
}

/**
 * @param i index into the band table, also selects the band info used for the defaults
 * @param mem_slot band memory slot to read from, differs from i while migrating a stored configuration
 */
void UiReadSettingsBandMode(const uint8_t i, const uint16_t mem_slot, const uint16_t band_mode, const uint16_t band_freq_high, const uint16_t  band_freq_low, VfoReg* vforeg, bool load_default)
{
    uint32_t value32;

    UiReadSettingEEPROM_UInt8x2_Split(band_mode + mem_slot,
            &vforeg->decod_mode, DEMOD_LSB, 0, DEMOD_MAX_MODE,
            &vforeg->digital_mode, DigitalMode_RTTY, DigitalMode_RTTY, DigitalMode_BPSK,
            load_default);

    // Try to read Freq saved values
    UiReadSettingEEPROM_UInt32(band_freq_high + mem_slot, band_freq_low + mem_slot,&value32,bandInfo[i]->tune + DEFAULT_FREQ_OFFSET,0,0xffffffff, load_default);
    {
        // We make sure to read only frequency which are permitted for band in given
        // configuration.
        vforeg->dial_value = UiConfiguration_LimitFrequency(bandInfo[i],value32, load_default);
    }
}

/*
static void __attribute__ ((noinline)) UiWriteSettingEEPROM_Bool(uint16_t addr, bool set_val, bool default_val ) {
    UiWriteSettingEEPROM_UInt16(addr,(uint16_t)set_val,(uint16_t)default_val);
}
*/

static uint16_t __attribute__ ((noinline)) UiWriteSettingEEPROM_UInt32_16(uint16_t addr, uint32_t set_val)
{
    return UiWriteSettingEEPROM_UInt16(addr,set_val);
}

static uint16_t __attribute__ ((noinline)) UiWriteSettingEEPROM_Int32_16(uint16_t addr, int32_t set_val)
{
    return UiWriteSettingEEPROM_UInt16(addr,(uint16_t)(int16_t)set_val);
}


static uint16_t UiWriteSettingsBandMode(const uint16_t i,const uint16_t band_mode, const uint16_t band_freq_high, const uint16_t band_freq_low, VfoReg* vforeg)
{

    // ------------------------------------------------------------------------------------
    // Read Band and Mode saved values - update if changed
    uint16_t value = (vforeg->decod_mode << 8) | (vforeg->digital_mode);
    uint16_t retval = UiWriteSettingEEPROM_UInt8x2(band_mode + i, value);

    // Try to read Freq saved values - update if changed
    if (retval == HAL_OK)
    {
        retval = UiWriteSettingEEPROM_UInt32(band_freq_high+i,band_freq_low+i, vforeg->dial_value);
    }

    return retval;
}

#if 0
static uint16_t UiWriteSettingEEPROM_UInt16_Array_2_Dim(uint16_t* array_ptr, const uint16_t base_idx, const size_t dim1, const size_t dim2)
{
    uint16_t retval = HAL_OK;

    for (uint16_t idx1 = 0; retval == HAL_OK && idx1 < dim1; idx1++)
    {
        for (uint16_t idx2 = 0; retval == HAL_OK && idx2 < dim2; idx2++)
        {
            retval = UiWriteSettingEEPROM_UInt16(base_idx+(idx1*dim2)+idx2, array_ptr+(idx1*dim2)+idx2);
        }
    }
    return retval;
}
#endif

static uint16_t UiWriteSettingEEPROM_Filter()
{
    uint16_t retval = HAL_OK;

    for (uint16_t idx = 0; retval == HAL_OK && idx < FILTER_MODE_MAX; idx++)
    {
        for (uint16_t mem_idx = 0; retval == HAL_OK && mem_idx < FILTER_PATH_MEM_MAX; mem_idx++)
        {
            retval = UiWriteSettingEEPROM_UInt16(EEPROM_FILTER_PATH_MAP_BASE+idx*FILTER_PATH_MEM_MAX+mem_idx,ts.filter_path_mem[idx][mem_idx]);
        }
    }
    return retval;
}

/**
 * Did the filter path memories already move to their own block?
 *
 * The block used to start at EEPROM_FILTER_PATH_MAP_LEGACY_BASE, where its 25 words covered the ids
 * of 22 single value settings. As long as the new block holds nothing that was ever written, those
 * memories are still read from the legacy address, which lets a radio keep what it has. Every path
 * index is smaller than AUDIO_FILTER_PATH_NUM, so an erased (or never written) word cannot be
 * confused with a stored value.
 *
 * @return true if no word of the current block was ever written
 */
static bool UiConfiguration_FilterMapNotStoredYet()
{
    for (uint16_t offset = 0; offset < EEPROM_FILTER_PATH_MAP_WORDS; offset++)
    {
        uint16_t value = 0xffff;      // a read that does not return a value counts as "not stored"
        if (ConfigStorage_ReadVariable(EEPROM_FILTER_PATH_MAP_BASE + offset, &value) == HAL_OK
                && value != 0xffff)
        {
            return false;
        }
    }
    return true;
}

void UiReadSettingEEPROM_Filter(bool load_default)
{
    int idx, mem_idx;
    uint16_t base = EEPROM_FILTER_PATH_MAP_BASE;

    if (config_layout_migration_active)
    {
        // A configuration of the previous layout kept its filter memories at the legacy base, which
        // is also the current position of that layout, so they are read from there and written to
        // the current block by the next save. The probe below must not decide this: it reads the
        // current block, and in the previous layout those words held the ids that now live at
        // 334..358 (scope, touchscreen calibration, CW decoder, entry count), so it would find
        // data there and report the block as already stored.
        base = EEPROM_FILTER_PATH_MAP_LEGACY_BASE;
    }
    else if (!load_default && UiConfiguration_FilterMapNotStoredYet())
    {
        base = EEPROM_FILTER_PATH_MAP_LEGACY_BASE;  // the memories of a configuration stored before the block moved
    }
    for (idx = 0; idx < FILTER_MODE_MAX; idx++)
    {
        for (mem_idx = 0; mem_idx < FILTER_PATH_MEM_MAX; mem_idx++)
        {
            UiReadSettingEEPROM_UInt16(base+idx*FILTER_PATH_MEM_MAX+mem_idx,&(ts.filter_path_mem[idx][mem_idx]),0,0,AUDIO_FILTER_PATH_NUM-1, load_default);
        }
    }
}

/**
 * compares the firmware version which saved the parameters to volatile memory and the given version number.
 * @param major range 0..255
 * @param minor range 0..255
 * @param release range 0..255
 * @return 0 if stored parameters have been saved by firmware identified by parameter, 1 if parameter were stored by older firmware, -1 if by newer
 */


static int32_t UiConfiguration_CompareConfigBuildVersions(uint major, uint32_t minor, uint32_t release)
{
    int32_t retval = 0;
    static uint16_t config_major = 0, config_minor = 0, config_release = 0;

    if (config_major == 0)
    {
        // while migrating, the version was stored at the address of the previous layout
        UiReadSettingEEPROM_UInt16(UiConfiguration_LayoutAddress(EEPROM_VERSION_MAJOR),&config_major,0,0,255, false);
        UiReadSettingEEPROM_UInt16(UiConfiguration_LayoutAddress(EEPROM_VERSION_MINOR),&config_minor,0,0,255, false);
        UiReadSettingEEPROM_UInt16(UiConfiguration_LayoutAddress(EEPROM_VERSION_RELEASE),&config_release,0,0,255, false);
    }

    if (major < config_major)
    {
        retval = -1;
    }
    else if (major > config_major)
    {
        retval = 1;
    }
    else if (minor < config_minor)
    {
        retval = -1;
    }
    else if (minor > config_minor)
    {
        retval = 1;
    }
    else if (release < config_release)
    {
        retval = -1;
    } else if (release > config_release)
    {
      retval = 1;
    }
    return retval;
}


void UiConfiguration_ReadConfigEntryData(const ConfigEntryDescriptor* ced_ptr, bool load_default, uint16_t addr)
{
    switch(ced_ptr->typeId & ConfigEntry_TypeMask)
    {

    case ConfigEntry_UInt8:
        UiReadSettingEEPROM_UInt8(addr,ced_ptr->val_ptr,ced_ptr->val_default,ced_ptr->val_min,ced_ptr->val_max, load_default);
        break;
    case ConfigEntry_UInt16:
        UiReadSettingEEPROM_UInt16(addr,ced_ptr->val_ptr,ced_ptr->val_default,ced_ptr->val_min,ced_ptr->val_max, load_default);
        break;
    case ConfigEntry_UInt32_16:
        UiReadSettingEEPROM_UInt32_16(addr,ced_ptr->val_ptr,ced_ptr->val_default,ced_ptr->val_min,ced_ptr->val_max, load_default);
        break;
    case ConfigEntry_Int32_16:
        UiReadSettingEEPROM_Int32_16(addr,ced_ptr->val_ptr,ced_ptr->val_default,ced_ptr->val_min,ced_ptr->val_max, load_default);
        break;
    case ConfigEntry_Int16:
        UiReadSettingEEPROM_Int16(addr,ced_ptr->val_ptr,ced_ptr->val_default,ced_ptr->val_min,ced_ptr->val_max, load_default);
        break;
    case ConfigEntry_UInt8x2:
        UiReadSettingEEPROM_UInt8x2(addr,ced_ptr->val_ptr,ced_ptr->val_default,ced_ptr->val_min,ced_ptr->val_max, load_default);
        break;
    case ConfigEntry_Int32:
    {
    	uint32_t Data;
    	uint16_t DataLo,DataHi;
    	UiReadSettingEEPROM_UInt16(addr,&DataHi,ced_ptr->val_default,ced_ptr->val_min,ced_ptr->val_max, load_default);
    	UiReadSettingEEPROM_UInt16(addr+1,&DataLo,ced_ptr->val_default,ced_ptr->val_min,ced_ptr->val_max, load_default);
    	Data=DataHi<<16|DataLo;
    	*(int32_t*)ced_ptr->val_ptr=(int32_t)Data;
    }
    	break;
//  case ConfigEntry_Bool:
//    UiReadSettingEEPROM_Bool(ced_ptr->id,ced_ptr->val_ptr,ced_ptr->val_default,ced_ptr->val_min,ced_ptr->val_max, load_default);
//    break;

    }
}
uint16_t UiConfiguration_WriteConfigEntryData(const ConfigEntryDescriptor* ced_ptr)
{
    uint16_t retval = HAL_ERROR;
    switch(ced_ptr->typeId & ConfigEntry_TypeMask)
    {

    case ConfigEntry_UInt8:
        retval = UiWriteSettingEEPROM_UInt16(ced_ptr->id,*(uint8_t*)ced_ptr->val_ptr);
        break;
    case ConfigEntry_UInt8x2:
        retval = UiWriteSettingEEPROM_UInt8x2(ced_ptr->id,*(uint16_t*)ced_ptr->val_ptr);
        break;
    case ConfigEntry_UInt16:
        retval = UiWriteSettingEEPROM_UInt16(ced_ptr->id,*(uint16_t*)ced_ptr->val_ptr);
        break;
    case ConfigEntry_UInt32_16:
        retval = UiWriteSettingEEPROM_UInt32_16(ced_ptr->id,*(uint32_t*)ced_ptr->val_ptr);
        break;
    case ConfigEntry_Int32_16:
        retval = UiWriteSettingEEPROM_Int32_16(ced_ptr->id,*(int32_t*)ced_ptr->val_ptr);
        break;
    case ConfigEntry_Int16:
        retval = UiWriteSettingEEPROM_Int16(ced_ptr->id,*(int16_t*)ced_ptr->val_ptr);
        break;
    case ConfigEntry_Int32:
    {
    	int32_t Data=*(int32_t*)ced_ptr->val_ptr;
    	uint16_t DataHi,DataLo;
    	DataHi=(uint16_t)((Data>>16)&0xffff);
    	DataLo=(uint16_t)(Data&0xffff);

        retval = UiWriteSettingEEPROM_UInt16(ced_ptr->id,DataHi);
        retval = UiWriteSettingEEPROM_UInt16(ced_ptr->id+1,DataLo);
    }
    	break;
//  case ConfigEntry_Bool:
//    UiWriteSettingEEPROM_Bool(ced_ptr->id,*(bool*)ced_ptr->val_ptr,ced_ptr->val_default);
//    break;
    }
    return retval;
}

uint16_t UiConfiguration_WriteConfigEntries()
{
    uint16_t retval = HAL_OK;
    int idx;

    // The stored entry count has to describe the layout of the firmware that writes it:
    // UiConfiguration_NeedsDefaultIndex() forces the default of every entry whose id is above
    // the stored count, because those ids did not exist in the firmware that saved them.
    // number_of_entries_cur_fw is never assigned anywhere else, it only holds whatever the last
    // save wrote back.  Without this line an upgraded radio re-writes the count of the previous
    // firmware for ever, and every entry added since then is reset on each boot.
    number_of_entries_cur_fw = EEPROM_FIRST_UNUSED;

    for (idx = 0; ConfigEntryInfo[idx].typeId != ConfigEntry_Stop ; idx++)
    {
        retval = UiConfiguration_WriteConfigEntryData(&ConfigEntryInfo[idx]);
    }
    return retval;
}

/**
 *
 * @param load_default shall we load defaults instead of the value from storage? true -> do it
 * @param force_default_above from which parameter index on shall we force loading defaults even if load_defaults is false
 *
 */
static void __attribute__ ((noinline)) UiConfiguration_ReadConfigEntries(bool load_default, int32_t force_default_above, bool migrate)
{
    int idx;
    for (idx = 0; ConfigEntryInfo[idx].typeId != ConfigEntry_Stop ; idx++)
    {
        bool load_this_default = (ConfigEntryInfo[idx].id>force_default_above) ? true : load_default;
        uint16_t addr = ConfigEntryInfo[idx].id;

        if (migrate)
        {
            uint16_t old_addr = UiConfiguration_OldLayoutAddress(addr);
            if (old_addr == CONFIG_LAYOUT_OLD_NO_VALUE)
            {
                // this value did not exist in the previous layout, use its default
                load_this_default = true;
            }
            else
            {
                addr = old_addr;
            }
        }
        UiConfiguration_ReadConfigEntryData(&ConfigEntryInfo[idx], load_this_default, addr);
    }
}

/**
 * Calculate based on current firmware version and stored configuration values, which parameters were not present in the stored configuration and
 * do need to be initialized / reset to the default value.
 * Code uses the stored parameter count (EEPROM_NUMBER_OF_ENTRIES and the version identifiers) to calculate a safe choice but useful value.
 * For older firmwares which did not support the EEPROM_NUMBER_OF_ENTRIES parameter, the initialization only covers parameters introduced after EEPROM_NUMBER_OF_ENTRIES.
 * This is mostly relevant for I2C EEPROM based parameter storage.
 *
 * @return the lowest parameter number, which does not need to reset/initialized to its default value. -1 indicates all values to default, INT32_MAX none
 */
static int32_t UiConfiguration_NeedsDefaultIndex()
{
    int32_t retval = INT32_MAX;
    uint16_t num_of_entries;

    if (ts.configstore_in_use == CONFIGSTORE_IN_USE_I2C)
    {
        UiReadSettingEEPROM_UInt16(EEPROM_NUMBER_OF_ENTRIES,&num_of_entries,0,0,0xfffe, false);
        if (num_of_entries == 0)
        {
            // we have a system which may be either uninitialized OR too old.
            if (UiConfiguration_CompareConfigBuildVersions(0,0,0) == 0)
            {
                // this is a new system, we can initialize it from the beginning
                retval = -1;
            }
            else
            {
                retval = EEPROM_NUMBER_OF_ENTRIES;
                // the index of the number of entries is as low as we can go here
                // anything above this entry can be initialized, since it did not exist before
            }
        }
        else
        {
            retval = num_of_entries;
            // the number of entries the currently stored configuration uses
            // must not be initialized, but anything above can.
        }
    }
    return retval;
}

/**
 * Fixes an specific issue with iq correction defaults not initialized correctly in version 2.9.85 / 2.9.86
 */
void UiConfiguration_FixDefaultsNotLoadedIssue()
{
    // we fix here an issue of our configuration approach which appears if you store int values and have the lower range below -1
    if (UiConfiguration_CompareConfigBuildVersions(2,9,87) == 1 && ts.configstore_in_use == CONFIGSTORE_IN_USE_I2C)
    {
        // values have been stored by older version of firmware
        uint32_t values[] = { IQ_20M, IQ_15M, IQ_10M_UP };
        bool not_all_minus_one = false;
        for (uint32_t i = 0; i < sizeof(values)/sizeof(values[0]) && not_all_minus_one == false ; i++)
        {
            not_all_minus_one = not_all_minus_one || (iq_adjust[values[i]].adj.tx[IQ_TRANS_ON].gain != -1 || iq_adjust[values[i]].adj.tx[IQ_TRANS_ON].phase != -1);
            not_all_minus_one = not_all_minus_one || (iq_adjust[values[i]].adj.tx[IQ_TRANS_OFF].gain != -1 || iq_adjust[values[i]].adj.tx[IQ_TRANS_OFF].phase != -1);
        }

        // okay, we have every value at -1 AND values stored in an older firmware than 2.9.86, we can safely rewrite all values to the right default
        if (not_all_minus_one == false)
        {
            for (uint32_t i = 0; i < sizeof(values)/sizeof(values[0]); i++)
            {
                iq_adjust[values[i]].adj.tx[IQ_TRANS_ON].gain = IQ_BALANCE_OFF;
                iq_adjust[values[i]].adj.tx[IQ_TRANS_ON].phase = IQ_BALANCE_OFF;
                iq_adjust[values[i]].adj.tx[IQ_TRANS_OFF].gain = IQ_BALANCE_OFF;
                iq_adjust[values[i]].adj.tx[IQ_TRANS_OFF].phase = IQ_BALANCE_OFF;
            }
        }
    }
}

/**
 * Loads values stored in non-volatile memory into ram.
 * @param load_freq_mode_defaults if true, frequency default values will be loaded instead of EEPROM values, all other values are untouched.
 * @param load_eeprom_defaults if true, default values will be loaded instead of EEPROM values.
 */
void UiConfiguration_LoadEepromValues(bool load_freq_mode_defaults, bool load_eeprom_defaults)
{
    ts.dsp.inhibit++;     // disable dsp while loading EEPROM data

    uint32_t value32;

    // A configuration stored with the previous EEPROM layout is read through the address
    // translation of that layout and written back in the current layout by the next save.
    config_layout_migration_active = !load_eeprom_defaults && UiConfiguration_LayoutMigrationRequired();
    const bool migrate = config_layout_migration_active;

    UiConfiguration_ReadConfigEntries(load_eeprom_defaults,
            migrate ? INT32_MAX : UiConfiguration_NeedsDefaultIndex(), migrate);

    // ------------------------------------------------------------------------------------
    // Try to read Band and Mode saved values, but read freq-limit-settings before
    uint8_t band_mem_idx;
    UiReadSettingEEPROM_UInt8x2_Split(EEPROM_BAND_MODE,
            &ts.dmod_mode, DEMOD_LSB, 0, DEMOD_MAX_MODE,
            &band_mem_idx, BAND_MODE_80, 0, migrate ? 0xff : MAX_BANDS -1,
            load_eeprom_defaults);

    if (migrate)
    {
        // the stored band memory index uses the previous band numbering
        band_mem_idx = UiConfiguration_NewBandSlot(band_mem_idx);
    }

    // restore the bandInfo for a given band info set and band memory index
    if (bandinfo_idx >= BAND_INFO_SET_NUM)
    {
        // if the stored band index is not available, we need to handle this
        // we cannot do this during reading as C does not allow us to use a const int as initializer constant
        bandinfo_idx = BAND_INFO_SET_DEFAULT; // reset to default
    }
    bandInfo = bandInfos[bandinfo_idx].bands;
    ts.band = RadioManagement_GetBandInfo(band_mem_idx);


    // demodulator mode might not be right for saved band!
    if(load_freq_mode_defaults)       // freq defaults to be loaded?
    {
        ts.dmod_mode = DEMOD_LSB;           // yes - set to LSB
    }

    // ------------------------------------------------------------------------------------
    // Try to read Freq saved values
    UiReadSettingEEPROM_UInt32(EEPROM_FREQ_HIGH,EEPROM_FREQ_LOW,&value32,0,0,0xffffffff, load_eeprom_defaults);
    {

        // We have loaded from eeprom the last used band, but can't just
        // load saved frequency, as it could be out of band, so do a
        // boundary check first (also check to see if defaults should be loaded)
        df.tune_new = UiConfiguration_LimitFrequency(ts.band, value32, load_eeprom_defaults || load_freq_mode_defaults);
    }
    // Try to read saved per-band values for frequency, mode and filter

    // the per band memory blocks start at a different address while migrating
    const uint16_t band_mode_a = UiConfiguration_BandMemBlockBase(EEPROM_BAND0_MODE_A);
    const uint16_t band_freq_high_a = UiConfiguration_BandMemBlockBase(EEPROM_BAND0_FREQ_HIGH_A);
    const uint16_t band_freq_low_a = UiConfiguration_BandMemBlockBase(EEPROM_BAND0_FREQ_LOW_A);
    const uint16_t band_mode_b = UiConfiguration_BandMemBlockBase(EEPROM_BAND0_MODE_B);
    const uint16_t band_freq_high_b = UiConfiguration_BandMemBlockBase(EEPROM_BAND0_FREQ_HIGH_B);
    const uint16_t band_freq_low_b = UiConfiguration_BandMemBlockBase(EEPROM_BAND0_FREQ_LOW_B);

    for(int i = 0; i < MAX_BANDS; i++)
    {
        bool load_band_defaults = load_eeprom_defaults || load_freq_mode_defaults;
        uint16_t mem_slot = i;

        if (migrate)
        {
            mem_slot = UiConfiguration_OldBandSlot(i);
            if (mem_slot == CONFIG_LAYOUT_OLD_NO_VALUE)
            {
                // the 11m band has no stored memories in the previous layout
                mem_slot = 0;
                load_band_defaults = true;
            }
        }

        // read from stored bands
        UiReadSettingsBandMode(i, mem_slot, band_mode_a, band_freq_high_a, band_freq_low_a, &vfo[VFO_A].band[i], load_band_defaults);
        UiReadSettingsBandMode(i, mem_slot, band_mode_b, band_freq_high_b, band_freq_low_b, &vfo[VFO_B].band[i], load_band_defaults);
    }


    UiReadSettingEEPROM_UInt32( UiConfiguration_LayoutAddress(EEPROM_XVERTER_OFFSET_HIGH),UiConfiguration_LayoutAddress(EEPROM_XVERTER_OFFSET_LOW),&ts.xverter_offset,0,0,XVERTER_OFFSET_MAX, load_eeprom_defaults);
    UiReadSettingEEPROM_UInt32( UiConfiguration_LayoutAddress(EEPROM_XVERTER_OFFSET_TX_HIGH),UiConfiguration_LayoutAddress(EEPROM_XVERTER_OFFSET_TX_LOW),&ts.xverter_offset_tx,0,0,XVERTER_OFFSET_MAX, load_eeprom_defaults);

    UiReadSettingEEPROM_Filter(load_eeprom_defaults);

    ConfigStorage_CopySerial2Array(EEPROM_KEYER_MEMORY_ADDRESS, (uint8_t *)ts.keyer_mode.macro, sizeof(ts.keyer_mode.macro));

    // post configuration loading actions below
    df.tuning_step  = tune_steps[df.selected_idx];
    ts.tx_gain[TX_AUDIO_LINEIN_R] = ts.tx_gain[TX_AUDIO_LINEIN_L];

    // this fixes an issue with bad initialization which happens if you had version 2.9.85 installed.
    UiConfiguration_FixDefaultsNotLoadedIssue();

    // from here on all version comparisons refer to the current layout again
    config_layout_migration_active = false;

    // TODO: Find a better solution: We need the negated setting for the menu to not display
    // disable rx iq settings in menu when autocorr is enabled
    ts.display_rx_iq = ts.iq_auto_correction == 0;



    if (ts.dsp.inhibit) { ts.dsp.inhibit--; } // restore setting
}

// ********************************************************************************************************************
//
//*----------------------------------------------------------------------------
//* Function Name       : UiDriverSaveEepromValues
//* Object              : save all values to EEPROM - called on power-down.  Does not check to see if they have changed
//* Input Parameters    :
//* Output Parameters   :
//* Functions called    :
//*----------------------------------------------------------------------------

uint16_t UiConfiguration_SaveEepromValues(void)
{
    uint16_t i, retval = 0x0;

    if(ts.txrx_mode != TRX_MODE_RX)
    {
        retval = 0xFF00;
    }
    else
    {
        const uint8_t dmod_mode = ts.dmod_mode;

        // TODO: THIS IS UGLY: We are switching to RAM based storage in order to gain speed
        // because we then can bulk write the data into the I2C later.
        // we don't do this for flash, since we cannot gain anything here.

        if(ts.configstore_in_use == CONFIGSTORE_IN_USE_I2C)
        {
            ConfigStorage_CopySerial2RAMCache();
        }

        if(RadioManagement_IsGenericBand(ts.band) == false && ts.cat_band_index == 255)			// not in a sandbox
        {
            // save current band/frequency/mode settings
            vfo[get_active_vfo()].band[ts.band->band_mode].dial_value = df.tune_new;
            // Save decode mode
            vfo[get_active_vfo()].band[ts.band->band_mode].decod_mode = dmod_mode;

            // TODO: move value to a static variable, so that it can be read/written with standard approach
            retval = UiWriteSettingEEPROM_UInt8x2(EEPROM_BAND_MODE,
                                       ((uint16_t)ts.band->band_mode| dmod_mode << 8));

            // TODO: move value to a static variable, so that it can be read/written with standard approach
            if (retval == HAL_OK)
            {
                retval = UiWriteSettingEEPROM_UInt32(EEPROM_FREQ_HIGH,EEPROM_FREQ_LOW, df.tune_new);
            }
        }
        else
        {
            ts.cat_band_index = 255;
        }

        // Save stored band/mode/frequency memory from RAM
        for(i = 0; retval == HAL_OK && i < MAX_BANDS; i++)      // scan through each band's frequency/mode data
        {
            // UiWriteSettingsBandMode(i,EEPROM_BAND0_MODE,EEPROM_BAND0_FREQ_HIGH,EEPROM_BAND0_FREQ_LOW,  &vfo[VFO_WORK].band[i]);
            retval = UiWriteSettingsBandMode(i,EEPROM_BAND0_MODE_A,EEPROM_BAND0_FREQ_HIGH_A,EEPROM_BAND0_FREQ_LOW_A, &vfo[VFO_A].band[i]);
            if (retval == HAL_OK)
            {
                UiWriteSettingsBandMode(i,EEPROM_BAND0_MODE_B,EEPROM_BAND0_FREQ_HIGH_B,EEPROM_BAND0_FREQ_LOW_B, &vfo[VFO_B].band[i]);
            }
        }

        if (retval == HAL_OK)
        {
            retval = UiConfiguration_WriteConfigEntries();
        }


        if (retval == HAL_OK)
        {
            retval = UiWriteSettingEEPROM_UInt32(EEPROM_XVERTER_OFFSET_HIGH,EEPROM_XVERTER_OFFSET_LOW,ts.xverter_offset);
        }
        if (retval == HAL_OK)
        {
            retval = UiWriteSettingEEPROM_UInt32(EEPROM_XVERTER_OFFSET_TX_HIGH,EEPROM_XVERTER_OFFSET_TX_LOW,ts.xverter_offset_tx);
        }

        if (retval == HAL_OK)
        {
            retval = UiWriteSettingEEPROM_Filter();
        }

        if (retval == HAL_OK)
        {
            // all values have been written in the current layout: mark the stored
            // configuration as migrated so the previous layout addresses are no longer used
            retval = ConfigStorage_WriteVariable(EEPROM_CONFIG_LAYOUT_VERSION, CONFIG_LAYOUT_VERSION_MAGIC);
        }

        if(retval == HAL_OK && ts.configstore_in_use == CONFIGSTORE_IN_USE_RAMCACHE)
        {
            retval = ConfigStorage_CopyRAMCache2Serial();
            // write ram cache to EEPROM and switch back to I2C EEPROM use
        }

        retval = ConfigStorage_CopyArray2Serial(EEPROM_KEYER_MEMORY_ADDRESS, (uint8_t *)ts.keyer_mode.macro, sizeof(ts.keyer_mode.macro));

    }
    return retval;
}
