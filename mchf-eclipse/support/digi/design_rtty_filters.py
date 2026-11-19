#!/usr/bin/env python3
"""Regenerate and check the RTTY receive filter bank of drivers/audio/rtty.c.

The seven band pass filters of the modulator/demodulator are stored as coefficients, with no record
of how they were made. That is how rtty_bp_12khz_1765 came to be an 1865 Hz design while its label
said 1765: nothing could ask whether a coefficient set matched the frequency it claimed. This script
asks, by rebuilding every filter from the design its own comment documents:

  order 2 Butterworth band pass, designed as the low pass prototype through the s -> s^2+w0^2 / (Bs)
  transform, bilinear transformed at 12 kHz with the band edges prewarped as w = tan(pi f / fs)

The transfer function is

  H(z) = (1 - q)^2 (1 + q)^2 / P(q),   q = z^-1
  P(q) = (1-q)^4 + a3 (1-q)^3 (1+q) + a2 (1-q)^2 (1+q)^2 + a1 (1-q) (1+q)^3 + a0 (1+q)^4
  a3 = sqrt(2) B,  a2 = 2 w0^2 + B^2,  a1 = sqrt(2) B w0^2,  a0 = w0^4,  w0^2 = w_lo w_hi

P is normalised to a constant term of 1, and the firmware stores its four z^-1..z^-4 coefficients
oldest delay first, because its recursion reads yv[0] as y[n-4]. The gain is |H| at the arithmetic
centre of the band, which is what makes the filter reach unity gain at the tone it is labelled with.

  python3 support/digi/design_rtty_filters.py          check the constants in rtty.c
  python3 support/digi/design_rtty_filters.py --print  print the blocks to paste into rtty.c

Exit status is non zero when a block is not the one the design produces, character for character, or
when a label is not the centre of the band its comment documents. The comparison is exact on purpose:
a tolerance would let a table drift away from its design in steps that each look harmless, and the
blocks in rtty.c are kept as the script prints them so that the file stays its own provenance.
"""

import cmath
import math
import os
import re
import sys

FS = 12000.0                     # the audio domain these filters live in, named in rtty.c
RTTC = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..',
                    'drivers', 'audio', 'rtty.c')

BLOCK = re.compile(r'static\s+rtty_bpf_config_t\s+(rtty_bp_12khz_(\d+))\s*=\s*\{(.*?)\};', re.S)
EDGES = re.compile(r'freqs:\s*(\d+)\s*-\s*(\d+)\s*Hz')
GAIN = re.compile(r'\.gain\s*=\s*([-+0-9.eE]+)')
COEFFS = re.compile(r'\.coeffs\s*=\s*\{([^}]*)\}')
FREQ = re.compile(r'\.freq\s*=\s*(\d+)')


def _mul(p, q):
    out = [0.0] * (len(p) + len(q) - 1)
    for i, a in enumerate(p):
        for j, b in enumerate(q):
            out[i + j] += a * b
    return out


def design(edge_lo, edge_hi):
    """coefficients and gain of one filter, from its two -3 dB edges in Hz"""
    w_lo = math.tan(math.pi * edge_lo / FS)      # prewarp, so the edges land where they are asked to
    w_hi = math.tan(math.pi * edge_hi / FS)
    w0_sq = w_lo * w_hi
    bandwidth = w_hi - w_lo

    a3 = math.sqrt(2.0) * bandwidth
    a2 = 2.0 * w0_sq + bandwidth * bandwidth
    a1 = math.sqrt(2.0) * bandwidth * w0_sq
    a0 = w0_sq * w0_sq

    # the bilinear transform s -> (1-q)/(1+q) of s^4 + a3 s^3 + a2 s^2 + a1 s + a0, times (1+q)^4
    poly = [0.0] * 5
    for coef, minus_powers, plus_powers in ((1.0, 4, 0), (a3, 3, 1), (a2, 2, 2), (a1, 1, 3), (a0, 0, 4)):
        term = [coef]
        for _ in range(minus_powers):
            term = _mul(term, [1.0, -1.0])
        for _ in range(plus_powers):
            term = _mul(term, [1.0, 1.0])
        for i, v in enumerate(term):
            poly[i] += v
    lead = poly[0]
    poly = [v / lead for v in poly]

    # the recursion of RttyDecoder_bandPassFreq(): yv[0] is y[n-4], so the taps run oldest first
    coeffs = [-poly[4], -poly[3], -poly[2], -poly[1]]

    # unity gain at the centre of the band, which is where the modem looks for the tone
    centre = 0.5 * (edge_lo + edge_hi)
    q = cmath.exp(-2j * math.pi * centre / FS)
    gain = abs(((1 - q) ** 2 * (1 + q) ** 2) / sum(c * q ** i for i, c in enumerate(poly)))
    return coeffs, gain


def parse(source):
    """[(name, label, gain, coeffs, edge_lo, edge_hi)] of every 12 kHz band pass filter"""
    found = []
    for m in BLOCK.finditer(source):
        body = m.group(3)
        edges = EDGES.findall(source[max(0, m.start() - 900):m.start()])
        if not edges:
            sys.exit('%s has no "freqs: <lo>-<hi> Hz" comment above it, the design of the filter '
                     'cannot be recovered. Put the two -3 dB edges back into the comment.' % m.group(1))
        edge_lo, edge_hi = int(edges[-1][0]), int(edges[-1][1])
        coeffs = [float(v) for v in COEFFS.search(body).group(1).split(',')]
        found.append((m.group(1), int(FREQ.search(body).group(1)),
                      float(GAIN.search(body).group(1)), coeffs, edge_lo, edge_hi, m.group(0)))
    if not found:
        sys.exit('no receive filter found in %s, has the filter bank moved?' % RTTC)
    return found


def block(name, label, gain, coeffs, edge_lo, edge_hi):
    return ('static rtty_bpf_config_t %s =\n{\n'
            '\t\t.gain = %.9e,\n'
            '\t\t.coeffs = { %.10f, %.10f, %.10f, %.10f },\n'
            '\t\t.freq = %d\n};\n'
            % (name, gain, coeffs[0], coeffs[1], coeffs[2], coeffs[3], label))


def main(argv):
    emit = '--print' in argv[1:]
    source = open(RTTC, encoding='utf-8', errors='surrogateescape').read()
    found = parse(source)
    failures = []

    for name, label, gain, coeffs, edge_lo, edge_hi, stored in found:
        want_coeffs, want_gain = design(edge_lo, edge_hi)
        centre = 0.5 * (edge_lo + edge_hi)

        notes = []
        if len(coeffs) != 4:
            # block() and the comparison both need four taps, so say this instead of indexing
            notes.append('%d coefficients, a fourth order denominator has 4' % len(coeffs))
        elif stored != block(name, centre, want_gain, want_coeffs, edge_lo,
                             edge_hi).rstrip('\n'):
            notes.append('not the block the design produces (largest coefficient difference %.1e, '
                         'gain %.9e against %.9e)'
                         % (max(abs(a - b) for a, b in zip(coeffs, want_coeffs)), gain, want_gain))
        if label != centre:
            notes.append('label %d Hz but the documented band %d..%d Hz has its centre at %d'
                         % (label, edge_lo, edge_hi, centre))

        if notes:
            failures.append((name, notes))

        if emit:
            # paste output only, the status lines would end up in the comment of rtty.c
            print('// order 2 Butterworth, freqs: %d-%d Hz, centre: %d Hz' % (edge_lo, edge_hi, centre))
            print(block(name, centre, want_gain, want_coeffs, edge_lo, edge_hi))
        elif notes:
            print('FAIL  %-22s %d-%d Hz  %s' % (name, edge_lo, edge_hi, '; '.join(notes)))
        else:
            print('ok    %-22s %d-%d Hz  centre %4d  identical to the block the design produces'
                  % (name, edge_lo, edge_hi, centre))

    if emit:
        if failures:
            print('\n%d filter(s) in rtty.c do not match their design, the blocks above are the ones '
                  'to paste:\n    %s'
                  % (len(failures), '\n    '.join('%s: %s' % (n, '; '.join(x)) for n, x in failures)),
                  file=sys.stderr)
        return 1 if failures else 0

    if failures:
        print('\nThe receive filter bank no longer matches the design its comments document. Every')
        print('filter is an order 2 Butterworth band pass, bilinear transformed at 12 kHz with its')
        print('edges prewarped; a filter that drifts off its label filters the wrong tone, which a')
        print('loopback test cannot see because it decides from the mark channel. Regenerate the')
        print('blocks with\n\n    python3 support/digi/design_rtty_filters.py --print\n')
        print('and replace the constants in drivers/audio/rtty.c.')
        return 1

    print('\nfilter bank: %d filters are exactly the blocks the design produces (order 2 Butterworth, '
          'prewarped bilinear, %d kHz)' % (len(found), int(FS / 1000)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
