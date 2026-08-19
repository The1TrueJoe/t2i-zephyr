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
wr(Buffer.from("2")); pump(2500);                       // leave bootloader, run the NCP
wr(Buffer.from([0x1a, 0xc0, 0x38, 0xbc, 0x7e])); pump(1500);   // ASH reset

acc = Buffer.alloc(0); txSeq = 0; rxSeq = 0;

// ---- EZSP coordinator bring-up: form a network and open permit-join. ----
// The T2i's EM250 runs an end-device image, so it can only ever JOIN. Nothing
// else on the CA-1 uses this radio (no zigbee stack in /etc/init.d), so forming
// here disturbs nothing; the busy channel-11 traffic is other hardware.
const CHAN = 15, PANID = 0x0035;   // low byte 0x35 = the pan_lo the T2i sends
const EPAN = [0x54, 0x32, 0x69, 0x00, 0x00, 0x00, 0x00, 0x01];
const HA_KEY = Buffer.from("ZigBeeAlliance09", "ascii");
const NET_KEY = Buffer.from("t2i-zephyr-key01", "ascii");

let seq = 0;
function ezsp(frameId, params, tag, ms) {
  const p = Buffer.concat([Buffer.from([seq++ & 0xff, 0x00, frameId]),
                           Buffer.from(params || [])]);
  wr(ashData(p));
  const fr = pump(ms || 2000);
  for (const f of fr) {
    if (f.length < 3 || (f[0] & 0x80)) continue;
    rxSeq = ((((f[0] >> 4) & 7) + 1) & 7); wr(ashAck());
    const r = xr(f.slice(1, f.length - 2));
    if (r[2] === frameId) { console.log(tag + ":", r.slice(3).toString("hex")); return r.slice(3); }
    console.log(tag + " (async " + r[2].toString(16) + "):", r.slice(3).toString("hex"));
  }
  console.log(tag + ": <no reply>");
  return null;
}
const u16 = (v) => [v & 0xff, (v >> 8) & 0xff];
const u32 = (v) => [v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >>> 24) & 0xff];

ezsp(0x00, [0x04], "version");
// Stack profile 2 (ZigBee PRO) and security level 5 must be set before forming.
ezsp(0x53, [0x0C, ...u16(2)], "cfg stackProfile=2");
ezsp(0x53, [0x0D, ...u16(5)], "cfg securityLevel=5");
ezsp(0x53, [0x05, ...u16(8)], "cfg addressTableSize=8");

// bitmask: HAVE_NETWORK_KEY|HAVE_PRECONFIGURED_KEY|TRUST_CENTER_GLOBAL_LINK_KEY
ezsp(0x68, [...u16(0x0304), ...HA_KEY, ...NET_KEY, 0x00,
            0, 0, 0, 0, 0, 0, 0, 0], "setInitialSecurityState");

ezsp(0x20, [], "leaveNetwork", 3000);
pump(1500);

// EmberNetworkParameters: epan[8], panId u16, txPower i8, channel u8,
// joinMethod u8, nwkManagerId u16, nwkUpdateId u8, channels u32
ezsp(0x1E, [...EPAN, ...u16(PANID), 0x08, CHAN, 0x00,
            ...u16(0), 0x00, ...u32(1 << CHAN)], "formNetwork", 6000);
pump(3000);

const np = ezsp(0x28, [], "getNetworkParameters", 3000);
if (np && np.length >= 12) {
  console.log("  status      =", np[0].toString(16));
  console.log("  nodeType    =", np[1]);
  console.log("  extendedPan =", Buffer.from(np.slice(2, 10)).toString("hex"));
  console.log("  panId       = 0x" + (np[10] | (np[11] << 8)).toString(16));
  console.log("  channel     =", np[13]);
}
ezsp(0x22, [0xFF], "permitJoining(forever)", 3000);
console.log("permit-join open; listening for joins...");
const until = Date.now() + 180000;
while (Date.now() < until) handle(pump(1000), "evt");
fs.closeSync(fd);
