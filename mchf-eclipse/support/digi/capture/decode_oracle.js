// Oracle: decode a captured WebSDR .stream with the server's own client code.
//
// websdr-sound.js is the decoder the browser runs.  We load it unchanged, stub just enough of the
// browser around it, and use its recorder start/finish calls - so the samples come from exactly the
// code path that produces a WAV in the page, not from a reimplementation.
//
// It refuses rather than guesses.  The client's message loop has no error case: a byte below 0x80
// falls through to `else if (128 != (b[a] & 128)) m = 1, s = 2` and the sample loop then invents
// samples out of whatever follows, so any byte soup "decodes".  A WAV appearing therefore proves
// nothing, so before the decoder is even loaded the file has to satisfy four things:
//
//   * every message is a sequence of known elements that ends exactly on its last byte
//   * the sample rate comes from the stream's rate element, and never changes
//   * no run of silence markers lasts longer than 2 s (the client's recorder stops appending there)
//   * no audio arrives in a message before the rate element (it could not be recorded at that rate)
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
//        node decode_oracle.js --self-test   (a built conformant stream must walk clean, and seven
//                                             streams built to break one rule each must be refused)
//
// --self-test needs no client, on purpose: every case is decided by the framing walk and the checks
// in front of the decoder, so CI can grade them without the client file or a network.  The
// conformant case is the one that catches a walk which has stopped accepting valid traffic - a
// refusal test alone cannot tell a strict walk from one that rejects everything - and the seven
// refusals pin the rules a later simplification could quietly loosen: random bytes, which stop on an
// unknown message type, a silence run over 2 s, a rate that changes mid-stream, audio in a message
// before the rate element, a block cut off, a stream with no rate element, and bytes after the last
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

// Walk one message against the format.  Returns null when the whole message is a sequence of known
// elements that ends exactly, or a string saying where it stopped being one.
function walkMessage(b, st, frame) {
  let a = 0;
  const n = b.length;
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
      a += 1;
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

// ---- the whole file, before the decoder sees any of it ---------------------------------------

// A stream that never declares a rate would be decoded at the client's 8000 Hz default and every
// sample count would be wrong, so both the walk and the decode refuse it - with the same words, so
// the rule cannot drift between them.
const NO_RATE = 'no usable sample-rate element: the decoder would fall back to its 8000 Hz default';

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
    frames.push({ body: msg, samplesBefore });
  }
  if (off !== raw.length) fail((raw.length - off) + ' trailing bytes after the last message');
  if (!st.rate) fail(NO_RATE);
  if (st.maxSilence > 2 * st.rate) {
    fail('the stream goes silent for ' + (st.maxSilence / st.rate).toFixed(2) + ' s (limit 2 s at ' +
         st.rate + ' Hz); the client\'s recorder stops appending after that and truncates the take ' +
         'without saying so');
  }
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

function decodeStream(streamPath, outPath, clientPath) {
  const { frames, st } = readStream(streamPath);

  // readStream refuses a stream with no rate element, and this makes the same refusal rather than
  // relying on that order: the two lines below index the messages by the frame the rate was found in,
  // and rateFrame is -1 when there was none, where there is no message and .samplesBefore throws.
  if (st.rateFrame < 0) fail(NO_RATE);

  // rec_start snapshots the client's current rate, which is its 8000 Hz default until the rate
  // element has been processed, so the frame carrying the rate is fed first and recording starts
  // after it.  Anything in an earlier message would be lost: that is a refusal.
  const firstRate = st.rateFrame;
  const after = (i) => (i + 1 < frames.length ? frames[i + 1].samplesBefore : st.samples);
  const beforeRec = after(firstRate);
  const earlier = frames[firstRate].samplesBefore;
  if (earlier) {
    fail((earlier / st.rate).toFixed(2) + ' s of audio arrives in messages before the rate ' +
         'element, so it could not be recorded at the stream\'s ' + st.rate + ' Hz');
  }

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
  const expected = st.samples - beforeRec;
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

// ---- self-test: a conformant stream must walk, and seven rules must still refuse -------------

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

function block8() {
  const b = Buffer.alloc(129);
  b[0] = 128;
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

  // One stream per refusal, each built to trip exactly one rule and be right everywhere else, with
  // the whole complaint it has to draw.  A refusal that still arrives but for another reason - or
  // from a rule that has moved behind loadDecoder(), where the missing client answers first - fails
  // on that line instead of passing on the exit status.  The 2 s run is the first whole count of
  // 128-zero markers over the rule; the random fill walks 281 bytes of the format before it reaches
  // a message type the client would have skipped silently.
  const RATE = 7119;
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
    ['the missing rate', streamOf([block8()]),
     /^no usable sample-rate element: the decoder would fall back to its 8000 Hz default$/],
    ['the trailing bytes', Buffer.concat([conformantStream(), Buffer.from([1, 2, 3])]),
     /^3 trailing bytes after the last message$/],
  ];
  for (const [name, buf, why] of refusals) expectRefusal(dir, name, buf, why);
  process.stdout.write('oracle: self-test ok - the built stream walks, and ' + refusals.length +
                       ' streams were refused for the reason each was built to hit\n');
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
