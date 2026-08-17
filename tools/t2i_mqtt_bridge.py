#!/usr/bin/env python3
"""
Publish T2i button presses to MQTT.

The remote streams key transitions on its USB CDC as lines:

    KEY DOWN 135 r5 c1
    KEY UP 135

This reads them and republishes to MQTT, so button presses are usable from Home
Assistant / Node-RED / zigbee2mqtt-adjacent tooling *without* the ZigBee radio.

Why this exists: the EM250 radio runs RTI's proprietary protocol, not standard
ZigBee, so it cannot join a zigbee2mqtt network without replacing the radio's own
firmware — which needs physical access we do not have. USB is the path that works
today, and the topic layout below deliberately mirrors what a z2m device would
publish so a later switch to real ZigBee does not change consumers.

Usage:
    pip install paho-mqtt pyserial
    ./t2i_mqtt_bridge.py --broker 192.168.1.10 [--port /dev/cu.usbmodem1101]
    ./t2i_mqtt_bridge.py --print-only        # no MQTT, just show events

Topics:
    t2i/<device>/action        -> "vol_up"        (z2m-style action, momentary)
    t2i/<device>/key           -> {"code":135,"name":"ok","row":5,"col":1,"state":"down"}
    t2i/<device>/availability  -> "online" / "offline"  (LWT)
"""
import argparse
import json
import glob
import sys
import time

# The firmware sends the button name with each event, so no table is needed here.
# Names come from stock RTI itself (see docs/BUTTON-NAMES.md) and are slugified
# below for MQTT: "Vol +" -> "vol_plus", "|<<" -> "skip_back".
SLUG = {
    "+": "plus", "-": "minus", "<<": "scan_back", ">>": "scan_fwd",
    "|<<": "skip_back", ">>|": "skip_fwd", "-/.": "dash_dot",
}


def slugify(name):
    if name in SLUG:
        return SLUG[name]
    out = name.lower().replace("+", " plus").replace("-", " minus")
    out = "".join(ch if ch.isalnum() else " " for ch in out)
    return "_".join(out.split()) or "unknown"


def find_port(explicit=None):
    if explicit:
        return explicit
    for pattern in ("/dev/cu.usbmodem*", "/dev/ttyACM*"):
        hits = sorted(glob.glob(pattern))
        if hits:
            return hits[0]
    return None


def parse_line(line):
    """'KEY DOWN 135 r5 c1' / 'KEY UP 135' -> dict, or None if not a key event."""
    parts = line.split()
    if len(parts) < 3 or parts[0] != "KEY":
        return None
    state = parts[1].lower()
    if state not in ("down", "up"):
        return None
    try:
        code = int(parts[2])
    except ValueError:
        return None

    # "KEY DOWN 135 OK r5 c1" -> name is everything between code and rN/cN
    name_parts = [p for p in parts[3:]
                  if not (len(p) > 1 and p[0] in "rc" and p[1:].lstrip("-").isdigit())]
    name = " ".join(name_parts) if name_parts else f"key_{code}"

    ev = {"code": code, "name": slugify(name), "label": name, "state": state}
    for p in parts[3:]:
        if p.startswith("r"):
            try:
                ev["row"] = int(p[1:])
            except ValueError:
                pass
        elif p.startswith("c"):
            try:
                ev["col"] = int(p[1:])
            except ValueError:
                pass
    return ev


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--broker")
    ap.add_argument("--mqtt-port", type=int, default=1883)
    ap.add_argument("--username")
    ap.add_argument("--password")
    ap.add_argument("--device", default="remote")
    ap.add_argument("--port", help="serial device (auto-detected if omitted)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--print-only", action="store_true")
    args = ap.parse_args()

    try:
        import serial
    except ImportError:
        sys.exit("pyserial missing:  pip install pyserial")

    if not args.print_only and not args.broker:
        sys.exit("need --broker, or pass --print-only")

    base = f"t2i/{args.device}"
    client = None
    if not args.print_only:
        try:
            import paho.mqtt.client as mqtt
        except ImportError:
            sys.exit("paho-mqtt missing:  pip install paho-mqtt")
        client = mqtt.Client()
        if args.username:
            client.username_pw_set(args.username, args.password)
        # Last-will so consumers can tell a disconnected remote from an idle one.
        client.will_set(f"{base}/availability", "offline", retain=True)
        client.connect(args.broker, args.mqtt_port, keepalive=60)
        client.loop_start()
        client.publish(f"{base}/availability", "online", retain=True)
        print(f"mqtt: {args.broker}:{args.mqtt_port} -> {base}/#")

    # Reconnect loop: the remote's USB drops on every power-cycle and firmware
    # update, which during development is constant.
    while True:
        port = find_port(args.port)
        if not port:
            print("waiting for the remote to appear on USB...", flush=True)
            time.sleep(2)
            continue
        try:
            with serial.Serial(port, args.baud, timeout=1) as ser:
                print(f"serial: {port}", flush=True)
                while True:
                    raw = ser.readline()
                    if not raw:
                        continue
                    line = raw.decode("utf-8", "replace").strip()
                    if not line:
                        continue
                    ev = parse_line(line)
                    if ev is None:
                        print(f"  (device) {line}", flush=True)
                        continue

                    print(f"  {ev['state']:4} {ev['name']} (code {ev['code']})", flush=True)
                    if client:
                        client.publish(f"{base}/key", json.dumps(ev))
                        # z2m-style: an "action" is a momentary event, so only on press
                        if ev["state"] == "down":
                            client.publish(f"{base}/action", ev["name"])
        except KeyboardInterrupt:
            break
        except Exception as e:                      # noqa: BLE001 - keep the bridge alive
            print(f"serial error ({e}); retrying", flush=True)
            time.sleep(2)

    if client:
        client.publish(f"{base}/availability", "offline", retain=True)
        client.loop_stop()


if __name__ == "__main__":
    main()
