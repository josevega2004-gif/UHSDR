#!/usr/bin/env python3
"""Check that fixtures/MANIFEST.md still describes the recordings it keeps.

The manifest records, per fixture, the stored rate, the channel count, the frame count and the byte
size. Those are the numbers a changed decimation, a different trim or a re-encode would move, and
nothing in the tree reads them back - so the manifest could go on describing a file that no longer
exists in that shape, and every later reader would take it at its word. This reads the WAV header,
compares it with the fixture's section of the manifest, and exits non-zero on a mismatch.
"""

import os
import re
import sys
import wave
from pathlib import Path

HERE = Path(__file__).resolve().parent
MANIFEST = HERE / 'fixtures' / 'MANIFEST.md'

# The manifest writes thousands with a space in them ("54 800 frames"), and a copy of it may carry
# a thin or non-breaking space instead of a plain one.
DIGITS = r'([0-9][0-9\s\u00a0\u2009]*)'


def count(text, unit):
    """the number the row gives before `unit`, with the spaces the manifest writes inside it"""
    match = re.search(DIGITS + r'\s*' + unit, text)
    return int(re.sub(r'\D', '', match.group(1))) if match else None


def manifest_sections():
    """the wav file name -> the text of its section, for every recording the manifest describes"""
    sections = {}
    name = None
    body = []
    for line in MANIFEST.read_text(encoding='utf-8').splitlines():
        heading = re.match(r'##\s+`([^`]+\.wav)`', line)
        if heading or line.startswith('## '):
            if name:
                sections[name] = '\n'.join(body)
            name = heading.group(1) if heading else None
            body = []
        else:
            body.append(line)
    if name:
        sections[name] = '\n'.join(body)
    return sections


def main():
    problems = []
    sections = manifest_sections()
    files = sorted((HERE / 'fixtures').glob('*.wav'))
    for path in files:
        row = sections.get(path.name)
        if row is None:
            problems.append('%s: the manifest has no section for it' % path.name)
            continue
        stored = re.search(r'\|\s*Stored as\s*\|(.*?)\|', row)
        if not stored:
            problems.append('%s: its section has no "Stored as" row' % path.name)
            continue
        stored = stored.group(1)
        rate = count(stored, 'kHz')
        rate = rate * 1000 if rate is not None else count(stored, 'Hz')
        channels = 1 if re.search(r'\bmono\b', stored) else \
            2 if re.search(r'\bstereo\b', stored) else None
        said = {'rate': rate, 'channels': channels,
                'frames': count(stored, 'frames'), 'bytes': count(stored, 'bytes')}

        with wave.open(str(path)) as wav:
            heard = {'rate': wav.getframerate(), 'channels': wav.getnchannels(),
                     'frames': wav.getnframes()}
        heard['bytes'] = os.path.getsize(str(path))

        print('fixture  : %s' % path.name)
        print('manifest : %s' % ', '.join('%s %s' % (k, said[k]) for k in said))
        print('file     : %s' % ', '.join('%s %s' % (k, heard[k]) for k in heard))
        for key in said:
            if said[key] is None:
                problems.append('%s: the manifest does not state its %s' % (path.name, key))
            elif said[key] != heard[key]:
                problems.append('%s: %s is %s in the manifest, %s in the file'
                                % (path.name, key, said[key], heard[key]))
    for name in sorted(set(sections) - {p.name for p in files}):
        problems.append('%s: described in the manifest but not in fixtures/' % name)

    if problems:
        for problem in problems:
            print('MISMATCH : %s' % problem)
        return 1
    print('ok: the manifest describes every fixture it keeps')
    return 0


if __name__ == '__main__':
    sys.exit(main())
