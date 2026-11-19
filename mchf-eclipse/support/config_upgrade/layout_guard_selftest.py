#!/usr/bin/env python3
"""Self-test of the layout guard in gen_config_upgrade.py.

The guard only earns its place if it still fails on the changes it exists to catch, so this applies
one mutation at a time to the working tree, runs the generator against it, and checks the verdict:

  1. a stored value moves to another word      -> the check fails, the recorder refuses to record it
  2. the marker moves, the map does not        -> the check fails
  3. the parser learns about a stored word     -> the check passes, "make layout-record" writes the
                                                 record, and a second run leaves it untouched
  4. the parser loses a stored word            -> the check fails
  5. the record does not parse                 -> the check fails

An assertion may look at the exit code and at the data a verdict has to name: the settings that
moved, the marker that was recorded, the record that did not parse. It may not look at the wording:
the sentences are there to be rewritten, the verdicts are not, and a needle on a phrase turns every
improvement of a message into a failing matrix. A traceback is never a verdict either: the generator
reports through sys.exit(), so a crash has to fail a case that expects a failure rather than pass it.

The five cases were run by hand after every change to the guard; this is that matrix, scripted.

Every mutation is undone with "git checkout --" before the next case, and again in a finally that also
runs on Ctrl-C or SIGTERM, so an interrupted run is no way to leave the tree mutated. That works
because the checkout is required to be clean: it refuses to start while any of the files it rewrites
differs from HEAD, and HEAD is then the only thing those files have to match.
"""
import re
import shlex
import signal
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GEN = ROOT / 'support/config_upgrade/gen_config_upgrade.py'
HEADER = ROOT / 'drivers/ui/ui_configuration.h'
EMITTED = ROOT / 'support/config_upgrade/config_upgrade.c'
# every record file, by name, because which one this build uses is only known once the header is
# read, and a run that was killed may have left that header saying anything
RECORD_GLOB = ':(glob)support/config_upgrade/layout-*.map'
REL = [str(path.relative_to(ROOT)) for path in (GEN, HEADER, EMITTED)] + [RECORD_GLOB]

FAILS = 1          # sys.exit('message') in the generator, and make reports that as 2

failures = []


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


def git(*args):
    return subprocess.run(['git', '-C', str(ROOT), *args], capture_output=True, text=True)


def checkout_command():
    """the command that puts the files this test rewrites back, usable from any directory"""
    return 'git -C %s checkout -- %s' % (shlex.quote(str(ROOT)),
                                         ' '.join(shlex.quote(path) for path in REL))


def uncommitted():
    """what git reports for the files this test rewrites, empty when a run cannot lose work"""
    try:
        status = git('status', '--porcelain', '--', *REL)
    except OSError as err:
        sys.exit('cannot ask git whether the files this self-test rewrites are clean (%s). Run it '
                 'from a git checkout, or commit or stash those files first.' % err)
    if status.returncode != 0:
        reason = status.stderr.strip() or status.stdout.strip()
        if 'not a git repository' in reason:
            sys.exit('this is not a git checkout, so the self-test cannot tell whether the files it '
                     'rewrites carry work that is not committed. Run it in the repository checkout.')
        sys.exit('git could not report the state of the files this self-test rewrites: %s\n'
                 'Commit or stash them first.' % reason)
    return [line for line in status.stdout.splitlines() if line.strip()]


def restore():
    """put the files this test rewrites back the way HEAD has them"""
    done = git('checkout', '--', *REL)
    if done.returncode != 0:
        sys.exit('could not put the files this test rewrites back to HEAD:\n%s\nRun\n  %s'
                 % ((done.stderr.strip() or done.stdout.strip()), checkout_command()))


def refuse_if_dirty():
    """stop before reading anything: a killed run leaves one of these files saying whatever its case

    wrote, so the discovery below would trip over the mutation and report that instead of the way
    out. Interrupting this test is only safe because HEAD is what everything is put back to.
    """
    dirty = uncommitted()
    if not dirty:
        return
    names = [line[3:].strip().strip('"') for line in dirty]
    sys.exit('refusing to run: this self-test rewrites these files in place and they already '
             'differ from HEAD:\n%s\n'
             'If a previous run was interrupted, put them back with\n'
             '  %s\n'
             'Otherwise commit or stash that work first, for example\n'
             '  git -C %s stash push -u -- %s\n'
             'and run "make layout-guard-selftest" again.'
             % ('\n'.join('  ' + line for line in dirty), checkout_command(),
                shlex.quote(str(ROOT)), ' '.join(shlex.quote(name) for name in names)))


# before the marker and the record are looked up, not after: a killed run can leave either of them
# saying something this build would not, and this refusal is what makes an interruption recoverable
refuse_if_dirty()
MARKER, RECORD_NAME, KNOWN_MARKERS = layout_record_of_this_build()
RECORD = ROOT / 'support/config_upgrade' / RECORD_NAME


def record_owners():
    """the settings a record lists, the data any verdict about the map has to name"""
    return {line.split(None, 1)[1] for line in RECORD.read_text(encoding='utf-8').splitlines()
            if line.strip() and not line.startswith('#')}


def run(*args):
    proc = subprocess.run([sys.executable, str(GEN), *args], cwd=str(ROOT),
                          capture_output=True, text=True)
    return proc.returncode, proc.stdout + proc.stderr


def expect(name, result, want_rc, *needles, any_of=()):
    """one assertion: the exit code, and the data the verdict has to name"""
    rc, out = result
    crashed = 'Traceback (most recent call last)' in out
    missing = [n for n in needles if n not in out]
    unnamed = bool(any_of) and not any(n in out for n in any_of)
    if rc == want_rc and not crashed and not missing and not unnamed:
        print('  ok    %s' % name)
        return
    why = []
    if rc != want_rc:
        why.append('exit %d, expected %d' % (rc, want_rc))
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
    RECORD.write_text(text.replace(rows[-1] + '\n', ''), encoding='utf-8')

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
    RECORD.write_text(RECORD.read_text(encoding='utf-8') + 'this line is not a word owner line\n',
                      encoding='utf-8')
    expect('a record with a line it cannot read', run(), FAILS, RECORD_NAME)


CASES = (
    ('1. a value moves to another word', case_renumbering),
    ('2. the marker moves, the map does not', case_marker_bumped),
    ('3. the parser learns a stored word', case_parser_addition),
    ('4. the parser loses a stored word', case_parser_regression),
    ('5. the record does not parse', case_broken_record),
)


def main():
    # Ctrl-C already unwinds through the finally below; these two would otherwise kill the process
    for sig in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, lambda *_: sys.exit(1))
    try:
        for title, case in CASES:
            print(title)
            try:
                case()
            except Exception as err:        # a case that breaks is a failure, not a traceback
                print('  FAIL  the case itself raised: %s' % err)
                failures.append(title)
            restore()
    finally:
        restore()
    if failures:
        print('\nlayout guard self-test: %d case(s) failed' % len(failures))
        return 1
    left = uncommitted()
    if left:
        print('\nlayout guard self-test: the tree was left mutated:\n%s'
              % '\n'.join('  ' + line for line in left))
        return 1
    print('\nlayout guard self-test: all %d cases behave as expected' % len(CASES))
    return 0


if __name__ == '__main__':
    sys.exit(main())
