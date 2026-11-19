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
import hashlib

import os
from pathlib import Path

# the firmware sources are two directories above this script (support/config_upgrade)
root = str(Path(__file__).resolve().parents[2]) + '/'

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
    for m in re.finditer(r'^\s*#define\s+(\w+)\s+(\(?[^{}\n]*\)?)\s*(?://.*)?$', text, re.M):
        name, body = m.group(1), m.group(2).strip()
        if not body or ',' in body or not re.fullmatch(r'[\w\s()\+\-*/%<>|&^~]+', body):
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

def resolve(expr, _seen=None):
    """value of a constant as the compiler would see it, None when it cannot be worked out here"""
    expr = expr.strip()
    expr = re.sub(r'\bCONFIG_UINT8x2_COMBINE\s*\(\s*([^,()]+)\s*,\s*([^,()]+)\s*\)', r'(\1 << 8 | \2)', expr)
    if re.fullmatch(r'-?\d+', expr):
        return int(expr)
    _seen = _seen or set()
    if expr in _seen:
        return None
    _seen = _seen | {expr}
    if re.fullmatch(r'\w+', expr) and expr not in vals:
        if expr not in const_exprs:
            return None
        v = resolve(const_exprs[expr], _seen)
        if v is not None:
            vals[expr] = v
        return v
    m = re.fullmatch(r'\(?(\w+)\)?\s*([*+-/|&^]|<<|>>)\s*\(?(\w+)\)?', expr)   # NAME op NAME or NAME op 10
    if m:
        a = resolve(m.group(1), _seen)
        btok = m.group(3)
        b = int(btok, 0) if re.fullmatch(r'-?\d+', btok) else resolve(btok, _seen)
        if a is None or b is None:
            return None
        try:
            return {'*': a * b, '+': a + b, '-': a - b, '/': a // b, '|': a | b, '&': a & b,
                    '^': a ^ b, '<<': a << b, '>>': a >> b}[m.group(2)]
        except ZeroDivisionError:
            return None
    try:
        return int(eval(expr, {'__builtins__': {}}, dict(vals)))
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


def table_rows(text):
    """rows of the ConfigEntry table together with the #if branch they are compiled from"""
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
        m = re.match(r'\{\s*(ConfigEntry_\w+)(?:\s*\|\s*\w+)?\s*,\s*(EEPROM_\w+)\s*,\s*&([^,]+)\s*,\s*([^,]+)\s*,\s*([^,]+)\s*,\s*([^,]+?)\s*\}', s)
        if m:
            rows.append((tuple(stack), ) + m.groups())
    return rows

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

# Every row that a #if branch removes from this list also removes itself from every comparison
# below, so a surprising drop here has to be noticed instead of quietly weakening the check.
# Rows are left out either by a #if branch or because their values come from a macro the parser
# does not expand; both are expected to stay a small part of the table, so a large fall here means
# the check has quietly stopped looking at the configuration.
MAX_NOT_COMPARED = 20
if len(all_rows) - len(table) > MAX_NOT_COMPARED:
    sys.exit('%d of the %d parsed table rows are not compared (%d behind a branch: %s). A row that '
             'is not compared is a setting that is not checked.'
             % (len(all_rows) - len(table), len(all_rows), skipped_branch,
                ', '.join(sorted(dropped_guards)) or 'none'))

unresolved = 0
for cond, ctype, idname, var, d, lo, hi in table:
    if idname not in vals:
        sys.exit('%s (&%s) uses an id that is not defined in ui_configuration.h' % (idname, var))
    if vals[idname] >= first_unused:
        sys.exit('%s (&%s) has id %d but EEPROM_FIRST_UNUSED is only %d'
                 % (idname, var, vals[idname], first_unused))
    dv, mn, mx = resolve(d), resolve(lo), resolve(hi)
    if None in (dv, mn, mx):
        unresolved += 1
        continue
    if not (mn <= dv <= mx):
        sys.exit('%s (&%s) has default %d outside its stored range %d..%d, a value in the middle of '
                 'the range the menu offers is replaced by the default at the next start'
                 % (idname, var, dv, mn, mx))

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
      'configuration does not use), %d rows have constants that cannot be resolved without a '
      'compiler, %d menu items were compared with their stored range'
      % (len(table), len(all_rows), skipped_branch, unresolved, compared))

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
# the firmware asserts the same thing at compile time, make sure nobody deletes it
if not re.search(r'UiConfiguration_FilterMapWordsFit', clean_cfg_c):
    sys.exit('the compile time check UiConfiguration_FilterMapWordsFit is gone from '
             'ui_configuration.c, EEPROM_FILTER_PATH_MAP_WORDS could drift from the loop bounds')

# UiReadSettingEEPROM_Filter() has to decide the previous-layout case BEFORE it probes the current
# block. In the layout before the renumbering those words held other ids (today's 334..358), so a
# configuration of that layout has data there, the probe reports the block as already stored and the
# radio reads its 25 filter memories from another setting's values. check_filter_map_migration()
# replays the cases on the host; this guard keeps the shape it models.
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
    (0x5AA5, 'a2dd5fa69241'),          # as of commit 92418c08
]

current_magic = vals['CONFIG_LAYOUT_VERSION_MAGIC']
layout_map = sorted((w, '|'.join(sorted({o[1] for o in lst})))
                    for w, lst in owners.items() if w < BASELINE_LAYOUT_FIRST_UNUSED)
layout_digest = hashlib.sha1(repr(layout_map).encode()).hexdigest()[:12]

markers = [m for m, _ in LAYOUT_HISTORY]
if len(set(markers)) != len(markers):
    sys.exit('two entries of LAYOUT_HISTORY share a marker (%s): one marker cannot stand for two '
             'map shapes. A radio holding the older shape would be read through the newer addresses.'
             % ', '.join('0x%x' % m for m in sorted({m for m in markers if markers.count(m) > 1})))

recorded = dict(LAYOUT_HISTORY)
if layout_digest not in recorded.values():
    sys.exit('the address map of words below %d changed shape (recorded %s, now %s) while the '
             'recorded marker for that shape is 0x%x.\n'
             'A radio holding a configuration written under the old map cannot tell, so it will be '
             'read through the wrong addresses. Three things must happen together:\n'
             '  1. raise CONFIG_LAYOUT_VERSION_MAGIC to a value no entry of LAYOUT_HISTORY uses;\n'
             '  2. extend UiConfiguration_LayoutMigrationRequired() to still recognise the marker '
             'the previous build wrote, otherwise every healthy radio is migrated through the '
             'pre-renumbering translation, which corrupts it;\n'
             '  3. append the pair to LAYOUT_HISTORY: (0x%x, %s), and raise '
             'BASELINE_LAYOUT_FIRST_UNUSED if the range grew.\n'
             'If the intent is to keep the old map readable instead, move the values back.'
             % (BASELINE_LAYOUT_FIRST_UNUSED,
                ', '.join(sorted(set(recorded.values()))) or '(nothing)', layout_digest,
                current_magic, current_magic, layout_digest))

if recorded.get(current_magic) != layout_digest:
    sys.exit('the address map below %d has the shape recorded for marker %s, but '
             'CONFIG_LAYOUT_VERSION_MAGIC is 0x%x. The marker decides which map a radio is read '
             'through, so either the marker was bumped without the map being recorded for it, or the '
             'map was recorded under a marker no build wrote. Record the pair (0x%x, %s) in '
             'LAYOUT_HISTORY, or put the marker back to %s.'
             % (BASELINE_LAYOUT_FIRST_UNUSED,
                ', '.join('0x%X' % m for m, d in LAYOUT_HISTORY if d == layout_digest) or '(none)',
                current_magic, current_magic, layout_digest,
                ', '.join('0x%X' % m for m, d in LAYOUT_HISTORY if d == layout_digest) or '(none)'))

# every marker that is no longer current has to stay recognised by the migration, otherwise the
# radios still carrying it are treated as if they had the current map
older = [(m, d) for m, d in LAYOUT_HISTORY if m != current_magic]
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
    for marker, shape in older:
        spelled = any(t in migrate_body for t in ('0x%X' % marker, '0x%x' % marker, '%d' % marker))
        via_define = any(value == marker and re.search(r'\b%s\b' % name, migrate_body)
                         for name, value in defined.items())
        if not (spelled or via_define):
            sys.exit('LAYOUT_HISTORY records map shape %s under marker 0x%X, but '
                     'UiConfiguration_LayoutMigrationRequired() no longer mentions that marker. A '
                     'radio holding a configuration written with it is read through the current '
                     'address map, which is the silent renumbering this check exists to prevent. '
                     'Recognise the old marker there, or remove the pair only if no radio can be '
                     'holding it.' % (shape, marker))

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
A('    const char* const shape[2] = { "previous layout", "layout before this one, block unwritten" };')
A('    image_t img[2];')
A('    int rc = 0;')
A('')
A('    /* the two configurations a migrating radio can be holding */')
A('    for (unsigned c = 0; c < 2; c++)')
A('    {')
A('        memset(&img[c], 0, sizeof(img[c]));')
A('        for (unsigned i = 0; i < sizeof(img[c].value)/sizeof(img[c].value[0]); i++)')
A('        {')
A('            img[c].value[i] = 0xFFFF;')
A('        }')
A('        for (unsigned k = 0; k < FILTER_MAP_WORDS; k++)')
A('        {')
A('            img[c].present[FILTER_MAP_LEGACY + k] = true;')
A('            img[c].value[FILTER_MAP_LEGACY + k] = (uint16_t)(1 + k);   /* the operator memories */')
A('        }')
A('        if (c == 0)')
A('        {')
A('            /* the previous layout kept the ids that now live at 334..358 at the words of the block */')
A('            for (unsigned k = 0; k < FILTER_MAP_WORDS; k++)')
A('            {')
A('                img[c].present[FILTER_MAP_BASE + k] = true;')
A('                img[c].value[FILTER_MAP_BASE + k] = (uint16_t)(40 + k);')
A('            }')
A('        }')
A('    }')
A('')
A('    for (unsigned c = 0; c < 2; c++)')
A('    {')
A('        uint8_t mem[FILTER_MAP_WORDS];')
A('        image_t after;')
A('')
A('        /* the boot that migrates: the memories come from the legacy base */')
A('        load_filter_memories(&img[c], true, mem);')
A('        for (unsigned k = 0; k < FILTER_MAP_WORDS; k++)')
A('        {')
A('            if (mem[k] != (uint8_t)(1 + k))')
A('            {')
A('                printf("CHECK FAILED: the migrated boot of a %s configuration reads memory %u as %u, "')
A('                       "expected %u\\n", shape[c], k, mem[k], 1 + k);')
A('                rc = 1;')
A('                break;')
A('            }')
A('        }')
A('')
A('        /* the radio saves; the next boot is a plain boot and has to find the block */')
A('        after = img[c];')
A('        save_filter_memories(&after, mem);')
A('        const uint16_t base = filter_map_base(&after, false, false);')
A('        if (base != FILTER_MAP_BASE)')
A('        {')
A('            printf("CHECK FAILED: after saving a migrated configuration (%s) the next boot still "')
A('                   "reads the filter path memories from %u instead of %u, so the memories it shows "')
A('                   "are not the ones it stored\\n", shape[c], base, FILTER_MAP_BASE);')
A('            rc = 1;')
A('            continue;')
A('        }')
A('        load_filter_memories(&after, false, mem);')
A('        for (unsigned k = 0; k < FILTER_MAP_WORDS; k++)')
A('        {')
A('            if (mem[k] != (uint8_t)(1 + k))')
A('            {')
A('                printf("CHECK FAILED: after saving a migrated configuration (%s) filter memory %u "')
A('                       "reads %u, expected %u\\n", shape[c], k, mem[k], 1 + k);')
A('                rc = 1;')
A('                break;')
A('            }')
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
RTYPES = {
    'ConfigEntry_UInt8': ('R_UINT8', 1),
    'ConfigEntry_UInt16': ('R_UINT16', 1),
    'ConfigEntry_UInt32_16': ('R_UINT32_16', 1),
    'ConfigEntry_Int16': ('R_INT16', 1),
    'ConfigEntry_Int32_16': ('R_INT32_16', 1),
    'ConfigEntry_Int32': ('R_INT32', 2),
}
RTYPE_LIMITS = {                     # what the stored word can hold after the firmware's cast
    'R_UINT8': (0, 0xFF),
    'R_UINT16': (0, 0xFFFF),
    'R_UINT32_16': (0, 0xFFFF),
    'R_INT16': (-0x8000, 0x7FFF),
    'R_INT32_16': (-0x8000, 0x7FFF),
    'R_INT32': (-0x80000000, 0x7FFFFFFF),
}
rrow_data = []
for cond, ctype, idname, var, d, lo, hi in map_rows:
    if ctype not in RTYPES:
        sys.exit('%s (&%s) is stored as %s, which the round trip check does not model. Add the type '
                 'to RTYPES in gen_config_upgrade.py.' % (idname, var, ctype))
    dv, mn, mx = resolve(d), resolve(lo), resolve(hi)
    if None in (dv, mn, mx):
        continue                     # a range built from macros the parser cannot expand
    rtype, words = RTYPES[ctype]
    low, high = RTYPE_LIMITS[rtype]
    # a stored value the firmware has to reject. A row that spans the whole width of its type has
    # none: for an UInt8 row from 0 to 255 every stored word is a legal value after truncation.
    outside = mx + 1 if mx < high else (mn - 1 if mn > low else None)
    rrow_data.append((idname, vals[idname], rtype, words, dv, mn, mx, outside))
if len(rrow_data) < 100:
    sys.exit('only %d rows of the configuration table have a resolved stored range, the round trip '
             'check would cover almost nothing' % len(rrow_data))

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
A('typedef enum { R_UINT8, R_UINT16, R_UINT32_16, R_INT16, R_INT32_16, R_INT32 } rtype_t;')
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
A('#define RROW_SKIPPED %du   /* rows whose stored range uses macros this harness cannot expand */'
  % (len(map_rows) - len(rrow_data)))
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
    img->present[r->id] = true;
    img->value[r->id] = (uint16_t)value;
    if (r->type == R_INT32)
    {
        img->present[r->id + 1] = true;
        img->value[r->id + 1] = (uint16_t)(((uint32_t)value) & 0xFFFFu);
    }
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
               "written, values outside the range fall back to the default (%u rows are not covered "
               "because their range uses macros this harness cannot expand)\n\n",
               probes, rrow_count, RROW_SKIPPED);
    }
    return rc;
}
""")
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
