#!/usr/bin/env python3
"""Capture audio from a WebSDR without a browser.

This half is the proven half: it connects to the same WebSocket the WebSDR's own HTML5 client
uses (``/~~stream``), asks for a frequency and a passband, and saves what arrives to
``part-NNN.stream`` files by index.  When the connection drops - and it does, that is what a shared
receiver is like - the collector reconnects, retunes, and carries on at the next index, so a take
that spans a drop is still a take, and a fragment left by a failed attempt is kept rather than
overwritten.

    ./websdr_capture.py --freq-khz 4581.41 --part-seconds 30 --parts 2 --out /tmp/ddk2

Each file holds one big-endian 32-bit length before every WebSocket message, because the server's
messages are the unit its client decodes in: a replay that dropped the boundaries would be a
different stream from the one received.

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


def tune_text(freq_khz, mode, lo_hz, hi_hz):
    """The server's own tune request: frequency in kHz, passband edges in kHz about it.

    Positive edges mean the upper sideband, negative the lower, so the mode field stays 0 for both
    and the sign carries the sideband - that is how the browser client sends it, and sending
    anything else leaves the receiver on the frequency its last user chose.
    """
    return "GET /~~param?f=%.3f&lo=%.3f&hi=%.3f&mode=%d" % (
        freq_khz, lo_hz / 1000.0, hi_hz / 1000.0, MODES.get(mode.lower(), 0))


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
    freq_hz = int(round(args.freq_khz * 1000))
    # A passband can sit on either side of the dial, so both edges may be signed.  The separator
    # is the dash between them, not any sign inside a number: "600-1700" is 600 up to 1700, and
    # "-1700--600" is 1700 below the carrier up to 600 below it.  Splitting on "-" reads that
    # second form as an empty string, and a plain number regex reads the first as 600 and -1700,
    # which asks the server for an inverted passband and leaves the receiver where it was.
    edges = re.match(r"^\s*(-?\d+)\s*-\s*(-?\d+)\s*$", args.audio)
    if not edges:
        raise SystemExit("--audio wants two edges in Hz, e.g. 600-1700 or -1700--600")
    lo, hi = int(edges.group(1)), int(edges.group(2))
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
            client.send_text(tune_text(freq_hz, args.mode, lo, hi))
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
            rec = {
                "index": index,
                "file": os.path.basename(rawpath),
                "utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(started or time.time())),
                "seconds": round(time.time() - started, 3) if started else 0.0,
                "bytes": rawlen,
                "messages": messages,
                "complete": complete,
            }
            meta.append(rec)
            json.dump(meta, open(meta_path, "w"), indent=1)
            print("part-%03d.stream  %6.1f s  %9d bytes  %5d messages  %s" %
                  (index, rec["seconds"], rawlen, messages, rec["utc"]))
            sys.stdout.flush()
            index += 1
            if complete:
                made += 1
                failures = 0

    return 0 if made else 2


def main(argv=None):
    ap = argparse.ArgumentParser(description="Capture WebSDR streams to indexed .stream parts.")
    ap.add_argument("--host", default="websdr.ewi.utwente.nl:8901",
                    help="host:port of the WebSDR (default: %(default)s)")
    ap.add_argument("--freq-khz", type=float, required=True, help="dial frequency in kHz")
    ap.add_argument("--mode", default="usb", help="demodulator: usb, lsb, am (default usb)")
    ap.add_argument("--audio", default="600-1700", help="audio passband in Hz, LO-HI")
    ap.add_argument("--part-seconds", type=float, default=20.0,
                    help="length of a part, measured on the wall clock")
    ap.add_argument("--parts", type=int, default=1)
    ap.add_argument("--deadline", type=float, default=0, help="give up after this many seconds")
    ap.add_argument("--out", default="capture")
    args = ap.parse_args(argv)
    return capture(args)


if __name__ == "__main__":
    sys.exit(main())
