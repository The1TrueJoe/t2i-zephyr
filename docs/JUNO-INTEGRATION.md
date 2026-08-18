# Getting the T2i into Juno

Where each piece lives, and why. The rule this is built around: **Juno stays generic and
never mentions the T2i.** Every RTI-specific byte lives in this repository.

## What we have and what we do not

This matters more than anything else here, and it is easy to get wrong:

| | Reversed? |
|---|---|
| **Host STM32 ↔ EM250 over USART3** (ZBX framing, TwTc session, SysCode payloads) | **Yes** — [ZIGBEE-PROTOCOL.md](ZIGBEE-PROTOCOL.md), implemented in [../src/zbx.c](../src/zbx.c), self-test passing on hardware |
| **What the EM250 puts on the air** | **No.** Not started, and not reachable by the route we used for everything else |

ZBX is the *UART* protocol between the STM32 and the radio module. The over-the-air format is
produced inside the EM250's own firmware, which is an Ember XAP2b core — no public toolchain,
no decompiler ([EM250-REFLASH.md](EM250-REFLASH.md)). So the OTA format cannot be read out of
an image the way the STM32 side was.

The only way to learn it is to **observe it**, which means a receiver and a transmitter.

## Therefore: keep a radio unit on stock firmware

The radio remote that identifies as `MstrBdrm ID 40` is a **known-good transmitter** of exactly
the protocol we need to decode. Flashing it destroys the only reference signal we have.

Its stock firmware is worth more than a second test target right now. Flash it only once the
EM357 side can capture and the OTA format is understood — and note there is no USB backup path
for it, so that flash is one-way ([USB-FLASHING.md](USB-FLASHING.md)).

## Layout

```
Control4 OpenHC (EM357)  ──serial──▶  zigbee-ap  ──EZSP over TLS──▶  core-zigbee  ──▶  Juno
        ^                            (generic)                       (generic)
        |
   T2i radio remote (stock EM250, RTI TwTc over the air)
```

### Juno side — generic, unchanged

Nothing here needs to know a T2i exists, and none of it should be modified to.

* **`zigbee-ap`** already drives a serial-attached Ember NCP: it terminates ASH and takes
  `--device /dev/serial/by-id/...`. An EM357 running an **EZSP NCP** image is precisely what it
  expects. If the OpenHC's EM357 speaks EZSP, this is a config change, not code.
* **`core-zigbee`** is EZSP/ember only and delegates every device-level meaning to
  `zigbee-herdsman-converters`. Its README states the rule directly — core never learns what a
  cluster means, because "here it is a driver update; in core it would be a controller release."
  So a T2i device definition is a converter or a driver package, never core code.
* **The proxy contract is `keypad`** (`driver-sdk/proxies/keypad.toml`), and it fits without
  stretching:

  | Contract | T2i |
  |---|---|
  | `key_count` | 53 |
  | `key_labels` | the stock names from [BUTTON-NAMES.md](BUTTON-NAMES.md), in code order 128..180 |
  | `has_hold`, `has_repeat` | the firmware already emits `KEY DOWN` / `KEY UP` |
  | `has_battery` | [../src/battery.c](../src/battery.c) |
  | `clicked` / `held` / `released` | key transitions |

  `ir_out` is the contract for the IR blaster, once IR transmit is safe to re-enable
  ([IR-BUZZER.md](IR-BUZZER.md) — currently disabled, it browns the remote out).

### This repository — everything RTI-specific

* [../src/zbx.c](../src/zbx.c) — ZBX framing, TwTc header, SysCode payloads
* [../src/keypad.c](../src/keypad.c) — the 8x7 matrix and all 53 stock key codes and names
* [../tools/t2i_mqtt_bridge.py](../tools/t2i_mqtt_bridge.py) — USB CDC key events to MQTT
* the EM357-side decoder, **once the OTA format is observed** — deliberately not written yet
* a Juno driver package (`driver-sdk`, WASM) implementing `keypad`

Two things are worth stating about the driver: it is a sandbox — no files, no sockets, no clock,
every side effect returns as a `HostCall` — and `driver-sdk` is tracked on `main` by branch with
the lockfile holding the commit, because that repository carries no tags on purpose.

## Two paths, and they are not equivalent

**Path A — USB, works today.** The remote on USB CDC, `t2i_mqtt_bridge.py`, into Juno as a
`keypad`. No radio, no EM250, no unknowns. It needs the remote tethered, which is wrong for a
remote, but it is real today and the topic layout already mirrors what a mesh device would
publish so consumers do not change later.

**Path B — over the air.** The prize, and it is gated on the unknown above. Ordered by
information per unit of risk:

1. **Identify the OpenHC's EM357 firmware.** Does it speak EZSP, or Control4's own application
   protocol? This is the fork in the road: EZSP means `zigbee-ap` works as-is; anything else
   means the EM357 needs an NCP image before any of this starts. Nothing else is worth doing
   first, and it needs no T2i involvement at all.
2. **Capture stock traffic.** With the EM357 receiving, press buttons on the stock
   `MstrBdrm ID 40` remote and correlate. `MstrBdrm ID 40` strongly suggests unit id **40**,
   which is one of the open questions in ZIGBEE-PROTOCOL.md §10 (the `unit`/`sys`/`cmd16`
   mapping) — that came free from a non-destructive handshake read.
3. **Decode, in this repository.** Only once there are captured frames to test against.
4. **Then** decide whether the Zephyr firmware drives the EM250 with our own ZBX
   implementation, or the EM250 gets reflashed.

Writing the decoder before step 2 would mean guessing a wire format. That is the one kind of
code here that cannot be checked, and `zbx.c` only earned its self-test because the STM32-side
format was actually known.
