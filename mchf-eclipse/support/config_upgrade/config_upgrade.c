/* generated from the repository headers by gen_config_upgrade.py, do not edit */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define FIRST_UNUSED 390u
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
