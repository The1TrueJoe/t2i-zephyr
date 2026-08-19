#!/usr/bin/env python3
"""Drive the firmware's diagnostic USB commands (0x90 AT passthrough, 0x91 SPI read, 0x92 SPI
search). Replies come back as normal telemetry text lines on the same CDC.

    ./t2i_diag.py at "ATI"                 # forward an AT command to the radio, print the reply
    ./t2i_diag.py read 0x01F84000 64       # hex-dump a SPI region
    ./t2i_diag.py search                   # scan all 32MB of SPI for the "xap2b" EM250 signature
"""
import glob
import struct
import sys
import time

import serial

FRAME = 64


def frame(cmd, body=b""):
    b = bytearray(FRAME)
    b[0] = cmd
    b[1:1 + len(body)] = body[:FRAME - 1]
    return bytes(b)


def port():
    p = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not p:
        sys.exit("no /dev/cu.usbmodem* — is a unit attached?")
    return p[0]


def pump(s, prefixes, done_substr, timeout):
    """Print lines whose text contains any prefix; stop on done_substr or timeout."""
    t = time.time()
    while time.time() - t < timeout:
        try:
            l = s.readline()
        except serial.SerialException:
            time.sleep(0.5)
            continue
        if not l:
            continue
        d = l.decode("utf-8", "replace").strip()
        if any(x in d for x in prefixes):
            print(d, flush=True)
        if done_substr and done_substr in d:
            return


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    s = serial.Serial(port(), 115200, timeout=1)
    cmd = sys.argv[1]

    if cmd == "at":
        at = sys.argv[2].encode()
        s.write(frame(0x90, bytes([len(at)]) + at))
        pump(s, ["ATPASS"], "ATPASS", 5)
    elif cmd == "read":
        addr = int(sys.argv[2], 0)
        ln = int(sys.argv[3], 0)
        s.write(frame(0x91, struct.pack("<IH", addr, ln)))
        pump(s, ["SPI "], "SPI read done", 30)
    elif cmd == "search":
        s.write(frame(0x92))
        pump(s, ["SPI HIT", "SPI search", "SPI .."], "search done", 300)
    else:
        sys.exit(f"unknown command: {cmd}")
    s.close()


if __name__ == "__main__":
    main()
