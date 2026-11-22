// Oracle: decode a captured WebSDR .stream with the server's own client code.
//
// websdr-sound.js is the decoder the browser runs.  We load it unchanged, stub just enough of the
// browser around it, and use its recorder start/finish calls - so the samples come from exactly the
// code path that produces a WAV in the page, not from a reimplementation.
//
// It refuses rather than guesses.  The client's message loop has no error case: a byte below 0x80
// falls through to `else if (128 != (b[a] & 128)) m = 1, s = 2` and the sample loop then invents
// samples out of whatever follows, so any byte soup "decodes".  A WAV appearing therefore proves
// nothing, so before the decoder is even loaded the file has to satisfy five things:
//
//   * every message is a sequence of known elements that ends exactly on its last byte
//   * the sample rate comes from the stream's rate element, and never changes
//   * no run of silence markers lasts longer than 2 s (the client's recorder stops appending there)
//   * no audio arrives in a message before the rate element (it could not be recorded at that rate)
//   * the take carries audio at all: 8-bit blocks of one code repeated 128 times are the fill an
//     empty band gets and a silence marker is 128 zeros, each one level held for 128 samples, so a
//     take made only of those - or with nothing after its rate element at all - decodes to levels,
//     not to a noise floor
//
// and after decoding, the recorder has to have produced exactly the samples the walk counted.
// The file is written last, so a refusal leaves no WAV behind.
//
// The stream format is WebSDR's, and so is the decoder: websdr-sound.js is Copyright 2013-2025
// pa3fwm@websdr.org, all rights reserved, with permission only for serving it unmodified from his
// servers to his clients.  It is therefore NOT committed here.  Save the script your receiver's
// page loads, unmodified, as websdr-sound.js beside this file, or point --client at it.  That file
// is the only part of this that is not ours: the framing walk, the rate, the silence rule and the
// WAV writer below are all checked into this tree.
//
// usage: node decode_oracle.js <stream-file> <out.wav> [--client <websdr-sound.js>]
//        node decode_oracle.js --self-test   (a built conformant stream must walk clean and stay
//                                             recordable, and ten streams built to break one rule
//                                             each must be refused)
//
// --self-test needs no client, on purpose: every case is decided by the framing walk and the checks
// in front of the decoder, so CI can grade them without the client file or a network.  Two streams
// have to stay acceptable where the other ten have to be refused, because refusals alone cannot tell
// a strict check from one that rejects everything: the built stream, which is what catches a walk
// that has stopped accepting valid traffic, and a stream of 8-bit blocks whose codes vary by one,
// which is what catches a rule in front of the decoder that has stopped telling fill from a signal.
// Those rules are read as a function rather than through a decode, since the client they would need
// is not committed: unrecordable() is what a later change could quietly over-tighten, and it is
// graded against both streams.  The ten refusals pin the rules a later simplification could quietly
// loosen instead: random bytes, which stop on an
// unknown message type, a silence run over 2 s, a rate that changes mid-stream, audio in a message
// before the rate element, a block cut off, a stream with no rate element, a take that is one code
// throughout, a take that records nothing, a take of fill and silence, and bytes after the last
// message.
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawnSync } = require('child_process');

function fail(msg) {
  process.stderr.write('oracle: FAIL ' + msg + '\n');
  process.exit(1);
}

// ---- the wire format, as the client parses it ------------------------------------------------
//
// Element sizes are the client's own: 0xF0..0xFF S-meter 2 bytes, 128 is 129 bytes, 129/130 are 3,
// 131 is 2, 132 is a single silence marker worth 128 zeros, 133 and 135 are 7, 134 is 1.  Packed
// blocks (a 144..223 header, and any byte below 0x80 continuing the same format) carry 128
// run-length coded samples whose byte count depends on the data.  Measured on a real capture:
// 2464 8-bit blocks, 3 blocks from a 0x94..0xDF header and 4 from a sub-0x80 byte, so all three
// forms are ordinary traffic.  136..143 and 224..239 are not: the client silently skips them.
const Z = [999, 999, 8, 4, 2, 1, 99, 99];   // the client's run-length thresholds, by G

// How many bytes the client consumes for one packed block, header included.  This mirrors the
// client's loop and has to: the count depends on the data, so there is no formula for it.  Only
// the parts that move `m` and `a` are kept - the client also builds the samples out of `f`, which
// we do not need.  m is the bits of the header already consumed (4 for a 144..223 header, 1 for a
// sub-0x80 start); G, the sample width, carries over from the last 144..223 header.
function packedBytes(b, start, m, G) {
  // the client always reads four bytes ahead, so the last block of a message needs up to three
  // bytes it never uses; those read as zero here, exactly as undefined & 255 would there
  const at = (i) => (i < b.length ? b[i] : 0);
  let a = start, s = 0;
  while (s < 128) {
    let f = (((at(a) << 24) | (at(a + 1) << 16) | (at(a + 2) << 8) | at(a + 3)) << m);
    let e = 0;
    let r = 15 - G;
    if (f !== 0) while ((f & 0x80000000) === 0 && e < r) { f <<= 1; e++; }
    if (e < r) { r = e; e++; } else { r = (f >> 24) & 0xFF; e += 8; }
    let S = 0;
    if (r >= Z[G]) S++;
    if (r >= Z[G - 1]) S++;
    if (S > G - 1) S = G - 1;
    for (m += e + G - S; m >= 8; ) { a++; m -= 8; }
    s++;
  }
  if (m === 0) a--;
  return a + 1 - start;
}

// How a message's samples arrived, which is what decodeStream reads over the recorded frames:
// `varies` is set by anything carrying changes - a delta block, or an 8-bit block whose 128 bytes
// are not one code - and `kinds` counts the elements that hold one level for 128 samples, by the
// code they hold it at ('0xd5') or as the silence marker.  A take with nothing recorded except
// levels has no audio in it, which is the one thing the counts of samples and blocks cannot say.
const SILENCE = 'silence';
const codeKey = (v) => '0x' + v.toString(16).padStart(2, '0');

function arrivalList(kinds) {
  const parts = Array.from(kinds.entries())
    .sort((x, y) => (x[0] === y[0] ? 0 : x[0] < y[0] ? -1 : 1))
    .map(([kind, count]) => kind === SILENCE
      ? count + (count === 1 ? ' silence marker' : ' silence markers')
      : count + (count === 1 ? ' block of 128 copies of ' : ' blocks of 128 copies of ') + kind);
  // not reachable while the no-recorded-sample rule is checked first, but a complaint that named
  // nothing at all would read as 'undefined', so the fallback keeps it a sentence
  if (!parts.length) return 'no block and no marker';
  return parts.length > 1
    ? parts.slice(0, -1).join(', ') + ' and ' + parts[parts.length - 1]
    : parts[0];
}

// Walk one message against the format.  Returns null when the whole message is a sequence of known
// elements that ends exactly, or a string saying where it stopped being one.
function walkMessage(b, st, frame) {
  let a = 0;
  const n = b.length;
  const arrival = { varies: false, kinds: new Map() };
  st.arrival = arrival;
  while (a < n) {
    const t = b[a];
    if ((t & 0xF0) === 0xF0) {                       // S-meter: 0xF0/0xF1 + value
      if (a + 2 > n) return 'S-meter element cut off at byte ' + a;
      a += 2;
    } else if (t === 128) {                          // 128 samples, 8-bit companded
      if (a + 129 > n) return '8-bit block cut off at byte ' + a + ' (' + (n - a) + ' of 129 bytes)';
      st.blocks8++;
      st.samples += 128;
      st.silence = 0;
      // An empty band arrives as 8-bit blocks of one code repeated 128 times, which the client's
      // table maps to a single level: not audio, though it counts like it.
      let flat = b[a + 1];
      for (let i = 2; i < 129 && flat >= 0; i++) if (b[a + i] !== flat) flat = -1;
      if (flat < 0) arrival.varies = true;
      else arrival.kinds.set(codeKey(flat), (arrival.kinds.get(codeKey(flat)) || 0) + 1);
      a += 129;
    } else if (t < 128 || (t >= 144 && t <= 223)) {  // packed block, 128 samples
      const m = t < 128 ? 1 : 4;
      const G = t < 128 ? st.G : 14 - (t >> 4);
      if (!G) return 'packed block at byte ' + a + ' starts before any 144..223 header';
      const used = packedBytes(b, a, m, G);
      if (a + used > n) return 'packed block at byte ' + a + ' does not fit (' + (n - a) + ' bytes left)';
      st.G = G;
      st.blocks14++;
      st.samples += 128;
      st.silence = 0;
      arrival.varies = true;                  // a delta block carries changes, so it is audio
      a += used;
    } else if (t === 129) {                          // sample rate
      if (a + 3 > n) return 'rate element cut off at byte ' + a;
      const r = b[a + 1] * 256 + b[a + 2];
      if (st.rate && r !== st.rate) return 'sample rate changed mid-stream: ' + st.rate + ' -> ' + r;
      if (!st.rate) { st.rate = r; st.rateFrame = frame; }
      a += 3;
    } else if (t === 130) {
      if (a + 3 > n) return 'element 130 cut off at byte ' + a;
      a += 3;
    } else if (t === 131) {                          // stereo flag
      if (a + 2 > n) return 'stereo element cut off at byte ' + a;
      a += 2;
    } else if (t === 132) {                          // silence marker: one byte, 128 zeros
      st.silence++;
      st.samples += 128;
      if (st.silence * 128 > st.maxSilence) st.maxSilence = st.silence * 128;
      arrival.kinds.set(SILENCE, (arrival.kinds.get(SILENCE) || 0) + 1);
      a += 1;                                 // zeros hold a level too: a marker is not modulation
    } else if (t === 133 || t === 135) {             // true frequency / time
      if (a + 7 > n) return 'element ' + t + ' cut off at byte ' + a;
      a += 7;
    } else if (t === 134) {
      a += 1;
    } else {
      return 'unknown message type 0x' + t.toString(16) + ' at byte ' + a +
             ' (the client would invent samples from it or skip it silently)';
    }
  }
  return null;
}

// ---- the framing: the whole file, before the decoder sees any of it --------------------------

// The walk alone: known elements, message lengths that add up, nothing after the last message.  What
// a take has to have before it can be recorded at all - a rate element, and no silence run past 2 s -
// is checked in decodeStream, where those values are used.
function readStream(streamPath) {
  const raw = fs.readFileSync(streamPath);
  const st = { rate: 0, rateFrame: -1, samples: 0, blocks8: 0, blocks14: 0,
               silence: 0, maxSilence: 0, G: 0 };
  const frames = [];
  let off = 0;
  while (off + 4 <= raw.length) {
    const at = off;
    const len = raw.readUInt32BE(off);
    off += 4;
    if (len === 0 || off + len > raw.length) {
      fail('message ' + frames.length + ' at byte ' + at + ' claims ' + len + ' bytes with ' +
           (raw.length - off) + ' left');
    }
    const msg = raw.subarray(off, off + len);
    off += len;
    const samplesBefore = st.samples;
    const err = walkMessage(msg, st, frames.length);
    if (err) fail('message ' + frames.length + ' at byte ' + at + ': ' + err);
    frames.push({ body: msg, samplesBefore, arrival: st.arrival });
  }
  if (off !== raw.length) fail((raw.length - off) + ' trailing bytes after the last message');
  return { frames, st };
}

// ---- browser stubs ---------------------------------------------------------------------------
function loadDecoder(clientPath) {
  if (!fs.existsSync(clientPath)) {
    fail('cannot find the WebSDR client at ' + clientPath + '. It is not committed here: its licence ' +
         'allows it only to be served unmodified by the receiver that sent the stream, so save the ' +
         'script your receiver\'s page loads as websdr-sound.js beside this file, or pass --client');
  }
  const silentNode = () => ({ connect() {}, disconnect() {}, onaudioprocess: null });
  const ctx = {
    sampleRate: 48000,
    state: 'running',
    destination: {},
    createScriptProcessor: silentNode,
    createJavaScriptNode: silentNode,
    createConvolver: () => Object.assign(silentNode(), { normalize: true, buffer: null }),
    createBuffer: (ch, len) => ({ getChannelData: () => new Float32Array(len) }),
    createChannelMerger: () => Object.assign(silentNode(), {}),
    resume() {},
  };
  const el = () => ({ style: {}, innerHTML: '', textContent: '' });

  global.window = global;
  global.navigator = { userAgent: 'node-oracle' };
  global.document = { ct: ctx, getElementById: () => el(), createElement: () => el() };
  global.location = { host: 'oracle.local', href: 'http://oracle.local/' };

  // the client opens its own socket; we keep the instance and feed it the captured bytes ourselves
  global.WebSocket = class {
    constructor() { global.__oracleSock = this; this.binaryType = 'arraybuffer'; }
    send() {}
    close() {}
  };

  const src = fs.readFileSync(clientPath, 'utf8');
  (0, eval)(src);                                   // defines window.prep_html5sound and calls it
  const sock = global.__oracleSock;
  if (!sock) throw new Error('decoder did not open a stream');
  const app = global.soundapplet;
  if (!app || !app.rec_start) throw new Error('no soundapplet with a recorder');
  return { sock, app };
}

// What the recorder is handed: the message carrying the rate, the samples that share that message
// and are dropped with it, the frames the recorder gets, and how many samples that is.
function recordingSpan(st, frames) {
  const firstRate = st.rateFrame;
  const next = frames[firstRate + 1];
  const beforeRec = next ? next.samplesBefore : st.samples;
  return { firstRate: firstRate, beforeRec: beforeRec, earlier: frames[firstRate].samplesBefore,
           recorded: frames.slice(firstRate + 1), recordedSamples: st.samples - beforeRec };
}

// Whether a take could be recorded at all - a complaint, or null.  These rules live apart from the
// walk because the rate is what they are read against, and apart from decodeStream because the
// self-test has no client to load yet still has to show that they accept valid traffic: ten
// refusals would also pass a rule that refused everything, which is the same blind spot the built
// stream covers for the walk.  In order, because each is a different reason a take is worth nothing:
//
//   * with no rate element the client decodes at its 8000 Hz default and every count is wrong.  A
//     usable rate always leaves rateFrame on a message, which is what keeps the frame index below
//     off -1, where frames[] holds no message to read
//   * the client's recorder stops appending after 2 s of silence and truncates the take silently
//   * rec_start snapshots the client's current rate, that default until the rate element has been
//     processed, so audio in an earlier message could not have been recorded at the stream's rate
//   * a take with no recorded sample is the recorder being handed nothing at all
//   * audio that is one level is not audio.  A block of 128 copies of one code is the fill a server
//     sends for an empty band and a silence marker is 128 zeros, and each of them holds one level
//     for 128 samples: a take made only of those arrives, walks and accounts for its samples
//     perfectly while nothing in it varies, which is how a dead 10 MHz band came back as a -68 dBFS
//     noise floor on 19 November (2464 copies of 0xD5 are 8 of 32768, and an rms of them is that 8).
//     Only the frames after the rate one are read, because that is all the recorder gets.
function unrecordable(st, frames) {
  if (!st.rate) {
    return 'no usable sample-rate element: the decoder would fall back to its 8000 Hz default';
  }
  if (st.maxSilence > 2 * st.rate) {
    return 'the stream goes silent for ' + (st.maxSilence / st.rate).toFixed(2) + ' s (limit 2 s '
           + 'at ' + st.rate + ' Hz); the client\'s recorder stops appending after that and '
           + 'truncates the take without saying so';
  }
  const span = recordingSpan(st, frames);
  if (span.earlier) {
    return (span.earlier / st.rate).toFixed(2) + ' s of audio arrives in messages before the rate '
           + 'element, so it could not be recorded at the stream\'s ' + st.rate + ' Hz';
  }
  if (!span.recordedSamples) {
    return 'no audio in the take: no message after the rate element carries a sample, so the '
           + 'recorder gets nothing to record and the WAV would be its 44 byte header alone';
  }
  if (span.recorded.every((f) => !f.arrival.varies)) {
    const kinds = new Map();
    span.recorded.forEach((f) => f.arrival.kinds.forEach(
      (count, kind) => kinds.set(kind, (kinds.get(kind) || 0) + count)));
    return 'no audio in the take: its ' + span.recordedSamples + ' samples arrive as '
           + arrivalList(kinds) + ', and nothing in them varies; a block of 128 copies of one code '
           + 'and a silence marker each hold one level for 128 samples rather than carry a signal, '
           + 'so an rms of this take reads a level, not a noise floor';
  }
  return null;
}

function decodeStream(streamPath, outPath, clientPath) {
  const { frames, st } = readStream(streamPath);

  const complaint = unrecordable(st, frames);
  if (complaint) fail(complaint);

  // the message carrying the rate is fed first, so rec_start snapshots this stream's rate rather
  // than the client's default, and everything recorded is what arrives after it
  const span = recordingSpan(st, frames);
  const firstRate = span.firstRate;
  const beforeRec = span.beforeRec;
  const recordedSamples = span.recordedSamples;

  const { sock, app } = loadDecoder(clientPath);
  const feed = (from, to) => {
    for (let i = from; i < to; i++) {
      const b = frames[i].body;
      sock.onmessage({ data: b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength) });
    }
  };
  feed(0, firstRate + 1);
  app.rec_start();                                   // now the client's rate is the stream's
  feed(firstRate + 1, frames.length);
  const rec = app.rec_finish();

  // the decoder has to have accounted for exactly the samples the walk counted, at the rate the
  // stream declares - a mismatch means it consumed the bytes differently from the format
  const expected = recordedSamples;
  const got = Math.floor(rec.len / 2);
  if (rec.sr !== st.rate) {
    fail('the recorder took ' + rec.sr + ' Hz, the stream declares ' + st.rate + ' Hz');
  }
  if (got !== expected) {
    fail('the decoder produced ' + got + ' samples where the stream carries ' + expected +
         ' (' + (got - expected) + '); it did not consume the messages as the format says');
  }

  const body = Buffer.concat(rec.wavdata.map((b) => Buffer.from(b))).subarray(0, rec.len);
  const rate = rec.sr;
  const hdr = Buffer.alloc(44);
  hdr.write('RIFF', 0);
  hdr.writeUInt32LE(36 + body.length, 4);
  hdr.write('WAVEfmt ', 8);
  hdr.writeUInt32LE(16, 16);
  hdr.writeUInt16LE(1, 20);
  hdr.writeUInt16LE(1, 22);
  hdr.writeUInt32LE(rate, 24);
  hdr.writeUInt32LE(rate * 2, 28);
  hdr.writeUInt16LE(2, 32);
  hdr.writeUInt16LE(16, 34);
  hdr.write('data', 36);
  hdr.writeUInt32LE(body.length, 40);

  let rms = 0;
  const n = Math.floor(body.length / 2);
  for (let i = 0; i < n; i++) {
    const v = body.readInt16LE(i * 2) / 32768;
    rms += v * v;
  }
  rms = Math.sqrt(rms / Math.max(1, n));

  // everything that could still be wrong has been checked, so the file is written now and only now
  fs.writeFileSync(outPath, Buffer.concat([hdr, body]));
  process.stdout.write(
    'oracle: ' + frames.length + ' messages, ' + st.rate + ' Hz, ' + n + ' samples, ' +
    (n / rate).toFixed(2) + ' s, rms ' + rms.toFixed(4) + '\n' +
    '        blocks: ' + st.blocks8 + ' 8-bit, ' + st.blocks14 + ' packed, ' + st.silence + ' silence\n' +
    (beforeRec ? '        ' + beforeRec + ' samples (' + (1000 * beforeRec / st.rate).toFixed(0) +
      ' ms) shared the rate message and are not in the file\n' : ''));
}

// ---- self-test: a conformant stream must walk and stay recordable, and ten rules refuse --------

// A packed block is not arbitrary bytes: it is 128 samples at width G = 4, each with a zero run
// length and a zero mantissa, so the client spends one bit on the run and four on the mantissa per
// sample.  That is 5 bits a sample - 640 bits - plus the bits the header byte already carries, which
// is why the walk has to land exactly on the byte this puts there.  `head` is that header: 0xA8 is
// a 144..223 header (G = 14 - 0xA = 4) whose low nibble holds the first four bits, 0x40 is a
// sub-0x80 continuation whose top zero is the one bit already consumed.
function packedBlock(m, head) {
  const bits = [];
  for (let i = 0; i < m; i++) bits.push((head >> (7 - i)) & 1);
  for (let s = 0; s < 128; s++) bits.push(1, 0, 0, 0, 0);
  while (bits.length % 8) bits.push(0);
  const out = Buffer.alloc(bits.length / 8);
  bits.forEach((bit, i) => { if (bit) out[i >> 3] |= 0x80 >> (i & 7); });
  return out;
}

function streamMessage(...parts) {
  const body = Buffer.concat(parts.map((p) => (Buffer.isBuffer(p) ? p : Buffer.from(p))));
  const head = Buffer.alloc(4);
  head.writeUInt32BE(body.length, 0);
  return Buffer.concat([head, body]);
}

const rateElement = (rate) => [129, (rate >> 8) & 0xFF, rate & 0xFF];

function block8(code = 0) {
  const b = Buffer.alloc(129);
  b[0] = 128;
  b.fill(code, 1);
  return b;
}

// 128 samples that step between two adjacent codes: what real traffic looks like at the bottom of
// the 8-bit table, and the one shape a rule about constant levels could mistake for fill.
function block8Alternating(code = 0xD5) {
  const b = Buffer.alloc(129);
  b[0] = 128;
  for (let i = 1; i < 129; i++) b[i] = i % 2 ? code : code + 1;
  return b;
}

// A small stream the format accepts, written element by element: 7119 Hz, one 8-bit block, one
// packed block from a 144..223 header, one from a sub-0x80 byte, and one silence marker - 512
// samples, 3 messages.  The rate comes first and carries no audio, as the recorder needs.
function conformantStream() {
  return Buffer.concat([
    streamMessage(rateElement(7119), [130, 0, 0], [131, 1],
                  [133, 0, 0, 0, 0, 0, 0], [134], [135, 0, 0, 0, 0, 0, 0], [0xF0, 0x40]),
    streamMessage(block8(), packedBlock(4, 0xA8), packedBlock(1, 0x40)),
    streamMessage([132]),
  ]);
}

// A whole stream from its messages, each message written out as its elements.
function streamOf(...messages) {
  return Buffer.concat(messages.map((elements) => streamMessage(...elements)));
}

// 169 frames of the size a real take arrives in, filled from a fixed linear congruential sequence
// rather than Math.random, so the refusal is the same on every run and can be pinned to its reason.
// A fresh draw each time would move which element it trips on, and whichever came first could mask
// a rule that had stopped firing.
function randomStream() {
  const buf = Buffer.alloc(169 * (4 + 905));
  let x = 12345;
  let off = 0;
  for (let i = 0; i < 169; i++) {
    buf.writeUInt32BE(905, off);
    off += 4;
    for (let k = 0; k < 905; k++) {
      x = (Math.imul(x, 1103515245) + 12345) >>> 0;
      buf[off++] = (x >>> 16) & 0xFF;
    }
  }
  return buf;
}

// Write one stream, run the script on it the way a capture is run, and require no WAV, a nonzero
// exit, the oracle's own complaint and the reason that stream was built to hit.  A crash also exits
// nonzero, so the exit status alone would not carry the weight.  No --client is passed: the point is
// that these are refused before a client is looked for.
function expectRefusal(dir, name, buf, why) {
  const slug = name.replace(/[^a-z0-9]+/gi, '-');
  const stream = path.join(dir, slug + '.stream');
  const out = path.join(dir, slug + '.wav');
  fs.writeFileSync(stream, buf);
  const r = spawnSync(process.execPath, [__filename, stream, out], { encoding: 'utf8' });
  if (fs.existsSync(out)) fail(name + ': left a WAV behind');
  if (r.status === 0) fail(name + ': accepted (exit 0)');
  const said = (r.stderr || '').trim().split('\n')[0];
  const prefix = 'oracle: FAIL ';
  if (!said.startsWith(prefix)) fail(name + ': not refused by a format check (a crash?): ' + said);
  // the cases below match the text after the prefix, and match all of it
  if (!why.test(said.slice(prefix.length))) fail(name + ': refused for the wrong reason: ' + said);
  process.stdout.write('oracle:   ' + name + ': refused - ' + said + '\n');
}

function selfTest() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'oracle-selftest-'));
  process.on('exit', () => fs.rmSync(dir, { recursive: true, force: true }));
  const RATE = 7119;              // the rate the built streams declare and the walk reads them at

  // the stream above has to pass the walk with the counts it was built from.  readStream refuses
  // with fail(), which exits, so a message naming the element it stopped on is the failure report.
  const good = path.join(dir, 'conformant.stream');
  fs.writeFileSync(good, conformantStream());
  const walk = readStream(good);
  const got = Object.assign({ frames: walk.frames.length }, walk.st);
  const want = { frames: 3, rate: 7119, samples: 512, blocks8: 1, blocks14: 2, silence: 1 };
  for (const k of Object.keys(want)) {
    if (got[k] !== want[k]) {
      fail('the conformant self-test stream walked as ' + k + '=' + got[k] + ', not ' + want[k] +
           ': the framing walk now miscounts or refuses valid traffic');
    }
  }
  process.stdout.write(
    'oracle:   built stream walked: ' + walk.frames.length + ' messages, ' + walk.st.rate + ' Hz, ' +
    walk.st.samples + ' samples (' + walk.st.blocks8 + ' 8-bit, ' + walk.st.blocks14 + ' packed, ' +
    walk.st.silence + ' silence)\n');

  // ...and the rules in front of the decoder, which are not the walk, have to accept it too.  No
  // refusal case can show this: ten refusals would pass a rule that refused everything.  They are
  // graded without a client here, which is the reason unrecordable() is a function rather than a run
  // of fail() calls inside decodeStream.
  const loosened = unrecordable(walk.st, walk.frames);
  if (loosened) fail('the conformant self-test stream is no longer recordable: ' + loosened);
  process.stdout.write('oracle:   the same stream still passes the rules in front of the decoder\n');

  // and so does audio that only just varies: a take of blocks stepping between two adjacent codes is
  // recordable, or the rule above has become one that refuses modulation instead of one that refuses
  // fill.  Nothing in the refusals below could show this either way round.
  const varying = path.join(dir, 'varying-blocks.stream');
  fs.writeFileSync(varying, streamOf([rateElement(RATE)], [block8Alternating()],
                                     [block8Alternating()]));
  const audio = readStream(varying);
  const tooStrict = unrecordable(audio.st, audio.frames);
  if (tooStrict) fail('a take of blocks stepping between 0xd5 and 0xd6 is no longer recordable: ' +
                      tooStrict);
  process.stdout.write('oracle:   a take of blocks that vary by one code is recordable too\n');

  // One stream per refusal, each built to trip exactly one rule and be right everywhere else, with
  // the whole complaint it has to draw.  A refusal that still arrives but for another reason - or
  // from a rule that has moved behind loadDecoder(), where the missing client answers first - fails
  // on that line instead of passing on the exit status.  The 2 s run is the first whole count of
  // 128-zero markers over the rule; the random fill walks 281 bytes of the format before it reaches
  // a message type the client would have skipped silently.  The missing rate row is the decode's own
  // guard, not a twin of one in the walk: readStream hands back a walk with no rate in it, so
  // deleting that guard turns this row red instead of leaving the refusal to the walk.
  const refusals = [
    ['random bytes, on an unknown message type', randomStream(),
     /^message 0 at byte 0: unknown message type 0x8c at byte 281 \(the client would invent samples from it or skip it silently\)$/],
    ['the 2 s silence run',
     streamOf([rateElement(RATE)], [Array(Math.floor(2 * RATE / 128) + 1).fill(132)]),
     /^the stream goes silent for 2\.01 s \(limit 2 s at 7119 Hz\); the client's recorder stops appending after that and truncates the take without saying so$/],
    ['the rate change', streamOf([rateElement(RATE)], [block8()], [rateElement(8000)]),
     /^message 2 at byte 140: sample rate changed mid-stream: 7119 -> 8000$/],
    ['the early audio', streamOf([block8(), block8(), block8()], [rateElement(RATE)], [block8()]),
     /^0\.05 s of audio arrives in messages before the rate element, so it could not be recorded at the stream's 7119 Hz$/],
    ['the cut-off block', streamOf([rateElement(RATE)], [block8().subarray(0, 128)]),
     /^message 1 at byte 7: 8-bit block cut off at byte 0 \(128 of 129 bytes\)$/],
    ['the missing rate, at the decode', streamOf([block8()]),
     /^no usable sample-rate element: the decoder would fall back to its 8000 Hz default$/],
    ['the take with no audio in it', streamOf([rateElement(RATE)], [block8()], [block8()]),
     /^no audio in the take: its 256 samples arrive as 2 blocks of 128 copies of 0x00, and nothing in them varies; a block of 128 copies of one code and a silence marker each hold one level for 128 samples rather than carry a signal, so an rms of this take reads a level, not a noise floor$/],
    ['the take that records nothing', streamOf([rateElement(RATE), [0xF0, 0x40]], [[0xF1, 0x41]]),
     /^no audio in the take: no message after the rate element carries a sample, so the recorder gets nothing to record and the WAV would be its 44 byte header alone$/],
    ['the take of fill and silence',
     streamOf([rateElement(RATE)], [block8(0xD5)], [[132]], [block8(0xD4)]),
     /^no audio in the take: its 384 samples arrive as 1 block of 128 copies of 0xd4, 1 block of 128 copies of 0xd5 and 1 silence marker, and nothing in them varies; a block of 128 copies of one code and a silence marker each hold one level for 128 samples rather than carry a signal, so an rms of this take reads a level, not a noise floor$/],
    ['the trailing bytes', Buffer.concat([conformantStream(), Buffer.from([1, 2, 3])]),
     /^3 trailing bytes after the last message$/],
  ];
  for (const [name, buf, why] of refusals) expectRefusal(dir, name, buf, why);
  process.stdout.write('oracle: self-test ok - the built stream walks and is still recordable, '
                       + 'and ' + refusals.length + ' streams were refused for the reason each was '
                       + 'built to hit\n');
}

// ---- main ------------------------------------------------------------------------------------
if (process.argv[2] === '--self-test') {
  selfTest();
} else {
  const args = process.argv.slice(2);
  let clientPath = process.env.WEBSDR_CLIENT || path.join(__dirname, 'websdr-sound.js');
  const rest = [];
  for (let i = 0; i < args.length; i++) {
    if (args[i] === '--client') clientPath = args[++i];
    else rest.push(args[i]);
  }
  if (rest.length !== 2 || !rest[0] || !rest[1]) {
    process.stderr.write('usage: node decode_oracle.js <stream-file> <out.wav> [--client <websdr-sound.js>]\n' +
                         '       node decode_oracle.js --self-test\n');
    process.exit(2);
  }
  decodeStream(rest[0], rest[1], clientPath);
}
