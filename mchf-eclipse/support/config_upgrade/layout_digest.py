#!/usr/bin/env python3
"""The self digest of a layout record.

A record is the history the address translation is checked against: an edit to one changes what
"correct" means for every radio that carries that layout, and the checks would then agree with the
edit instead of catching it. Each record therefore ends in

    #digest <sha256 of every other line of the file>

The recorder writes it, the reader verifies it before the file is used, and a change to a record
still has one way through: write the new digest too, which puts the change in the diff of the file
instead of hiding it.

    python3 support/config_upgrade/layout_digest.py layout-before-0x5AA5.map   reseal one record
"""
import hashlib
import sys

PREFIX = '#digest '


def digest(text):
    """the digest of a record, over every line except the digest line itself"""
    body = '\n'.join(line for line in text.splitlines() if not line.startswith(PREFIX))
    return hashlib.sha256(body.encode('utf-8')).hexdigest()


def seal(text):
    """the record with its digest written last, replacing the one it may already carry"""
    body = [line for line in text.splitlines() if not line.startswith(PREFIX)]
    return '\n'.join(body).rstrip('\n') + '\n%s%s\n' % (PREFIX, digest(text))


def problem(text, name):
    """None when the record carries its own digest, a sentence naming what is wrong otherwise"""
    recorded = [line[len(PREFIX):].strip() for line in text.splitlines()
                if line.startswith(PREFIX)]
    if not recorded:
        return ('%s carries no "#digest" line, so an edit to it cannot be told from the history it '
                'records. Seal it with "python3 support/config_upgrade/layout_digest.py %s".'
                % (name, name))
    if len(recorded) > 1:
        return ('%s carries %d "#digest" lines, only one of them can be the digest of the file'
                % (name, len(recorded)))
    want = digest(text)
    if recorded[0] != want:
        return ('%s does not match its own digest: the file records %s, its lines hash to %s. The '
                'record is the history the address translation is checked against, so an edit has '
                'to be announced by writing the new digest. If the change is intended, seal it with '
                '"python3 support/config_upgrade/layout_digest.py %s" and commit the result.'
                % (name, recorded[0][:16], want[:16], name))
    return None


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit(__doc__.strip())
    with open(sys.argv[1], encoding='utf-8') as handle:
        text = handle.read()
    fault = problem(text, sys.argv[1])
    with open(sys.argv[1], 'w', encoding='utf-8') as handle:
        handle.write(seal(text))
    print('%s: %s, sealed as %s'
          % (sys.argv[1], fault or 'the digest already matched', digest(text)[:16]))
