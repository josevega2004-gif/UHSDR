/* generated from the repository headers by gen_config_upgrade.py, do not edit */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define FIRST_UNUSED 383u

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

    run("upstream configuration, none of the AM additions present", &upstream, 375u, STORE_I2C, false);
    run("fork build before the meter settings", &before_meter, 379, STORE_I2C, false);
    run("fork build before the warning delay", &before_warn, 382, STORE_I2C, false);
    run("current build, everything stored", &img, FIRST_UNUSED, STORE_I2C, false);
    run("current build on flash, warning delay never written", &before_warn, FIRST_UNUSED, STORE_FLASH, false);
    run("factory reset of the current build", &img, FIRST_UNUSED, STORE_I2C, true);

    /* the self checks which make this program fail, so that CI catches a broken configuration */
    int rc = 0;
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
