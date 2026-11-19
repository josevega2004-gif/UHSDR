#!/usr/bin/env python3
#
# Configuration storage upgrade check for the mcHF / UHSDR firmware
#
# Reads the value ids, defaults and ranges of the AM configuration values and of the WDSP AGC
# parameters straight from the firmware sources and generates config_upgrade.c, a host program that
# replays the configuration load path over configurations stored by older builds. It shows which
# values an existing configuration receives as defaults and which are loaded from storage, and it
# checks that every AGC value the operator can change in the menu is stored, survives a save/load
# round trip and keeps the range the menu offers.
#
# It also compiles the address translation of the previous layout, UiConfiguration_OldLayoutAddress()
# in ui_configuration.c, and compares its output with the map of that layout and the map of the
# current one: a wrong branch there reads every migrated radio from the wrong words, and nothing
# else in the tree would notice.
#
# Usage:  ./gen_config_upgrade.py && gcc -O2 -Wall -o config_upgrade config_upgrade.c && ./config_upgrade
#
# Rerun after adding, removing or renumbering configuration values (ui_configuration.h and the
# ConfigEntry table in ui_configuration.c), after changing an AGC menu item range or after changing
# the AGC parameter struct in audio_agc.h.
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
import sys

import os
from pathlib import Path

import layout_digest      # the self digest of a layout record, see layout_digest.py

# the firmware sources are two directories above this script (support/config_upgrade)
root = str(Path(__file__).resolve().parents[2]) + '/'

# --record-layout rewrites the LAYOUT_HISTORY entry of the marker this build writes and stops there.
# It is the one command for a change that only renamed a stored value or made one visible to the
# table parser: neither moves a word, so neither needs a new marker (see the layout guard below).
RECORD_LAYOUT = '--record-layout' in sys.argv[1:]

cfg_h = open(root+'drivers/ui/ui_configuration.h', encoding='utf-8', errors='surrogateescape').read()
cfg_c = open(root+'drivers/ui/ui_configuration.c', encoding='utf-8', errors='surrogateescape').read()
aud_h = open(root+'drivers/audio/audio_driver.h', encoding='utf-8', errors='surrogateescape').read()
rm_h  = open(root+'drivers/ui/radio_management.h', encoding='utf-8', errors='surrogateescape').read()
agc_h = open(root+'drivers/audio/audio_agc.h', encoding='utf-8', errors='surrogateescape').read()
menu_c = open(root+'drivers/ui/menu/ui_menu.c', encoding='utf-8', errors='surrogateescape').read()

def strip_comments(text):
    text = re.sub(r'//.*', '', text)
    return re.sub(r'/\*.*?\*/', '', text, flags=re.S)

def call_arguments(text, name_pattern):
    """arguments of every call of name_pattern in text, found by matching the brackets instead of
       by a regex over the argument list: one greedy match would otherwise swallow the calls that
       follow it and silently reduce the number of items that are checked"""
    calls = []
    for m in re.finditer(name_pattern, text):
        i = text.index('(', m.start())
        depth = 0
        end = i
        while end < len(text):
            if text[end] == '(':
                depth += 1
            elif text[end] == ')':
                depth -= 1
                if depth == 0:
                    break
            end += 1
        inner = text[i + 1:end]
        args = []
        depth = 0
        cur = ''
        for ch in inner:
            if ch in '([':
                depth += 1
            elif ch in ')]':
                depth -= 1
            if ch == ',' and depth == 0:
                args.append(cur.strip())
                cur = ''
            else:
                cur += ch
        args.append(cur.strip())
        calls.append(args)
    return calls


def defines(text):
    """integer #define constants; hex and parenthesised values included, but only when the whole
       body is one number (an expression is handled by define_exprs below)"""
    out = {}
    for m in re.finditer(r'^\s*#define\s+(\w+)\s+\(?\s*(-?\d+|0[xX][0-9a-fA-F]+)\s*\)?\s*(?://.*)?$', text, re.M):
        out[m.group(1)] = int(m.group(2), 0)
    return out

def define_exprs(text):
    """#define constants whose body is a small arithmetic expression, e.g. (1 << 3) or (0x0f|0x10)"""
    out = {}
    for m in re.finditer(r'^[ \t]*#define[ \t]+(\w+)[ \t]+(\(?[^{}\n]*\)?)[ \t]*(?://.*)?$',
                         text, re.M):
        name, body = m.group(1), m.group(2).strip()
        # no \s between the name and the body: an include guard has no body, and matching across
        # the newline made it swallow the definition on the next line
        if not body or ',' in body or not re.fullmatch(r'[\w\s().\+\-*/%<>|&^~]+', body):
            continue
        if re.search(r'\b(defined|if|else|return|sizeof)\b', body):
            continue
        out.setdefault(name, body)
    return out

def enum_values(text):
    """names of the enums in a header with their values, as far as they can be worked out here"""
    out = {}
    for body in re.findall(r'enum[^{;{}]*\{([^{}]*)\}', text, re.S):
        body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
        nxt = 0
        for item in [i.strip() for i in body.split(',') if i.strip()]:
            m = re.match(r'(\w+)\s*(?:=\s*(.+))?$', item, re.S)
            if not m:
                nxt += 1
                continue
            out.setdefault(m.group(1), m.group(2) if m.group(2) else str(nxt))
            try:
                nxt = int(m.group(2)) + 1 if m.group(2) else nxt + 1
            except Exception:
                nxt = nxt + 1
    return out

CONST_HEADERS = ('drivers/ui/ui_configuration.h', 'drivers/audio/audio_driver.h',
                 'hardware/uhsdr_board.h', 'drivers/ui/radio_management.h',
                 'drivers/audio/cw/cw_decoder.h', 'drivers/audio/audio_management.h',
                 'drivers/ui/menu/ui_menu_internal.h', 'drivers/audio/audio_agc.h')

const_exprs = {}
for header in CONST_HEADERS:
    try:
        text = open(root + header, encoding='utf-8', errors='surrogateescape').read()
    except OSError:
        continue
    text = strip_comments(text)
    const_exprs.update(define_exprs(text))
    const_exprs.update(enum_values(text))

# Every other header of the tree as well. A name that two headers define differently is dropped:
# guessing which definition the build sees would put a wrong range into the check, and a wrong
# range is worse than a row that is reported as uncovered.
const_bodies = {}
for name, body in list(const_exprs.items()):
    const_bodies.setdefault(name, set()).add(body)
for dirpath, _dirs, files in os.walk(root):
    if any(part in dirpath for part in ('/support/', '/basesw/', '/build/', '/.git')):
        continue
    for fn in sorted(files):
        if not fn.endswith('.h'):
            continue
        try:
            text = open(os.path.join(dirpath, fn), encoding='utf-8',
                        errors='surrogateescape').read()
        except OSError:
            continue
        text = strip_comments(text)
        for name, body in list(define_exprs(text).items()) + list(enum_values(text).items()):
            const_bodies.setdefault(name, set()).add(body)
const_conflicts = {name for name, bodies in const_bodies.items() if len(bodies) > 1}
for name, bodies in const_bodies.items():
    if name not in const_conflicts:
        const_exprs[name] = next(iter(bodies))
for name in const_conflicts:
    const_exprs.pop(name, None)
# stdint.h is not part of the tree, its limits are what a few ranges are written with
const_exprs.update({'INT8_MIN': '-128', 'INT8_MAX': '127', 'UINT8_MAX': '255',
                    'INT16_MIN': '-32768', 'INT16_MAX': '32767', 'UINT16_MAX': '65535',
                    'INT32_MIN': '-2147483648', 'INT32_MAX': '2147483647',
                    'UINT32_MAX': '4294967295'})
print('  constants: %d names from the tree headers, %d left out because two headers disagree'
      % (len(const_exprs), len(const_conflicts)))

def resolve(expr, _seen=None):
    """value of a constant as the compiler would see it, None when it cannot be worked out here"""
    expr = expr.strip()
    expr = re.sub(r'\bCONFIG_UINT8x2_COMBINE\s*\(\s*([^,()]+)\s*,\s*([^,()]+)\s*\)', r'(\1 << 8 | \2)', expr)
    if re.fullmatch(r'-?(?:0[xX][0-9A-Fa-f]+|0[bB][01]+|\d+)', expr):
        return int(expr, 0)          # a literal, including the hex and binary ones a header uses
    if re.fullmatch(r'-?\d+', expr):
        return int(expr)
    if re.fullmatch(r'-?\d*\.\d+(?:[eE][-+]?\d+)?', expr):
        return int(float(expr))      # a scale factor such as TX_POWER_FACTOR_MAX_INTERNAL
    _seen = _seen or set()
    if expr in _seen:
        return None
    _seen = _seen | {expr}
    if re.fullmatch(r'[A-Za-z_]\w*', expr) and expr not in vals:
        if expr not in const_exprs:
            return None
        v = resolve(const_exprs[expr], _seen)
        if v is not None:
            vals[expr] = v
        return v
    m = re.fullmatch(r'\(?(\w+)\)?\s*([*/|&^+\-]|<<|>>)\s*\(?(\w+)\)?', expr)  # NAME op NAME or NAME op 10
    if m:
        a = resolve(m.group(1), _seen)
        btok = m.group(3)
        b = int(btok, 0) if re.fullmatch(r'-?\d+', btok) else resolve(btok, _seen)
        if a is not None and b is not None:
            op = m.group(2)
            try:
                if op == '*':
                    return a * b
                if op == '+':
                    return a + b
                if op == '-':
                    return a - b
                if op == '/':
                    return a // b
                if op == '|':
                    return a | b
                if op == '&':
                    return a & b
                if op == '^':
                    return a ^ b
                if op == '<<':
                    return a << b
                if op == '>>':
                    return a >> b
            except ZeroDivisionError:
                return None
        # an operand that cannot be worked out here: the general evaluation below tries the whole
        # expression again with every name replaced by its value
    # an expression of any shape - (A | B) & ~C, a ternary, a cast - with every name in it
    # replaced by its value. Without this a range written as (FLAGS1_A|FLAGS1_B) stayed unresolved
    # because those names are constants of a header, not of the file the value is stored in.
    env = {}
    for name in set(re.findall(r'\b[A-Za-z_]\w*\b', expr)):
        if name in vals:
            env[name] = vals[name]
            continue
        v = resolve(name, _seen)
        if v is None:
            return None
        env[name] = v
    try:
        return int(eval(expr, {'__builtins__': {}}, env))
    except Exception:
        return None

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

# how many value slots the host image provides, must cover every id used below
IMAGE_SLOTS = 1024

# guard against a silently empty or wrong parse: a passing check with no entries means nothing
if not entries:
    sys.exit('no ConfigEntry_UInt8 / EEPROM_AM_ entries matched the table in ui_configuration.c, '
             'the table layout may have changed, update the regex in gen_config_upgrade.py')
for var, idname, eid, d, lo, hi in entries:
    if eid >= first_unused:
        sys.exit('%s has id %d but EEPROM_FIRST_UNUSED is only %d, fix ui_configuration.h' % (idname, eid, first_unused))
    if eid >= IMAGE_SLOTS:
        sys.exit('%s has id %d which does not fit the host image of %d slots' % (idname, eid, IMAGE_SLOTS))

# a value the older firmware stored in the demo images, different from the default so that the
# check can tell a loaded value apart from a defaulted one
demo_values = { 'am_carrier_level': 80, 'am_tx_sideband': 2, 'am_carrier_pep_hold': 10,
                'am_carrier_pep_dim': 0, 'am_carrier_pep_target': 8, 'am_carrier_pep_warn': 2 }
for var, idname, eid, d, lo, hi in entries:
    if var not in kinds:
        sys.exit("add '%s' (%s) to the kinds table in gen_config_upgrade.py" % (var, idname))
    if var not in demo_values:
        sys.exit("add a demo value for '%s' (%s) to demo_values in gen_config_upgrade.py" % (var, idname))



# a hypothetical early build of the current (post renumbering) layout which stored 375 entries.
# Real configurations from the previous layout hold 432 entries and are not identified through
# the stored entry count at all, the firmware recognizes them by the missing layout marker and
# reads them through the address translation of UiConfiguration_LayoutMigrationRequired.
upstream_first_unused = 375

# ---------------------------------------------------------------------------------------------
# the WDSP AGC parameters
#
# Every value the operator can change in the AGC menu has to have an entry in the ConfigEntry
# table, otherwise it silently returns to its initial value at the next power cycle. The hang time
# and the hang threshold of the AGC spent years without one, so this is checked here rather than
# trusted.

agc_struct = re.search(r'typedef struct\s*\{(.*?)\}\s*agc_wdsp_params_t', agc_h, re.S)
if not agc_struct:
    sys.exit('the agc_wdsp_params_t struct was not found in audio_agc.h, update gen_config_upgrade.py')
agc_fields = {}       # field name -> (declared c type, number of array elements)
for m in re.finditer(r'(uint8_t|int32_t|int|float32_t)\s+(\w+)\s*(\[(\d+)\])?\s*;', agc_struct.group(1)):
    ctype, name, _, dim = m.groups()
    agc_fields[name] = (ctype, int(dim) if dim else 1)

agc_stored_types = {  # the ConfigEntry type which matches the declared type of the parameter
    'uint8_t':  'ConfigEntry_UInt8',
    'int':      'ConfigEntry_Int32_16',
    'int32_t':  'ConfigEntry_Int32_16',
}
agc_c_types = { 'ConfigEntry_UInt8': 'ST_UINT8', 'ConfigEntry_Int32_16': 'ST_INT32_16' }

def num(t):
    """value of a constant used in an AGC row: a number, an EEPROM id or a named constant such as
       AGC_WDSP_THRESH_DEFAULT of audio_agc.h"""
    v = resolve(t)
    if v is None:
        sys.exit('cannot work out the value of %r used by an AGC row of the ConfigEntry table' % t)
    return v

agc_stored = {}
for m in re.finditer(r'\{\s*(ConfigEntry_\w+)(?:\s*\|\s*\w+)?\s*,\s*(EEPROM_\w+)\s*,\s*&agc_wdsp_conf\.(\w+)(\[(\d+)\])?\s*,\s*(-?\w+)\s*,\s*(-?\w+)\s*,\s*(-?\w+)\s*\}', cfg_c):
    ctype, idname, field, _, index, d, lo, hi = m.groups()
    key = field if index is None else '%s[%s]' % (field, index)
    if key in agc_stored:
        sys.exit('agc_wdsp_conf.%s appears twice in the ConfigEntry table' % key)
    agc_stored[key] = {'key': key, 'field': field, 'ctype': ctype, 'idname': idname, 'id': vals[idname],
                       'def': num(d), 'min': num(lo), 'max': num(hi)}
if not agc_stored:
    sys.exit('no &agc_wdsp_conf. entry matched the ConfigEntry table in ui_configuration.c, '
             'the table layout may have changed, update the regex in gen_config_upgrade.py')

# the array elements an AGC menu item can reach: tau_decay[] is indexed by the AGC mode, so every
# element of the array is editable and has to be stored
agc_menu = {}
menu_src = strip_comments(menu_c)
for args in call_arguments(menu_src, r'UiDriverMenuItemChange\w+'):
    if len(args) < 7 or not args[2].startswith('&agc_wdsp_conf.'):
        continue
    field = args[2][len('&agc_wdsp_conf.'):].strip()
    lo, hi, dflt = args[3], args[4], args[5]
    array = re.match(r'^(\w+)\[.*\]$', field)
    if array:
        dim = agc_fields[array.group(1)][1]
        keys = ['%s[%d]' % (array.group(1), i) for i in range(dim)]
    else:
        keys = [field]
    for key in keys:
        if key in agc_menu:
            sys.exit('agc_wdsp_conf.%s is changed by more than one AGC menu item, the check cannot '
                     'tell their ranges apart' % key)
        agc_menu[key] = (num(lo), num(hi), num(dflt))
if not agc_menu:
    sys.exit('no AGC menu item matched ui_menu.c, update the regex in gen_config_upgrade.py')

for key, (lo, hi, dflt) in sorted(agc_menu.items()):
    stored = agc_stored.get(key)
    if stored is None:
        sys.exit('the AGC menu changes agc_wdsp_conf.%s in the range %d..%d but there is no entry for '
                 'it in the ConfigEntry table of ui_configuration.c, the value is lost at power off'
                 % (key, lo, hi))
    if lo < stored['min'] or hi > stored['max']:
        sys.exit('the AGC menu offers agc_wdsp_conf.%s in the range %d..%d but only %d..%d is stored, '
                 'a value outside the stored range is reset to its default at the next start'
                 % (key, lo, hi, stored['min'], stored['max']))
    if not (stored['min'] <= dflt <= stored['max']):
        sys.exit('the default of the AGC menu item for agc_wdsp_conf.%s is %d, outside the stored '
                 'range %d..%d' % (key, dflt, stored['min'], stored['max']))

for key, entry in sorted(agc_stored.items()):
    if entry['id'] >= first_unused:
        sys.exit('%s has id %d but EEPROM_FIRST_UNUSED is only %d, fix ui_configuration.h'
                 % (entry['idname'], entry['id'], first_unused))
    if entry['id'] >= IMAGE_SLOTS:
        sys.exit('%s has id %d which does not fit the host image of %d slots'
                 % (entry['idname'], entry['id'], IMAGE_SLOTS))
    ctype = agc_fields[entry['field']][0]
    wanted = agc_stored_types.get(ctype)
    if wanted is None:
        sys.exit('agc_wdsp_conf.%s has the type %s, teach gen_config_upgrade.py how that is stored'
                 % (key, ctype))
    if entry['ctype'] != wanted:
        sys.exit('agc_wdsp_conf.%s is a %s but its entry uses %s, values of that type are not '
                 'written back the way they are read' % (key, ctype, entry['ctype']))
    if ctype in ('int', 'int32_t') and not (-32768 <= entry['min'] and entry['max'] <= 32767):
        sys.exit('agc_wdsp_conf.%s range %d..%d does not fit the single 16 bit slot of %s'
                 % (key, entry['min'], entry['max'], entry['ctype']))

# parameters the firmware keeps in the same struct but which are runtime only state, they are
# derived on every start and must not gain a storage entry by accident
for key in ('action', 'switch_mode', 'hang_action'):
    if key in agc_stored:
        sys.exit('agc_wdsp_conf.%s is runtime only state and should not be stored' % key)

# ---------------------------------------------------------------------------------------------
# the whole configuration table, not only the subsystems above
#
# Three defects of one family turned up while checking the AGC values: a menu item whose value has
# no entry at all, an entry whose stored range is narrower than the range the menu offers (such a
# value is silently reset at the next start), and an entry whose default lies outside its own
# stored range. All three are silent, so they are checked for every row of the table instead of
# only for the ones somebody remembered.


def split_fields(text):
    """split a table row at the commas that are not inside parentheses"""
    out, depth, cur = [], 0, ''
    for ch in text:
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur)
            cur = ''
        else:
            cur += ch
    out.append(cur)
    return [c.strip() for c in out]

def table_rows(text):
    """rows of the ConfigEntry table together with the #if branch they are compiled from"""
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)   # callers hand over the raw source as well
    text = re.sub(r'//[^\n]*', '', text)
    rows = []
    stack = []
    for line in text.splitlines():
        s = line.strip()
        m = re.match(r'#\s*(ifdef|ifndef|if|else|elif|endif)\b(.*)', s)
        if m:
            keyword, rest = m.group(1), m.group(2).strip()
            if keyword == 'ifdef':
                stack.append('defined(%s)' % rest)
            elif keyword == 'ifndef':
                stack.append('!defined(%s)' % rest)
            elif keyword == 'if':
                stack.append(rest)
            elif keyword in ('else', 'elif') and stack:
                stack[-1] = '!' + stack[-1]
            elif keyword == 'endif' and stack:
                stack.pop()
            continue
        if not s.startswith('{'):
            continue
        s = s.rstrip()
        if s.endswith(','):
            s = s[:-1].rstrip()
        if not s.endswith('}'):
            continue
        fields = split_fields(s[1:-1])
        if len(fields) != 6:
            continue
        m = re.match(r'(ConfigEntry_\w+)(?:\s*\|\s*\w+)?$', fields[0])
        if not m or not re.fullmatch(r'EEPROM_\w+', fields[1]) or not fields[2].startswith('&'):
            continue
        rows.append((tuple(stack), m.group(1), fields[1], fields[2][1:].strip(),
                     fields[3], fields[4], fields[5]))
    return rows

RTYPES = {
    'ConfigEntry_UInt8': ('R_UINT8', 1),
    'ConfigEntry_UInt16': ('R_UINT16', 1),
    'ConfigEntry_UInt32_16': ('R_UINT32_16', 1),
    'ConfigEntry_Int16': ('R_INT16', 1),
    'ConfigEntry_Int32_16': ('R_INT32_16', 1),
    'ConfigEntry_Int32': ('R_INT32', 2),
    'ConfigEntry_UInt8x2': ('R_UINT8X2', 1),
}
RTYPE_LIMITS = {                     # what the stored word can hold after the firmware's cast
    'R_UINT8': (0, 0xFF),
    'R_UINT16': (0, 0xFFFF),
    'R_UINT32_16': (0, 0xFFFF),
    'R_INT16': (-0x8000, 0x7FFF),
    'R_INT32_16': (-0x8000, 0x7FFF),
    'R_INT32': (-0x80000000, 0x7FFFFFFF),
    'R_UINT8X2': (0, 0xFFFF),
}


def as_stored(rtype, value):
    """what the firmware's read function makes of a value, per its own cast before the range check"""
    mask, signed = {'R_UINT8': (0xFF, False), 'R_UINT16': (0xFFFF, False),
                    'R_UINT32_16': (0xFFFF, False), 'R_UINT8X2': (0xFFFF, False),
                    'R_INT16': (0xFFFF, True), 'R_INT32_16': (0xFFFF, True),
                    'R_INT32': (0xFFFFFFFF, True)}[rtype]
    stored = value & mask
    if signed and stored & (1 << (mask.bit_length() - 1)):
        stored -= mask + 1
    return stored


# the switches of the default firmware configuration: the board feature switches of
# hardware/uhsdr_board_config.h are already in const_exprs, what is still missing are the -D
# options the Makefile uses when nothing else is given, they decide which board the rows below
# belong to.
build_flags = set()

# The feature switches that decide which rows of the table are compiled are spread over the
# hardware configuration and the driver headers.  Without them a branch such as
# #ifdef USE_LMS_AUTONOTCH cannot be judged and the rows it protects silently drop out of every
# comparison below, which is exactly how the pinned DSP notch parameters escaped this check once.
# A name that is defined nowhere in the tree is not defined for anybody, that branch is off.
macro_defs = set()
for dirpath, _dirs, files in os.walk(root):
    if any(part in dirpath for part in ('/support/', '/basesw/', '/build/', '/.git')):
        continue
    for fn in files:
        if not fn.endswith(('.h', '.c')):
            continue
        try:
            text = open(os.path.join(dirpath, fn), encoding='utf-8', errors='surrogateescape').read()
        except OSError:
            continue
        macro_defs.update(re.findall(r'^\s*#define\s+(\w+)', strip_comments(text), re.M))
try:
    mk = open(root + 'Makefile', encoding='utf-8', errors='surrogateescape').read()
    for m in re.finditer(r'-D(\w+)', mk[mk.index('ifndef CONFIGFLAGS'):mk.index('endif', mk.index('ifndef CONFIGFLAGS'))]):
        build_flags.add(m.group(1))
    for m in re.finditer(r'-D(\w+)(?=\\|\n)', mk[mk.index('MACHFLAGS_F4 :='):mk.index('BASECFLAGS_F4')]):
        build_flags.add(m.group(1))
except Exception:
    pass

def branch_active(cond):
    """is a #if branch of the table compiled in this configuration? None when we cannot tell"""
    for c in cond:
        m = re.fullmatch(r'defined\((\w+)\)', c)
        if m:
            name = m.group(1)
            if name in build_flags or name in macro_defs or name in vals or name in const_exprs:
                continue      # defined for this configuration, the branch is compiled
            return False      # defined nowhere in the tree, the branch is dead code
        m = re.fullmatch(r'!defined\((\w+)\)', c)
        if m:
            name = m.group(1)
            if name in build_flags or name in macro_defs or name in vals or name in const_exprs:
                return False      # defined for this configuration, the negated branch is out
            continue
        v = resolve(c)
        if v is None:
            return None
        if not v:
            return False
    return True

def active_only(text):
    """the source with the lines a #if branch does not compile here blanked out, line numbers kept

    The menu has blocks behind #ifdef OBSOLETE_NR and #if 0 whose calls would otherwise be compared
    against the configuration table, checking values the firmware does not even contain."""
    out = []
    stack = []
    for line in text.splitlines(True):
        m = re.match(r'\s*#\s*(ifdef|ifndef|if|else|elif|endif)\b(.*)', line)
        if m:
            kw, rest = m.group(1), m.group(2).strip()
            if kw == 'ifdef':
                stack.append('defined(%s)' % rest)
            elif kw == 'ifndef':
                stack.append('!defined(%s)' % rest)
            elif kw == 'if':
                stack.append(rest)
            elif kw in ('else', 'elif') and stack:
                stack[-1] = '!' + stack[-1]
            elif kw == 'endif' and stack:
                stack.pop()
            out.append(line)
            continue
        out.append(line if not stack or branch_active(stack) else '\n')
    return ''.join(out)

table_body = cfg_c[cfg_c.index('ConfigEntryInfo[]'):]
table_body = table_body[:table_body.index('\n};')]
all_rows = table_rows(table_body)
if len(all_rows) < 100:
    sys.exit('only %d rows of the ConfigEntry table were parsed, the layout of the table may have '
             'changed, update table_rows() in gen_config_upgrade.py' % len(all_rows))

# the rows that this board configuration actually compiles, that is the set the firmware reads and
# writes with. A branch that cannot be judged is an error: silently leaving rows out is how a
# configuration defect used to pass this check unnoticed.
table = []
skipped_branch = 0
dropped_guards = set()
for row in all_rows:
    active = branch_active(row[0])
    if not active:
        skipped_branch += 1
        dropped_guards.update(row[0])
        continue
    table.append(row)

# The rows this check leaves out, pinned: how many, and which #if conditions removed them. A row
# behind a guard whose macro some header defines is compared here regardless of which board the
# build targets, so the union of all configurations is checked at once and only dead code is left
# out. That union is why a new guard on a macro nobody defines has to be noticed: six rows can
# silently become seven, and the seventh is a setting nothing looks at any more.
UNCOMPARED_ROWS = (6, ('defined(OBSOLETE_AGC)', 'defined(OBSOLETE_NR)'))
if (len(all_rows) - len(table), tuple(sorted(dropped_guards))) != UNCOMPARED_ROWS:
    sys.exit('the rows of the ConfigEntry table that are not compared changed: %d row(s) behind '
             '%s, expected %d behind %s. A row is only left out when no header of the tree defines '
             'the macro its branch tests, that is when it is dead code - every other branch is '
             'compared. If the change is intended, update UNCOMPARED_ROWS in gen_config_upgrade.py; '
             'if not, the check just stopped looking at a setting.'
             % (len(all_rows) - len(table), ', '.join(sorted(dropped_guards)) or 'none',
                UNCOMPARED_ROWS[0], ', '.join(UNCOMPARED_ROWS[1]) or 'none'))

unresolved = 0
for cond, ctype, idname, var, d, lo, hi in table:
    if idname not in vals:
        sys.exit('%s (&%s) uses an id that is not defined in ui_configuration.h' % (idname, var))
    if vals[idname] >= first_unused:
        sys.exit('%s (&%s) has id %d but EEPROM_FIRST_UNUSED is only %d'
                 % (idname, var, vals[idname], first_unused))
    span = RTYPES.get(ctype, ('', 1))[1]
    if vals[idname] + span > first_unused:
        sys.exit('%s (&%s) is stored in %d words and ends at word %d, above EEPROM_FIRST_UNUSED '
                 '%d: the second word is written outside the configuration range, where no record '
                 'claims it and every check here stops looking. Give it a lower id, or raise '
                 'EEPROM_FIRST_UNUSED and the baseline with it'
                 % (idname, var, span, vals[idname] + span - 1, first_unused))
    dv, mn, mx = resolve(d), resolve(lo), resolve(hi)
    if None in (dv, mn, mx):
        unresolved += 1
        continue
    if mn > mx:
        sys.exit('%s (&%s) has the stored range %d..%d, which holds no value at all: the firmware '
                 'rejects every read of it, so the setting stays at its default %d forever'
                 % (idname, var, mn, mx, dv))
    # The ends of the range have to be values the type can hold for the same reason the default
    # does: the firmware assigns the stored word to the type before it range checks it, so a bound
    # above the width of the type is not a bound. A row declaring 0..1000 as UInt8 accepts every
    # word, because 0x012C has already become 44 by the time the comparison runs, and the values
    # the table says it stores above 255 are silently replaced by their low byte.
    rtype = RTYPES.get(ctype)
    if rtype is not None:
        low, high = RTYPE_LIMITS[rtype[0]]
        for bound_name, bound in (('minimum', mn), ('maximum', mx)):
            if not (low <= bound <= high):
                sys.exit('%s (&%s) declares the stored range %d..%d, but %s holds only %d..%d: the '
                         'firmware assigns the stored word to the type before it range checks it, '
                         'so its %s %d reads back as %d and every stored value is folded into that '
                         'range before the check. Fit the range to the type, or store the value in '
                         'a wider one'
                         % (idname, var, mn, mx, ctype, low, high, bound_name, bound,
                            as_stored(rtype[0], bound)))
    if not (mn <= dv <= mx):
        sys.exit('%s (&%s) has default %d outside its stored range %d..%d, a value in the middle of '
                 'the range the menu offers is replaced by the default at the next start'
                 % (idname, var, dv, mn, mx))
    if rtype is not None:
        stored = as_stored(rtype[0], dv)
        if stored != dv:
            sys.exit('%s (&%s) has default %d, which %s cannot hold: the firmware assigns the stored '
                     'word to the type before it range checks it, so this default reads back as %d '
                     'and the setting never holds what the table asks for. Fit the default to the '
                     'type, or store it in a wider one'
                     % (idname, var, dv, ctype, stored))

# one id must not be claimed by two rows of the same configuration: the second one silently
# overwrites what the first one read and wrote. Rows of different #if branches are not part of the
# same configuration, that is what the filter above is for.
by_id = {}
for cond, ctype, idname, var, d, lo, hi in table:
    by_id.setdefault(vals[idname], []).append((idname, var))
for eid, users in sorted(by_id.items()):
    for i in range(len(users)):
        for j in range(i + 1, len(users)):
            sys.exit('eeprom id %d (%s) is used twice in the ConfigEntry table of this '
                     'configuration: &%s and &%s, one of them silently overwrites the other'
                     % (eid, users[i][0], users[i][1], users[j][1]))

# The other half of the same rule: one RAM variable must not be stored under two ids. The address
# map cannot see this - the two rows have different words - but the load writes both into the same
# field, whichever row comes last wins, and the save writes that one value into both words, so the
# operator sees one of the two settings mirror the other no matter which he changes.
by_var = {}
for cond, ctype, idname, var, d, lo, hi in table:
    by_var.setdefault(var.strip(), []).append(idname)
for var, ids in sorted(by_var.items()):
    if len(ids) > 1:
        sys.exit('&%s is stored under %s in the ConfigEntry table of this configuration. Both rows '
                 'load into the same variable, so the last one wins and the save writes its value '
                 'into both words: one of the two settings mirrors the other. Give one of them its '
                 'own variable, or store it under the other id only.'
                 % (var, ' and '.join(sorted(ids))))

# which items the menu actually offers, and where their code lives: the case bodies of items that
# the menu structure does not list any more are dead code and must not be compared, they check
# values the operator cannot even reach
strc_src = active_only(strip_comments(open(root + 'drivers/ui/menu/ui_menu_structure.c',
                               encoding='utf-8', errors='surrogateescape').read()))
menu_items = set(re.findall(r'\{\s*MENU_\w+\s*,\s*MENU_ITEM\s*,\s*(\w+)\s*,', strc_src))
if len(menu_items) < 100:
    sys.exit('only %d menu items found in ui_menu_structure.c, the structure of the menu table may '
             'have changed, update this check in gen_config_upgrade.py' % len(menu_items))

menu_src_active = active_only(menu_src)
case_start = [(m.start(), m.group(1)) for m in re.finditer(r'\n\s*case ([A-Za-z_]\w*)\s*:', menu_src_active)]
case_blocks = {}
for i, (pos, item) in enumerate(case_start):
    if item in menu_items:
        case_blocks[item] = menu_src_active[pos:case_start[i + 1][0] if i + 1 < len(case_start) else len(menu_src_active)]

# a menu item may not offer a range its stored entry cannot keep
compared = 0
for item, block in sorted(case_blocks.items()):
    for args in call_arguments(block, r'UiDriverMenuItemChange\w*'):
        if len(args) < 7 or not args[2].startswith('&'):
            continue
        var = args[2][1:].strip()
        lo, hi, dflt = args[3], args[4], args[5]
        rows = [r for r in table if r[3].strip() == var]
        if len(rows) != 1:      # arrays and the macro generated band tables stay with the subsystem
            continue            # specific checks above
        _c, ctype, idname, _var, d, rmin, rmax = rows[0]
        menu_lo, menu_hi, menu_def = resolve(lo), resolve(hi), resolve(dflt)
        stored_lo, stored_hi = resolve(rmin), resolve(rmax)
        if None in (menu_lo, menu_hi, menu_def, stored_lo, stored_hi):
            continue
        compared += 1
        if menu_lo < stored_lo or menu_hi > stored_hi:
            sys.exit('%s (&%s, item %s): the menu offers %d..%d but only %d..%d is stored, a value '
                     'outside the stored range is reset to its default at the next start'
                     % (idname, var, item, menu_lo, menu_hi, stored_lo, stored_hi))
        if not (stored_lo <= menu_def <= stored_hi):
            sys.exit('%s (&%s, item %s): the menu restores %d as default, outside the stored range '
                     '%d..%d' % (idname, var, item, menu_def, stored_lo, stored_hi))

print('  %d rows of the %d in the table are compiled here (%d belong to a branch this '
      'configuration does not use: %s), %d rows have constants that cannot be resolved without a '
      'compiler, %d menu items were compared with their stored range'
      % (len(table), len(all_rows), skipped_branch,
         ', '.join(sorted(dropped_guards)) or 'none', unresolved, compared))

# ---------------------------------------------------------------------------------------------
# every reachable menu item has to write into something the configuration table stores
#
# The AGC hang time and the numerical FWD/REV power meter display are the two items this firmware
# got wrong: both edited a variable that no table row covers, so the setting was applied until the
# next power cycle and then silently lost. New settings items fail here until somebody writes down
# why they must not be saved, and an explanation that no longer matches the code fails too.

transient_values = {        # value the menu changes -> (why it is not stored, the items that do)
    'temp_var_u8': ('the item copies the value back into a stored variable (ts.flags1, ts.flags2, '
                    'ts.dsp.active, ts.lcd_backlight_blanking, ts.low_power_config, ts.xverter_mode, '
                    'ts.cw_offset_mode, df.temp_enabled, cw_decoder_config flags)',
                    ('CONFIG_AM_TX_FILTER_DISABLE', 'CONFIG_LCD_AUTO_OFF_MODE',
                     'CONFIG_LOW_POWER_THRESHOLD', 'CONFIG_XVTR_OFFSET_MULT',
                     'MENU_CW_AUTO_MODE_SELECT', 'MENU_CW_DECODER_SPIKECANCEL', 'MENU_CW_OFFSET_MODE',
                     'MENU_FM_DEV_MODE', 'MENU_LOW_POWER_SHUTDOWN', 'MENU_SPECTRUM_MODE',
                     'MENU_TCXO_C_F', 'MENU_TCXO_MODE')),
    'temp_var_bool': ('cw_decoder_config flag bits, stored in EEPROM_CW_DECODER_FLAGS',
                      ('MENU_CW_DECODER_NOISECANCEL', 'MENU_CW_DECODER_SHOW_CW_LED',
                       'MENU_CW_DECODER_SNAP_ENABLE', 'MENU_CW_DECODER_USE_3_GOERTZEL')),
    'flag': ('local copy of the stored ts.tx_disable', ('CONFIG_TX_DISABLE',)),
    'digital_mode': ('local copy of the stored ts.digital_mode', ('MENU_DIGITAL_MODE_SELECT',)),
    'test': ('actions (backup, restore, reset), they store no value of their own',
             ('CONFIG_RESET_SER_EEPROM', 'CONFIG_RESET_SER_EEPROM_SIGNATURE',
              'MENU_BACKUP_CONFIG', 'MENU_RESTORE_CONFIG')),
    'sm.config.alphaSplit.AttackAlpha': ('union member of the stored sm.config.alphaCombined',
                                         ('CONFIG_SMETER_ATTACK',)),
    'sm.config.alphaSplit.DecayAlpha': ('union member of the stored sm.config.alphaCombined',
                                        ('CONFIG_SMETER_DECAY',)),
    'ts.alc_decay_var': ('mirrored into the stored ts.alc_decay when compression uses the selectable '
                         'value', ('MENU_ALC_RELEASE',)),
    'ts.alc_tx_postfilt_gain_var': ('mirrored into the stored ts.alc_tx_postfilt_gain in the same '
                                    'mode', ('MENU_ALC_POSTFILT_GAIN',)),
    'rtc.Hours': ('the clock itself, the RTC backup domain keeps it', ('CONFIG_RTC_HOUR',)),
    'rtc.Minutes': ('the clock itself, the RTC backup domain keeps it', ('CONFIG_RTC_MIN',)),
    'rtc.Seconds': ('the clock itself, the RTC backup domain keeps it', ('CONFIG_RTC_SEC',)),
    'ts.tune_tone_mode': ('deliberately transient, the code of the item says so',
                          ('CONFIG_TUNE_TONE_MODE',)),
}
DEBUG_PREFIX = 'MENU_DEBUG_'

strc_src = active_only(strip_comments(open(root + 'drivers/ui/menu/ui_menu_structure.c',
                               encoding='utf-8', errors='surrogateescape').read()))
menu_items = set(re.findall(r'\{\s*MENU_\w+\s*,\s*MENU_\w+\s*,\s*(\w+)\s*,', strc_src))
if len(menu_items) < 100:
    sys.exit('only %d menu items found in ui_menu_structure.c, the structure of the menu table may '
             'have changed, update this check in gen_config_upgrade.py' % len(menu_items))

stored_bases = {re.split(r'\[', r[3].strip())[0] for r in all_rows}
stored_paths = {re.sub(r'\[[^\]]*\]', '', r[3].strip()) for r in all_rows}

reachable = len(case_blocks)
unsaved = {}
for item, block in sorted(case_blocks.items()):
    for args in call_arguments(block, r'UiDriverMenuItemChange\w*\('):
        if len(args) < 6 or not args[2].startswith('&'):
            continue
        base = re.split(r'\[', args[2][1:].strip())[0]
        if base in stored_bases:
            continue
        unsaved.setdefault(base, set()).add(item)

for target, items in sorted(unsaved.items()):
    if all(i.startswith(DEBUG_PREFIX) for i in items):
        continue                        # the debug menu is transient on purpose
    entry = transient_values.get(target)
    if entry is None:
        sys.exit('the menu item(s) %s change %s, which no row of the ConfigEntry table stores: the '
                 'setting is lost at the next power off. Add an entry, or say why it must not be '
                 'saved in transient_values of gen_config_upgrade.py' % (', '.join(sorted(items)), target))
    if items != set(entry[1]):
        sys.exit('transient_values explains %s for %s, but the menu changes it in %s: the exception '
                 'is out of date' % (target, ', '.join(sorted(entry[1])), ', '.join(sorted(items))))
for target, (reason, items) in transient_values.items():
    if target not in unsaved:
        sys.exit('transient_values explains %s (%s) but no menu item changes it any more, drop the '
                 'exception' % (target, reason))

print('  %d reachable menu items change a value, %d of them touch a variable the table does not '
      'store, each one either a debug item or explained in transient_values'
      % (reachable, len(unsaved)))

# ---------------------------------------------------------------------------
# The front panel: encoders, buttons and touch actions change values too, and that code does not
# go through the menu at all.  Every value they adjust has to be stored as well, unless it is
# deliberately session state, which then has to be listed in transient_panel_values with a reason.
# radio_management.h holds the inline toggles the button actions call, so it belongs to the panel.
drv_src = ""
for panel_file in ('drivers/ui/ui_driver.c', 'drivers/ui/radio_management.h'):
    drv_src += strip_comments(open(root + panel_file, encoding='utf-8', errors='surrogateescape').read())
    drv_src += "\n"

transient_panel_values = {
    'ts.rit_value': ('RIT offset: the knob is relative and the display shows the offset of the '
                     'current session, a stored RIT would start every QSO on an shifted receive '
                     'frequency'),
    'ts.vfo_mem_flag': ('which of VFO A/B is a memory, runtime state of the tuning controls, the '
                        'toggle lives in radio_management.h'),
    'ts.buffered_tx': ('TX audio buffering, switched for the current transmission only'),
    'ts.mem_disp': ('spectrum reference trace display state, UiAction_MemDisp'),
    'ts.keyer_mode.active': ('arming of the keyer macro recorder, must not survive a power cycle'),
    'ts.frequency_lock': ('held while the operator keeps the button pressed, a lock that survived '
                          'the power cycle could not be released from the menu'),
}

# the panel writes array elements, e.g. ts.rx_gain[RX_AUDIO_SPKR].value, so the paths of the table
# are normalised the same way before they are compared
stored_paths = {re.sub(r'\[[^\]]*\]', '', r[3].strip()) for r in all_rows}

panel_targets = set()
panel_unsaved = {}
for m in re.finditer(r'change_and_limit_(?:int|uint)\(\s*([^,]+?)\s*,\s*[^,]+?\s*,\s*[^,()]+?\s*,\s*[^,()]+?\)', drv_src):
    target = re.sub(r'\[[^\]]*\]', '', m.group(1).strip())
    if '.' not in target:      # a local variable of a helper, not a value of the radio
        continue
    panel_targets.add(target)
    if target not in stored_paths:
        panel_unsaved.setdefault(target, set()).add('encoder')
for m in re.finditer(r'((?:ts|ads|swrm|agc_wdsp_conf|df|sd)\.[\w.]+)\s*=\s*!\s*\1\s*;', drv_src):
    target = re.sub(r'\[[^\]]*\]', '', m.group(1))
    panel_targets.add(target)
    if target not in stored_paths:
        panel_unsaved.setdefault(target, set()).add('button')

for target in sorted(panel_unsaved):
    if target not in transient_panel_values:
        sys.exit('the encoders/buttons change %s, which no row of the ConfigEntry table stores: the '
                 'value is lost at the next power off. Add an entry, or say why it must stay session '
                 'state in transient_panel_values of gen_config_upgrade.py' % target)
for target in sorted(transient_panel_values):
    if target not in panel_unsaved:
        sys.exit('transient_panel_values explains %s (%s) but the front panel does not change it any '
                 'more, drop the exception' % (target, transient_panel_values[target]))

# the range the panel can produce must be inside the range that is stored, otherwise a perfectly
# legal encoder setting is thrown away when the configuration is loaded again
panel_rows = {re.sub(r'\[[^\]]*\]', '', r[3].strip()): r for r in all_rows}
panel_range_problems = 0
for m in re.finditer(r'change_and_limit_(?:int|uint)\(\s*([^,]+?)\s*,\s*[^,]+?\s*,\s*([^,()]+?)\s*,\s*([^,()]+?)\)', drv_src):
    target, lo, hi = (t.strip() for t in m.groups())
    target = re.sub(r'\[[^\]]*\]', '', target)
    row = panel_rows.get(target)
    if row is None:
        continue
    plo, phi, slo, shi = (resolve(x) for x in (lo, hi, row[5], row[6]))
    if None in (plo, phi, slo, shi):
        continue            # a limit that depends on a runtime value, e.g. ts.rx_gain[i].max
    if plo < slo or phi > shi:
        sys.exit('%s can be set to %d..%d by the front panel but only %d..%d is stored [%s]: the '
                 'setting is silently discarded at the next load'
                 % (target, plo, phi, slo, shi, row[2]))

print('  %d values are changed by encoders/buttons/touch actions, %d of them are not stored and '
      'each one is explained in transient_panel_values'
      % (len(panel_targets), len(panel_unsaved)))

# EEPROM_FIRST_UNUSED of the build which did not yet store the AGC hang time, the hang threshold
# and the decay time constant of the AGC OFF mode. The upgrade scenario of the generated check
# replays a configuration stored by that build.
agc_first_unused_before = 383

# ---------------------------------------------------------------------------
# The EEPROM address map: one stored value per word.
#
# Every configuration value owns one 16 bit word of the store, addressed by its id, and the filter
# path memories own a block of 25. Two values on the same word destroy each other: whichever was
# written last is what the next boot finds, and from the operator's point of view that looks exactly
# like a setting which was never saved. The filter path map sat at 297 and so covered the ids of 22
# single value settings - among them the two I2C bus speeds, the receive IQ autocorrection and the
# WDSP AGC threshold - and because UiConfiguration_SaveEepromValues() writes the table first and the
# filter map afterwards, the map won. That is what this check keeps from ever happening again.

def expand_def_macros(text, marker, what):
    """text with the invocations of every #define whose body contains marker replaced by that body

    The ConfigEntry table and the menu build whole groups of rows/cases with argument macros, so a
    line based parser does not see them at all. Without this expansion the per band IQ calibration
    and power factor rows - and the words they occupy - are invisible to the checks below."""
    defs = {}
    def grab(m):
        name, params, rest = m.group(1), m.group(2), m.group(3)
        if marker not in rest:
            return m.group(0)
        defs[name] = (params, rest)
        return '\n'
    text = re.sub(r'(?m)^#define\s+(\w+)\(([^)]*)\)((?:[^\n]*\\\n)*[^\n]*)', grab, text)
    for name, (params, rest) in sorted(defs.items()):
        clean = '\n'.join(l.rstrip().rstrip('\\').strip() for l in rest.splitlines())
        names = [p.strip() for p in params.split(',')]
        def sub(mo):
            args = [a.strip() for a in mo.group(1).split(',')]
            out = clean
            for i, p in enumerate(names):
                v = args[i] if i < len(args) else ''
                out = out.replace('##%s##' % p, v)
                out = re.sub(r'##\s*' + re.escape(p) + r'\b', v, out)
            return out
        text, n = re.subn(r'(?m)^[ \t]*%s\(([^)]*)\)[ \t]*,?[ \t]*$' % re.escape(name), sub, text)
        if n == 0:
            sys.exit('the %s macro %s is defined but never invoked, drop it or update '
                     'expand_def_macros() in gen_config_upgrade.py' % (what, name))
    return text, set(defs)

clean_cfg_c = strip_comments(cfg_c)
tbl_start = clean_cfg_c.index('ConfigEntryInfo[]')
tbl_end = clean_cfg_c.index('\n};', tbl_start)
macro_starts = [m.start() for m in re.finditer(r'(?m)^#define\s+\w+\([^)]*\)[^\n]*ConfigEntry_', clean_cfg_c)
                if m.start() < tbl_start]
map_body = clean_cfg_c[min(macro_starts) if macro_starts else tbl_start:tbl_end]
map_body, row_macros = expand_def_macros(map_body, 'ConfigEntry_', 'ConfigEntry row')
map_rows = [r for r in table_rows(map_body) if branch_active(r[0])]
if len(map_rows) < len(table):
    sys.exit('the expanded ConfigEntry table has %d compiled rows, the plain parse found %d: the '
             'expansion cannot be trusted, update gen_config_upgrade.py'
             % (len(map_rows), len(table)))

# word counts of the values that occupy more than one word, taken from the code that writes them so
# that changing the loop bound without moving the block fails here
MAX_BANDS = resolve('MAX_BANDS')
FPM = resolve('FILTER_PATH_MEM_MAX')
m = re.search(r'FILTER_MODE_CW\s*=\s*0,([^}]*)FILTER_MODE_MAX',
              open(root + 'drivers/audio/audio_filter.h', encoding='utf-8',
                   errors='surrogateescape').read(), re.S)
if not (m and MAX_BANDS and FPM):
    sys.exit('MAX_BANDS / FILTER_PATH_MEM_MAX / FILTER_MODE_MAX could not be resolved, the address '
             'map check cannot run, update gen_config_upgrade.py')
FMM = 1 + len([x for x in m.group(1).split(',') if x.strip()])

def hdr_define(name):
    g = re.search(r'(?m)^\s*#define\s+%s\s+((?:0[xX])?[0-9A-Fa-f]+)\s*(?://|$)' % re.escape(name),
                  strip_comments(cfg_h))
    if not g:
        sys.exit('%s is not a plain numeric #define of ui_configuration.h, the address map check '
                 'cannot run' % name)
    return int(g.group(1), 0)

filter_words = hdr_define('EEPROM_FILTER_PATH_MAP_WORDS')
if filter_words != FMM * FPM:
    sys.exit('EEPROM_FILTER_PATH_MAP_WORDS is %d but the firmware writes %d x %d = %d filter path '
             'memories: the block is too small and would run into the next ids'
             % (filter_words, FMM, FPM, FMM * FPM))
# UiReadSettingEEPROM_Filter() has to decide the previous-layout case BEFORE it probes the current
# block: in the layout before the renumbering those words held other ids, so every configuration of
# that layout has data there and the probe would report the block as already stored.
filter_read = re.search(r'void UiReadSettingEEPROM_Filter\s*\([^)]*\)\s*\{(.*?)\n\}', clean_cfg_c, re.S)
if not filter_read:
    sys.exit('UiReadSettingEEPROM_Filter() is gone from ui_configuration.c, update this check')
fbody = filter_read.group(1)
migrate_at = fbody.find('config_layout_migration_active')
probe_at = fbody.find('UiConfiguration_FilterMapNotStoredYet')
if migrate_at < 0:
    sys.exit('UiReadSettingEEPROM_Filter() no longer looks at config_layout_migration_active, so a '
             'configuration of the previous layout reads its filter memories from the current block')
if probe_at < 0:
    sys.exit('UiReadSettingEEPROM_Filter() no longer probes the current block, update this check')
if migrate_at > probe_at:
    sys.exit('UiReadSettingEEPROM_Filter() probes the current block before it handles the layout '
             'migration: a configuration of the previous layout has data in those words, so the '
             'probe reports them as stored and the legacy fallback never runs')
if 'EEPROM_FILTER_PATH_MAP_LEGACY_BASE' not in fbody:
    sys.exit('UiReadSettingEEPROM_Filter() no longer uses EEPROM_FILTER_PATH_MAP_LEGACY_BASE, so a '
             'configuration of the previous layout can no longer be read')

fmap_base = hdr_define('EEPROM_FILTER_PATH_MAP_BASE')
fmap_legacy = hdr_define('EEPROM_FILTER_PATH_MAP_LEGACY_BASE')
if fmap_legacy == fmap_base:
    sys.exit('the legacy base of the filter path memories equals the current base %d, '
             'check_filter_map_migration() cannot tell the two layouts apart' % fmap_base)
if fmap_legacy + filter_words > first_unused:
    sys.exit('the legacy base %d and %d words reach above EEPROM_FIRST_UNUSED %d'
             % (fmap_legacy, filter_words, first_unused))

# the save side of the same rule: a migrated radio only converges if its next save writes the block
# at the current base. If it wrote the legacy base, or nothing, the probe would keep reporting the
# block as never written and every later boot would read the legacy memories again.
filter_write = re.search(r'static\s+uint16_t\s+UiWriteSettingEEPROM_Filter\s*\([^)]*\)\s*\{(.*?)\n\}',
                         clean_cfg_c, re.S)
if not filter_write:
    sys.exit('UiWriteSettingEEPROM_Filter() is gone from ui_configuration.c, update this check')
wbody = filter_write.group(1)
if 'EEPROM_FILTER_PATH_MAP_BASE' not in wbody:
    sys.exit('UiWriteSettingEEPROM_Filter() no longer writes EEPROM_FILTER_PATH_MAP_BASE, so a '
             'migrated radio never gets its memories into the current block: check_filter_map_save() '
             'would fail on the next boot')
if 'EEPROM_FILTER_PATH_MAP_LEGACY_BASE' in wbody:
    sys.exit('UiWriteSettingEEPROM_Filter() writes EEPROM_FILTER_PATH_MAP_LEGACY_BASE, which is read '
             'only: the current block would stay empty, the probe would report it as never written '
             'and every boot would read the legacy memories instead of the stored ones')

# check_row_roundtrip() models UiReadSettingEEPROM_UInt8() as it is written: the stored word is
# assigned to a uint8_t before the range check, so the high byte is lost. If that changes, the model
# is describing a firmware that no longer exists.
u8_read = re.search(r'UiReadSettingEEPROM_UInt8\s*\(\s*uint16_t\s+addr\s*,\s*volatile\s+uint8_t'
                    r'[^)]*\)\s*\{(.*?)\n\}', clean_cfg_c, re.S)
if not u8_read:
    sys.exit('UiReadSettingEEPROM_UInt8() is gone or its signature changed, the truncation model of '
             'check_row_roundtrip() has nothing to describe')
u8_body = u8_read.group(1)
x2_read = re.search(r'UiReadSettingEEPROM_UInt8x2\s*\(\s*uint16_t\s+addr\s*,\s*volatile\s+uint16_t'
                    r'[^)]*\)\s*\{(.*?)\n\}', clean_cfg_c, re.S)
if not x2_read:
    sys.exit('UiReadSettingEEPROM_UInt8x2() is gone or its signature changed, the packed model of '
             'check_row_roundtrip() has nothing to describe')
x2_body = x2_read.group(1)
for needle in ('*val_ptr >> 8', 'min_val >> 8', '*val_ptr & 0xff', 'min_val & 0xff',
               'default_val & 0xff00', 'default_val & 0x00ff'):
    if needle not in x2_body:
        sys.exit('UiReadSettingEEPROM_UInt8x2() no longer repairs its two bytes separately (no %r '
                 'in the function). check_row_roundtrip() models each byte against its own half of '
                 'the stored range and expects the other byte to survive, so the model and the '
                 'firmware have to change together.' % needle)

# check_interrupted_save() expects half an Int32 value to be no value, which only holds while the
# loader tests both words before it composes them: a save that stopped between the two would
# otherwise leave a number made of one stored word and one default.
int32_read = re.search(r'case\s+ConfigEntry_Int32:\s*\{(.*?)\n    \}', clean_cfg_c, re.S)
if not int32_read:
    sys.exit('case ConfigEntry_Int32: is gone from ui_configuration.c, the Int32 read model of '
             'check_row_roundtrip() and check_interrupted_save() has nothing to describe')
for needle in ('ConfigStorage_ReadVariable(addr,&probe)',
               'ConfigStorage_ReadVariable(addr+1,&probe)'):
    if needle not in int32_read.group(1):
        sys.exit('the loader composes a ConfigEntry_Int32 value without testing that both of its '
                 'words are stored (no %r in that case). A save that stopped between the two words '
                 'then reads a number made of one stored half and one default, and '
                 'check_interrupted_save() expects the default instead.' % needle)

u8_assign = u8_body.find('*val_ptr = value')
u8_check = u8_body.find('*val_ptr < min_val')
if u8_assign < 0 or u8_check < 0 or u8_assign > u8_check:
    sys.exit('UiReadSettingEEPROM_UInt8() no longer truncates the stored word before it range '
             'checks it. check_row_roundtrip() models the truncation for every UInt8 row, so the '
             'model and the firmware have to change together.')

bulk_blocks = []            # (base, words, who, the code that proves it)
if not re.search(r'EEPROM_FILTER_PATH_MAP_BASE\s*\+\s*idx\s*\*\s*FILTER_PATH_MEM_MAX\s*\+\s*mem_idx',
                 clean_cfg_c):
    sys.exit('the filter path map is no longer written as base+idx*FILTER_PATH_MEM_MAX+mem_idx, '
             'update the address map of gen_config_upgrade.py')
bulk_blocks.append((hdr_define('EEPROM_FILTER_PATH_MAP_BASE'), filter_words,
                    'filter path memories (%d modes x %d slots)' % (FMM, FPM)))

band_bases = ('EEPROM_BAND0_MODE_A', 'EEPROM_BAND0_FREQ_HIGH_A', 'EEPROM_BAND0_FREQ_LOW_A',
              'EEPROM_BAND0_MODE_B', 'EEPROM_BAND0_FREQ_HIGH_B', 'EEPROM_BAND0_FREQ_LOW_B')
for b in band_bases:
    if not re.search(r'UiWriteSettingsBandMode\s*\([^)]*\b%s\b' % b, clean_cfg_c) and \
       not re.search(r'UiReadSettingsBandMode\s*\([^)]*\b%s\b' % b, clean_cfg_c) and \
       not re.search(r'BandMemBlockBase\s*\(\s*%s\s*\)' % b, clean_cfg_c):
        sys.exit('%s is no longer used as a per band memory base, update the address map of '
                 'gen_config_upgrade.py' % b)
    bulk_blocks.append((hdr_define(b), MAX_BANDS, '%s (%d band slots)' % (b, MAX_BANDS)))

# values the firmware writes outside the ConfigEntry table, one word each
single_direct = ['EEPROM_BAND_MODE', 'EEPROM_FREQ_HIGH', 'EEPROM_FREQ_LOW',
                 'EEPROM_XVERTER_OFFSET_HIGH', 'EEPROM_XVERTER_OFFSET_LOW',
                 'EEPROM_XVERTER_OFFSET_TX_HIGH', 'EEPROM_XVERTER_OFFSET_TX_LOW',
                 'EEPROM_CONFIG_LAYOUT_VERSION']
for n in single_direct:
    if not re.search(r'\b%s\b' % n, clean_cfg_c):
        sys.exit('%s is not used in ui_configuration.c any more, drop it from the address map of '
                 'gen_config_upgrade.py' % n)
    bulk_blocks.append((hdr_define(n), 1, '%s (written outside the table)' % n))

# -----------------------------------------------------------------------------------------------
# What the save path writes outside the table, derived from the save path itself
#
# The list above is how the map learned about these words, and until now nothing tied it to the
# code: the only check was that each name still occurs somewhere in the file. A new direct write -
# the kind that destroyed the 22 settings under the filter path block - would not be in the map at
# all, and a removed one would stay in it and keep its word reserved. The save function is the one
# place those writes happen, so the set of EEPROM names in it has to be exactly the set the map
# claims outside the table.
save_call = re.search(r'uint16_t\s+UiConfiguration_SaveEepromValues\s*\(\s*void\s*\)\s*\{(.*?)\n\}',
                      clean_cfg_c, re.S)
if not save_call:
    sys.exit('UiConfiguration_SaveEepromValues() is gone from ui_configuration.c or its signature '
             'changed, the words the save path writes outside the table cannot be checked')
save_body = save_call.group(1)
save_written = {name for name in re.findall(r'\b(EEPROM_\w+)\b', save_body + wbody)
                if name in vals and vals[name] < first_unused}
map_direct = set(single_direct) | set(band_bases) | {'EEPROM_FILTER_PATH_MAP_BASE'}
unmapped = sorted(save_written - map_direct)
unwritten = sorted(map_direct - save_written)
if unmapped or unwritten:
    sys.exit('the save path and the address map disagree about the words written outside the '
             'ConfigEntry table.\n'
             '  written by UiConfiguration_SaveEepromValues() but not in the map: %s\n'
             '  in the map but no longer written by it: %s\n'
             'A word the map does not know can be handed to a table row and destroyed at every '
             'save; a word the save no longer writes keeps a word reserved for nothing. Add it to '
             'single_direct or bulk_blocks of gen_config_upgrade.py, or drop it from there.'
             % (', '.join(unmapped) or 'none', ', '.join(unwritten) or 'none'))

# The marker is what stops the next boot from translating the configuration it just saved. Writing
# it before the values would leave a current marker over words of the previous layout, and not
# writing it at all would migrate the radio on every boot, each time reading its current words
# through the previous addresses.
marker_write = re.search(r'ConfigStorage_WriteVariable\(\s*EEPROM_CONFIG_LAYOUT_VERSION\s*,\s*'
                         r'CONFIG_LAYOUT_VERSION_MAGIC\s*\)', save_body)
if not marker_write:
    sys.exit('UiConfiguration_SaveEepromValues() no longer writes CONFIG_LAYOUT_VERSION_MAGIC to '
             'EEPROM_CONFIG_LAYOUT_VERSION. Without it the next boot migrates the configuration '
             'again and reads its current words through the previous layout addresses.')
values_at = save_body.find('UiConfiguration_WriteConfigEntries')
if values_at >= 0 and marker_write.start() < values_at:
    sys.exit('UiConfiguration_SaveEepromValues() writes the layout marker before the configuration '
             'values. A save that stops in between leaves the current marker over values of the '
             'previous layout, and the next boot reads them through the current addresses. Write '
             'the marker after the values.')
print('  save path: %d words written outside the table (%s), all of them in the address map, the '
      'layout marker written after the values'
      % (len(save_written), ', '.join(sorted(save_written))))

# the keyer macro memory lives in its own byte area below the variables, make sure it stayed there
if hdr_define('EEPROM_KEYER_MEMORY_ADDRESS') < first_unused:
    sys.exit('EEPROM_KEYER_MEMORY_ADDRESS %d is inside the configuration ids (< %d), it would '
             'overwrite them' % (hdr_define('EEPROM_KEYER_MEMORY_ADDRESS'), first_unused))

owners = {}                 # word -> list of (who, idname)
for cond, ctype, idname, var, d, lo, hi in map_rows:
    if idname not in vals:
        continue
    span = 2 if ctype == 'ConfigEntry_Int32' else 1
    for w in range(vals[idname], vals[idname] + span):
        owners.setdefault(w, []).append(('table row %s (&%s)' % (idname, var.strip()), idname))
for base, words, who in bulk_blocks:
    for w in range(base, base + words):
        owners.setdefault(w, []).append((who, who))

clashes = {w: o for w, o in owners.items() if len(o) > 1}
if clashes:
    lines = []
    for w in sorted(clashes)[:40]:
        lines.append('    word %3d: %s' % (w, '  <->  '.join(sorted({o[0] for o in clashes[w]}))))
    sys.exit('%d EEPROM word(s) are claimed by more than one stored value:\n%s\n'
             'The value written last destroys the other one at every save, which the operator sees '
             'as a setting that is never remembered. Give one of them a free word and raise '
             'EEPROM_FIRST_UNUSED if that means the address range grows.'
             % (len(clashes), '\n'.join(lines)))

# -----------------------------------------------------------------------------------------------
# The filter path memories of the layout before the renumbering sat at
# EEPROM_FILTER_PATH_MAP_LEGACY_BASE, and UiReadSettingEEPROM_Filter() still reads a configuration of
# that layout from there. Commit 72933b24 fixed the read side; this is the write side of the rule.
#
# The range cannot be reserved as a whole, because it is not free: 21 of its words were handed to
# ordinary settings when the map was renumbered (manual notch, IQ balance, bass and treble, the SAM
# PLL, the I2C speeds, the WDSP AGC). Reallocating any of those changes what a stored old image
# means, and the layout digest below already refuses to accept that without a marker bump and a
# migration, so they are covered.
#
# What is left are the words of the range that no setting owns. Those have to stay free: a radio
# written by that layout can hold a filter memory at such a word, and because the id of a new row is
# below the entry count that layout saved, the value is not defaulted - the setting would come up
# showing a filter index, on migrated radios only, and be overwritten from that menu.
# -----------------------------------------------------------------------------------------------
legacy_base = hdr_define('EEPROM_FILTER_PATH_MAP_LEGACY_BASE')
if legacy_base + filter_words > first_unused:
    sys.exit('the filter path range of the previous layout, %d..%d, ends above EEPROM_FIRST_UNUSED '
             '%d, so UiReadSettingEEPROM_Filter() reads words that are not part of the configuration'
             % (legacy_base, legacy_base + filter_words - 1, first_unused))
LEGACY_FREE_WORDS = (297, 298, 305, 317)   # unclaimed inside the legacy range, keep them so
legacy_free = sorted(w for w in range(legacy_base, legacy_base + filter_words) if w not in owners)
for w in sorted(set(LEGACY_FREE_WORDS) & set(owners)):
    sys.exit('word %d of the filter path range of the previous layout (%d..%d) is now claimed by %s.\n'
             'A radio written by that layout can keep a filter memory there, and the new id is below '
             'the entry count it saved, so the value is not defaulted: the setting would be read back '
             'as that filter index and overwritten from its own menu. Put the new value above %d '
             'instead.'
             % (w, legacy_base, legacy_base + filter_words - 1,
                '  <->  '.join(sorted({o[0] for o in owners[w]})), legacy_base + filter_words - 1))
if legacy_free != sorted(LEGACY_FREE_WORDS):
    sys.exit('the words of the legacy filter path range %d..%d that no setting claims are %s, the '
             'generator expects %s. A word that became free keeps a filter memory of an old '
             'configuration and has to stay free as well: update LEGACY_FREE_WORDS here.'
             % (legacy_base, legacy_base + filter_words - 1, legacy_free,
                sorted(LEGACY_FREE_WORDS)))

# -----------------------------------------------------------------------------------------------
# The layout marker in ui_configuration.c is what tells a stored configuration which address map
# wrote it: UiConfiguration_LayoutMigrationRequired() compares it against CONFIG_LAYOUT_VERSION_MAGIC
# and migrates when it differs. That only works if the marker changes whenever the meaning of a word
# that old firmware already wrote changes. It is checked here by hashing the (word, owner) pairs of
# the whole area the old firmware could have written, because that is exactly the part a radio can
# be holding in the wrong shape. Handing out a new id at a fresh word above the baseline is allowed
# and needs no bump: an old configuration simply has no value there and the entry defaults.
#
# The failure this rule exists for is real and was shipped: commit 92418c08 moved the filter path
# block from 297 to 391 while leaving the marker at 0x5AA5, so radios written by 7a76e65a and by
# 92418c08 carry the same marker over two different maps. That survived only because
# UiConfiguration_FilterMapNotStoredYet() tells the two layouts apart by probing the new block.
# -----------------------------------------------------------------------------------------------
BASELINE_LAYOUT_FIRST_UNUSED = 416

# One entry per (marker, map shape) pair that radios in the field can be holding: the value
# EEPROM_CONFIG_LAYOUT_VERSION held in that build, and the digest of the id -> word map that build
# wrote through it. Append only. A map shape that appears under a marker already used for another
# shape is the failure this list exists to prevent - radios carrying the old shape cannot tell.
LAYOUT_HISTORY = [
    # One entry per (marker, map file) pair a radio in the field can be holding: the value
    # EEPROM_CONFIG_LAYOUT_VERSION held in that build, and the id -> word map it wrote as one
    # "word owner" line per claimed word below BASELINE_LAYOUT_FIRST_UNUSED. Append only.
    #
    # The map is what lets the guards below tell a renumbering from a word this harness only
    # learned to see later. A renumbering moves a stored value and needs a new marker; a word that
    # merely became visible leaves every recorded word where it was, and then the marker has to
    # stay, because bumping it pushes every healthy radio through the layout migration.
    # "make layout-record" rewrites the file of the current marker from the table: it takes a
    # renamed value with it, writes the name the value had as a "(was <name>)" suffix on its own
    # word line so the previous layout translation still finds it, and refuses when a word changed
    # hands or disappeared.
    (0x5AA5, 'layout-0x5AA5.map'),
]

current_magic = vals['CONFIG_LAYOUT_VERSION_MAGIC']
layout_map = sorted((w, '|'.join(sorted({o[1] for o in lst})))
                    for w, lst in owners.items() if w < BASELINE_LAYOUT_FIRST_UNUSED)

markers = [m for m, _name in LAYOUT_HISTORY]
if len(set(markers)) != len(markers):
    sys.exit('two entries of LAYOUT_HISTORY share a marker (%s): one marker cannot stand for two '
             'map shapes. A radio holding the older shape would be read through the newer addresses.'
             % ', '.join('0x%x' % m for m in sorted({m for m in markers if markers.count(m) > 1})))

def parse_map_file(path, name):
    """the "word owner" lines of one recorded layout, if the record matches its own digest"""
    text = path.read_text(encoding='utf-8')
    fault = layout_digest.problem(text, name)
    if fault:
        sys.exit(fault + '\nHistory that can be edited without saying so cannot be the thing the '
                 'translation is judged against.')
    out = {}
    for lineno, line in enumerate(text.splitlines(), 1):
        line = line.split('#')[0].strip()
        if not line:
            continue
        word, _, owner = line.partition(' ')
        if not word.isdigit() or not owner.strip():
            sys.exit('%s line %d is not a "word owner" line: %r' % (name, lineno, line))
        out[int(word)] = owner.strip()
    if not out:
        sys.exit('%s records no word at all, an empty record says nothing about the layout' % name)
    return out


def read_layout_map(name, marker, lenient=False):
    """the "word owner" lines of one recorded layout, from the file next to this script"""
    path = Path(__file__).resolve().parent / name
    if not path.exists():
        if lenient:
            return {}          # the recorder is about to write this record, see below
        sys.exit('the layout record %s of marker 0x%X is missing. Restore it, or append the pair '
                 '(0x%X, %s) to LAYOUT_HISTORY with the map that build wrote.'
                 % (name, marker, marker, name))
    return parse_map_file(path, name)


def read_previous_layout(name):
    """the record of the layout that UiConfiguration_OldLayoutAddress() translates"""
    path = Path(__file__).resolve().parent / name
    if not path.exists():
        sys.exit('the record of the previous layout, %s, is missing. It is the map the address '
                 'translation in ui_configuration.c is checked against, so the check would have '
                 'nothing to compare with. Restore the file, do not create it from the current '
                 'table: it describes a layout this firmware no longer writes.' % name)
    return parse_map_file(path, name)


def layout_file_text(marker, rows):
    return layout_digest.seal(
        '# The id -> word map below word %d that CONFIG_LAYOUT_VERSION_MAGIC 0x%X wrote.\n' \
           '# One "word owner" line per claimed word. Rewritten by "make layout-record".\n' \
           '# A "(was <name>)" suffix records a value that kept its word and got another name,\n' \
           '# so the translation of the previous layout still finds it.\n%s\n' \
        % (BASELINE_LAYOUT_FIRST_UNUSED, marker,
           '\n'.join('%d %s' % (word, owner) for word, owner in rows)))


def describe(rows):
    return '\n'.join('  word %d: %s' % (word, text) for word, text in rows[:6]) + \
        ('\n  ... and %d more' % (len(rows) - 6) if len(rows) > 6 else '')


def plain_name(owner):
    """the value a "word owner" string names, without how the word is written there"""
    return owner.split(' (')[0].strip()


def alias_of(owner):
    """the name a value had, from the "(was <name>)" suffix of its line, or None

    The suffix rides on the line of the word it describes, so it cannot point at another word and
    the record needs no second kind of line to carry it. plain_name() strips it, along with the
    other annotations a line can end in."""
    m = re.search(r'\s\(was ([^()]+)\)$', owner)
    return m.group(1).strip() if m else None


def map_shape(mapping):
    """the words a map claims, without the names at them

    The name at a word is for whoever reads the file. What a radio in the field depends on is which
    word holds a value, so a renamed value leaves the shape of the layout untouched."""
    return frozenset(mapping)


# the records only cover the words below the baseline, so a setting stored above it would be written
# by the firmware and never compared with the map a radio in the field carries
if BASELINE_LAYOUT_FIRST_UNUSED != first_unused:
    sys.exit('BASELINE_LAYOUT_FIRST_UNUSED is %d but EEPROM_FIRST_UNUSED is %d. Raise the baseline '
             'to %d and run "make layout-record": the words that become visible that way are '
             'reported, a word stored above the baseline would be skipped without a word.'
             % (BASELINE_LAYOUT_FIRST_UNUSED, first_unused, first_unused))

recorded = {marker: read_layout_map(name, marker,
                                    lenient=RECORD_LAYOUT and marker == current_magic)
            for marker, name in LAYOUT_HISTORY}
current_map = dict(layout_map)

# two markers for one map shape mean a bump without a map change: every radio holding the older
# marker is pushed through the layout migration although its configuration is the same
for i, (m1, _n1) in enumerate(LAYOUT_HISTORY):
    for m2, _n2 in LAYOUT_HISTORY[i + 1:]:
        if map_shape(recorded[m1]) == map_shape(recorded[m2]):
            sys.exit('markers 0x%X and 0x%X claim the same words below %d. A marker has to stand for '
                     'one map shape, so two markers for one shape mean a bump without a map change, '
                     'and every radio holding the older one is pushed through the layout migration '
                     'for nothing. A marker only moves together with a real map change.'
                     % (m1, m2, BASELINE_LAYOUT_FIRST_UNUSED))

if current_magic not in recorded:
    same_shape = [m for m, mp in recorded.items() if map_shape(mp) == map_shape(current_map)]
    if same_shape:
        sys.exit('the words below %d are exactly the ones recorded for marker %s, but '
                 'CONFIG_LAYOUT_VERSION_MAGIC is 0x%X. A radio carrying the recorded shape is then '
                 'read through a marker no build ever wrote, and every healthy radio is pushed '
                 'through the layout migration for nothing. Put the marker back to %s, or record a '
                 'map change and add its pair to LAYOUT_HISTORY.'
                 % (BASELINE_LAYOUT_FIRST_UNUSED, ', '.join('0x%X' % m for m in same_shape),
                    current_magic, ', '.join('0x%X' % m for m in same_shape)))
    sys.exit('CONFIG_LAYOUT_VERSION_MAGIC is 0x%X, which no entry of LAYOUT_HISTORY knows, and the '
             'address map below %d matches no recorded shape either. A radio holding a configuration '
             'written under the previous marker cannot tell which map it is read through, so three '
             'things have to happen together:\n'
             '  1. the marker above has to be the one this build writes, and no entry of '
             'LAYOUT_HISTORY may use it;\n'
             '  2. UiConfiguration_LayoutMigrationRequired() has to keep recognising the marker the '
             'previous build wrote, otherwise every healthy radio is migrated through the '
             'pre-renumbering translation, which corrupts it;\n'
             '  3. append (0x%X, %s) to LAYOUT_HISTORY and run "make layout-record" with the map the '
             'firmware now stores.'
             % (current_magic, BASELINE_LAYOUT_FIRST_UNUSED, current_magic,
                'layout-0x%X.map' % current_magic))

rec_map = recorded[current_magic]
records = dict(LAYOUT_HISTORY)

# What differs is judged by shape first. A value that only got another name keeps its word, its
# range and its meaning, so it is the same layout: renaming it must neither need a marker nor push
# a single radio through the layout migration, and "make layout-record" takes the new name while
# keeping the old one, which is the name the translation of the previous layout has for that value.
#
# Two values that trade words are not that. Nothing was renamed - both names still exist before and
# after - so the value of one is simply read at the word of the other. That is the renumbering this
# guard exists for. Asking whether either name at a changed word still exists in the other map tells
# the two apart, without knowing what any id is called.
names_before = {plain_name(owner) for owner in rec_map.values()}
names_after = {plain_name(owner) for owner in current_map.values()}
renamed = []        # the word kept its value, only the name of that value changed
relocated = []      # the word changed hands between two values that both still exist
for w in sorted(set(current_map) & set(rec_map)):
    now, was = plain_name(current_map[w]), plain_name(rec_map[w])
    if now == was:
        continue
    entry = (w, '%s is stored there now, the record names %s' % (now, was))
    if now in names_before or was in names_after:
        relocated.append(entry)
    else:
        renamed.append(entry)
gone = [(w, 'the record has %s, nothing is stored there any more' % rec_map[w])
        for w in sorted(rec_map) if w not in current_map]
added = [(w, '%s is stored there' % current_map[w]) for w in sorted(current_map)
         if w not in rec_map]

if RECORD_LAYOUT:
    if relocated or gone:
        sys.exit('refusing to record the layout map: %d word(s) below %d moved or disappeared.\n%s\n'
                 'A renumbering is not a record to refresh. It needs a new '
                 'CONFIG_LAYOUT_VERSION_MAGIC and a migration that still recognises the marker the '
                 'previous build wrote, otherwise every radio holding the old layout is read '
                 'through the wrong addresses.'
                 % (len(relocated) + len(gone), BASELINE_LAYOUT_FIRST_UNUSED,
                    describe(relocated + gone)))
    # A value renamed twice keeps the name the previous layout knows it by: the suffix already on
    # its line is the older name and moves along with the value, rather than the intermediate one
    # replacing it. A suffix that has caught up with the current name is no rename any more.
    fresh = {word for word, _text in renamed}
    rows = []
    for word, owner in layout_map:
        had = alias_of(rec_map.get(word, ''))
        if word in fresh:
            had = had or plain_name(rec_map[word])
        if had == plain_name(owner):
            had = None
        rows.append((word, owner + (' (was %s)' % had if had else '')))
    path = Path(__file__).resolve().parent / records[current_magic]
    text = layout_file_text(current_magic, rows)
    if path.exists() and path.read_text(encoding='utf-8') == text:
        print('  layout record: marker 0x%X already records the %d words below %d this build '
              'stores, nothing to do.'
              % (current_magic, len(current_map), BASELINE_LAYOUT_FIRST_UNUSED))
        sys.exit(0)
    path.write_text(text, encoding='utf-8')
    print('  layout record: marker 0x%X now records %d words below %d in %s%s. Commit it together '
          'with the change that made it stale; the marker does not move.'
          % (current_magic, len(current_map), BASELINE_LAYOUT_FIRST_UNUSED, records[current_magic],
             (', renaming %s and recording the name it had' %
              (', '.join('%d %s' % (w, current_map[w]) for w, _t in renamed[:3]) +
               (' ...' if len(renamed) > 3 else ''))) if renamed else
             (', including %s' % ', '.join('%d %s' % (w, current_map[w]) for w, _t in added[:3]))
             if added else ''))
    sys.exit(0)

if relocated or gone:
    sys.exit('the words below %d are not the ones the record names:\n%s\n'
             'A radio holding a configuration written under the recorded map cannot tell, so it will '
             'be read through the wrong addresses. A value that was only renamed is not this: it '
             'keeps its word and "make layout-record" takes the new name.\n'
             'If the values really moved, three things have to happen together:\n'
             '  1. raise CONFIG_LAYOUT_VERSION_MAGIC to a value no entry of LAYOUT_HISTORY uses;\n'
             '  2. extend UiConfiguration_LayoutMigrationRequired() to still recognise the marker '
             'the previous build wrote, otherwise every healthy radio is migrated through the '
             'pre-renumbering translation, which corrupts it;\n'
             '  3. append (0x%X, %s) to LAYOUT_HISTORY and run "make layout-record" with the map the '
             'firmware now writes. Raise BASELINE_LAYOUT_FIRST_UNUSED if the range grew.\n'
             'If the intent is to keep the old layout readable, move the values back.'
             % (BASELINE_LAYOUT_FIRST_UNUSED, describe(relocated + gone), current_magic,
                'layout-0x%X.map' % current_magic))

if renamed:
    print('  layout map: %d word(s) below %d are stored by a value the record names differently '
          '(%s). Each of them kept its word and neither name appears anywhere else in the other '
          'map, so this is a rename: the marker stays 0x%X, run "make layout-record" and commit it '
          'next to the rename. The record keeps the name the value had, so the previous layout '
          'translation still finds the value it moves.'
          % (len(renamed), BASELINE_LAYOUT_FIRST_UNUSED,
             ', '.join('%d %s' % (w, current_map[w]) for w, _t in renamed[:3]) +
             (' ...' if len(renamed) > 3 else ''), current_magic))

if added:
    print('  layout map: %d word(s) below %d are stored by the table but not by the recorded map '
          '(%s). Nothing moved, so the marker stays 0x%X: run "make layout-record" and commit it '
          'together with the change that made the word visible.'
          % (len(added), BASELINE_LAYOUT_FIRST_UNUSED,
             ', '.join('%d %s' % (w, current_map[w]) for w, _t in added[:3]) +
             (' ...' if len(added) > 3 else ''), current_magic))

# every marker that is no longer current has to stay recognised by the migration, otherwise the
# radios still carrying it are treated as if they had the current map
older = [m for m, _name in LAYOUT_HISTORY if m != current_magic]
if older:
    migrate_fn = re.search(r'static\s+bool\s+UiConfiguration_LayoutMigrationRequired\s*\([^)]*\)\s*'
                           r'\{(.*?)\n\}', clean_cfg_c, re.S)
    if not migrate_fn:
        sys.exit('UiConfiguration_LayoutMigrationRequired() is gone from ui_configuration.c, the '
                 'markers of older layouts cannot be checked')
    migrate_body = migrate_fn.group(1)
    defined = {name: int(value, 0) for name, value in
               re.findall(r'(?m)^\s*#define\s+([A-Za-z0-9_]+)\s+((?:0[xX])?[0-9A-Fa-f]+)\s*(?://|$)',
                          strip_comments(cfg_h))}
    for marker in older:
        spelled = any(t in migrate_body for t in ('0x%X' % marker, '0x%x' % marker, '%d' % marker))
        via_define = any(value == marker and re.search(r'\b%s\b' % name, migrate_body)
                         for name, value in defined.items())
        if not (spelled or via_define):
            sys.exit('LAYOUT_HISTORY records a map under marker 0x%X, but '
                     'UiConfiguration_LayoutMigrationRequired() no longer mentions that marker. A '
                     'radio holding a configuration written with it is read through the current '
                     'address map, which is the silent renumbering this check exists to prevent. '
                     'Recognise the old marker there, or remove the pair only if no radio can be '
                     'holding it.' % marker)

# -----------------------------------------------------------------------------------------------
# The address translation of the previous layout, compiled and compared with the two records
#
# UiConfiguration_OldLayoutAddress() is hand written arithmetic: it maps every current id to the
# word the layout before the band renumbering stored that value at, and reports 0xFFFF for a value
# that did not exist there. A wrong offset in one of its branches fails nothing else in the tree:
# the firmware still compiles, the current map is untouched, and only a radio holding the previous
# configuration is read through the wrong words - silently, because every word it does read holds
# a plausible value of some other setting.
#
# So the function itself is compiled for the host, together with the record of the previous layout
# and the record of the current marker, and every current word is asked what the two maps imply:
#   - a value both records name once: exactly the word the previous record gives it;
#   - a per band block, whose words are positions that were renumbered: any word of the block the
#     previous record gives it, and no word twice;
#   - a value the previous layout did not have: 0xFFFF.
# The bands are compared as blocks because the record names them by position - EEPROM_BAND5_5W is
# the sixth slot, not 5m - so which band a slot held cannot be read from it. A branch that trades
# two bands keeps every block inside its range and passes; a branch that moves an offset does not.
PREVIOUS_LAYOUT_RECORD = 'layout-before-0x5AA5.map'
BAND_BLOCK_OWNERS = ('_MODE_A', '_MODE_B', '_FREQ_HIGH_A', '_FREQ_HIGH_B',
                     '_FREQ_LOW_A', '_FREQ_LOW_B', '_5W', '_FULL')


def layout_owner_key(owner):
    """the value a word belongs to, across the two records

    A record notes how a word is written (" (written outside the table)") or how many slots a
    block has, and it names the per band blocks by position, so the band number is dropped: the
    position is what was renumbered, not the value."""
    name = plain_name(owner)
    m = re.match(r'^EEPROM_BAND\d+(_\w+)$', name)
    return 'band' + m.group(1) if m and m.group(1) in BAND_BLOCK_OWNERS else name


def firmware_function(name):
    """one uint16_t function of ui_configuration.c, compiled for the host exactly as it is"""
    m = re.search(r'static\s+uint16_t\s+%s\s*\([^)]*\)\s*\{.*?\n\}' % name, clean_cfg_c, re.S)
    if not m:
        sys.exit('%s() is gone from ui_configuration.c or its signature changed, so the address '
                 'translation of the previous layout can no longer be compiled and checked. '
                 'Update this check together with the function.' % name)
    return m.group(0)


# The "(was <name>)" suffix is what tells the translation of the previous layout which value it is
# looking at: a value that kept its word and got another name is still the value that record names.
# A renumbering is not that - the guard above refuses a word that changed hands - so following a
# suffix cannot hide one, and because the suffix sits on the line of the word it describes it
# cannot name another word the way a line of its own could.
older_name = {}         # the word -> the name the previous record has for the value stored there
for word, owner in rec_map.items():
    had = alias_of(owner)
    if had:
        older_name[word] = layout_owner_key(had)


previous_map = read_previous_layout(PREVIOUS_LAYOUT_RECORD)
previous_keys = {}
for word, owner in previous_map.items():
    previous_keys.setdefault(layout_owner_key(owner), []).append(word)
for owner, words in sorted(previous_keys.items()):
    if len(words) > 1 and max(words) - min(words) + 1 != len(words):
        sys.exit('%s gives %s the words %s, which are not contiguous. The block check compares a '
                 'translated address with the range of the block, so a block has to be contiguous '
                 'in the record.' % (PREVIOUS_LAYOUT_RECORD, owner, ', '.join(map(str, sorted(words)))))

current_keys = {}
for word, owner in current_map.items():
    current_keys.setdefault(older_name.get(word, layout_owner_key(owner)), []).append(word)

# A rename keeps its word. So a word the previous layout stored under a name this table no longer
# has, and that now stores a name the previous layout never had, is either that rename or a new
# setting on a reused word - and the two records cannot tell those apart, which is what the
# "(was <name>)" suffix is for. Only one of them is safe to translate as it stands: the firmware
# still returns a renamed value's old address, so without the suffix this fails at the translation,
# naming a value the previous layout "did not have", instead of here.
withdrawn = []
for word, owner in sorted(previous_map.items()):
    if word not in current_map or layout_owner_key(owner) in current_keys:
        continue
    if plain_name(rec_map.get(word, '')) == plain_name(owner):
        continue        # the record still names the value the way the previous layout does
    if layout_owner_key(current_map[word]) in previous_keys:
        continue        # the word changed hands between two values the previous layout knew
    withdrawn.append((word, 'the previous layout stored %s, this table stores %s'
                      % (plain_name(owner), plain_name(current_map[word]))))
if withdrawn:
    sys.exit('%d word(s) below %d carry a name the previous layout does not have, at a word that '
             'now carries one it never had:\n%s\n'
             'A rename keeps its word, and the record follows one through the "(was <name>)" suffix '
             'on its word line. Without it the translation is judged against a value the previous '
             'layout did not have, while the firmware still returns the renamed value\'s old '
             'address, so the check fails on the translation and names that instead of this. The '
             'record of a new marker is written from the table and starts with no suffix, which is '
             'how a rename committed together with the marker bump gets here.\n'
             'Sequence the two changes: bump the marker first with the value still under its old '
             'name, then rename it in its own commit and run "make layout-record", which writes the '
             'name it had. If the bump is already recorded, add " (was <the old name>)" to that '
             'word\'s line in %s and reseal it with "python3 '
             'support/config_upgrade/layout_digest.py %s".\n'
             'If the value really was removed and its word given to a new setting, remove it and '
             'bump first, then add the new setting in a later commit: a lost suffix and a reused '
             'word read the same in the two records, so the guard will not guess between them.'
             % (len(withdrawn), BASELINE_LAYOUT_FIRST_UNUSED, describe(withdrawn),
                records[current_magic], records[current_magic]))

old_layout_rows = []            # (word, kind, lo, hi, owner) for every word of the current record
claimed_previous = set()
for key, cur_words in sorted(current_keys.items()):
    cur_words = sorted(cur_words)
    words = sorted(previous_keys.get(key, []))
    if not words:
        for word in cur_words:
            old_layout_rows.append((word, 'none', 0, 0, current_map[word]))
        continue
    claimed_previous.update(words)
    # A value stored in several consecutive words under one EEPROM name - the touchscreen
    # calibration is one 32 bit value in two words - is compared word by word in order. Anything
    # else with several words is a block whose words are positions, and only the range of the
    # block can be compared.
    runs = (cur_words == list(range(cur_words[0], cur_words[0] + len(cur_words))) and
            words == list(range(words[0], words[0] + len(words))) and
            len(words) == len(cur_words) and key.startswith('EEPROM_'))
    for offset, word in enumerate(cur_words):
        if runs:
            old_layout_rows.append((word, 'one', words[offset], words[offset], current_map[word]))
        elif len(words) == 1 and len(cur_words) == 1:
            old_layout_rows.append((word, 'one', words[0], words[0], current_map[word]))
        else:
            old_layout_rows.append((word, 'block', min(words), max(words), current_map[word]))
previous_only = len(set(previous_map) - claimed_previous)

# The band memories and the power factor rows are stored per band, and the bands were renumbered.
# A record names those words by position - EEPROM_BAND8_5W is the ninth slot, not 8m - so the
# range check above cannot see a translation that trades two bands: swapping 10m and 6m keeps
# every word inside its block. Both plans are declared, the previous one in the record and the
# current one in the BAND_MODE enum and the table macros, so every band word gets the exact old
# address of its band instead.
previous_text = (Path(__file__).resolve().parent / PREVIOUS_LAYOUT_RECORD).read_text(encoding='utf-8')
plan = re.search(r'(?m)^#bands\s+(.+)$', previous_text)
if not plan:
    sys.exit('%s does not declare its band plan in a "#bands" line. Without it the per band words '
             'can only be compared with the range of their block, and a translation that trades '
             'two bands passes.' % PREVIOUS_LAYOUT_RECORD)
old_bands = plan.group(1).split()

enum_next = 0
current_bands = []
for m in re.finditer(r'(?m)^\s*(BAND_MODE_\w+)\s*(?:=\s*(\d+))?\s*,',
                     open(root + 'drivers/ui/radio_management.h', encoding='utf-8',
                          errors='surrogateescape').read()):
    if m.group(1) == 'BAND_MODE_MAX':
        break
    enum_next = int(m.group(2)) if m.group(2) else enum_next
    current_bands.append((enum_next, m.group(1)))
    enum_next += 1
current_bands = [name for value, name in current_bands if value < MAX_BANDS]
if len(current_bands) != MAX_BANDS:
    sys.exit('%d bands below MAX_BANDS %d were found in the BAND_MODE enum of radio_management.h, '
             'the band plan of the current layout cannot be read' % (len(current_bands), MAX_BANDS))
if len(old_bands) != len(previous_keys['band_MODE_A']):
    sys.exit('%s declares %d bands but its per band memories have %d slots, so the plan and the map '
             'belong to different layouts'
             % (PREVIOUS_LAYOUT_RECORD, len(old_bands), len(previous_keys['band_MODE_A'])))

old_index = {band: slot for slot, band in enumerate(old_bands)}
old_bases = {key: min(previous_keys[key]) for key in
             ('band_5W', 'band_FULL', 'band_MODE_A', 'band_FREQ_HIGH_A', 'band_FREQ_LOW_A',
              'band_MODE_B', 'band_FREQ_HIGH_B', 'band_FREQ_LOW_B')}


def band_target(band, key):
    """the word the previous layout stored this band's value at, or none if it had no such band"""
    if band not in old_index:
        return ('none', 0, 0)
    return ('one', old_bases[key] + old_index[band], old_bases[key] + old_index[band])


band_overrides = {}
for kind_key, block_key in (('5W', 'band_5W'), ('FULL', 'band_FULL')):
    table_band = {}
    for m in re.finditer(r'UI_C_EEPROM_BAND_%s_PF\(\s*(\d+)\s*,\s*(\d+)\s*,' % kind_key, clean_cfg_c):
        table_band[int(m.group(1))] = 'BAND_MODE_' + m.group(2)
    if sorted(table_band) != list(range(len(table_band))):
        sys.exit('the %s power factor rows are not numbered 0..%d, the band of each row cannot be '
                 'read from the table macros' % (kind_key, len(table_band) - 1))
    for slot, band in table_band.items():
        band_overrides[hdr_define('EEPROM_BAND0_%s' % kind_key) + slot] = band_target(band, block_key)
vfo_base = hdr_define('EEPROM_BAND0_MODE_A')
for block, block_key in enumerate(('band_MODE_A', 'band_FREQ_HIGH_A', 'band_FREQ_LOW_A',
                                   'band_MODE_B', 'band_FREQ_HIGH_B', 'band_FREQ_LOW_B')):
    for slot, band in enumerate(current_bands):
        band_overrides[vfo_base + block * MAX_BANDS + slot] = band_target(band, block_key)

old_layout_rows = [(word, *(band_overrides.get(word) or (kind, lo, hi)), owner)
                   for word, kind, lo, hi, owner in old_layout_rows]

old_defines = []
start = clean_cfg_c.find('#define CONFIG_LAYOUT_OLD_NO_VALUE')
if start < 0:
    sys.exit('CONFIG_LAYOUT_OLD_NO_VALUE is gone from ui_configuration.c, so the address translation '
             'of the previous layout cannot be compiled for the host')
for line in clean_cfg_c[start:].splitlines():
    if not line.startswith('#define'):
        break
    old_defines.append(line)
translation = firmware_function('UiConfiguration_OldBandSlot') + '\n\n' + \
    firmware_function('UiConfiguration_OldLayoutAddress')
translation_defines = []
named = {re.match(r'#define\s+(\w+)', line).group(1) for line in old_defines}
for token in sorted(set(re.findall(r'\b[A-Z][A-Z0-9_]+\b', '\n'.join(old_defines) + '\n' + translation))):
    if token in named:
        continue
    value = resolve(token)
    if value is None:
        sys.exit('%s is used by the address translation of the previous layout but this harness '
                 'cannot work out its value, so the translation cannot be compiled for the host. '
                 'Teach gen_config_upgrade.py how to read it.' % token)
    translation_defines.append((token, value))

translation_c = []              # the section of config_upgrade.c which checks the translation
T = translation_c.append
T('/* ---------------------------------------------------------------------------------------------')
T('   The address translation of the previous layout, compiled from ui_configuration.c itself and')
T('   compared with the two recorded maps below. A wrong offset in one of its branches corrupts')
T('   every radio that is migrated, and nothing else in the tree would notice. */')
for line in old_defines:
    T(line)
for name, value in translation_defines:
    T('#define %s %d' % (name, value))
T('')
for line in translation.splitlines():
    T(line)
T('')
T('typedef enum { OLD_NONE, OLD_ONE, OLD_BLOCK } old_layout_kind_t;')
T('typedef struct { uint16_t id, lo, hi; old_layout_kind_t kind; const char* owner; } old_layout_t;')
T('static const old_layout_t old_layout[] =')
T('{')
for word, kind, lo, hi, owner in old_layout_rows:
    T('    { %d, %d, %d, %s, "%s" },'
      % (word, lo, hi, {'none': 'OLD_NONE', 'one': 'OLD_ONE', 'block': 'OLD_BLOCK'}[kind], owner))
T('};')
T('static const unsigned old_layout_count = sizeof(old_layout)/sizeof(old_layout[0]);')
T('')
T('static const char* old_layout_expectation(const old_layout_t* e)')
T('{')
T('    static char txt[32];')
T('    switch (e->kind)')
T('    {')
T('    case OLD_NONE:  return "that the value did not exist";')
T('    case OLD_ONE:   snprintf(txt, sizeof(txt), "word %u", e->lo); return txt;')
T('    default:        snprintf(txt, sizeof(txt), "a word in %u..%u", e->lo, e->hi); return txt;')
T('    }')
T('}')
T('')
T('static int check_old_layout_translation(void)')
T('{')
T('    static uint16_t seen[%d];' % IMAGE_SLOTS)
T('    unsigned bad = 0, mapped = 0;')
T('    memset(seen, 0, sizeof(seen));')
T('    for (unsigned i = 0; i < old_layout_count; i++)')
T('    {')
T('        const old_layout_t* e = &old_layout[i];')
T('        const uint16_t got = UiConfiguration_OldLayoutAddress(e->id);')
T('        int ok;')
T('        switch (e->kind)')
T('        {')
T('        case OLD_NONE:  ok = (got == CONFIG_LAYOUT_OLD_NO_VALUE); break;')
T('        case OLD_ONE:   ok = (got == e->lo); break;')
T('        default:        ok = (got == CONFIG_LAYOUT_OLD_NO_VALUE) ||')
T('                            (got >= e->lo && got <= e->hi); break;')
T('        }')
T('        if (got != CONFIG_LAYOUT_OLD_NO_VALUE)')
T('        {')
T('            mapped++;')
T('            if (got < sizeof(seen)/sizeof(seen[0]))')
T('            {')
T('                if (seen[got])')
T('                {')
T('                    ok = 0;         /* two values translated onto the same word */')
T('                }')
T('                seen[got] = 1;')
T('            }')
T('        }')
T('        if (!ok)')
T('        {')
T('            bad++;')
T('            if (bad <= 10)')
T('            {')
T('                printf("CHECK FAILED: %s (id %u) translates to ", e->owner, e->id);')
T('                if (got == CONFIG_LAYOUT_OLD_NO_VALUE)')
T('                {')
T('                    printf("no previous address, the two records say %s\\n", old_layout_expectation(e));')
T('                }')
T('                else')
T('                {')
T('                    printf("%u, the two records say %s\\n", got, old_layout_expectation(e));')
T('                }')
T('            }')
T('        }')
T('    }')
T('    if (bad == 0)')
T('    {')
T('        printf("previous layout translation: %u stored values, %u of them land on a word the "')
T('               "recorded previous map gives them, none twice\\n\\n", old_layout_count, mapped);')
T('    }')
T('    return bad ? 1 : 0;')
T('}')

# The lifecycle of a migration, which the static comparison of the two maps cannot see: the save
# has to carry each translated value to its current word and write the marker, and the marker has
# to be what stops the next boot from translating the configuration a second time.
lifecycle_c = []
L2 = lifecycle_c.append
L2('/* ---------------------------------------------------------------------------------------------')
L2('   the migration lifecycle: one save turns a previous layout configuration into a current one,')
L2('   and the marker that save writes stops the next boot from translating it once more */')
L2('#define MARKER_WORD %du' % hdr_define('EEPROM_CONFIG_LAYOUT_VERSION'))
L2('#define LAYOUT_MAGIC 0x%Xu' % current_magic)
L2('')
L2('/* mirror of UiConfiguration_LayoutMigrationRequired(): only the current magic means the words')
L2('   were stored in this layout already */')
L2('static bool needs_migration(const image_t* img)')
L2('{')
L2('    return !(img->present[MARKER_WORD] && img->value[MARKER_WORD] == LAYOUT_MAGIC);')
L2('}')
L2('')
L2('/* the save of a migrated configuration: every value read through the translation is written')
L2('   at its current word, and the marker is written last */')
L2('static void save_as_current(image_t* img)')
L2('{')
L2('    const image_t before = *img;')
L2('    for (unsigned w = 0; w < FIRST_UNUSED; w++)')
L2('    {')
L2('        const uint16_t old = UiConfiguration_OldLayoutAddress((uint16_t)w);')
L2('        if (old != CONFIG_LAYOUT_OLD_NO_VALUE && old < %d)' % IMAGE_SLOTS)
L2('        {')
L2('            img->value[w] = before.value[old];')
L2('            img->present[w] = before.present[old];')
L2('        }')
L2('    }')
L2('    img->value[MARKER_WORD] = LAYOUT_MAGIC;')
L2('    img->present[MARKER_WORD] = true;')
L2('}')
L2('')
L2('static int check_migration_lifecycle(void)')
L2('{')
L2('    unsigned probed = 0;')
L2('    for (unsigned i = 0; i < old_layout_count; i++)')
L2('    {')
L2('        const old_layout_t* e = &old_layout[i];')
L2('        image_t img;')
L2('        if (e->kind != OLD_ONE)')
L2('        {')
L2('            continue;           /* a block word needs its band, the translation check has it */')
L2('        }')
L2('        blank_image(&img);')
L2('        img.value[e->lo] = 0x1234;')
L2('        img.present[e->lo] = true;')
L2('        if (!needs_migration(&img))')
L2('        {')
L2('            printf("CHECK FAILED: a configuration without the layout marker is not migrated, "')
L2('                   "its words are read as if they were current\\n");')
L2('            return 1;')
L2('        }')
L2('        save_as_current(&img);')
L2('        if (needs_migration(&img))')
L2('        {')
L2('            printf("CHECK FAILED: the save did not mark the configuration as current, so the "')
L2('                   "next boot translates %s (id %u) again\\n", e->owner, e->id);')
L2('            return 1;')
L2('        }')
L2('        if (!img.present[e->id] || img.value[e->id] != 0x1234)')
L2('        {')
L2('            printf("CHECK FAILED: %s (id %u) was stored at word %u in the previous layout and "')
L2('                   "did not arrive at its current word after the save\\n",')
L2('                   e->owner, e->id, e->lo);')
L2('            return 1;')
L2('        }')
L2('        probed++;')
L2('    }')
L2('    printf("migration lifecycle: %u values stored in the previous layout are read at their "')
L2('           "current word after one save, and the marker that save writes stops the next boot "')
L2('           "from translating them again\\n\\n", probed);')
L2('    return 0;')
L2('}')

# The shapes a save leaves behind when it does not finish. None of them is a configuration: a chip
# that was never written, a save that wrote the marker and died, and a save that wrote values and
# died before the marker. The loader has to hand every row its default rather than a word that was
# half written, and an image carrying the current marker must not be translated again.
lifecycle_c.extend(r"""
/* ---------------------------------------------------------------------------------------------
   the shapes a save leaves behind when it does not finish. A word that was never written is not a
   value: every row has to read its default rather than a number made of a blank or stale
   neighbour, and an image that carries the current marker must not be translated again */
static int expect_read(const rrow_t* r, int32_t got, int32_t want, const char* shape)
{
    if (got == want)
    {
        return 0;
    }
    printf("CHECK FAILED: %s (id %u) reads %d instead of %d on %s\n", r->idname, r->id, got, want, shape);
    return 1;
}

static int check_interrupted_save(void)
{
    image_t img;
    const rrow_t* partial = NULL;
    int rc = 0;

    /* a chip that was never written: nothing is stored, so every row loads its default, and the
       absent marker means the boot translates it - which on an empty image has to change nothing */
    blank_image(&img);
    for (unsigned i = 0; i < rrow_count; i++)
    {
        rc |= expect_read(&rrows[i], row_read(&img, &rrows[i], false), rrows[i].def,
                          "a chip that was never written");
    }
    if (!needs_migration(&img))
    {
        printf("CHECK FAILED: a chip that was never written carries the layout marker, so its words "
               "are read as if they were current\n");
        rc = 1;
    }
    save_as_current(&img);
    for (unsigned i = 0; i < rrow_count; i++)
    {
        rc |= expect_read(&rrows[i], row_read(&img, &rrows[i], false), rrows[i].def,
                          "a chip that was never written, after the save that boot causes");
    }

    /* the marker was written and the save stopped there: the words claim the current layout, so
       nothing may be translated, and a row the save never reached has to read its default */
    blank_image(&img);
    for (unsigned i = 0; i < rrow_count; i += 2)
    {
        row_store(&img, &rrows[i], rrows[i].max);
    }
    img.present[MARKER_WORD] = true;
    img.value[MARKER_WORD] = LAYOUT_MAGIC;
    if (needs_migration(&img))
    {
        printf("CHECK FAILED: an image carrying the current layout marker is migrated, so every "
               "value is read through the translation of the previous layout\n");
        rc = 1;
    }
    for (unsigned i = 0; i < rrow_count; i++)
    {
        const int32_t want = (i % 2) ? rrows[i].def : rrows[i].max;
        rc |= expect_read(&rrows[i], row_read(&img, &rrows[i], false), want,
                          (i % 2) ? "a half written image, a row the save never reached"
                                  : "a half written image, a row the save reached");
    }
    /* an Int32 row with only its high word written: half a value is not a value */
    for (unsigned i = 0; i < rrow_count; i++)
    {
        if (rrows[i].type != R_INT32)
        {
            continue;
        }
        blank_image(&img);
        img.present[rrows[i].id] = true;
        img.value[rrows[i].id] = 0x1111;
        rc |= expect_read(&rrows[i], row_read(&img, &rrows[i], false), rrows[i].def,
                          "an Int32 row whose low word was never written");
        break;
    }

    /* the save wrote values and stopped before the marker: the boot migrates, and what it installs
       are the words the previous layout held. The words the interrupted save wrote are replaced,
       not mixed into the value that comes back */
    blank_image(&img);
    for (unsigned i = 0; i < rrow_count; i++)
    {
        const rrow_t* r = &rrows[i];
        const uint16_t old = UiConfiguration_OldLayoutAddress((uint16_t)r->id);
        if (r->type == R_INT32 || old == CONFIG_LAYOUT_OLD_NO_VALUE || old >= FIRST_UNUSED)
        {
            continue;
        }
        img.present[old] = true;
        img.value[old] = (uint16_t)r->max;
    }
    /* a row the interrupted save can reach without writing a word the migration has to read */
    for (unsigned i = 0; i < rrow_count; i++)
    {
        const rrow_t* r = &rrows[i];
        bool taken = false;
        if (r->type == R_INT32 || r->id == MARKER_WORD ||
            UiConfiguration_OldLayoutAddress((uint16_t)r->id) == CONFIG_LAYOUT_OLD_NO_VALUE)
        {
            continue;
        }
        for (unsigned j = 0; j < rrow_count; j++)
        {
            if (rrows[j].type != R_INT32 &&
                UiConfiguration_OldLayoutAddress((uint16_t)rrows[j].id) == r->id)
            {
                taken = true;
                break;
            }
        }
        if (!taken)
        {
            partial = r;
            break;
        }
    }
    if (partial)
    {
        row_store(&img, partial, partial->min);
    }
    if (!needs_migration(&img))
    {
        printf("CHECK FAILED: a save that stopped before the marker leaves an image that is not "
               "migrated\n");
        rc = 1;
    }
    save_as_current(&img);
    if (needs_migration(&img))
    {
        printf("CHECK FAILED: after the boot that migrates an interrupted save the configuration is "
               "still not current, so every boot translates it again\n");
        rc = 1;
    }
    for (unsigned i = 0; i < rrow_count; i++)
    {
        const rrow_t* r = &rrows[i];
        const uint16_t old = UiConfiguration_OldLayoutAddress((uint16_t)r->id);
        if (r->type == R_INT32 || old == CONFIG_LAYOUT_OLD_NO_VALUE || old >= FIRST_UNUSED)
        {
            continue;
        }
        rc |= expect_read(r, row_read(&img, r, false), r->max,
                          "an interrupted save, after the boot that migrates it");
    }

    if (rc == 0)
    {
        printf("interrupted saves: a never written chip and a row the save never reached read their "
               "defaults, an image with the current marker is not translated, and an image whose "
               "save stopped before the marker loses the words that save wrote\n\n");
    }
    return rc;
}
""".strip('\n').split('\n'))

print('  previous layout translation: %d words of the current map, %d with the exact word the '
      'previous layout stored them at (%d of them per band, from the %d band plans), %d compared '
      'with the range of their block, %d values the previous layout did not have, %d words of the '
      'previous layout left without a current value'
      % (len(old_layout_rows), sum(1 for r in old_layout_rows if r[1] == 'one'),
         sum(1 for w in band_overrides if w in dict((r[0], r) for r in old_layout_rows)),
         len(old_bands), sum(1 for r in old_layout_rows if r[1] == 'block'),
         sum(1 for r in old_layout_rows if r[1] == 'none'), previous_only))

print('  EEPROM address map: %d table rows (after expanding %d row macros) and %d multi word '
      'blocks, %d words claimed, none twice, %d words of the legacy filter range kept free (%s)'
      % (len(map_rows), len(row_macros), len(bulk_blocks), len(owners),
         len(LEGACY_FREE_WORDS), ', '.join(str(w) for w in LEGACY_FREE_WORDS)))

# the map is replayed by the generated program as well, so that the order in which the firmware
# writes it is part of the check and not only the addresses: writing the table first and a block
# that covers it afterwards is what destroyed the settings
rt_rows = []                          # (word, idname) of every row of the compiled table
for cond, ctype, idname, var, d, lo, hi in map_rows:
    if idname not in vals:
        continue
    if vals[idname] >= IMAGE_SLOTS:
        sys.exit('%s has id %d which does not fit the host image of %d slots' % (idname, vals[idname], IMAGE_SLOTS))
    if vals[idname] >= first_unused:
        sys.exit('%s has id %d but EEPROM_FIRST_UNUSED is only %d, so the load of a configuration '
                 'stored by an older firmware resets it to its default on every boot'
                 % (idname, vals[idname], first_unused))
    span = 2 if ctype == 'ConfigEntry_Int32' else 1
    for w in range(vals[idname], vals[idname] + span):
        rt_rows.append((w, idname))
rt_blocks = [(base, words, who) for base, words, who in bulk_blocks if words > 1]

# The stored entry count (EEPROM_NUMBER_OF_ENTRIES) is the ceiling that UiConfiguration_Needs-
# DefaultIndex() compares every id against: ids above it are taken to be absent from the stored
# configuration and are loaded as defaults. number_of_entries_cur_fw is assigned nowhere else in
# the firmware, it only ever holds what the last read left there, so the save path has to record
# the ceiling of the firmware doing the write. If it does not, the count left behind by an older
# firmware resets every id introduced since then on every single boot.
write_entries = re.search(r'uint16_t\s+UiConfiguration_WriteConfigEntries\s*\(\s*\)\s*\{(.*?)\n\}',
                          strip_comments(cfg_c), re.S)
if not write_entries:
    sys.exit('UiConfiguration_WriteConfigEntries() did not match, update gen_config_upgrade.py')
save_records_entry_count = bool(re.search(
    r'number_of_entries_cur_fw\s*=\s*EEPROM_FIRST_UNUSED\s*;', write_entries.group(1)))

print('parsed from the repository:')
for var, idname, eid, d, lo, hi in entries:
    print('  %-22s %-26s id %3d  default %3d  range %3d..%3d' % (var, idname, eid, d, lo, hi))
print('  EEPROM_FIRST_UNUSED = %d (stored entry count of the current build)\n' % first_unused)
print('  save path records that count: %s' % ('yes' if save_records_entry_count else
      'NO - ids above the count of an older firmware reset on every boot'))
print('WDSP AGC parameters:')
for key in sorted(agc_stored, key=lambda k: agc_stored[k]['id']):
    e = agc_stored[key]
    menu = agc_menu.get(key)
    print('  %-18s %-26s id %3d  %-19s default %5d  stored %6d..%-6d  menu %s'
          % (key, e['idname'], e['id'], e['ctype'], e['def'], e['min'], e['max'],
             '%d..%d (default %d)' % menu if menu else 'not editable'))
for key in sorted(agc_menu):
    menu = agc_menu[key]
    stored = agc_stored.get(key)
    if stored and '[' not in key and menu[2] != stored['def']:
        sys.exit('agc_wdsp_conf.%s: the menu restores %d but the stored factory default is %d, the '
                 'operator would get one value from the menu reset and another one from a '
                 'configuration reset. Both have to use the AGC_WDSP_*_DEFAULT of audio_agc.h.'
                 % (key, menu[2], stored['def']))


L = []
A = L.append
A('/* generated from the repository headers by gen_config_upgrade.py, do not edit */')
A('#include <stdio.h>')
A('#include <stdint.h>')
A('#include <stdbool.h>')
A('#include <string.h>')
A('')
A('#define FIRST_UNUSED %du' % first_unused)
A('#define PREVIOUS_FIRST_UNUSED %du' % agc_first_unused_before)
A('#define SAVE_RECORDS_ENTRY_COUNT %d' % (1 if save_records_entry_count else 0))
A('')
for line in translation_c:
    A(line)
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
A('typedef struct { uint16_t value[%d]; bool present[%d]; } image_t;' % (IMAGE_SLOTS, IMAGE_SLOTS))
A('')
A('/* a value an older firmware never stored is simply erased in the image */')
A('static void erase(image_t* img, uint16_t id)')
A('{')
A('    img->value[id] = 0xFFFF;')
A('    img->present[id] = false;')
A('}')
A('')
A('/* mirror of UiReadSettingEEPROM_UInt8(): a failed read, an out of range value or a requested')
A('   default loads the default, everything else is used as stored.')
A('   Note: the firmware truncates the stored 16 bit value to uint8 BEFORE the range check, this')
A('   mirror checks the 16 bit value. The two agree as long as the maximum stays below 255, the')
A('   firmware default value is always in range and the erased value is 0xFFFF */')
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
A('/* ---------------------------------------------------------------------------------------------')
A('   the WDSP AGC parameters, parsed from the ConfigEntry table, the AGC menu items and')
A('   agc_wdsp_params_t */')
A('typedef enum { ST_UINT8, ST_INT32_16 } agc_stype_t;')
A('typedef struct')
A('{')
A('    const char* name;')
A('    const char* idname;')
A('    uint16_t id;')
A('    agc_stype_t stype;')
A('    int32_t def, min, max;')
A('    int32_t menu_min, menu_max, menu_def;     /* what the AGC menu offers, menu_max < 0 if not editable */')
A('} agc_entry_t;')
A('static const agc_entry_t agc_entries[] =')
A('{')
for key in sorted(agc_stored, key=lambda k: agc_stored[k]['id']):
    e = agc_stored[key]
    menu = agc_menu.get(key)
    A('    { "%s", "%s", %d, %s, %d, %d, %d, %d, %d, %d },'
      % (key, e['idname'], e['id'], agc_c_types[e['ctype']], e['def'], e['min'], e['max'],
         menu[0] if menu else 0, menu[1] if menu else -1, menu[2] if menu else 0))
A('};')
A('static const unsigned agc_entry_count = sizeof(agc_entries)/sizeof(agc_entries[0]);')
A('')
A('/* what the write path of UiConfiguration_WriteConfigEntryData() leaves in the 16 bit slot */')
A('static uint16_t agc_store_bits(const agc_entry_t* e, int32_t v)')
A('{')
A('    return e->stype == ST_UINT8 ? (uint16_t)(uint8_t)v : (uint16_t)(int16_t)v;')
A('}')
A('')
A('/* what the read path makes out of that slot: both UiReadSettingEEPROM_UInt8() and')
A('   UiReadSettingEEPROM_Int32_16() convert to the type of the ram variable BEFORE the range check */')
A('static int32_t agc_load_bits(const agc_entry_t* e, uint16_t raw)')
A('{')
A('    return e->stype == ST_UINT8 ? (int32_t)(uint8_t)raw : (int32_t)(int16_t)raw;')
A('}')
A('')
A('static void agc_save(image_t* img, const agc_entry_t* e, int32_t v)')
A('{')
A('    img->value[e->id] = agc_store_bits(e, v);')
A('    img->present[e->id] = true;')
A('}')
A('')
A('static int32_t agc_read(const image_t* img, const agc_entry_t* e, bool load_default, store_t store)')
A('{')
A('    if (store == STORE_FLASH && !img->present[e->id])')
A('    {')
A('        return e->def;                        /* a flash slot that was never written fails to read */')
A('    }')
A('    const int32_t v = agc_load_bits(e, img->value[e->id]);')
A('    if ((v < e->min) || (v > e->max) || load_default)')
A('    {')
A('        return e->def;')
A('    }')
A('    return v;')
A('}')
A('')
A('static const char* agc_reason(const image_t* img, const agc_entry_t* e, int32_t force_above, store_t store)')
A('{')
A('    if (e->id > force_above)         return "not in the stored configuration";')
A('    if (store == STORE_FLASH && !img->present[e->id]) return "never written, read fails";')
A('    if (!img->present[e->id])        return "never written";')
A('    const int32_t v = agc_load_bits(e, img->value[e->id]);')
A('    if (v < e->min || v > e->max)    return "stored value out of range";')
A('    return "loaded from storage";')
A('}')
A('')
A('static void run_agc(const char* title, const image_t* img, uint16_t stored_count, store_t store)')
A('{')
A('    const int32_t force_above = needs_default_index(stored_count, store);')
A('    printf("%s\\n  stored entry count %u, %s\\n", title, stored_count,')
A('           store == STORE_I2C ? "serial EEPROM / RAM cache" : "flash");')
A('    for (unsigned i = 0; i < agc_entry_count; i++)')
A('    {')
A('        const agc_entry_t* e = &agc_entries[i];')
A('        const int32_t v = agc_read(img, e, e->id > force_above, store);')
A('        char stored[16];')
A('        if (img->present[e->id]) snprintf(stored, sizeof(stored), "%d", (int)img->value[e->id]);')
A('        else                     snprintf(stored, sizeof(stored), "erased");')
A('        printf("    %-18s id %3u  stored %-8s -> %6d  %s\\n", e->name, e->id, stored,')
A('               (int)v, agc_reason(img, e, force_above, store));')
A('    }')
A('    printf("\\n");')
A('}')
A('')
A('/* every value the AGC menu can produce has to survive a save/load cycle, on the serial EEPROM')
A('   as well as on the flash of the boards that have no external EEPROM */')
A('static int check_upgrade_persistence(void)')
A('{')
A('    int rc = 0;')
A('    for (unsigned i = 0; i < agc_entry_count; i++)')
A('    {')
A('        const agc_entry_t* e = &agc_entries[i];')
A('        if (e->id < PREVIOUS_FIRST_UNUSED)')
A('        {')
A('            continue;                 /* the older firmware knew this one already */')
A('        }')
A('        const int32_t want = e->menu_max >= 0 ? e->menu_max : e->max;')
A('        if (want < e->min || want > e->max || agc_load_bits(e, agc_store_bits(e, want)) != want)')
A('        {')
A('            continue;')
A('        }')
A('        image_t img;')
A('        memset(&img, 0, sizeof(img));')
A('        for (unsigned k = 0; k < sizeof(img.value)/sizeof(img.value[0]); k++)')
A('        {')
A('            img.value[k] = 0xFFFF;')
A('        }')
A('        uint16_t count = PREVIOUS_FIRST_UNUSED;')
A('')
A('        /* first boot after the upgrade: the id did not exist, the default is expected */')
A('        if (agc_read(&img, e, e->id > needs_default_index(count, STORE_I2C), STORE_I2C) != e->def)')
A('        {')
A('            printf("CHECK FAILED: %s: a value unknown to the older firmware does not come up as default\\n", e->idname);')
A('            rc = 1;')
A('        }')
A('')
A('        /* the operator sets it, the firmware saves */')
A('        agc_save(&img, e, want);')
A('        if (SAVE_RECORDS_ENTRY_COUNT)')
A('        {')
A('            count = FIRST_UNUSED;')
A('        }')
A('')
A('        /* the next boot has to find it */')
A('        const int32_t got = agc_read(&img, e, e->id > needs_default_index(count, STORE_I2C),')
A('                                      STORE_I2C);')
A('        if (got != want)')
A('        {')
A('            printf("CHECK FAILED: %s (id %u) set after an upgrade is lost on the next boot\\n", e->idname, e->id);')
A('            printf("           the stored entry count stayed %u while EEPROM_FIRST_UNUSED is %u,\\n",')
A('                   (unsigned)count, FIRST_UNUSED);')
A('            printf("           so the load resets the value back to its default.\\n");')
A('            printf("           UiConfiguration_WriteConfigEntries() has to assign\\n");')
A('            printf("           number_of_entries_cur_fw = EEPROM_FIRST_UNUSED before saving.\\n");')
A('            rc = 1;')
A('        }')
A('    }')
A('    return rc;')
A('}')
A('')
A('/* ---------------------------------------------------------------------------------------------')
A('   the EEPROM address map of this build, replayed in the order in which the firmware writes it.')
A('   UiConfiguration_SaveEepromValues() writes the whole ConfigEntry table first and the multi word')
A('   blocks afterwards, so a word that a block covers never keeps the value of the row that claims')
A('   it. That is what destroyed the I2C bus speeds, the WDSP AGC threshold and the receive IQ')
A('   autocorrection: the filter path memories owned their words. Every row has to still own its')
A('   words once the save order above ran through.')
A('*/')
A('typedef struct { uint16_t word; const char* idname; } map_row_t;')
A('static const map_row_t map_rows_rt[] = {')
for w, idname in sorted(set(rt_rows)):
    A('    { %d, "%s" },' % (w, idname))
A('};')
A('static const unsigned map_row_count = sizeof(map_rows_rt)/sizeof(map_rows_rt[0]);')
A('')
A('typedef struct { uint16_t base; uint16_t words; const char* name; } map_block_t;')
A('static const map_block_t map_blocks_rt[] = {')
for base, words, who in rt_blocks:
    A('    { %d, %d, "%s" },' % (base, words, who))
A('};')
A('static const unsigned map_block_count = sizeof(map_blocks_rt)/sizeof(map_blocks_rt[0]);')
A('')
A('static int check_save_order(void)')
A('{')
A('    int owner[%d];        /* >= 0: index into map_rows_rt, < 0: -(block + 1) */' % IMAGE_SLOTS)
A('    for (unsigned i = 0; i < %d; i++) owner[i] = -1000;' % IMAGE_SLOTS)
A('    for (unsigned i = 0; i < map_row_count; i++) owner[map_rows_rt[i].word] = (int)i;')
A('    for (unsigned b = 0; b < map_block_count; b++)')
A('    {')
A('        for (unsigned k = 0; k < map_blocks_rt[b].words; k++)')
A('        {')
A('            owner[map_blocks_rt[b].base + k] = -((int)b + 1);')
A('        }')
A('    }')
A('    int rc = 0;')
A('    int shown = 0;')
A('    for (unsigned i = 0; i < map_row_count; i++)')
A('    {')
A('        const int o = owner[map_rows_rt[i].word];')
A('        if (o == (int)i)')
A('        {')
A('            continue;')
A('        }')
A('        if (shown++ < 10)')
A('        {')
A('            printf("CHECK FAILED: after a save the row %s no longer owns word %u, %s wrote it "')
A('                   "last, the setting is destroyed on every power off\\n",')
A('                   map_rows_rt[i].idname, map_rows_rt[i].word,')
A('                   o < 0 ? map_blocks_rt[-o - 1].name : "another table row");')
A('        }')
A('        rc = 1;')
A('    }')
A('    if (rc == 0)')
A('    {')
A('        printf("save order: %d table words and %d multi word blocks, every table row still owns "')
A('               "its words after the save\\n\\n", map_row_count, map_block_count);')
A('    }')
A('    return rc;')
A('}')
A('')
A('/* ---------------------------------------------------------------------------------------------')
A('   the filter path memories: which base a stored configuration is read from.')
A('')
A('   UiReadSettingEEPROM_Filter() decides it, and the case that matters is a configuration of the')
A('   previous layout: words %d..%d then held other ids, so the "has this block ever been written"'
  % (fmap_base, fmap_base + filter_words - 1))
A('   probe finds data in every old configuration and cannot be asked first.')
A('*/')
A('#define FILTER_MAP_BASE   %du' % fmap_base)
A('#define FILTER_MAP_WORDS  %du' % filter_words)
A('#define FILTER_MAP_LEGACY %du' % fmap_legacy)
A('')
A('/* mirror of UiReadSettingEEPROM_Filter(): the previous layout is handled before the probe, a')
A('   factory reset reads the current block with defaults, otherwise the probe decides */')
A('static uint16_t filter_map_base(const image_t* img, bool migrate, bool load_default)')
A('{')
A('    if (migrate)')
A('    {')
A('        return FILTER_MAP_LEGACY;')
A('    }')
A('    if (load_default)')
A('    {')
A('        return FILTER_MAP_BASE;')
A('    }')
A('    for (unsigned k = 0; k < FILTER_MAP_WORDS; k++)')
A('    {')
A('        if (img->present[FILTER_MAP_BASE + k] && img->value[FILTER_MAP_BASE + k] != 0xFFFF)')
A('        {')
A('            return FILTER_MAP_BASE;')
A('        }')
A('    }')
A('    return FILTER_MAP_LEGACY;')
A('}')
A('')
A('static int check_filter_map_migration(void)')
A('{')
A('    image_t previous, moved, current;')
A('    int rc = 0;')
A('')
A('    /* a configuration of the previous layout: its filter memories sat at the legacy base, and')
A('       the words of the current block held the ids that now live at 334..358, so they are written')
A('       there too - which is exactly why the probe cannot tell the two layouts apart */')
A('    memset(&previous, 0, sizeof(previous));')
A('    for (unsigned i = 0; i < sizeof(previous.value)/sizeof(previous.value[0]); i++)')
A('    {')
A('        previous.value[i] = 0xFFFF;')
A('    }')
A('    for (unsigned k = 0; k < FILTER_MAP_WORDS; k++)')
A('    {')
A('        previous.present[FILTER_MAP_LEGACY + k] = true;')
A('        previous.value[FILTER_MAP_LEGACY + k] = (uint16_t)(1 + k);   /* the operator memories */')
A('        previous.present[FILTER_MAP_BASE + k] = true;')
A('        previous.value[FILTER_MAP_BASE + k] = (uint16_t)(40 + k);    /* another setting of that layout */')
A('    }')
A('')
A('    /* the layout just before this one: block never written, so the probe has to say so */')
A('    moved = previous;')
A('    for (unsigned k = 0; k < FILTER_MAP_WORDS; k++)')
A('    {')
A('        erase(&moved, (uint16_t)(FILTER_MAP_BASE + k));')
A('    }')
A('')
A('    /* the current layout: the block holds the operator memories */')
A('    current = previous;')
A('    for (unsigned k = 0; k < FILTER_MAP_WORDS; k++)')
A('    {')
A('        erase(&current, (uint16_t)(FILTER_MAP_LEGACY + k));')
A('        current.value[FILTER_MAP_BASE + k] = (uint16_t)(60 + k);')
A('    }')
A('')
A('    const struct { const char* what; const image_t* img; bool migrate; bool load_default; uint16_t want; } cases[] =')
A('    {')
A('        { "configuration of the previous layout",    &previous, true,  false, FILTER_MAP_LEGACY },')
A('        { "layout before this one, block unwritten", &moved,    false, false, FILTER_MAP_LEGACY },')
A('        { "current layout, block written",           &current,  false, false, FILTER_MAP_BASE },')
A('        { "factory reset",                           &current,  false, true,  FILTER_MAP_BASE },')
A('    };')
A('')
A('    for (unsigned c = 0; c < sizeof(cases)/sizeof(cases[0]); c++)')
A('    {')
A('        const uint16_t got = filter_map_base(cases[c].img, cases[c].migrate, cases[c].load_default);')
A('        if (got != cases[c].want)')
A('        {')
A('            printf("CHECK FAILED: %s reads the filter path memories from %u, expected %u\\n",')
A('                   cases[c].what, got, cases[c].want);')
A('            rc = 1;')
A('        }')
A('    }')
A('')
A('    /* and the previous layout has to deliver the operator memories, not the values of the ids')
A('       that took those words later */')
A('    const uint16_t base = filter_map_base(&previous, true, false);')
A('    for (unsigned k = 0; k < FILTER_MAP_WORDS; k++)')
A('    {')
A('        if (previous.value[base + k] != (uint16_t)(1 + k))')
A('        {')
A('            printf("CHECK FAILED: previous layout filter memory %u reads %u from word %u, expected %u\\n",')
A('                   k, previous.value[base + k], base + k, 1 + k);')
A('            rc = 1;')
A('            break;')
A('        }')
A('    }')
A('')
A('    if (rc == 0)')
A('    {')
A('        printf("filter path memories: previous layout from %u, unwritten block from %u, current from %u, "')
A('               "factory reset from %u\\n\\n",')
A('               filter_map_base(&previous, true, false), filter_map_base(&moved, false, false),')
A('               filter_map_base(&current, false, false), filter_map_base(&current, false, true));')
A('    }')
A('    return rc;')
A('}')
A('')
A('/* ---------------------------------------------------------------------------------------------')
A('   the migration has to finish: once a migrated radio saves, the block sits at the current base,')
A('   the probe finds it and the next boot reads it like any other configuration. A save that writes')
A('   the legacy base, or nothing, leaves the radio taking the legacy branch on every boot.')
A('*/')
A('static void save_filter_memories(image_t* img, const uint8_t* mem)')
A('{')
A('    /* mirror of UiWriteSettingEEPROM_Filter(): every memory goes to the current block */')
A('    for (unsigned k = 0; k < FILTER_MAP_WORDS; k++)')
A('    {')
A('        img->present[FILTER_MAP_BASE + k] = true;')
A('        img->value[FILTER_MAP_BASE + k] = (uint16_t)mem[k];')
A('    }')
A('}')
A('')
A('static void load_filter_memories(const image_t* img, bool migrate, uint8_t* mem)')
A('{')
A('    const uint16_t base = filter_map_base(img, migrate, false);')
A('    for (unsigned k = 0; k < FILTER_MAP_WORDS; k++)')
A('    {')
A('        mem[k] = (uint8_t)img->value[base + k];')
A('    }')
A('}')
A('')
A('static int check_filter_map_save(void)')
A('{')
A('    image_t before, after;')
A('    uint8_t mem[FILTER_MAP_WORDS];')
A('    int rc = 0;')
A('')
A('    /* a migrated radio: the memories sit at the legacy base and the current block was never')
A('       written, which is the state the save has to move it out of */')
A('    memset(&before, 0, sizeof(before));')
A('    for (unsigned i = 0; i < sizeof(before.value)/sizeof(before.value[0]); i++)')
A('    {')
A('        before.value[i] = 0xFFFF;')
A('    }')
A('    for (unsigned k = 0; k < FILTER_MAP_WORDS; k++)')
A('    {')
A('        before.present[FILTER_MAP_LEGACY + k] = true;')
A('        before.value[FILTER_MAP_LEGACY + k] = (uint16_t)(1 + k);   /* the operator memories */')
A('    }')
A('    if (filter_map_base(&before, false, false) != FILTER_MAP_LEGACY)')
A('    {')
A('        printf("CHECK FAILED: a migrated configuration that has not been saved yet is already "')
A('               "read from the current block, so the save below proves nothing\\n");')
A('        rc = 1;')
A('    }')
A('')
A('    /* the boot that migrates reads the legacy memories, the save puts them at the current base */')
A('    load_filter_memories(&before, true, mem);')
A('    after = before;')
A('    save_filter_memories(&after, mem);')
A('    if (filter_map_base(&after, false, false) != FILTER_MAP_BASE)')
A('    {')
A('        printf("CHECK FAILED: after a migrated configuration is saved the next boot still reads "')
A('               "the filter path memories from %u instead of %u\\n",')
A('               filter_map_base(&after, false, false), FILTER_MAP_BASE);')
A('        return 1;')
A('    }')
A('    load_filter_memories(&after, false, mem);')
A('    for (unsigned k = 0; k < FILTER_MAP_WORDS; k++)')
A('    {')
A('        if (mem[k] != (uint8_t)(1 + k))')
A('        {')
A('            printf("CHECK FAILED: after saving a migrated configuration filter memory %u reads %u, "')
A('                   "expected %u\\n", k, mem[k], 1 + k);')
A('            return 1;')
A('        }')
A('    }')
A('')
A('    if (rc == 0)')
A('    {')
A('        printf("filter path memories: a migrated configuration is saved at %u..%u and the next boot "')
A('               "reads it from there\\n\\n", FILTER_MAP_BASE, FILTER_MAP_BASE + FILTER_MAP_WORDS - 1);')
A('    }')
# -----------------------------------------------------------------------------------------------
# One round trip per row of the table: the value the firmware writes has to be the value it reads
# back. The AM and AGC checks above were written by hand for the settings somebody was looking at;
# this walks every row of the compiled table that has a resolved stored range, which is where the
# signed 16 bit values and the Uint8 truncation actually live.
#
# The type rules are the firmware's own: UiReadSettingEEPROM_UInt8() assigns the stored word to a
# uint8_t BEFORE it range checks it, so 0x0107 is accepted as 7, and UiReadSettingEEPROM_Int32_16()
# casts the word to int16_t first, so a negative value survives as long as it is stored that way.
# -----------------------------------------------------------------------------------------------
rrow_data = []
unresolved = []
for cond, ctype, idname, var, d, lo, hi in map_rows:
    if ctype not in RTYPES:
        sys.exit('%s (&%s) is stored as %s, which the round trip check does not model. Add the type '
                 'to RTYPES in gen_config_upgrade.py.' % (idname, var, ctype))
    dv, mn, mx = resolve(d), resolve(lo), resolve(hi)
    if None in (dv, mn, mx):
        unresolved.append('%s (&%s)' % (idname, var))
        continue
    rtype, words = RTYPES[ctype]
    low, high = RTYPE_LIMITS[rtype]
    # a stored value the firmware has to reject. A row that spans the whole width of its type has
    # none: for an UInt8 row from 0 to 255 every stored word is a legal value after truncation.
    if rtype == 'R_UINT8X2':
        outside = None              # "just outside" is a property of one half here, not of the word
    else:
        outside = mx + 1 if mx < high else (mn - 1 if mn > low else None)
    rrow_data.append((idname, vals[idname], rtype, words, dv, mn, mx, outside))
# Every row has to be covered, not most of them. The only rows that can drop out here are the ones
# whose stored range is built from a macro resolve() does not expand, and a dropped row is a setting
# that nothing round trips. A count floor would let a table that grows hide the losses.
if unresolved:
    sys.exit('%d of the %d rows of the configuration table have a stored range this parser cannot '
             'expand, so the round trip check would skip %s. Teach resolve() the macro or make the '
             'range a plain constant: a row the check skips is a setting nothing checks.'
             % (len(unresolved), len(map_rows),
                ', '.join(unresolved[:4]) + (', ...' if len(unresolved) > 4 else '')))

A('    return rc;')
A('}')
A('')
def AL(text):
    """emit a block of C source line by line, exactly as written"""
    for line in text.strip('\n').split('\n'):
        A(line)

A('/* ---------------------------------------------------------------------------------------------')
A('   one round trip per row of the configuration table. The read rules are those of the firmware:')
A('   the stored word goes through the type before the range check, which is where a Uint8 row')
A('   loses its high byte and an Int32_16 row gets its sign back')
A('*/')
A('typedef enum { R_UINT8, R_UINT8X2, R_UINT16, R_UINT32_16, R_INT16, R_INT32_16, R_INT32 } rtype_t;')
A('typedef struct { const char* idname; uint16_t id; rtype_t type; uint16_t words; int32_t def, min, max, outside; } rrow_t;')
A('')
A('#define R_NO_OUTSIDE INT32_MIN')
A('static const rrow_t rrows[] =')
A('{')
for idname, eid, rtype, words, dv, mn, mx, outside in rrow_data:
    A('    { "%s", %du, %s, %du, %d, %d, %d, %s },'
      % (idname, eid, rtype, words, dv, mn, mx,
         'R_NO_OUTSIDE' if outside is None else str(outside)))
A('};')
A('static const unsigned rrow_count = sizeof(rrows)/sizeof(rrows[0]);')
A('')
AL(r"""
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
""")
A('')

# the lifecycle model reads and writes an image, so it belongs after the image helpers above
for line in lifecycle_c:
    A(line)
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
    value = demo_values[var]
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
A('    run("early build of the current layout, none of the AM additions present", &upstream, %du, STORE_I2C, false);' % upstream_first_unused)
A('    run("fork build before the meter settings", &before_meter, %d, STORE_I2C, false);' % ids['am_carrier_pep_hold'])
A('    run("fork build before the warning delay", &before_warn, %d, STORE_I2C, false);' % ids['am_carrier_pep_warn'])
A('    run("current build, everything stored", &img, FIRST_UNUSED, STORE_I2C, false);')
A('    run("current build on flash, warning delay never written", &before_warn, FIRST_UNUSED, STORE_FLASH, false);')
A('    run("factory reset of the current build", &img, FIRST_UNUSED, STORE_I2C, true);')
A('')
A('    /* ---------------------------------------------------------------------------------------------')
A('       the AGC values */')
A('    image_t agc_all;')
A('    memset(&agc_all, 0, sizeof(agc_all));')
A('    for (unsigned i = 0; i < sizeof(agc_all.value)/sizeof(agc_all.value[0]); i++)')
A('    {')
A('        agc_all.value[i] = 0xFFFF;')
A('    }')
A('    for (unsigned i = 0; i < agc_entry_count; i++)')
A('    {')
A('        const agc_entry_t* e = &agc_entries[i];')
A('        /* a stored value that differs from the default, so that the check below can tell the')
A('           two apart: the highest value of the range the menu offers */')
A('        agc_save(&agc_all, e, e->menu_max >= 0 ? e->menu_max : e->max);')
A('    }')
A('    image_t agc_before_hang_values = agc_all;')
for key in sorted(agc_stored, key=lambda k: agc_stored[k]['id']):
    e = agc_stored[key]
    if e['id'] >= agc_first_unused_before:
        A('    erase(&agc_before_hang_values, %d);   /* %s */' % (e['id'], key))
A('    run_agc("AGC, configuration stored before the hang and OFF mode values existed",')
A('            &agc_before_hang_values, %du, STORE_I2C);' % agc_first_unused_before)
A('    run_agc("AGC, current build, everything stored", &agc_all, FIRST_UNUSED, STORE_I2C);')
A('    run_agc("AGC, current build on flash", &agc_all, FIRST_UNUSED, STORE_FLASH);')
A('')
A('    /* the self checks which make this program fail, so that CI catches a broken configuration */')
A('    int rc = 0;')
A('')
A('    rc |= check_row_roundtrip();')
A('    rc |= check_upgrade_persistence();')
A('    rc |= check_save_order();')
A('    rc |= check_filter_map_migration();')
A('    rc |= check_filter_map_save();')
A('    rc |= check_old_layout_translation();')
A('    rc |= check_migration_lifecycle();')
A('    rc |= check_interrupted_save();')
A('    if (entry_count == 0)')
A('    {')
A('        printf("CHECK FAILED: no entries parsed, the generator matched nothing\\n");')
A('        rc = 1;')
A('    }')
A('    for (unsigned i = 0; i < entry_count; i++)')
A('    {')
A('        const entry_t* e = &entries[i];')
A('        if (e->def < e->min || e->def > e->max)')
A('        {')
A('            printf("CHECK FAILED: %s default %d is outside its range %d..%d\\n", e->idname, e->def, e->min, e->max);')
A('            rc = 1;')
A('        }')
A('        if (e->id >= FIRST_UNUSED)')
A('        {')
A('            printf("CHECK FAILED: %s id %u is not below EEPROM_FIRST_UNUSED %u\\n", e->idname, e->id, FIRST_UNUSED);')
A('            rc = 1;')
A('        }')
A('    }')
A('    if (rc == 0) printf("all self checks passed\\n");')
A('    return rc;')
A('}')
out_file = Path(__file__).resolve().parent / 'config_upgrade.c'
open(out_file,'w').write('\n'.join(L)+'\n')
print('wrote %s' % out_file)
