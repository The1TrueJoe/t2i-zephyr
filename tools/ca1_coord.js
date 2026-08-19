// CA-1 EM357 as a ZigBee coordinator for the Telegesis remote to join.
//
// We control both ends' keys, so the RTI key problem is gone: the coordinator forms with a known
// network key and a known trust-centre link key, and the Telegesis radio joins with the same link
// key. Then it unicasts button data here and we see it via incomingMessageHandler.
//
// Usage: node ca1_coord.js [channel] [panid] [seconds]
// Leaves the network up and prints every join and message until it exits.
const fs = require("fs");
const FLAG = 0x7e, ESC = 0x7d, XON = 0x11, XOFF = 0x13, SUB = 0x18, CANB = 0x1a;
const CHAN = Number(process.argv[2] || 15);
const PANID = Number(process.argv[3] || 0x1a2b);
const SECS = Number(process.argv[4] || 300);
const EPAN = [0x54, 0x32, 0x69, 0x00, 0x00, 0x00, 0x00, 0x01];
// Shared secrets. The Telegesis side is set to match these (link key = TCLK, and the coordinator
// hands it the network key during join).
const TCLK = [...Buffer.from("ZigBeeAlliance09", "ascii")];   // trust-centre link key
const NWKK = [...Buffer.from("t2iJunoNetKey001", "ascii")];   // network key

const sleep = (ms) => { const e = Date.now() + ms; while (Date.now() < e) {} };
function crc16(b) { let c = 0xffff; for (const x of b) { c ^= x << 8; for (let i = 0; i < 8; i++) c = (c & 0x8000) ? ((c << 1) ^ 0x1021) & 0xffff : (c << 1) & 0xffff; } return c; }
const needsEsc = (b) => [FLAG, ESC, XON, XOFF, SUB, CANB].includes(b);
const stuff = (b) => Buffer.from([].concat(...[...b].map(v => needsEsc(v) ? [ESC, v ^ 0x20] : [v])));
function unstuff(b) { const o = []; let e = false; for (const v of b) { if (v === ESC) { e = true; continue; } o.push(e ? v ^ 0x20 : v); e = false; } return Buffer.from(o); }
function randmask(n) { const o = []; let r = 0x42; for (let i = 0; i < n; i++) { o.push(r); r = (r & 1) ? (r >> 1) ^ 0xb8 : r >> 1; } return o; }
const xr = (b) => { const m = randmask(b.length); return Buffer.from(b.map((v, i) => v ^ m[i])); };
const stamp = () => new Date().toISOString().substr(11, 12);

// Remote key code -> remote-contract command, mirroring juno-driver/src/lib.rs. This is the
// "button press -> zigbee command" resolution, done here on the CA-1 as the driver would.
const KEYMAP = {
  128: "exit", 129: "mute_toggle", 131: "up", 132: "left", 133: "right", 134: "down",
  135: "select", 138: "volume_up", 139: "volume_down", 140: "channel_up", 141: "channel_down",
  142: "guide", 143: "menu", 144: "info", 145: "power_off", 146: "play", 147: "pause",
  148: "stop", 149: "record", 150: "scan_reverse", 151: "scan_forward", 152: "skip_back",
  153: "skip_forward", 165: "enter", 171: "power_on",
};

const fd = fs.openSync("/dev/ttymxc4", "r+");
const rb = Buffer.alloc(1024); let acc = Buffer.alloc(0);
const wr = (b) => fs.writeSync(fd, b, 0, b.length, null);
function pump(ms) {
  const end = Date.now() + ms; const out = [];
  while (Date.now() < end) {
    let n = 0; try { n = fs.readSync(fd, rb, 0, rb.length, null); } catch (e) { n = 0; }
    if (n > 0) acc = Buffer.concat([acc, rb.slice(0, n)]); else { sleep(2); continue; }
    let i; while ((i = acc.indexOf(FLAG)) >= 0) { const raw = acc.slice(0, i); acc = acc.slice(i + 1); if (raw.length) out.push(unstuff(raw)); }
  }
  return out;
}
let txSeq = 0, rxSeq = 0;
function ashData(p) { const ctrl = (txSeq << 4) | rxSeq; txSeq = (txSeq + 1) & 7; const body = Buffer.concat([Buffer.from([ctrl]), xr(p)]); const c = crc16(body); return Buffer.concat([stuff(Buffer.concat([body, Buffer.from([c >> 8 & 0xff, c & 0xff])])), Buffer.from([FLAG])]); }
const ashAck = () => { const c = crc16(Buffer.from([0x80 | rxSeq])); return Buffer.concat([stuff(Buffer.from([0x80 | rxSeq, c >> 8 & 0xff, c & 0xff])), Buffer.from([FLAG])]); };
const u16 = (v) => [v & 0xff, (v >> 8) & 0xff];
const u32 = (v) => [v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >>> 24) & 0xff];

let seq = 0;
function ezsp(frameId, params, tag, ms) {
  const p = Buffer.concat([Buffer.from([seq++ & 0xff, 0x00, frameId]), Buffer.from(params || [])]);
  wr(ashData(p));
  const fr = pump(ms || 2000);
  let ret = null;
  for (const f of fr) {
    if (f.length < 3 || (f[0] & 0x80)) continue;
    rxSeq = (((f[0] >> 4) & 7) + 1) & 7; wr(ashAck());
    const r = xr(f.slice(1, f.length - 2));
    if (r[2] === frameId && ret === null) { ret = r.slice(3); if (tag) console.log(`${stamp()} ${tag}: ${ret.toString("hex")}`); }
    else { decodeCallback(r); }
  }
  if (ret === null && tag) console.log(`${stamp()} ${tag}: <no reply>`);
  return ret;
}

// Decode the async callbacks that matter: a node joining, and an incoming message.
function decodeCallback(r) {
  const id = r[2], p = r.slice(3);
  if (id === 0x24) {                                   // trustCenterJoinHandler
    const nodeId = p[0] | (p[1] << 8);
    const eui = Buffer.from(p.slice(2, 10)).reverse().toString("hex");
    console.log(`${stamp()} >>> NODE JOINED nodeId=0x${nodeId.toString(16)} eui=${eui} status=${p[10]} decision=${p[11]}`);
  } else if (id === 0x19) {                            // childJoinHandler
    console.log(`${stamp()} >>> childJoin ${p.toString("hex")}`);
  } else if (id === 0x45) {                            // incomingMessageHandler
    // p[0]=type, apsFrame = p[1..2]profile p[3..4]cluster p[5]srcEp p[6]dstEp p[7..8]opts
    // p[9..10]group p[11]seq, then p[12]lqi p[13]rssi p[14..15]sender p[16]bind p[17]addr
    // p[18]len p[19..]message
    const profile = p[1] | (p[2] << 8), cluster = p[3] | (p[4] << 8);
    const srcEp = p[5];
    const sender = p[14] | (p[15] << 8);
    const mlen = p[18];
    const msg = p.slice(19, 19 + mlen);
    const text = msg.toString("ascii").replace(/[^\x20-\x7e]/g, ".");
    const m = text.match(/^K(\d+)/);
    if (m) {
      const code = Number(m[1]);
      const cmd = KEYMAP[code] || `custom(${code})`;
      console.log(`${stamp()} *** BUTTON from 0x${sender.toString(16)}: code ${code} "${text}"  ->  remote::${cmd}`);
    } else {
      console.log(`${stamp()} *** MESSAGE from 0x${sender.toString(16)} ep=${srcEp} : ${msg.toString("hex")} "${text}"`);
    }
  } else if (id === 0x0f || id === 0x62) {
    // stackStatusHandler(0x19?) etc — ignore noise
  }
}

// one open: bootloader -> run app -> ASH reset -> EZSP
wr(Buffer.from("\r\n")); pump(1000);
wr(Buffer.from("2")); pump(2500);
wr(Buffer.from([0x1a, 0xc0, 0x38, 0xbc, 0x7e])); pump(1500);
acc = Buffer.alloc(0); txSeq = 0; rxSeq = 0;

ezsp(0x00, [0x04], "version");
ezsp(0x53, [0x0C, ...u16(2)], "cfg stackProfile=2");
ezsp(0x53, [0x0D, ...u16(5)], "cfg securityLevel=5");
ezsp(0x53, [0x05, ...u16(16)], "cfg addressTableSize");
// register a wildcard-ish endpoint so unicasts are delivered up. Telegesis default profile is
// 0xC091; also register the HA profile 0x0104. addEndpoint(ep, profile, deviceId, ver, inClusters, outClusters)
ezsp(0x02, [0x01, ...u16(0xC091), ...u16(0x0000), 0x00, 0x01, 0x01, ...u16(0x0001), ...u16(0x0001)], "addEndpoint C091");

// HAVE_NETWORK_KEY|HAVE_PRECONFIGURED_KEY|TRUST_CENTER_GLOBAL_LINK_KEY = 0x0304. The trust centre
// encrypts the network key with the preconfigured link key (ZigBeeAlliance09) during join, which
// is the HA standard the Telegesis expects (it rejects clear-text keys in secure mode).
ezsp(0x68, [...u16(0x0304), ...TCLK, ...NWKK, 0x00, 0,0,0,0,0,0,0,0], "setInitialSecurityState");
ezsp(0x20, [], "leaveNetwork", 3000); pump(1500);
// form: EmberNetworkParameters
ezsp(0x1E, [...EPAN, ...u16(PANID), 0x08, CHAN, 0x00, ...u16(0), 0x00, ...u32(1 << CHAN)], "formNetwork", 6000);
pump(3000);
const np = ezsp(0x28, [], "getNetworkParameters", 3000);
if (np && np.length >= 14) {
  console.log(`  status=${np[0].toString(16)} channel=${np[13]} pan=0x${(np[10] | (np[11] << 8)).toString(16)} epan=${Buffer.from(np.slice(2,10)).toString("hex")}`);
}
// Encrypt the key with the preconfigured link key (decision 1), and allow the joiner to request
// the TC link key. Policies are set AFTER forming (formNetwork resets them). Read back to confirm.
ezsp(0x55, [0x00, 0x01], "setPolicy tc=usePreconfigured");
ezsp(0x55, [0x05, 0x51], "setPolicy tcKeyRequest=allow");
const gp = ezsp(0x56, [0x00], "getPolicy tc");
if (gp) console.log(`  tc policy readback = 0x${gp[1].toString(16)}`);
ezsp(0x22, [0xFF], "permitJoining(forever)");
console.log(`${stamp()} coordinator up on channel ${CHAN}, pan 0x${PANID.toString(16)}; listening...`);
const until = Date.now() + SECS * 1000;
while (Date.now() < until) { for (const f of pump(1000)) { if (f.length >= 3 && !(f[0] & 0x80)) { rxSeq = (((f[0] >> 4) & 7) + 1) & 7; wr(ashAck()); decodeCallback(xr(f.slice(1, f.length - 2))); } } }
ezsp(0x22, [0x00], "permitJoining(off)");
fs.closeSync(fd);
