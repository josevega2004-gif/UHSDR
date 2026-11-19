#!/usr/bin/env python3
"""Self-test of the layout guard in gen_config_upgrade.py.

The guard only earns its place if it still fails on the changes it exists to catch, so this applies
one mutation at a time to the working tree, runs the generator against it, and checks the verdict:

  1. a stored value moves to another word      -> the check fails, the recorder refuses to record it
  2. the marker moves, the map does not        -> the check fails
  3. the parser learns about a stored word     -> the check passes and says what to run, and
                                                 "make layout-record" restores the recorded words exactly
  4. the parser loses a stored word            -> the check fails
  5. the record does not parse                 -> the check fails

The five cases were run by hand after every change to the guard; this is that matrix, scripted. Every
mutation is undone again, also when a case fails, so the tree is left as it was found.
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GEN = ROOT / 'support/config_upgrade/gen_config_upgrade.py'
HEADER = ROOT / 'drivers/ui/ui_configuration.h'
EMITTED = ROOT / 'support/config_upgrade/config_upgrade.c'


def layout_record_of_this_build():
    """the marker the firmware writes and the record LAYOUT_HISTORY keeps for it

    Both are read, never spelled out here: the next CONFIG_LAYOUT_VERSION_MAGIC bump renames the
    record file, and a self-test that still looked for the old name would fail on a healthy tree.
    """
    magic = re.search(r'#define\s+CONFIG_LAYOUT_VERSION_MAGIC\s+(0x[0-9A-Fa-f]+|\d+)',
                      HEADER.read_text(encoding='utf-8'))
    if not magic:
        sys.exit('CONFIG_LAYOUT_VERSION_MAGIC is gone from %s, this self-test needs it'
                 % HEADER.name)
    marker = int(magic.group(1), 0)
    source = GEN.read_text(encoding='utf-8')
    start = source.index('LAYOUT_HISTORY = [')
    stop = source.index('\n]\n', start)
    entries = {int(m.group(1), 16): m.group(2) for m in
               re.finditer(r"\(\s*0x([0-9A-Fa-f]+)\s*,\s*'([^']+)'\s*\)", source[start:stop])}
    if marker not in entries:
        sys.exit('LAYOUT_HISTORY records no map for marker 0x%X, the marker this firmware writes. '
                 'Add (0x%X, layout-0x%X.map) and run "make layout-record" first.'
                 % (marker, marker, marker))
    return marker, entries[marker], sorted(entries)


MARKER, RECORD_NAME, KNOWN_MARKERS = layout_record_of_this_build()
RECORD = ROOT / 'support/config_upgrade' / RECORD_NAME
TOUCHED = (GEN, HEADER, RECORD, EMITTED)

FAILS = 1          # sys.exit('message') in the generator, and make reports that as 2

failures = []


def record_words(path):
    """the "word owner" lines of a record, without its generated comment header"""
    return [line for line in path.read_text(encoding='utf-8').splitlines()
            if line.strip() and not line.startswith('#')]


def run(*args):
    proc = subprocess.run([sys.executable, str(GEN), *args], cwd=str(ROOT),
                          capture_output=True, text=True)
    return proc.returncode, proc.stdout + proc.stderr


def expect(name, result, want_rc, *needles):
    rc, out = result
    missing = [n for n in needles if n not in out]
    if rc == want_rc and not missing:
        print('  ok    %s' % name)
        return
    print('  FAIL  %s: exit %d, expected %d%s'
          % (name, rc, want_rc, ', missing %s' % ', '.join(repr(m) for m in missing) if missing else ''))
    for line in out.strip().splitlines()[-6:]:
        print('        | ' + line)
    failures.append(name)


def case_renumbering():
    """two stored values trading words: the change no radio in the field survives"""
    text = HEADER.read_text(encoding='utf-8')
    rows = [m for m in re.finditer(r'(?m)^#define\s+(EEPROM_\w+)\s+(\d+)\s*$', text)
            if int(m.group(2)) < 416]
    (name_a, id_a), (name_b, id_b) = rows[0].groups(), rows[1].groups()
    text = re.sub(r'(?m)^#define\s+%s\s+%s\s*$' % (name_a, id_a),
                  '#define %s %s' % (name_a, id_b), text)
    text = re.sub(r'(?m)^#define\s+%s\s+%s\s*$' % (name_b, id_b),
                  '#define %s %s' % (name_b, id_a), text)
    HEADER.write_text(text, encoding='utf-8')
    expect('%s and %s trade words' % (name_a, name_b), run(), FAILS,
           'the map of the words below', 'move the values back')
    expect('the recorder refuses to record a renumbering', run('--record-layout'), FAILS,
           'refusing to record the layout map')


def case_marker_bumped():
    """a marker without a map change pushes healthy radios through the layout migration"""
    bumped = MARKER + 1
    while bumped in KNOWN_MARKERS:          # a marker no record uses, whatever the history holds
        bumped += 1
    text = HEADER.read_text(encoding='utf-8')
    new = re.sub(r'(#define\s+CONFIG_LAYOUT_VERSION_MAGIC\s+)(0x[0-9A-Fa-f]+|\d+)',
                 lambda m: m.group(1) + '0x%X' % bumped, text)
    assert new != text
    HEADER.write_text(new, encoding='utf-8')
    expect('marker bumped, map untouched', run(), FAILS,
           'exactly the one recorded for marker 0x%X' % MARKER)


def case_parser_addition():
    """a word the table always stored but the parser only just sees: no marker change, no failure"""
    before_words = record_words(RECORD)
    text = RECORD.read_text(encoding='utf-8')
    rows = [line for line in text.splitlines() if line.strip() and not line.startswith('#')]
    lost = rows[-1]
    word, owner = lost.split(None, 1)
    RECORD.write_text(text.replace(lost + '\n', ''), encoding='utf-8')

    expect('a stored word the record does not know', run(), 0,
           'make layout-record', '%s %s' % (word, owner))
    expect('the recorder writes it down', run('--record-layout'), 0, 'now records')
    restored = record_words(RECORD) == before_words
    print('  %s  the recorder restores the recorded words exactly' % ('ok  ' if restored else 'FAIL'))
    if not restored:
        failures.append('record restored')
    expect('the recorder is a no-op afterwards', run('--record-layout'), 0, 'nothing to do')


def case_parser_regression():
    """losing a word is not an improvement: the record says the word is stored and the parse does not"""
    source = GEN.read_text(encoding='utf-8')
    brackets = ("        if ch == '(':\n"
                "            depth += 1\n"
                "        elif ch == ')':\n"
                "            depth -= 1\n"
                "        if ch == ',' and depth == 0:\n")
    assert source.count(brackets) == 1
    GEN.write_text(source.replace(brackets, "        if ch == ',':\n"), encoding='utf-8')
    expect('the parser drops a word the record has', run(), FAILS, 'nothing is stored there any more')


def case_broken_record():
    """a record that does not parse compares nothing, which is worse than saying nothing"""
    RECORD.write_text(RECORD.read_text(encoding='utf-8') + 'this line is not a word owner line\n',
                      encoding='utf-8')
    expect('a record with a line it cannot read', run(), FAILS, 'is not a "word owner" line')


CASES = (
    ('1. a value moves to another word', case_renumbering),
    ('2. the marker moves, the map does not', case_marker_bumped),
    ('3. the parser learns a stored word', case_parser_addition),
    ('4. the parser loses a stored word', case_parser_regression),
    ('5. the record does not parse', case_broken_record),
)


def main():
    snapshot = {path: path.read_bytes() for path in TOUCHED}
    try:
        for title, case in CASES:
            print(title)
            case()
            for path in TOUCHED:
                path.write_bytes(snapshot[path])
    finally:
        for path in TOUCHED:
            path.write_bytes(snapshot[path])
    left = [str(path.relative_to(ROOT)) for path in TOUCHED
            if path.read_bytes() != snapshot[path]]
    if left:
        print('  FAIL  these files were not left as they were found: %s' % ', '.join(left))
        failures.append('tree restored')
    if failures:
        print('\nlayout guard self-test: %d case(s) failed' % len(failures))
        return 1
    print('\nlayout guard self-test: all %d cases behave as expected' % len(CASES))
    return 0


if __name__ == '__main__':
    sys.exit(main())
