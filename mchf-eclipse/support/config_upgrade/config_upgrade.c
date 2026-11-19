/* generated from the repository headers by gen_config_upgrade.py, do not edit */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define FIRST_UNUSED 416u
#define PREVIOUS_FIRST_UNUSED 383u
#define SAVE_RECORDS_ENTRY_COUNT 1

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
static int check_agc_roundtrip(void)
{
    int rc = 0;
    for (unsigned i = 0; i < agc_entry_count; i++)
    {
        const agc_entry_t* e = &agc_entries[i];
        const int32_t probes[] = { e->def, e->min, e->max, e->menu_min, e->menu_max, e->menu_def };
        for (unsigned p = 0; p < sizeof(probes)/sizeof(probes[0]); p++)
        {
            const int32_t want = probes[p];
            if ((want < e->min) || (want > e->max)) continue;
            if (e->menu_max < 0 && p >= 3) continue;
            if (agc_load_bits(e, agc_store_bits(e, want)) != want)
            {
                printf("CHECK FAILED: %s cannot keep %d in its storage slot\n", e->idname, (int)want);
                rc = 1;
                continue;
            }
            for (unsigned s = 0; s < 2; s++)
            {
                image_t img;
                memset(&img, 0, sizeof(img));
                for (unsigned k = 0; k < sizeof(img.value)/sizeof(img.value[0]); k++)
                {
                    img.value[k] = 0xFFFF;
                }
                agc_save(&img, e, want);
                const int32_t got = agc_read(&img, e, false, (store_t)s);
                if (got != want)
                {
                    printf("CHECK FAILED: %s: menu value %d does not survive save/load (%s reads %d)\n",
                           e->idname, (int)want, s ? "flash" : "i2c eeprom", (int)got);
                    rc = 1;
                }
            }
        }
        if (e->menu_max >= 0)
        {
            if ((e->menu_min < e->min) || (e->menu_max > e->max))
            {
                printf("CHECK FAILED: %s can be set to %d..%d but only %d..%d is stored\n",
                       e->idname, (int)e->menu_min, (int)e->menu_max, (int)e->min, (int)e->max);
                rc = 1;
            }
        }
        if ((e->def < e->min) || (e->def > e->max))
        {
            printf("CHECK FAILED: %s default %d is outside its stored range %d..%d\n",
                   e->idname, (int)e->def, (int)e->min, (int)e->max);
            rc = 1;
        }
    }
    return rc;
}

/* The upgrade case: the configuration on the radio was written by a firmware whose entry count
   was below the id of a value that exists now. A value the operator sets after the upgrade has to
   survive the next power cycle, which means the save has to record the ceiling of the current
   firmware, not the one it read. */
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
    const char* const shape[2] = { "previous layout", "layout before this one, block unwritten" };
    image_t img[2];
    int rc = 0;

    /* the two configurations a migrating radio can be holding */
    for (unsigned c = 0; c < 2; c++)
    {
        memset(&img[c], 0, sizeof(img[c]));
        for (unsigned i = 0; i < sizeof(img[c].value)/sizeof(img[c].value[0]); i++)
        {
            img[c].value[i] = 0xFFFF;
        }
        for (unsigned k = 0; k < FILTER_MAP_WORDS; k++)
        {
            img[c].present[FILTER_MAP_LEGACY + k] = true;
            img[c].value[FILTER_MAP_LEGACY + k] = (uint16_t)(1 + k);   /* the operator memories */
        }
        if (c == 0)
        {
            /* the previous layout kept the ids that now live at 334..358 at the words of the block */
            for (unsigned k = 0; k < FILTER_MAP_WORDS; k++)
            {
                img[c].present[FILTER_MAP_BASE + k] = true;
                img[c].value[FILTER_MAP_BASE + k] = (uint16_t)(40 + k);
            }
        }
    }

    for (unsigned c = 0; c < 2; c++)
    {
        uint8_t mem[FILTER_MAP_WORDS];
        image_t after;

        /* the boot that migrates: the memories come from the legacy base */
        load_filter_memories(&img[c], true, mem);
        for (unsigned k = 0; k < FILTER_MAP_WORDS; k++)
        {
            if (mem[k] != (uint8_t)(1 + k))
            {
                printf("CHECK FAILED: the migrated boot of a %s configuration reads memory %u as %u, "
                       "expected %u\n", shape[c], k, mem[k], 1 + k);
                rc = 1;
                break;
            }
        }

        /* the radio saves; the next boot is a plain boot and has to find the block */
        after = img[c];
        save_filter_memories(&after, mem);
        const uint16_t base = filter_map_base(&after, false, false);
        if (base != FILTER_MAP_BASE)
        {
            printf("CHECK FAILED: after saving a migrated configuration (%s) the next boot still "
                   "reads the filter path memories from %u instead of %u, so the memories it shows "
                   "are not the ones it stored\n", shape[c], base, FILTER_MAP_BASE);
            rc = 1;
            continue;
        }
        load_filter_memories(&after, false, mem);
        for (unsigned k = 0; k < FILTER_MAP_WORDS; k++)
        {
            if (mem[k] != (uint8_t)(1 + k))
            {
                printf("CHECK FAILED: after saving a migrated configuration (%s) filter memory %u "
                       "reads %u, expected %u\n", shape[c], k, mem[k], 1 + k);
                rc = 1;
                break;
            }
        }
    }

    if (rc == 0)
    {
        printf("filter path memories: a migrated configuration is saved at %u..%u and the next boot "
               "reads it from there\n\n", FILTER_MAP_BASE, FILTER_MAP_BASE + FILTER_MAP_WORDS - 1);
    }
    return rc;
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

    rc |= check_agc_roundtrip();
    rc |= check_upgrade_persistence();
    rc |= check_save_order();
    rc |= check_filter_map_migration();
    rc |= check_filter_map_save();
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
