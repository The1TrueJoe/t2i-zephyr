#!/usr/bin/env python3
"""Forward T2i button events from the remote's USB CDC to the CA-1.

The ZigBee hop is unavailable (see docs/ZIGBEE-PROTOCOL.md: the EM250 requires a preconfigured
link key that cannot be supplied or extracted), so this carries the same event lines over USB and
TCP instead. Format is unchanged from the firmware, so the Juno remote driver consumes it either
way and a future RF hop is a drop-in replacement.

    ./t2i_ca1_bridge.py --host 192.168.1.178 [--port 9099] [--serial /dev/cu.usbmodemXXXX]
"""
import argparse
import glob
import socket
import sys
import time

import serial


def find_port() -> str:
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        sys.exit("no /dev/cu.usbmodem* found - is the remote plugged in?")
    return ports[0]


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--port", type=int, default=9099)
    ap.add_argument("--serial", default=None)
    ap.add_argument("--seconds", type=float, default=0.0, help="stop after N seconds (0 = forever)")
    args = ap.parse_args()

    dev = args.serial or find_port()
    ser = serial.Serial(dev, 115200, timeout=2)
    print(f"reading {dev} -> {args.host}:{args.port}", flush=True)

    sock = None
    started = time.time()
    sent = 0
    while not args.seconds or time.time() - started < args.seconds:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode("utf-8", "replace").strip()
        if not line.startswith("KEY "):
            continue
        if sock is None:
            # Reconnect lazily: the CA-1 listener may be restarted independently.
            try:
                sock = socket.create_connection((args.host, args.port), timeout=5)
            except OSError as e:
                print(f"connect failed: {e}", flush=True)
                time.sleep(2)
                continue
        try:
            sock.sendall((line + "\n").encode())
            sent += 1
            print(f"-> {line}", flush=True)
        except OSError as e:
            print(f"send failed, will reconnect: {e}", flush=True)
            sock = None
    print(f"forwarded {sent} events", flush=True)


if __name__ == "__main__":
    main()
