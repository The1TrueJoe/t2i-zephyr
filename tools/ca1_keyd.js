// Receives T2i button events on the CA-1.
//
// The remote's radio cannot join a network we control (RTI's EM250 demands a preconfigured link
// key that is not in the host, not in the tool, and not readable out of the radio), so the RF hop
// is not available. This is the transport that works today: the remote's USB CDC is forwarded here
// over TCP. The line format is exactly what the firmware emits and what the Juno remote driver
// parses, so swapping in a real ZigBee hop later changes nothing above this line.
const net = require("net"), fs = require("fs");
const PORT = Number(process.argv[2] || 9099);
const LOG = "/tmp/t2i-keys.log";

const stamp = () => new Date().toISOString().substr(11, 12);

net.createServer((sock) => {
  const who = sock.remoteAddress;
  console.log(`${stamp()} connected: ${who}`);
  let buf = "";
  sock.on("data", (d) => {
    buf += d.toString("utf8");
    let i;
    while ((i = buf.indexOf("\n")) >= 0) {
      const line = buf.slice(0, i).trim();
      buf = buf.slice(i + 1);
      if (!line) continue;
      const out = `${stamp()} ${line}`;
      console.log(out);
      fs.appendFileSync(LOG, out + "\n");
    }
  });
  sock.on("close", () => console.log(`${stamp()} disconnected: ${who}`));
  sock.on("error", (e) => console.log(`${stamp()} error: ${e.message}`));
}).listen(PORT, () => console.log(`${stamp()} t2i keyd listening on ${PORT}, logging to ${LOG}`));
