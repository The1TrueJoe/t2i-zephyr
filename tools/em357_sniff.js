// EM357 NCP as a raw 802.15.4 sniffer, via EZSP mfglib over ASH.
// Usage: node sniff.js <channel> <seconds>
const fs = require("fs");
const DEV = "/dev/ttymxc4";
const FLAG = 0x7e, ESC = 0x7d, XON = 0x11, XOFF = 0x13, SUB = 0x18, CANB = 0x1a;
let CH = 11; const SECS = 3;

const sleep = (ms) => { const e = Date.now() + ms; while (Date.now() < e) {} };
function crc16(b) { let c = 0xffff;
  for (const x of b) { c ^= x << 8;
    for (let i = 0; i < 8; i++) c = (c & 0x8000) ? ((c << 1) ^ 0x1021) & 0xffff : (c << 1) & 0xffff; }
  return c; }
const needsEsc = (b) => [FLAG, ESC, XON, XOFF, SUB, CANB].includes(b);
const stuff = (b) => Buffer.from([].concat(...[...b].map(v => needsEsc(v) ? [ESC, v ^ 0x20] : [v])));
function unstuff(b) { const o = []; let e = false;
  for (const v of b) { if (v === ESC) { e = true; continue; } o.push(e ? v ^ 0x20 : v); e = false; }
  return Buffer.from(o); }
function randmask(n) { const o = []; let r = 0x42;
  for (let i = 0; i < n; i++) { o.push(r); r = (r & 1) ? (r >> 1) ^ 0xb8 : r >> 1; } return o; }
const xr = (b) => { const m = randmask(b.length); return Buffer.from(b.map((v, i) => v ^ m[i])); };

const fd = fs.openSync(DEV, "r+");
const rb = Buffer.alloc(1024);
let acc = Buffer.alloc(0);
const wr = (b) => fs.writeSync(fd, b, 0, b.length, null);
function pump(ms) {                       // returns array of complete unstuffed ASH frames
  const end = Date.now() + ms; const out = [];
  while (Date.now() < end) {
    let n = 0; try { n = fs.readSync(fd, rb, 0, rb.length, null); } catch (e) { n = 0; }
    if (n > 0) acc = Buffer.concat([acc, rb.slice(0, n)]); else { sleep(3); continue; }
    let i;
    while ((i = acc.indexOf(FLAG)) >= 0) {
      const raw = acc.slice(0, i); acc = acc.slice(i + 1);
      if (raw.length) out.push(unstuff(raw));
    }
  }
  return out;
}
let txSeq = 0, rxSeq = 0;
function ashData(payload) {
  const ctrl = (txSeq << 4) | rxSeq; txSeq = (txSeq + 1) & 7;
  const body = Buffer.concat([Buffer.from([ctrl]), xr(payload)]);
  const c = crc16(body);
  return Buffer.concat([stuff(Buffer.concat([body, Buffer.from([c >> 8 & 0xff, c & 0xff])])), Buffer.from([FLAG])]);
}
const ashAck = () => { const c = crc16(Buffer.from([0x80 | rxSeq]));
  return Buffer.concat([stuff(Buffer.from([0x80 | rxSeq, c >> 8 & 0xff, c & 0xff])), Buffer.from([FLAG])]); };

function handle(frames, tag) {
  for (const f of frames) {
    if (f.length < 3) continue;
    const ctrl = f[0];
    if ((ctrl & 0x80) === 0) {                       // DATA
      rxSeq = (((ctrl >> 4) & 7) + 1) & 7;
      wr(ashAck());
      const p = xr(f.slice(1, f.length - 2));
      const fid = p[2];
      if (fid === 0x8e) {                            // mfglibRxHandler
        const len = p[3];
        console.log("PACKET ch" + CH + " len=" + len + " lqi=" + p[4] + " rssi=" + (p[5] << 24 >> 24) +
                    "  " + p.slice(6).toString("hex"));
      } else if (tag) console.log(tag + ":", p.toString("hex"));
    }
  }
}

// one open: bootloader -> run app -> ASH reset -> EZSP
wr(Buffer.from("\r\n")); pump(1000);
wr(Buffer.from("2")); pump(2500);
wr(Buffer.from([0x1a, 0xc0, 0x38, 0xbc, 0x7e])); pump(1500);
acc = Buffer.alloc(0); txSeq = 0; rxSeq = 0;

wr(ashData(Buffer.from([0x00, 0x00, 0x00, 0x04])));            handle(pump(1500), "version");
wr(ashData(Buffer.from([0x01, 0x00, 0x83, 0x01])));            handle(pump(1500), "mfglibStart");
wr(ashData(Buffer.from([0x02, 0x00, 0x8a, CH])));              handle(pump(1500), "setChannel");
console.log("sniffing channel " + CH + " for " + SECS + "s ...");
const end = Date.now() + SECS * 1000;
while (Date.now() < end) handle(pump(1000));
wr(ashData(Buffer.from([0x03, 0x00, 0x84])));                  handle(pump(800), "mfglibEnd");
fs.closeSync(fd);
