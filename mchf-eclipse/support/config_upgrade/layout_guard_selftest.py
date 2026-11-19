#!/usr/bin/env python3
"""Self-test of the configuration checks in gen_config_upgrade.py.

The guard only earns its place if it still fails on the changes it exists to catch, so this applies
one mutation at a time and checks the verdict:

  1. a stored value moves to another word      -> the check fails, the recorder refuses to record it
  2. a stored value is only renamed           -> the check passes, the recorder takes the new name
                                                 and keeps the name the value had, the marker
                                                 stays, and the compiled translation still finds it
  3. an offset of the previous layout translation moves
                                              -> the compiled translation disagrees with the two
                                                 records, naming the settings it reads wrongly
  4. the marker moves, the map does not        -> the check fails
  5. the parser learns about a stored word     -> the check passes, "make layout-record" writes the
                                                 record, and a second run leaves it untouched
  6. the parser loses a stored word            -> the check fails
  7. the record does not parse                 -> the check fails
  8. a record edited without sealing it       -> the check fails, naming the digest
  9. two rows claim one eeprom id              -> the check fails
  9. two rows store into one variable          -> the check fails
 10. the menu offers more than the row stores  -> the check fails
 12. the whole marker bump, one edit at a time -> the check fails until all three edits are there
 13. a rename committed with a marker bump    -> the check fails, naming the word and the
                                                 sequencing, instead of the translation

An assertion may look at the exit code and at the data a verdict has to name: the settings that
were named, the marker that was recorded, the record that did not parse. It may not look at the
wording: the sentences are there to be rewritten, the verdicts are not, and a needle on a phrase
turns every improvement of a message into a failing matrix. A traceback is never a verdict either:
the generator reports through sys.exit(), so a crash has to fail a case that expects a failure
rather than pass it.

Every case runs in a temporary copy of the files the checks read - the headers, the sources, the
Makefile and support/config_upgrade itself - and the copy is thrown away afterwards. The checkout
this runs in is never written to, so an interrupted run cannot leave it mutated, a dirty tree is no
reason to refuse, and a case cannot lose work that was not committed. Each case is followed by
putting the fixture's files back, so the cases stay independent of each other.
"""
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
from pathlib import Path

import layout_digest      # a record edited on purpose has to say so

SOURCE_ROOT = Path(__file__).resolve().parents[2]
FIXTURE = None
ROOT = SOURCE_ROOT
GEN = HEADER = TABLE = EMITTED = RECORD = None
MARKER = 0
RECORD_NAME = ''
KNOWN_MARKERS = []
SNAPSHOT = {}

FAILS = 1          # sys.exit('message') in the generator, and make reports that as 2

failures = []


def build_fixture():
    """a copy of what the checks read: every header and source, the Makefile, the harness itself

    build/ and basesw/ are left out - the generator skips them and they are far larger than
    everything else. support/config_upgrade is copied whole, because the runs rewrite the emitted
    harness and the records there.
    """
    global FIXTURE
    FIXTURE = Path(tempfile.mkdtemp(prefix='layout-guard-'))
    root = FIXTURE / SOURCE_ROOT.name
    for dirpath, dirs, files in os.walk(SOURCE_ROOT):
        dirs[:] = [d for d in dirs if d not in ('build', 'basesw', '.git')]
        here = Path(dirpath)
        rel = here.relative_to(SOURCE_ROOT)
        (root / rel).mkdir(parents=True, exist_ok=True)
        for name in files:
            if name.endswith(('.h', '.c', '.mak')) or name == 'Makefile':
                shutil.copy2(here / name, root / rel / name)
    shutil.copytree(SOURCE_ROOT / 'support/config_upgrade', root / 'support/config_upgrade',
                    dirs_exist_ok=True)
    return root


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


def use_fixture():
    """point every path at a fresh copy of the tree and remember what to put back"""
    global ROOT, GEN, HEADER, TABLE, EMITTED, RECORD, MARKER, RECORD_NAME, KNOWN_MARKERS
    ROOT = build_fixture()
    GEN = ROOT / 'support/config_upgrade/gen_config_upgrade.py'
    HEADER = ROOT / 'drivers/ui/ui_configuration.h'
    TABLE = ROOT / 'drivers/ui/ui_configuration.c'
    EMITTED = ROOT / 'support/config_upgrade/config_upgrade.c'
    MARKER, RECORD_NAME, KNOWN_MARKERS = layout_record_of_this_build()
    RECORD = ROOT / 'support/config_upgrade' / RECORD_NAME
    records = sorted((ROOT / 'support/config_upgrade').glob('layout-*.map'))
    for path in (GEN, HEADER, TABLE, EMITTED, *records):
        SNAPSHOT[path] = path.read_bytes()


def restore():
    """put the fixture's files back the way the checkout has them"""
    for path, data in SNAPSHOT.items():
        path.write_bytes(data)
    for path in (ROOT / 'support/config_upgrade').glob('layout-*.map'):
        if path not in SNAPSHOT:
            path.unlink()          # a record a case created for a marker this build does not write


def record_owners():
    """the settings a record lists, the data any verdict about the map has to name"""
    return {line.split(None, 1)[1] for line in RECORD.read_text(encoding='utf-8').splitlines()
            if line.strip() and not line.startswith('#')}


def run(*args):
    proc = subprocess.run([sys.executable, str(GEN), *args], cwd=str(ROOT),
                          capture_output=True, text=True)
    return proc.returncode, proc.stdout + proc.stderr


def run_check():
    """the whole check as CI runs it: generator, host compiler, generated program

    One case needs it, because what it perturbs is firmware C that only the compiled harness can
    see. make reports a failing recipe as 2, the program itself returns 1, so the assertion there
    is on any failure rather than on one exit code.
    """
    proc = subprocess.run(['make', '-C', str(ROOT), 'config-upgrade-check'],
                          capture_output=True, text=True)
    return proc.returncode, proc.stdout + proc.stderr


def expect(name, result, want_rc, *needles, any_of=()):
    """one assertion: the exit code (None for any failure), and the data the verdict has to name"""
    rc, out = result
    crashed = 'Traceback (most recent call last)' in out
    missing = [n for n in needles if n not in out]
    unnamed = bool(any_of) and not any(n in out for n in any_of)
    failure_seen = rc != 0 if want_rc is None else rc == want_rc
    if failure_seen and not crashed and not missing and not unnamed:
        print('  ok    %s' % name)
        return
    why = []
    if not failure_seen:
        why.append('exit %d, expected %s'
                   % (rc, 'a failure' if want_rc is None else want_rc))
    if crashed:
        why.append('the generator crashed instead of reporting a verdict')
    if missing:
        why.append('does not name %s' % ', '.join(repr(m) for m in missing))
    if unnamed:
        why.append('names none of the settings the record lists')
    print('  FAIL  %s: %s' % (name, '; '.join(why)))
    for line in out.strip().splitlines()[-6:]:
        print('        | ' + line)
    failures.append(name)


def case_renumbering():
    """two stored values trading words: the change no radio in the field survives"""
    # both have to be words the record lists, or the swap is a stored value landing on a word the
    # record does not have - a disappearance, which the next case covers on its own
    stored = record_owners()
    text = HEADER.read_text(encoding='utf-8')
    rows = [(m.group(1), m.group(2)) for m in
            re.finditer(r'(?m)^#define\s+(EEPROM_\w+)\s+(\d+)\s*$', text)
            if m.group(1) in stored]
    (name_a, id_a), (name_b, id_b) = rows[0], rows[1]
    assert id_a != id_b, 'the first two stored words are the same word, this case needs two'
    text = re.sub(r'(?m)^#define\s+%s\s+%s\s*$' % (name_a, id_a),
                  '#define %s %s' % (name_a, id_b), text)
    text = re.sub(r'(?m)^#define\s+%s\s+%s\s*$' % (name_b, id_b),
                  '#define %s %s' % (name_b, id_a), text)
    HEADER.write_text(text, encoding='utf-8')
    expect('%s and %s trade words' % (name_a, name_b), run(), FAILS, name_a, name_b)
    expect('the recorder refuses to record a renumbering', run('--record-layout'), FAILS, name_a)


def case_rename():
    """a stored value that only got another name: same word, same layout, no marker

    The name is changed where the table declares it - the header and the row - and the record is
    left naming the value the way it was called before, which is the state a rename puts the tree
    in. The recorder has to take the new name and write down the name the value had; the compiled
    harness then has to go on translating the value, because the record kept that name for it.
    Losing the link there is what used to leave a marker bump as the only way through.
    """
    text = HEADER.read_text(encoding='utf-8')
    match = re.search(r'(?m)^#define\s+(EEPROM_FREQ_STEP)\s+(\d+)\s*$', text)
    assert match, 'the define this case renames is gone, update this case'
    owner, word = match.group(1), match.group(2)
    stale = owner + '_RENAMED'
    for path in (HEADER, TABLE):
        source = path.read_text(encoding='utf-8')
        path.write_text(re.sub(r'\b%s\b' % owner, stale, source), encoding='utf-8')

    expect('a stored value renamed where the table declares it', run(), 0,
           '%s %s' % (word, stale))
    expect('the recorder takes the name the table stores', run('--record-layout'), 0, RECORD_NAME)
    written = RECORD.read_text(encoding='utf-8')
    if ('%s %s' % (word, stale)) not in written:
        print('  FAIL  the record does not name the value %s' % stale)
        failures.append('the recorder did not take the new name')
    if ('%s %s (was %s)\n' % (word, stale, owner)) not in written:
        print('  FAIL  the record dropped the name the value had (%s)' % owner)
        failures.append('the recorder did not keep the previous name')
    expect('the recorder runs again without complaining', run('--record-layout'), 0)
    expect('the compiled check still translates the renamed value', run_check(), 0)


def case_rename_with_a_bump():
    """a value renamed in the same commit as a marker bump: the new record has no suffix to follow

    The record of a new marker is written from the table, so a rename that goes in with the bump
    arrives with the new name on the word line and nothing that says the previous layout knew that
    word by another name. The firmware still returns the value's old address, so the check used to
    fail on the translation of a value the previous layout "did not have"; it has to name the word
    and the sequencing that lost the suffix instead.
    """
    text = HEADER.read_text(encoding='utf-8')
    match = re.search(r'(?m)^#define\s+(EEPROM_FREQ_STEP)\s+(\d+)\s*$', text)
    assert match, 'the define this case renames is gone, update this case'
    owner, word = match.group(1), match.group(2)
    stale = owner + '_RENAMED'
    for path in (HEADER, TABLE):
        source = path.read_text(encoding='utf-8')
        path.write_text(re.sub(r'\b%s\b' % owner, stale, source), encoding='utf-8')
    # the record a bump writes: the new name on the same word, and no suffix
    record = RECORD.read_text(encoding='utf-8')
    RECORD.write_text(layout_digest.seal(record.replace('%s %s\n' % (word, owner),
                                                        '%s %s\n' % (word, stale))),
                      encoding='utf-8')
    expect('a rename with no suffix to follow it', run(), FAILS, 'word %s' % word, stale)


def case_old_layout_translation():
    """one offset of UiConfiguration_OldLayoutAddress() changed

    A wrong branch there reads every migrated radio from the wrong words and nothing else in the
    tree notices: the firmware compiles, the current map is untouched. The check compares the
    compiled function with the two recorded maps, so the whole harness has to run for this one.
    """
    source = TABLE.read_text(encoding='utf-8')
    branch = 'return id + 57;'
    assert source.count(branch) == 1, \
        'the branch of UiConfiguration_OldLayoutAddress() this case perturbs is gone, update this case'
    TABLE.write_text(source.replace(branch, 'return id + 56;'), encoding='utf-8')
    expect('an offset of the previous layout translation moves', run_check(), None,
           'CHECK FAILED', any_of=record_owners())


def case_marker_bumped():
    """a marker without a map change pushes healthy radios through the layout migration"""
    bumped = MARKER + 1
    while bumped in KNOWN_MARKERS:          # a marker no record uses, whatever the history holds
        bumped += 1
    text = HEADER.read_text(encoding='utf-8')
    new = re.sub(r'(#define\s+CONFIG_LAYOUT_VERSION_MAGIC\s+)(0x[0-9A-Fa-f]+|\d+)',
                 lambda m: m.group(1) + '0x%X' % bumped, text)
    assert new != text, 'the marker define this case rewrites is gone, update this case'
    HEADER.write_text(new, encoding='utf-8')
    expect('marker bumped, map untouched', run(), FAILS, '0x%X' % MARKER)


def case_parser_addition():
    """a word the table always stored but the parser only just sees: no marker change, no failure"""
    text = RECORD.read_text(encoding='utf-8')
    rows = [line for line in text.splitlines() if line.strip() and not line.startswith('#')]
    word, owner = rows[-1].split(None, 1)
    RECORD.write_text(layout_digest.seal(text.replace(rows[-1] + '\n', '')), encoding='utf-8')

    expect('a stored word the record does not know', run(), 0, '%s %s' % (word, owner))
    expect('the recorder writes it down', run('--record-layout'), 0, RECORD_NAME)
    written = RECORD.read_bytes()
    expect('the recorder runs again without complaining', run('--record-layout'), 0)
    if RECORD.read_bytes() != written:
        print('  FAIL  the recorder rewrote the record although it already matched')
        failures.append('record left untouched')


def case_parser_regression():
    """losing a word is not an improvement: the record says the word is stored and the parse does not"""
    source = GEN.read_text(encoding='utf-8')
    brackets = ("        if ch == '(':\n"
                "            depth += 1\n"
                "        elif ch == ')':\n"
                "            depth -= 1\n"
                "        if ch == ',' and depth == 0:\n")
    assert source.count(brackets) == 1, \
        'the parser fragment this case removes is gone, update this case'
    GEN.write_text(source.replace(brackets, "        if ch == ',':\n"), encoding='utf-8')
    expect('the parser drops a word the record has', run(), FAILS, any_of=record_owners())


def case_broken_record():
    """a record that does not parse compares nothing, which is worse than saying nothing"""
    RECORD.write_text(layout_digest.seal(RECORD.read_text(encoding='utf-8')
                                         + 'this line is not a word owner line\n'), encoding='utf-8')
    expect('a record with a line it cannot read', run(), FAILS, RECORD_NAME)


def case_record_digest():
    """a record edited without sealing it: the history would silently redefine what is checked"""
    text = RECORD.read_text(encoding='utf-8')
    rows = [line for line in text.splitlines() if line.strip() and not line.startswith('#')]
    assert rows, 'the record lists no words, this case needs one'
    word, owner = rows[0].split(None, 1)
    RECORD.write_text(text.replace('%s %s\n' % (word, owner), '%s %s_HAND_EDITED\n' % (word, owner)),
                      encoding='utf-8')
    expect('a record edited by hand', run(), FAILS, RECORD_NAME, 'digest')


def case_duplicate_id():
    """two rows on one word: the second silently overwrites what the first loaded and wrote"""
    source = TABLE.read_text(encoding='utf-8')
    anchor = 'EEPROM_FREQ_STEP,&df.selected_idx'
    assert anchor in source, 'the table row this case rewrites is gone, update this case'
    TABLE.write_text(source.replace(anchor, 'EEPROM_SPEC_SCOPE_SPEED,&df.selected_idx'),
                     encoding='utf-8')
    expect('two rows claiming one eeprom id', run(), FAILS, 'EEPROM_SPEC_SCOPE_SPEED')


def case_shared_variable():
    """two ids loading into one variable: each of them mirrors what the other stored"""
    source = TABLE.read_text(encoding='utf-8')
    anchor = 'EEPROM_NB_SETTING,&ts.dsp.nb_setting'
    assert anchor in source, 'the table row this case rewrites is gone, update this case'
    TABLE.write_text(source.replace(anchor, 'EEPROM_NB_SETTING,&ts.scope_speed'),
                     encoding='utf-8')
    expect('two rows storing into one variable', run(), FAILS,
           'EEPROM_NB_SETTING', 'EEPROM_SPEC_SCOPE_SPEED')


def case_menu_wider_than_stored():
    """the menu offers a range the row does not store, so the value is reset at the next start"""
    source = TABLE.read_text(encoding='utf-8')
    row = ('{ ConfigEntry_UInt8, EEPROM_SPEC_SCOPE_SPEED,&ts.scope_speed,'
           'SPECTRUM_SCOPE_SPEED_DEFAULT,0,SPECTRUM_SCOPE_SPEED_MAX}')
    assert row in source, 'the table row this case rewrites is gone, update this case'
    TABLE.write_text(source.replace(row, row.replace('SPECTRUM_SCOPE_SPEED_DEFAULT', '0')
                                    .replace('SPECTRUM_SCOPE_SPEED_MAX', '1')), encoding='utf-8')
    expect('a stored range narrower than the menu', run(), FAILS, 'EEPROM_SPEC_SCOPE_SPEED')


def case_marker_bump_procedure():
    """the whole bump, edit by edit: the map changes, then the marker, then the history

    The procedure the header comment prescribes, pinned. Moving a value to a free word is a real
    map change, so it fails without a marker; the marker alone fails while the previous one is no
    longer recognised by the migration; and with all three edits plus a fresh record the check
    passes, which is what makes the documented sequence trustworthy.
    """
    text = HEADER.read_text(encoding='utf-8')
    moved = re.sub(r'(?m)^(#define\s+EEPROM_AM_PEP_WARN\s+)382\b', r'\g<1>390', text)
    assert moved != text, 'the define this case moves is gone, update this case'
    HEADER.write_text(moved, encoding='utf-8')
    expect('a map change without a marker bump', run(), FAILS, 'EEPROM_AM_PEP_WARN')

    bumped = MARKER + 1
    while bumped in KNOWN_MARKERS:
        bumped += 1
    HEADER.write_text(re.sub(r'(#define\s+CONFIG_LAYOUT_VERSION_MAGIC\s+)(0x[0-9A-Fa-f]+|\d+)',
                             lambda m: m.group(1) + '0x%X' % bumped, moved), encoding='utf-8')
    expect('the marker bumped, the previous one unrecognised', run(), FAILS, '0x%X' % bumped)

    table = TABLE.read_text(encoding='utf-8')
    guard = 'layout_version != CONFIG_LAYOUT_VERSION_MAGIC'
    assert guard in table, 'the migration check this case extends is gone, update this case'
    TABLE.write_text(table.replace(guard, 'layout_version != 0x%X && %s' % (MARKER, guard)),
                     encoding='utf-8')
    source = GEN.read_text(encoding='utf-8')
    source = source.replace('LAYOUT_HISTORY = [',
                            'LAYOUT_HISTORY = [\n    (0x%X, \'layout-0x%X.map\'),'
                            % (bumped, bumped), 1)
    GEN.write_text(source, encoding='utf-8')

    expect('the new pair recorded', run('--record-layout'), 0, 'layout-0x%X.map' % bumped)
    expect('the whole bump', run(), 0)


CASES = (
    ('1. a value moves to another word', case_renumbering),
    ('2. a value is only renamed', case_rename),
    ('3. an offset of the previous layout translation moves', case_old_layout_translation),
    ('4. the marker moves, the map does not', case_marker_bumped),
    ('5. the parser learns a stored word', case_parser_addition),
    ('6. the parser loses a stored word', case_parser_regression),
    ('7. the record does not parse', case_broken_record),
    ('8. a record edited by hand', case_record_digest),
    ('9. two rows claim one eeprom id', case_duplicate_id),
    ('10. two rows store into one variable', case_shared_variable),
    ('11. the menu offers more than the row stores', case_menu_wider_than_stored),
    ('12. the marker bump, edit by edit', case_marker_bump_procedure),
    ('13. a rename the previous layout cannot follow', case_rename_with_a_bump),
)


def main():
    # Ctrl-C already unwinds through the finally below; these two would otherwise kill the process
    for sig in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, lambda *_: sys.exit(1))
    try:
        use_fixture()
        for title, case in CASES:
            print(title)
            try:
                case()
            except Exception as err:        # a case that breaks is a failure, not a traceback
                print('  FAIL  the case itself raised: %s' % err)
                failures.append(title)
            restore()
        # the fixture has to be looked at before it is thrown away: a case that did not put its
        # files back would make the next case (or a reader here) blame the wrong change
        left = [path.name for path in SNAPSHOT
                if path.exists() and path.read_bytes() != SNAPSHOT[path]]
        if left:
            print('\nlayout guard self-test: the fixture was left mutated: %s' % ', '.join(left))
            failures.append('the fixture was left mutated')
    finally:
        shutil.rmtree(FIXTURE, ignore_errors=True)
    if failures:
        print('\nlayout guard self-test: %d case(s) failed' % len(failures))
        return 1
    print('\nlayout guard self-test: all %d cases behave as expected' % len(CASES))
    return 0


if __name__ == '__main__':
    sys.exit(main())
