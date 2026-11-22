#!/usr/bin/env python3
"""Capture audio from a WebSDR without a browser.

This half is the proven half: it connects to the same WebSocket the WebSDR's own HTML5 client
uses (``/~~stream``), asks for a frequency, a band and a passband, and saves what arrives to
``part-NNN.stream`` files by index.  When the connection drops - and it does, that is what a shared
receiver is like - the collector reconnects, retunes, and carries on at the next index, so a take
that spans a drop is still a take, and a fragment left by a failed attempt is kept rather than
overwritten.

    ./websdr_capture.py --freq-khz 4581.41 --part-seconds 30 --parts 2 --out /tmp/ddk2

The band is not optional on a multi-band receiver.  Its page's client sends ``f``, ``band``, ``lo``,
``hi`` and ``mode`` in one request, and the band is the index of the front-end band that covers the
dial, read off the receiver's own band table (``/tmp/bandinfo.js``).  Without it the server applies
the passband and the mode but leaves the front end on the band the connection started in, so every
take is that band's audio whatever the dial says - which is how a 24 MHz control and a 10.1 MHz take
came back as the same hum, on a receiver whose eight bands have gaps between 15.7 and 20.9 MHz and
between 21.9 and 27 MHz.  A dial that no band of the receiver covers is refused here rather than
captured: the take would not be the frequency that was asked for.  ``--band`` names an index instead
of letting the dial choose, for a receiver whose table cannot be read: an index outside a table that
*was* read is refused, and a named band that does not cover the dial is warned about on stderr and
then used as asked - overriding the dial is the reason the flag exists.

Each file holds one big-endian 32-bit length before every WebSocket message, because the server's
messages are the unit its client decodes in: a replay that dropped the boundaries would be a
different stream from the one received.

Neither half of that needs a receiver to be graded: ``./websdr_capture.py --self-test`` reads
``fixtures/bandinfo-8band.js``, the table one front end actually serves, and checks the tune request
built from it, the dials that fall in its gaps, a ``--band`` outside its range, and the fetch that
reads it - so a dial sent out as hertz, or a band left out, or a table read from a truncated body
fails a case rather than a capture.

What is not here, on purpose: the DPCM decode.  A port of the server's client decoder is not
trusted yet - it did not survive comparison long enough to be relied on - so this writes only bytes
the server sent, and no WAV, no assembly and no sample counts are produced or claimed.  Nothing in
``parts.json`` depends on decoding: it records the wall clock length of the part and its size.

The decoder is not part of the transport, so a bad decode cannot make a take unreliable here: the
bytes are saved whatever a later decoder makes of them.

Standard library only, on purpose: this has to run unattended.
"""

import argparse
import base64
import hashlib
import json
import os
import random
import re
import socket
import struct
import sys
import time


class WebSocketClient(object):
    """Just enough WebSocket for one server: masked text out, binary in, ping answered."""

    def __init__(self, host, port, path, timeout=20, tls=False):
        self.host = host
        self.port = port
        self.path = path
        self.timeout = timeout
        self.tls = tls
        self.sock = None
        self.rbuf = b""
        self.frag = None
        self.frag_op = 0

    def connect(self):
        self.sock = socket.create_connection((self.host, self.port), timeout=self.timeout)
        if self.tls:
            # some receivers sit behind https, and their certificates are the proxy's, so this is
            # a transport for a public receiver's audio, not a place to authenticate anything
            import ssl
            ctx = ssl.create_default_context()
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
            self.sock = ctx.wrap_socket(self.sock, server_hostname=self.host)
        key = base64.b64encode(os.urandom(16)).decode()
        origin = "%s://%s:%d" % ("https" if self.tls else "http", self.host, self.port)
        req = ("GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nOrigin: %s\r\n"
               "User-Agent: digi_bench-capture/1\r\n\r\n"
               % (self.path, self.host, self.port, key, origin))
        self.sock.sendall(req.encode())
        head = b""
        while b"\r\n\r\n" not in head:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise IOError("server closed during handshake")
            head += chunk
            if len(head) > 65536:
                raise IOError("handshake response too long")
        line, rest = head.split(b"\r\n\r\n", 1)
        status = line.split(b"\r\n")[0].decode(errors="replace")
        if " 101 " not in status:
            expect = base64.b64encode(hashlib.sha1(
                (key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
            if expect.encode() not in line:
                raise IOError("websocket refused: %s" % status)
        self.rbuf = rest
        return self

    def _parse(self):
        while True:
            b = self.rbuf
            if len(b) < 2:
                return None
            b0, b1 = b[0], b[1]
            fin = b0 & 0x80
            op = b0 & 0x0F
            masked = b1 & 0x80
            ln = b1 & 0x7F
            off = 2
            if ln == 126:
                if len(b) < 4:
                    return None
                ln = struct.unpack(">H", b[2:4])[0]
                off = 4
            elif ln == 127:
                if len(b) < 10:
                    return None
                ln = struct.unpack(">Q", b[2:10])[0]
                off = 10
            mask = None
            if masked:
                if len(b) < off + 4:
                    return None
                mask = b[off:off + 4]
                off += 4
            if len(b) < off + ln:
                return None
            payload = bytearray(b[off:off + ln])
            if mask:
                for i in range(len(payload)):
                    payload[i] ^= mask[i & 3]
            self.rbuf = b[off + ln:]
            return fin, op, bytes(payload)

    def send_text(self, text):
        data = text.encode()
        mask = os.urandom(4)
        ln = len(data)
        if ln < 126:
            hdr = struct.pack(">BB", 0x81, 0x80 | ln)
        elif ln < 65536:
            hdr = struct.pack(">BBH", 0x81, 0x80 | 126, ln)
        else:
            hdr = struct.pack(">BBQ", 0x81, 0x80 | 127, ln)
        self.sock.sendall(hdr + mask + bytes(c ^ mask[i & 3] for i, c in enumerate(data)))

    def _send_control(self, op, payload=b""):
        mask = os.urandom(4)
        hdr = struct.pack(">BB", 0x80 | op, 0x80 | len(payload))
        self.sock.sendall(hdr + mask + bytes(c ^ mask[i & 3] for i, c in enumerate(payload)))

    def recv(self):
        """Returns (opcode, payload).  Handles ping/pong/close and continuation frames."""
        while True:
            frame = self._parse()
            if frame is None:
                chunk = self.sock.recv(65536)
                if not chunk:
                    raise IOError("connection closed")
                self.rbuf += chunk
                continue
            fin, op, payload = frame
            if op == 0x9:
                self._send_control(0xA, payload)
                continue
            if op == 0xA:
                continue
            if op == 0x8:
                raise IOError("server sent close")
            if op == 0x0:
                self.frag = (self.frag or b"") + payload
                if fin:
                    data, opcode = self.frag, self.frag_op
                    self.frag = None
                    return opcode, data
                continue
            if not fin:
                self.frag = payload
                self.frag_op = op
                continue
            return op, payload

    def close(self):
        try:
            self._send_control(0x8)
        except Exception:
            pass
        try:
            self.sock.close()
        except Exception:
            pass


def next_index(outdir):
    """Resume: never overwrite a take that is already there."""
    best = -1
    for name in os.listdir(outdir):
        if name.startswith("part-") and name.endswith(".stream"):
            try:
                best = max(best, int(name[5:8]))
            except ValueError:
                pass
    return best + 1


MODES = {"usb": 0, "lsb": 0, "cw": 0, "am": 1, "amsync": 2, "fm": 4}

BANDINFO = ("/tmp/bandinfo.js", "/bandinfo.js")
BAND_FIXTURE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "..", "fixtures", "bandinfo-8band.js")
MAX_BODY = 8 << 20      # the table the receiver serves is 29 kB; a claim past 8 MB is not a band
                        # table, and a body with no length to stop at still needs a ceiling


def get_request(path, host, port):
    """The plain GET for one of the receiver's own files.

    Accept-Encoding: identity because a gzipped band table reads as nothing: the parser would find
    no entries, the fetch would report no table, and the tune would go out without band= - which is
    the one fallback here that quietly returns the wrong band's audio instead of an error.
    """
    return ("GET %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n"
            "Accept-Encoding: identity\r\n"
            "User-Agent: digi_bench-capture/1\r\n\r\n" % (path, host, port))


def http_get(host, port, path, timeout=20, connect=None):
    """One plain GET, for the receiver's own band table.  The stream is unencrypted too.

    The receiver answers with a Content-Length and then keeps the connection open, so the body is
    read to its length: waiting for the close waits for a close that never comes.  A body that
    stops short of that length is an error rather than a short answer, because a table with an
    entry missing from it is a table whose band indices are all one place out, and the tune that
    follows would sit the front end on a band nobody asked for.  `connect` is a seam for
    --self-test, which grades all of this with no network.
    """
    connect = connect or socket.create_connection
    sock = connect((host, port), timeout=timeout)
    buf = b""
    try:
        sock.sendall(get_request(path, host, port).encode())
        while b"\r\n\r\n" not in buf:
            chunk = sock.recv(65536)
            if not chunk:
                raise IOError("%s: closed during the response headers" % path)
            buf += chunk
            if len(buf) > 4 << 20:
                raise IOError("%s: response headers too long" % path)
        head, _, body = buf.partition(b"\r\n\r\n")
        status = head.split(b"\r\n")[0].decode(errors="replace")
        if " 200 " not in status:
            raise IOError("%s: %s" % (path, status))
        encoding = re.search(rb"content-encoding:\s*([^\s;,]+)", head, re.I)
        if encoding and encoding.group(1).lower() != b"identity":
            raise IOError("%s: body is %s-encoded, so the band table read from it would have no "
                          "bands in it"
                          % (path, encoding.group(1).decode(errors="replace")))
        transfer = re.search(rb"transfer-encoding:\s*([^\s;,]+)", head, re.I)
        if transfer:
            # a chunked body would reach parse_bands with chunk sizes interleaved between the
            # entries, which is the one input shape the whole-or-nothing guard cannot promise to
            # notice - it only sees entries that moved, not entries with digits glued to them
            raise IOError("%s: body is %s transfer-encoded, which this reader does not unwrap"
                          % (path, transfer.group(1).decode(errors="replace")))
        length = re.search(rb"content-length:\s*(\d+)", head, re.I)
        if length:
            want = int(length.group(1))
            if want > MAX_BODY:
                raise IOError("%s: claims %d bytes, over the %d byte ceiling"
                              % (path, want, MAX_BODY))
            while len(body) < want:
                chunk = sock.recv(65536)
                if not chunk:
                    raise IOError("%s: closed with %d of %d body bytes" % (path, len(body), want))
                body += chunk
            body = body[:want]
        else:
            # no length to read to: the close ends it, and a keep-alive never closes
            sock.settimeout(5.0)
            try:
                while True:
                    chunk = sock.recv(65536)
                    if not chunk:
                        break
                    body += chunk
                    if len(body) > MAX_BODY:
                        raise IOError("%s: more than %d bytes with no length to stop at"
                                      % (path, MAX_BODY))
            except socket.timeout:
                pass
    finally:
        sock.close()
    return body.decode(errors="replace")


def parse_bands(js):
    """The receiver's band table, as its own page states it: a centre, a width and a name per band.

    Each entry is a flat JS object, so a brace pair holding a centre frequency and a sample rate is
    a band and nothing else in the file is.  In kHz, like the dial.

    The answer is the list, or None for "this file cannot be read as a table" - and that distinction
    matters more than the list itself, because band= is a *position* in this table.  An entry that
    half-reads - a centre with no width, or a width with no centre - moves every index after it,
    and the tune that follows sits the front end on a neighbouring band while the log names the
    band it thought it had: the same fault as a missing band=, one dial quieter.  So a brace pair
    carrying either of the two keys has to carry both, and a table that declares its own count has
    to match the entries found.  An empty list is still just no table, which the caller says aloud.
    """
    bands = []
    for match in re.finditer(r"\{[^{}]*\}", js):
        block = match.group(0)
        centre = re.search(r"centerfreq:\s*(-?[0-9.]+)", block, re.I)
        rate = re.search(r"samplerate:\s*(-?[0-9.]+)", block, re.I)
        if not (centre or rate):
            continue                            # not a band entry at all
        if not (centre and rate):
            return None                         # a band entry only half read: the indices move
        name = re.search(r"name:\s*'([^']*)'", block)
        bands.append({"name": name.group(1) if name else "",
                      "centre": float(centre.group(1)),
                      "rate": float(rate.group(1))})
    declared = re.search(r"var nbands\s*=\s*([0-9]+)", js)
    if declared and int(declared.group(1)) != len(bands):
        return None                             # the page says how many bands it has and these are
                                                # not all of them, so at least one index is wrong
    return bands


def fetch_bands(host, port, timeout=20, connect=None):
    """The receiver's band table, the reason there is none, and the digest of the file it came from.

    The table is None for a server that serves none *and* for one whose table could not be read
    whole: the caller then tunes without band= and says why, which is a worse take and a better
    report than an index out of a table whose entries moved.  The digest is recorded beside a take
    so a later reader can tell whether two parts were tuned against the same table or two
    different ones.  The reasons come back rather than going to stderr here so --self-test can pin
    them, and a receiver that serves no table costs one line instead of two per attempt.  `connect`
    reaches http_get for that same self-test, which has no network.
    """
    reasons = []
    for path in BANDINFO:
        try:
            body = http_get(host, port, path, timeout, connect)
        except Exception as exc:
            reasons.append("%s: %s" % (path, exc))
            continue
        bands = parse_bands(body)
        if bands is None:
            reasons.append("%s: a band entry did not read whole, or the entries are not the count "
                           "the page declares" % path)
            continue
        if bands:
            return bands, None, hashlib.sha256(body.encode()).hexdigest()
        reasons.append("%s: no band entries in it" % path)
    return None, "; ".join(reasons) if reasons else "nothing answered", None


def band_for(bands, freq_khz):
    """The index of the receiver band that covers a dial, or None.

    A receiver's bands are what its front end can hear: a dial in a gap between them is not
    reachable, and asking for it leaves the front end where it was.
    """
    for index, band in enumerate(bands):
        half = band["rate"] / 2.0
        if band["centre"] - half <= freq_khz <= band["centre"] + half:
            return index
    return None


def band_span(band):
    return "%s %.3f-%.3f kHz" % (band["name"] or "band",
                                  band["centre"] - band["rate"] / 2.0,
                                  band["centre"] + band["rate"] / 2.0)


def band_spans_text(bands):
    """The table as the receiver states it, one band per line, for the complaints that quote it."""
    return "".join("  %s\n" % band_span(b) for b in bands)


def band_refusal(bands, freq_khz, host):
    """The whole complaint for a dial that sits in no band of this receiver."""
    return ("--freq-khz %.3f is in none of %s's %d bands:\n%s"
            "The server keeps the receiver on the band the connection started in, so the\n"
            "take would be that band's audio whatever the dial says.  Pick a frequency\n"
            "inside a band, or name one with --band."
            % (freq_khz, host, len(bands), band_spans_text(bands)))


def band_range_refusal(bands, band, host):
    """The whole complaint for a --band outside the table that was just read.

    An index past the end of the table is not a band and not a gap either: it names nothing the
    server has, so the reply is whatever it falls back to, which is the audio this whole check
    exists to stop anyone mistaking for the dial.  The table is in hand here, so it is the table
    that gets quoted back.
    """
    return ("--band %d is out of range: %s's table has %d bands, numbered 0 to %d:\n%s"
            "band= is a position in this table, so an index past its end names no band and the\n"
            "server answers with whatever it falls back to.  Name one of the bands above, or\n"
            "drop --band and let the dial choose."
            % (band, host, len(bands), len(bands) - 1, band_spans_text(bands)))


def band_mismatch_note(bands, band, freq_khz, host):
    """Why a named band is not the one the dial is in, or None when it is.

    --band is the way to reach a receiver whose table cannot be read, so it overrides the band the
    dial falls in; but when a table has been read, the pair is checkable, and a band that does not
    cover the dial is the 20 November hum one index short.  Warned about rather than refused: the
    override is there to be used, and a named band may be exactly what is being asked for.
    """
    named = bands[band]
    covering = band_for(bands, freq_khz)
    if covering == band:
        return None
    if covering is None:
        return ("--band %d is %s, and %.3f kHz is in none of %s's bands.\n"
                "The take will be that band's audio, which the dial does not reach.\n"
                "Asking for it as named."
                % (band, band_span(named), freq_khz, host))
    return ("--band %d is %s, which does not cover %.3f kHz; %s does.\n"
            "The take will be the named band's audio whatever the dial says, which is\n"
            "what band= was added to stop.  Asking for it as named."
            % (band, band_span(named), freq_khz, band_span(bands[covering])))


def choose_band(bands, freq_khz, band, host, why=None):
    """The band to tune with and the word to say about it, the rules apart from the fetch.

    `bands` is the receiver's table or None for a receiver that serves none - or that served one
    which could not be read whole, with `why` saying which; `band` is what --band named, or None to
    let the dial choose.  A dial in a gap and an index past the end of the table are both refusals,
    because each would be recorded as a take of a band nobody asked for; a named band that does not
    cover the dial is only a warning, since naming a band is how a receiver with no readable table
    is reached.
    """
    missing = "no band table from %s" % host + (" (%s)" % why if why else "")
    if band is None:
        if bands:
            band = band_for(bands, freq_khz)
            if band is None:
                raise SystemExit(band_refusal(bands, freq_khz, host))
            return band, None
        return None, missing + "; tuning without band="
    if bands:
        if not 0 <= band < len(bands):
            raise SystemExit(band_range_refusal(bands, band, host))
        return band, band_mismatch_note(bands, band, freq_khz, host)
    return band, missing + "; taking --band %d as given" % band


def tune_text(freq_khz, mode, lo_hz, hi_hz, band=None):
    """The server's own tune request: frequency in kHz, band index, passband edges in kHz about it.

    Positive edges mean the upper sideband, negative the lower, so the mode field stays 0 for both
    and the sign carries the sideband - that is how the browser client sends it, and sending
    anything else leaves the receiver on the frequency its last user chose.

    The band is the index of the front-end band that covers the dial, and the page's client always
    sends it.  Left out, a multi-band receiver keeps the band the connection started in, applies the
    passband and the mode to it, and returns that band's audio however the dial is set.

    `f` is the dial in kHz, and it goes out as given.  Scaling it to Hz here made every request a
    frequency 1000x too high, which no receiver can tune; the server leaves the dial where it
    already was, so on 20 November every take in an attempt came off the same fallback channel
    whatever --freq-khz said, and it looked like a band that was shut rather than a bug.
    """
    text = "GET /~~param?f=%.3f" % freq_khz
    if band is not None:
        text += "&band=%d" % band
    return text + "&lo=%.3f&hi=%.3f&mode=%d" % (
        lo_hz / 1000.0, hi_hz / 1000.0, MODES.get(mode.lower(), 0))


def part_provenance(args, tune, band, bands, note, why, digest):
    """What a part was asked for, recorded in parts.json beside what arrived.

    The 20 November takes were indistinguishable from live radio in the metadata this file used to
    keep: index, name, time, bytes, messages and a band index, and nothing about the dial, the
    passband or the request that produced them - so "five dials, one channel" had to be found by
    comparing the files themselves.  Recording the tune string that went out makes that one field to
    diff, the band's span says whether the index could reach the dial at all, and the table's digest
    says whether two parts were even tuned against the same bands.
    """
    row = {"host": args.host,
           "freq_khz": args.freq_khz,
           "mode": args.mode,
           "audio": args.audio,
           "tune": tune,
           "band": band,
           "band_span": band_span(bands[band]) if (bands and 0 <= band < len(bands)) else None,
           "bandinfo": "sha256:" + digest if digest else None,
           "deadline": args.deadline or None}
    said = note or why
    if said:
        row["band_note"] = said
    return row


def capture(args):
    outdir = args.out
    os.makedirs(outdir, exist_ok=True)
    meta_path = os.path.join(outdir, "parts.json")
    meta = []
    if os.path.exists(meta_path):
        try:
            meta = json.load(open(meta_path))
        except Exception:
            meta = []

    host, _, port = args.host.partition(":")
    port = int(port or 8901)
    # which of the receiver's front-end bands the dial is in: the page's client sends this with the
    # tune, and without it a multi-band receiver answers with whatever band the connection opened
    # in.  The table is read whether or not --band names a band, because the name and the span an
    # index covers are the only thing that index can be checked against.  With no table, the index
    # is taken as given and said so on stderr
    bands, why, digest = fetch_bands(host, port)
    band, note = choose_band(bands, args.freq_khz, args.band, args.host, why)
    if note:
        print(note, file=sys.stderr)
    # A passband can sit on either side of the dial, so both edges may be signed.  The separator
    # is the dash between them, not any sign inside a number: "600-1700" is 600 up to 1700, and
    # "-1700--600" is 1700 below the carrier up to 600 below it.  Splitting on "-" reads that
    # second form as an empty string, and a plain number regex reads the first as 600 and -1700,
    # which asks the server for an inverted passband and leaves the receiver where it was.
    edges = re.match(r"^\s*(-?\d+)\s*-\s*(-?\d+)\s*$", args.audio)
    if not edges:
        raise SystemExit("--audio wants two edges in Hz, e.g. 600-1700 or -1700--600")
    lo, hi = int(edges.group(1)), int(edges.group(2))
    # the request is built once and sent as that same string every attempt, so parts.json can record
    # the exact text a take was asked with rather than a reconstruction of it
    tune = tune_text(args.freq_khz, args.mode, lo, hi, band)
    provenance = part_provenance(args, tune, band, bands, note, why, digest)
    index = next_index(outdir)
    deadline = time.time() + args.deadline if args.deadline else None
    failures = 0
    made = 0

    while made < args.parts:
        if deadline and time.time() > deadline:
            print("deadline reached before %d parts; %d made" % (args.parts, made), file=sys.stderr)
            break
        client = WebSocketClient(host, port, "/~~stream")
        rawpath = os.path.join(outdir, "part-%03d.stream" % index)
        rawfile = None
        started = None
        rawlen = 0
        messages = 0
        complete = False
        try:
            client.connect()
            client.send_text(tune)
            rawfile = open(rawpath, "wb")
            while True:
                op, payload = client.recv()
                if op != 0x2:
                    continue
                # the stream is saved with one big-endian length before each WebSocket message:
                # the server's messages are the decode unit, so a replay has to carry their
                # boundaries or it decodes a different stream than was received
                rawfile.write(struct.pack(">I", len(payload)))
                rawfile.write(payload)
                rawlen += len(payload)
                messages += 1
                if started is None:
                    started = time.time()
                # a part is a length of stream, so it is measured on the wall clock; with no decode
                # there is no sample count to reach, and inventing one would be the thing this
                # collector just stopped doing
                if time.time() - started >= args.part_seconds:
                    complete = True
                    break
                if deadline and time.time() > deadline:
                    break
        except KeyboardInterrupt:
            print("interrupted; parts kept", file=sys.stderr)
            break
        except Exception as exc:
            failures += 1
            wait = min(30, 2 ** failures) + random.random()
            print("%s  stream failed (%s); reconnecting in %.1f s" %
                  (time.strftime("%H:%M:%SZ", time.gmtime()), exc, wait), file=sys.stderr)
            sys.stderr.flush()
            time.sleep(wait)
        finally:
            try:
                client.close()
            except Exception:
                pass
            if rawfile is not None:
                rawfile.close()

        if rawlen:
            # whatever arrived is kept, complete or not: a fragment is still radio that will not
            # come again, and the next attempt must not truncate it
            rec = dict(provenance)
            rec.update({
                "index": index,
                "file": os.path.basename(rawpath),
                "utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(started or time.time())),
                "seconds": round(time.time() - started, 3) if started else 0.0,
                "bytes": rawlen,
                "messages": messages,
                "complete": complete,
            })
            meta.append(rec)
            json.dump(meta, open(meta_path, "w"), indent=1)
            print("part-%03d.stream  %6.1f s  %9d bytes  %5d messages  %s  band %s" %
                  (index, rec["seconds"], rawlen, messages, rec["utc"],
                   band_description(bands, band)))
            sys.stdout.flush()
            index += 1
            if complete:
                made += 1
                failures = 0

    return 0 if made else 2


def band_description(bands, band):
    """What a part's log line says about the band it was tuned with.

    A named band by name, an index with no table behind it as unnamed rather than a blank - a blank
    is what a take that skipped the band check would look like too - and no index at all as none.
    """
    if band is None:
        return "none"
    name = bands[band]["name"] if (bands and 0 <= band < len(bands)) else ""
    return "%d (%s)" % (band, name) if name else "%d (unnamed)" % band


class _ScriptedSocket(object):
    """A socket that replays canned reads, so the table fetch can be graded with no network."""

    def __init__(self, chunks):
        self.chunks = list(chunks)
        self.sent = b""

    def sendall(self, data):
        self.sent = data

    def settimeout(self, seconds):
        pass

    def recv(self, size):
        return self.chunks.pop(0) if self.chunks else b""

    def close(self):
        pass


def _replaying(chunks):
    """A connect() that hands back a replay of `chunks` instead of reaching for the network."""
    def connect(address, timeout=None):
        return _ScriptedSocket(chunks)
    return connect


def self_test():
    """The collector's own checks: the tune it builds, the table it reads, and what it refuses.

    No receiver, no client and no network, so CI can grade the half that went wrong on 20 November:
    a dial that left as hertz because it was multiplied by 1000, which no receiver can tune, and a
    tune that left band= out, which sits a multi-band front end on whatever band the connection
    opened in.  Both are decided by arithmetic in front of any socket, and the fetch is graded
    against a replay of the receiver's own bytes, so a table that arrives truncated or encoded is
    refused as the error it is rather than read as a short table with its indices moved.
    """
    problems = []
    total = [0]

    def case(name, got, want):
        total[0] += 1
        if got == want:
            print("capture: %-56s ok" % name)
            return True
        problems.append(name)
        print("capture: %-56s FAIL\n           got  %r\n           want %r" % (name, got, want))
        return False

    try:
        table = open(BAND_FIXTURE, "rb").read()
    except IOError as exc:
        print("capture: SELF-TEST FAILED - no band fixture at %s (%s)" % (BAND_FIXTURE, exc))
        return 1
    bands = parse_bands(table.decode(errors="replace"))
    host = "na5b.co:8901"
    path = "/tmp/bandinfo.js"

    def response(body_bytes, length=None, extra=""):
        return ("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %d%s\r\n"
                "Connection: close\r\n\r\n" % (len(body_bytes) if length is None else length,
                                               extra)).encode()

    def fetch(name, chunks, want):
        try:
            got = http_get(host.split(":")[0], 8901, path, connect=_replaying(chunks))
        except IOError as exc:
            got = str(exc)
        case(name, got, want)

    # the tune request: a dial in kHz, and the band that covers it in the same request
    case("the dial goes out in kHz, with the band covering it",
         tune_text(4581.41, "usb", 600, 1700, 1),
         "GET /~~param?f=4581.410&band=1&lo=0.600&hi=1.700&mode=0")
    case("and not once multiplied by a thousand",
         tune_text(10100.8, "usb", 450, 1350, 3),
         "GET /~~param?f=10100.800&band=3&lo=0.450&hi=1.350&mode=0")
    case("band 0 is sent, not read as no band",
         tune_text(1500.0, "usb", 600, 1700, 0),
         "GET /~~param?f=1500.000&band=0&lo=0.600&hi=1.700&mode=0")
    case("a tune with no table behind it leaves band= out",
         tune_text(7040.0, "usb", 600, 1700, None),
         "GET /~~param?f=7040.000&lo=0.600&hi=1.700&mode=0")
    case("a lower sideband keeps both edges signed",
         tune_text(4581.41, "lsb", -1700, -600, 1),
         "GET /~~param?f=4581.410&band=1&lo=-1.700&hi=-0.600&mode=0")

    # the table the receiver serves, as the fixture states it
    case("the fixture's table is eight bands", len(bands), 8)
    case("whose spans are the ones the README lists",
         "\n".join(band_span(b) for b in bands),
         "\n".join(["MW-160 0.000-2048.000 kHz", "80m 3200.000-5248.000 kHz",
                    "40m 5324.000-7372.000 kHz", "30m 8825.000-10873.000 kHz",
                    "25m 11599.000-13647.000 kHz", "20m 13655.000-15703.000 kHz",
                    "15m 20888.000-21912.000 kHz", "10m 26961.000-29009.000 kHz"]))
    case("and one of those eight under each of eight dials",
         [band_for(bands, f) for f in (1500.0, 4581.41, 7040.0, 10100.8,
                                       11895.0, 14200.0, 21450.0, 28300.0)],
         [0, 1, 2, 3, 4, 5, 6, 7])
    case("a band's own edges belong to that band",
         [band_for(bands, f) for f in (2048.0, 2048.001, 3200.0, 5248.0)],
         [0, None, 1, 1])
    case("and the gaps between them belong to none",
         [band_for(bands, f) for f in (2500.0, 5300.0, 8000.0, 11000.0,
                                       13650.0, 18100.0, 24000.0, 30000.0)],
         [None] * 8)
    case("a body with no table in it parses to no bands",
         parse_bands("var bandinfo = [ { name: 'not a band' } ];"), [])

    # ...but a table that is *nearly* read is not no table, it is a table with the wrong indices,
    # and band= is an index: this is the difference between a take of the wrong band and a take the
    # log says was tuned without one
    text = table.decode(errors="replace")
    case("an entry that lost its width distrusts the whole table",
         parse_bands(text.replace("    samplerate: 2048.000000,\n", "", 1)), None)
    case("and that holds with no count on the page to check it against",
         parse_bands(re.sub(r"var nbands=\d+;\n", "", text, count=1)
                     .replace("    samplerate: 2048.000000,\n", "", 1)), None)
    case("and an entry that lost its centre does too",
         parse_bands(re.sub(r"centerfreq: [0-9.]+,", "centreFreq: 6348.000000,", text, count=1)),
         None)
    case("so does a count the page declares and the entries do not fill",
         parse_bands(re.sub(r"var nbands=\d+;", "var nbands=9;", text)), None)
    case("and a table cut off mid-file is not half a table", parse_bands(text[:len(text) // 2]),
         None)

    def chose(route):
        """What the band choice answers, the complaint it raises, or what it threw instead.

        A rule that loosened into a crash has to read as a failed case rather than end the run on a
        traceback, which graded nothing at all.
        """
        try:
            return route()
        except SystemExit as exc:
            return str(exc)
        except Exception as exc:
            return "raised %s: %s" % (type(exc).__name__, exc)

    def lines(answer):
        """A complaint by line, so a route that stopped refusing reads as a failed case."""
        return answer.splitlines() if isinstance(answer, str) else []

    refusal = chose(lambda: choose_band(bands, 24000.0, None, host))
    case("a dial in a gap is refused with the dial and the count named",
         lines(refusal)[0:1],
         ["--freq-khz 24000.000 is in none of na5b.co:8901's 8 bands:"])
    case("and every band the receiver has, one per line",
         [line.strip() for line in lines(refusal)[1:9]],
         [band_span(b) for b in bands])
    case("and what to do about it",
         lines(refusal)[9:],
         ["The server keeps the receiver on the band the connection started in, so the",
          "take would be that band's audio whatever the dial says.  Pick a frequency",
          "inside a band, or name one with --band."])
    over = chose(lambda: choose_band(bands, 10100.8, 99, host))
    case("a --band past the end of the table is refused by number",
         lines(over)[0:1],
         ["--band 99 is out of range: na5b.co:8901's table has 8 bands, numbered 0 to 7:"])
    case("with the table it is a position in quoted back",
         [line.strip() for line in lines(over)[1:9]],
         [band_span(b) for b in bands])
    case("and the way out named",
         lines(over)[9:],
         ["band= is a position in this table, so an index past its end names no band and the",
          "server answers with whatever it falls back to.  Name one of the bands above, or",
          "drop --band and let the dial choose."])
    case("a --band that covers the dial is not worth a word",
         chose(lambda: choose_band(bands, 10100.8, 3, host)), (3, None))
    case("a --band that does not cover it says which one does",
         chose(lambda: choose_band(bands, 10100.8, 0, host)),
         (0, "--band 0 is MW-160 0.000-2048.000 kHz, which does not cover 10100.800 kHz; "
             "30m 8825.000-10873.000 kHz does.\nThe take will be the named band's audio "
             "whatever the dial says, which is\nwhat band= was added to stop.  Asking for it as "
             "named."))
    case("a named band with a dial in no band says the dial does not reach",
         chose(lambda: choose_band(bands, 24000.0, 6, host)),
         (6, "--band 6 is 15m 20888.000-21912.000 kHz, and 24000.000 kHz is in none of "
             "na5b.co:8901's bands.\nThe take will be that band's audio, which the dial does not "
             "reach.\nAsking for it as named."))
    case("the dial chooses its own band when --band is not given",
         chose(lambda: choose_band(bands, 10100.8, None, host)), (3, None))
    case("a gap dial named with --band is a word, not a refusal",
         isinstance(chose(lambda: choose_band(bands, 24000.0, 6, host)), tuple), True)
    case("a receiver with no table tunes without band=, with the reason",
         chose(lambda: choose_band(None, 10100.8, None, host,
                                   "/tmp/bandinfo.js: HTTP/1.1 404 Not Found")),
         (None, "no band table from na5b.co:8901 (/tmp/bandinfo.js: HTTP/1.1 404 Not Found); "
                "tuning without band="))
    case("and takes a named band as given, with the reason",
         chose(lambda: choose_band(None, 10100.8, 4, host, "nothing answered")),
         (4, "no band table from na5b.co:8901 (nothing answered); taking --band 4 as given"))
    case("a table that simply is not there is still said plainly",
         chose(lambda: choose_band(None, 10100.8, None, host)),
         (None, "no band table from na5b.co:8901; tuning without band="))
    case("a part logs the band it was tuned with by name",
         band_description(bands, 3), "3 (30m)")
    case("an index with no table behind it by that it is unnamed",
         band_description(None, 0), "0 (unnamed)")
    case("an index a table does not reach as unnamed too",
         band_description(bands[:2], 5), "5 (unnamed)")
    case("and no band at all as none", band_description(bands, None), "none")

    # the fetch: what the request asks for, and what is refused before a table is trusted
    case("the table is asked for unencoded, so it cannot read as empty",
         get_request(path, "na5b.co", 8901),
         "GET /tmp/bandinfo.js HTTP/1.1\r\nHost: na5b.co:8901\r\nConnection: close\r\n"
         "Accept-Encoding: identity\r\nUser-Agent: digi_bench-capture/1\r\n\r\n")
    fetch("a table that arrives whole comes back as its own bytes",
          [response(table) + table], table.decode(errors="replace"))
    fetch("one that stops short of its length is an error",
          [response(table) + table[:64]],
          "%s: closed with 64 of %d body bytes" % (path, len(table)))
    fetch("one that arrives encoded is an error, not an empty table",
          [response(b"var nbands=0;", extra="\r\nContent-Encoding: gzip") + b"var nbands=0;"],
          "%s: body is gzip-encoded, so the band table read from it would have no bands in it"
          % path)
    fetch("an explicit identity passes, so only real encodings are refused",
          [response(b"var nbands=1;", extra="\r\nContent-Encoding: identity") + b"var nbands=1;"],
          "var nbands=1;")
    fetch("and a length past the ceiling is refused before it is read",
          [response(table, length=MAX_BODY + 1)],
          "%s: claims %d bytes, over the %d byte ceiling" % (path, MAX_BODY + 1, MAX_BODY))

    # fetch_bands over the same replays: what it hands the tune, what it says when it cannot, and
    # which file it read them from
    def fetched(name, chunks, want_bands, want_why_contains, want_digest=False):
        total[0] += 1
        got_bands, got_why, got_digest = fetch_bands("na5b.co", 8901, connect=_replaying(chunks))
        ok = (len(got_bands) if got_bands else None) == want_bands
        ok = ok and (want_why_contains is None or
                     (got_why or "").find(want_why_contains) >= 0)
        ok = ok and bool(got_digest) == want_digest
        if ok:
            print("capture: %-56s ok" % name)
        else:
            problems.append(name)
            print("capture: %-56s FAIL\n           got  %r, %r, %s\n           want %r in it, %r, "
                  "digest %s" % (name, got_bands, got_why, bool(got_digest), want_why_contains,
                                 want_bands, want_digest))

    fetched("a whole table comes back as eight bands, no excuse, and its own digest",
            [response(table) + table], 8, None, True)
    fetched("a half-read one comes back as none, and says which path",
            [response(text[:len(text) // 2].encode()) + text[:len(text) // 2].encode()],
            None, "a band entry did not read whole")
    fetched("and a server that answers 404 says that instead",
            [("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n").encode()], None, "404")

    # what a part records about the ask, which is the field a later reader compares instead of the
    # bytes: the 20 November takes differed from live radio only in what had been requested
    ns = argparse.Namespace(host="na5b.co:8901", freq_khz=10100.8, mode="usb",
                            audio="450-1350", deadline=0.0)
    asked = part_provenance(ns, tune_text(10100.8, "usb", 450, 1350, 3), 3, bands,
                            None, None, "0123456789abcdef" * 4)
    case("a part records the request string, not a reconstruction of it", asked["tune"],
         "GET /~~param?f=10100.800&band=3&lo=0.450&hi=1.350&mode=0")
    case("the dial, the mode and the passband it was asked with",
         (asked["host"], asked["freq_khz"], asked["mode"], asked["audio"]),
         ("na5b.co:8901", 10100.8, "usb", "450-1350"))
    case("the band's own span, so an index can be checked against the dial",
         (asked["band"], asked["band_span"]), (3, "30m 8825.000-10873.000 kHz"))
    case("which table the index came from", asked["bandinfo"],
         "sha256:" + "0123456789abcdef" * 4)
    case("and no band_note when there was nothing to say", asked.get("band_note"), None)
    blind = part_provenance(ns, tune_text(10100.8, "usb", 450, 1350, None), None, None, None,
                            "/tmp/bandinfo.js: HTTP/1.1 404 Not Found", None)
    case("a take tuned without a table says so, in the file it lands in",
         (blind["band"], blind["band_span"], blind["bandinfo"], blind.get("band_note")),
         (None, None, None, "/tmp/bandinfo.js: HTTP/1.1 404 Not Found"))
    warned = part_provenance(ns, tune_text(10100.8, "usb", 450, 1350, 0), 0, bands,
                             band_mismatch_note(bands, 0, 10100.8, "na5b.co:8901"), None,
                             "0123456789abcdef" * 4)
    case("and a take whose named band misses the dial records the warning too",
         next(iter((warned.get("band_note") or "").splitlines()), "no note recorded"),
         "--band 0 is MW-160 0.000-2048.000 kHz, which does not cover 10100.800 kHz; "
         "30m 8825.000-10873.000 kHz does.")
    case("a part records the give-up it was given", asked["deadline"], None)
    case("and the give-up when there is one",
         part_provenance(argparse.Namespace(host="h", freq_khz=1.0, mode="usb", audio="1-2",
                                            deadline=90.0), "t", 0, bands, None, None, "d")
         ["deadline"], 90.0)

    fetch("a chunked body is refused, not read as entries with digits glued to them",
          [("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n").encode()
           + b"11\r\nvar nbands=0;\r\n0\r\n\r\n"],
          "%s: body is chunked transfer-encoded, which this reader does not unwrap" % path)

    if problems:
        print("capture: SELF-TEST FAILED - %d of %d cases: %s"
              % (len(problems), total[0], ", ".join(problems)))
        return 1
    print("capture: self-test ok - %d cases: the tune request in kHz with its band, the table it "
          "came from, what a part records about the ask, the dials in the gaps, a --band outside "
          "the table, and the fetch that reads it" % total[0])
    return 0


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if "--self-test" in argv:
        # before the parser: the self-test asks for no dial, no host and nothing on the network,
        # and every other route here requires --freq-khz
        return self_test()
    ap = argparse.ArgumentParser(description="Capture WebSDR streams to indexed .stream parts.")
    ap.add_argument("--host", default="websdr.ewi.utwente.nl:8901",
                    help="host:port of the WebSDR (default: %(default)s)")
    ap.add_argument("--freq-khz", type=float, required=True, help="dial frequency in kHz")
    ap.add_argument("--mode", default="usb", help="demodulator: usb, lsb, am (default usb)")
    ap.add_argument("--audio", default="600-1700", help="audio passband in Hz, LO-HI")
    ap.add_argument("--part-seconds", type=float, default=20.0,
                    help="length of a part, measured on the wall clock")
    ap.add_argument("--parts", type=int, default=1)
    ap.add_argument("--band", type=int, default=None,
                    help="receiver band index; default: the band covering --freq-khz, from the "
                         "receiver's own band table")
    ap.add_argument("--deadline", type=float, default=0, help="give up after this many seconds")
    ap.add_argument("--out", default="capture")
    ap.add_argument("--self-test", action="store_true",
                    help="grade the tune request, the band table and the refusals with no network "
                         "(handled before the parser, so no other flag is needed)")
    args = ap.parse_args(argv)
    return capture(args)


if __name__ == "__main__":
    sys.exit(main())
