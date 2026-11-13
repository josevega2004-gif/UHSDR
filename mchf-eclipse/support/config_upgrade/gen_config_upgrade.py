#!/usr/bin/env python3
#
# Configuration storage upgrade check for the mcHF / UHSDR firmware
#
# Reads the value ids, defaults and ranges of the AM configuration values straight from the
# firmware sources and generates config_upgrade.c, a host program that replays the configuration
# load path over configurations stored by older builds. It shows which values an existing
# configuration receives as defaults and which are loaded from storage.
#
# Usage:  ./gen_config_upgrade.py && gcc -O2 -Wall -o config_upgrade config_upgrade.c && ./config_upgrade
#
# Rerun after adding, removing or renumbering configuration values (ui_configuration.h and the
# ConfigEntry table in ui_configuration.c).
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#

import re

from pathlib import Path

# the firmware sources are two directories above this script (support/config_upgrade)
root = str(Path(__file__).resolve().parents[2]) + '/'

cfg_h = open(root+'drivers/ui/ui_configuration.h', encoding='utf-8', errors='surrogateescape').read()
cfg_c = open(root+'drivers/ui/ui_configuration.c', encoding='utf-8', errors='surrogateescape').read()
aud_h = open(root+'drivers/audio/audio_driver.h', encoding='utf-8', errors='surrogateescape').read()
rm_h  = open(root+'drivers/ui/radio_management.h', encoding='utf-8', errors='surrogateescape').read()

def defines(text):
    return {m.group(1): int(m.group(2))
            for m in re.finditer(r'^#define\s+(\w+)\s+\(?\s*(-?\d+)\s*\)?', text, re.M)}

vals = defines(cfg_h)
vals.update({k: v for k, v in defines(aud_h).items() if k not in vals})
enum_names = re.findall(r'^\s*(AmSideband_\w+)(?:\s*=\s*(\d+))?\s*,', rm_h, re.M)
nextval = 0
for name, explicit in enum_names:
    if name == 'AmSideband_Num':
        continue
    nextval = int(explicit) if explicit else nextval
    vals[name] = nextval
    nextval += 1

kinds = {                      # how a stored value is shown to the operator
 'am_carrier_level':        ('KIND_PERCENT',      '"carrier level"'),
 'am_tx_sideband':          ('KIND_SIDEBAND',     '"AM TX sideband"'),
 'am_carrier_pep_hold':     ('KIND_HALF_SECONDS', '"meter hold"'),
 'am_carrier_pep_dim':      ('KIND_ON_OFF',       '"meter dim"'),
 'am_carrier_pep_target':   ('KIND_DB',           '"PEP target"'),
 'am_carrier_pep_warn':     ('KIND_HALF_SECONDS', '"warn delay"'),
}

entries = []
for m in re.finditer(r'\{\s*ConfigEntry_UInt8\s*,\s*(EEPROM_AM_\w+)\s*,\s*&ts\.(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*\}', cfg_c):
    idname, var, d, lo, hi = m.groups()
    num = lambda t: int(t) if re.fullmatch(r'-?\d+', t) else vals[t]
    entries.append((var, idname, vals[idname], num(d), num(lo), num(hi)))

first_unused = vals['EEPROM_FIRST_UNUSED']
print('parsed from the repository:')
for var, idname, eid, d, lo, hi in entries:
    print('  %-22s %-26s id %3d  default %3d  range %3d..%3d' % (var, idname, eid, d, lo, hi))
print('  EEPROM_FIRST_UNUSED = %d (stored entry count of the current build)\n' % first_unused)

L = []
A = L.append
A('/* generated from the repository headers by gen_config_upgrade.py, do not edit */')
A('#include <stdio.h>')
A('#include <stdint.h>')
A('#include <stdbool.h>')
A('#include <string.h>')
A('')
A('#define FIRST_UNUSED %du' % first_unused)
A('')
A('typedef enum { KIND_PERCENT, KIND_SIDEBAND, KIND_HALF_SECONDS, KIND_ON_OFF, KIND_DB } kind_t;')
A('typedef struct { const char* name; const char* idname; uint16_t id; int16_t def, min, max; kind_t kind; } entry_t;')
A('static const entry_t entries[] = {')
for var, idname, eid, d, lo, hi in entries:
    kind, label = kinds[var]
    A('    { %s, "%s", %du, %d, %d, %d, %s },' % (label, idname, eid, d, lo, hi, kind))
A('};')
A('static const unsigned entry_count = sizeof(entries)/sizeof(entries[0]);')
A('')
A('typedef enum { STORE_I2C, STORE_FLASH } store_t;')
A('typedef struct { uint16_t value[1024]; bool present[1024]; } image_t;')
A('')
A('/* a value an older firmware never stored is simply erased in the image */')
A('static void erase(image_t* img, uint16_t id)')
A('{')
A('    img->value[id] = 0xFFFF;')
A('    img->present[id] = false;')
A('}')
A('')
A('/* mirror of UiReadSettingEEPROM_UInt8(): a failed read, an out of range value or a requested')
A('   default loads the default, everything else is used as stored */')
A('static uint8_t read_uint8(const image_t* img, const entry_t* e, bool load_default, store_t store)')
A('{')
A('    if (store == STORE_FLASH && !img->present[e->id])')
A('    {')
A('        return (uint8_t)e->def;         /* Flash_ReadVariable() reports a value that was never stored */')
A('    }')
A('    const uint16_t value = img->value[e->id];')
A('    if ((value < e->min) || (value > e->max) || load_default)')
A('    {')
A('        return (uint8_t)e->def;')
A('    }')
A('    return (uint8_t)value;')
A('}')
A('')
A('/* mirror of UiConfiguration_NeedsDefaultIndex(): values above the stored entry count did not')
A('   exist in the firmware that stored the configuration, on flash the count is not used */')
A('static int32_t needs_default_index(uint16_t stored_count, store_t store)')
A('{')
A('    return (store == STORE_I2C) ? (int32_t)stored_count : INT32_MAX;')
A('}')
A('')
A('static const char* describe(const entry_t* e, uint8_t v)')
A('{')
A('    static char txt[24];')
A('    switch (e->kind)')
A('    {')
A('    case KIND_HALF_SECONDS: snprintf(txt, sizeof(txt), v ? "%u.%u s" : "OFF", v / 2, (v % 2) * 5); break;')
A('    case KIND_ON_OFF:       snprintf(txt, sizeof(txt), "%s", v ? "ON" : "OFF"); break;')
A('    case KIND_DB:           snprintf(txt, sizeof(txt), "-%u.%u dB", v / 2, (v % 2) * 5); break;')
A('    case KIND_PERCENT:      snprintf(txt, sizeof(txt), "%u%%", v); break;')
A('    default:                snprintf(txt, sizeof(txt), "%s", v == 0 ? "BOTH" : (v == 1 ? "LSB" : "USB")); break;')
A('    }')
A('    return txt;')
A('}')
A('')
A('static void run(const char* title, const image_t* img, uint16_t stored_count, store_t store, bool load_defaults)')
A('{')
A('    const int32_t force_above = needs_default_index(stored_count, store);')
A('    printf("%s\\n  stored entry count %u, %s%s\\n", title, stored_count,')
A('           store == STORE_I2C ? "serial EEPROM / RAM cache" : "flash",')
A('           load_defaults ? ", factory defaults requested" : "");')
A('    for (unsigned i = 0; i < entry_count; i++)')
A('    {')
A('        const entry_t* e = &entries[i];')
A('        const bool forced = (e->id > force_above);')
A('        const bool present = img->present[e->id];')
A('        const uint16_t raw = img->value[e->id];')
A('        const uint8_t v = read_uint8(img, e, forced || load_defaults, store);')
A('        char stored[16];')
A('        if (present && raw != 0xFFFF) snprintf(stored, sizeof(stored), "%u", raw);')
A('        else                          snprintf(stored, sizeof(stored), "erased");')
A('        const char* why =')
A('            (e->id > force_above)          ? "not in the stored configuration" :')
A('            load_defaults                  ? "defaults requested" :')
A('            (store == STORE_FLASH && !present) ? "never written, read fails" :')
A('            (!present || raw == 0xFFFF)    ? "never written, out of range" :')
A('            (raw < e->min || raw > e->max) ? "stored value out of range" : "loaded from storage";')
A('        printf("    %-16s id %3u  stored %-8s -> %3u  %-8s %s\\n",')
A('               e->name, e->id, stored, v, describe(e, v), why);')
A('    }')
A('    printf("\\n");')
A('}')
A('')
A('int main(void)')
A('{')
A('    image_t img;')
A('    memset(&img, 0, sizeof(img));')
A('    for (unsigned i = 0; i < sizeof(img.value)/sizeof(img.value[0]); i++)')
A('    {')
A('        img.value[i] = 0xFFFF;          /* erased storage reads as 0xFFFF */')
A('    }')
for var, idname, eid, d, lo, hi in entries:
    value = { 'am_carrier_level': 80, 'am_tx_sideband': 2, 'am_carrier_pep_hold': 10,
              'am_carrier_pep_dim': 0, 'am_carrier_pep_target': 8, 'am_carrier_pep_warn': 2 }[var]
    A('    img.present[%d] = true; img.value[%d] = %d;   /* %s */' % (eid, eid, value, idname))
A('')
ids = {var: eid for var, idname, eid, d, lo, hi in entries}
A('    image_t before_meter      = img;   /* stored by the build without the meter settings */')
for var in ('am_carrier_pep_hold', 'am_carrier_pep_dim', 'am_carrier_pep_target', 'am_carrier_pep_warn'):
    A('    erase(&before_meter, %d);   /* %s */' % (ids[var], var))
A('    image_t before_warn       = img;   /* stored by the build without the warning delay */')
A('    erase(&before_warn, %d);   /* am_carrier_pep_warn */' % ids['am_carrier_pep_warn'])
A('    image_t upstream          = before_meter;  /* no AM additions at all */')
for var in ('am_carrier_level', 'am_tx_sideband'):
    A('    erase(&upstream, %d);   /* %s */' % (ids[var], var))
A('')
A('    run("upstream configuration, none of the AM additions present", &upstream, 375, STORE_I2C, false);')
A('    run("fork build before the meter settings", &before_meter, %d, STORE_I2C, false);' % ids['am_carrier_pep_hold'])
A('    run("fork build before the warning delay", &before_warn, %d, STORE_I2C, false);' % ids['am_carrier_pep_warn'])
A('    run("current build, everything stored", &img, FIRST_UNUSED, STORE_I2C, false);')
A('    run("current build on flash, warning delay never written", &before_warn, FIRST_UNUSED, STORE_FLASH, false);')
A('    run("factory reset of the current build", &img, FIRST_UNUSED, STORE_I2C, true);')
A('    return 0;')
A('}')
out_file = Path(__file__).resolve().parent / 'config_upgrade.c'
open(out_file,'w').write('\n'.join(L)+'\n')
print('wrote %s' % out_file)
