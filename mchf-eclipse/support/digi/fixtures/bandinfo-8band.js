// The band table one WebSDR front end serves as tmp/bandinfo.js, copied from NA5B's receiver on
// 21 November 2026 - the eight bands whose spans support/digi/README.md documents, and the file
// capture/websdr_capture.py --self-test reads.  Only the scaleimgs pyramids are trimmed, to one
// row per band: the keys, their order, their spacing and the number formatting are what the
// receiver sends, because that is what the parser has to survive.  A band whose entry the parser
// skips moves every index after it, and the tune that follows sits the front end on a band nobody
// asked for, so this file is graded as the receiver's own rather than as a tidy stand-in.
var nbands=8;
var ini_freq=1500.000000;
var ini_mode='am';
var chseq=2;
var bandinfo= [
{ centerfreq: 1024.000000,
    samplerate: 2048.000000,
    tuningstep: 0.031250,
    maxlinbw: 8.000000,
    vfo: 1034.000000,
    maxzoom: 6,
    name: 'MW-160',
    scaleimgs: [
      ["tmp/0-b0z0i0.png"]
    ]
  },
{ centerfreq: 4224.000000,
    samplerate: 2048.000000,
    tuningstep: 0.031250,
    maxlinbw: 8.000000,
    vfo: 4234.000000,
    maxzoom: 6,
    name: '80m',
    scaleimgs: [
      ["tmp/0-b1z0i0.png"]
    ]
  },
{ centerfreq: 6348.000000,
    samplerate: 2048.000000,
    tuningstep: 0.031250,
    maxlinbw: 8.000000,
    vfo: 6358.000000,
    maxzoom: 6,
    name: '40m',
    scaleimgs: [
      ["tmp/0-b2z0i0.png"]
    ]
  },
{ centerfreq: 9849.000000,
    samplerate: 2048.000000,
    tuningstep: 0.031250,
    maxlinbw: 8.000000,
    vfo: 9859.000000,
    maxzoom: 6,
    name: '30m',
    scaleimgs: [
      ["tmp/0-b3z0i0.png"]
    ]
  },
{ centerfreq: 12623.000000,
    samplerate: 2048.000000,
    tuningstep: 0.031250,
    maxlinbw: 8.000000,
    vfo: 12633.000000,
    maxzoom: 6,
    name: '25m',
    scaleimgs: [
      ["tmp/0-b4z0i0.png"]
    ]
  },
{ centerfreq: 14679.000000,
    samplerate: 2048.000000,
    tuningstep: 0.031250,
    maxlinbw: 8.000000,
    vfo: 14689.000000,
    maxzoom: 6,
    name: '20m',
    scaleimgs: [
      ["tmp/0-b5z0i0.png"]
    ]
  },
{ centerfreq: 21400.000000,
    samplerate: 1024.000000,
    tuningstep: 0.031250,
    maxlinbw: 8.000000,
    vfo: 21410.000000,
    maxzoom: 5,
    name: '15m',
    scaleimgs: [
      ["tmp/0-b6z0i0.png"]
    ]
  },
{ centerfreq: 27985.000000,
    samplerate: 2048.000000,
    tuningstep: 0.031250,
    maxlinbw: 8.000000,
    vfo: 27995.000000,
    maxzoom: 6,
    name: '10m',
    scaleimgs: [
      ["tmp/0-b7z0i0.png"]
    ]
  }
];

var dxinfoavailable=1;
var idletimeout=0;
var has_mobile=1;
