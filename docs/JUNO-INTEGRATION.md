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
* **The proxy contract is `remote`**, added as `driver-sdk/proxies/remote.toml`. `keypad` was the
  wrong fit: it reports "key 3 clicked", which means whatever a rule says it means, so a 53-key
  remote would need dozens of hand-authored rules — the programming step this is meant to delete.
  A `remote` emits named commands from a closed vocabulary that is deliberately the command names
  `room`, `media_player` and `tv` already use, so `volume_up` from a remote bound to a room is
  `room::volume_up` in that room and the pathfinder resolves it against whatever is playing.

  A remote addresses a **room**, never a device. That is the property that makes the mapping
  automatic, and the reason the T2i firmware must not learn any device names.

  IR transmit stays firmware-side ([IR-BUZZER.md](IR-BUZZER.md), currently disabled — it browns
  the remote out).

### T2i keys onto the `remote` vocabulary

Stock codes and names from [BUTTON-NAMES.md](BUTTON-NAMES.md). Everything universal routes itself;
everything else arrives as `custom` with the label the handset prints, and binds like a keypad key.

| T2i key (code) | `remote` notification |
|---|---|
| Play 146, Pause 147, Stop 148, Record 149 | `command` — `play`, `pause`, `stop`, `record` |
| `<<` 150, `>>` 151 | `command` — `scan_reverse`, `scan_forward` |
| `\|<<` 152, `>>\|` 153 | `command` — `skip_back`, `skip_forward` |
| Up 131, Down 134, Left 132, Right 133, OK 135 | `command` — `up`, `down`, `left`, `right`, `select` |
| Scroll Up/Down/Left/Right 166/168/169/170, Click 167 | the same d-pad commands — a joystick is navigation |
| Vol + 138, Vol - 139, Mute 129 | `command` — `volume_up`, `volume_down`, `mute_toggle` |
| Ch + 140, Ch - 141, Prev 178 | `command` — `channel_up`, `channel_down`, `previous_channel` |
| On 171, Off 145 | `command` — `power_on`, `power_off` |
| Menu 143, Guide 142, Info 144, Exit 128, Back 179 | `command` — `menu`, `guide`, `info`, `exit`, `back` |
| 0-9 154-163 | `digit` |
| Enter 165 | `command` — `enter` |
| Softkeys 130/136/137/177, Red/Green/Yellow/Blue 173-176, List 172, `-/.` 164 | `custom` |
| Backlight 180 | not emitted — a local handset function |

Capabilities the T2i declares: `has_transport`, `has_dpad`, `has_numeric`, `has_channel`,
`has_volume`, `has_power`, `has_menu`, `has_hold`, `has_battery`.

`has_hold` is the one that earns its keep. The firmware already emits `KEY DOWN` / `KEY UP`
separately, so a held `Vol +` becomes `command { volume_up, hold }` and the consumer answers with
`media_player::hold { what = "volume_up" }` — a ramp, rather than forty steps.

### This repository — everything RTI-specific

* [../src/zbx.c](../src/zbx.c) — ZBX framing, TwTc header, SysCode payloads
* [../src/keypad.c](../src/keypad.c) — the 8x7 matrix and all 53 stock key codes and names
* [../tools/t2i_mqtt_bridge.py](../tools/t2i_mqtt_bridge.py) — USB CDC key events to MQTT
* the EM357-side decoder, **once the OTA format is observed** — deliberately not written yet
* `juno-driver/` — the Juno driver package implementing `remote`, built here and kept here. It
  is the T2i-specific half by definition: the key-code table above is exactly the knowledge Juno
  must not carry.

Two things are worth stating about the driver: it is a sandbox — no files, no sockets, no clock,
every side effect returns as a `HostCall` — and `driver-sdk` is tracked on `main` by branch with
the lockfile holding the commit, because that repository carries no tags on purpose.

## Why the EM250 keeps RTI's firmware

Worth restating, because "program the EM250 ourselves" is the obvious question and the answer is
counter-intuitive. [EM250-REFLASH.md](EM250-REFLASH.md) splits it in two: **reflashable, yes**
(Ember's serial bootloader is present and RTI's own `zbconfig.exe` drives it over XMODEM);
**able to write firmware for it, no**. It is a 16-bit XAP2b core, the only compiler was Ember's
proprietary Windows-only xIDE, EOL around 2011-13, with no GCC or LLVM backend in existence, and
EmberZNet shipped as licensed pre-compiled XAP2b libraries. The `.ebl` embedded in `zbconfig.exe`
is RTI's ZB-Pro dongle image — flashable, but not ours and not a remote.

If a pre-compiled binary ever turns up that exposes the EM250's stack the way a modern Ember part
exposes EZSP, that changes everything and the decomps are the way in — there is a complete valid
`.ebl` and a fully reversed flash procedure already. Until then this stands.

One update to that document: it listed **two** independently fatal blockers, and choosing EZSP on
the OpenHC removes one. The second was zigbee2mqtt's EZSP v13 floor plus the fact that EZSP was
never an EM250 protocol — that only bit while the EM250 was going to be the *coordinator*. With the
EM357 as the NCP, the EM250 only has to be an end device. The toolchain is the only blocker left,
and it is enough on its own.

## What is done, and what is untested

Landed in Juno (all three, uncommitted):

| Repo | Change | Verified |
|---|---|---|
| `driver-sdk` | `proxies/remote.toml` | loads and passes structural validation, incl. `values_require`; `junodrv docs` generates `remote.md` |
| `core-zigbee` | `KIND.remote`, `kindOf` detects an `action` enum, `remoteActionOf` alias table, capabilities from the action vocabulary, `fromZigbee` emits `command`/`digit`/`custom`, `manifest.toml` child | 80 tests pass, 12 of them new in `test/remote.test.mjs` — fixtures, no radio needed |
| `core` | `remote` added to the `has_repeat`-without-`has_hold` lint; new lint for a remote declaring no command groups | full suite passes (two `backtest.rs` failures are pre-existing — confirmed identical on a clean tree) |

In this repository:

| Piece | Change | Verified |
|---|---|---|
| `juno-driver/` | the `remote` driver: manifest, key-code table, serial line parser | **13 tests pass**; builds for `wasm32-wasip1` (298 KB); `junodrv check` validates the manifest against the contract |
| `src/main.c` | `KEY HELD <code> <name>` emitted once at `KEY_HOLD_MS` (400 ms) | **verified on hardware** — a held key gives `DOWN` → `HELD` → `UP` |
| `src/keypad.c` | 30 ms time-based debounce in `keypad_scan()`, column settle 20 µs → 50 µs | **verified on hardware** — see below |

### The keypad was chattering, and had been all along

Testing `KEY HELD` turned up a bug that predates it. Holding one key produced a *continuous* stream
of alternating `KEY DOWN`/`KEY UP` — 35 lines from two presses — for as long as it was held. Every
consumer of the key stream has therefore been seeing dozens of presses for one, including
`t2i_mqtt_bridge.py`. It also meant the hold timer was reset before it could ever expire, so
`KEY HELD` almost never fired.

This is **not** contact bounce: that settles in a few milliseconds, and this persisted for seconds.
Something in the matrix scan misreads the row on some passes and the cause is still worth a scope.
`keypad_scan()` now requires a reading to hold for 30 ms — three passes of the 10 ms main loop —
before believing it, which filters the effect whatever the cause. Same two presses after the fix:
6 lines, clean `DOWN` → `HELD` → `UP`.

Still unconfirmed: a **fast tap** producing `DOWN`/`UP` with no `HELD`. Both test presses ran past
400 ms. The driver's `click` path is covered by its own tests but has not been seen on hardware.

`KEY HELD` exists because a driver sandbox cannot read a clock: it sees DOWN and UP but not the gap
between them. The first version of the driver timed the hold itself, which silently never fired
while the manifest claimed `has_hold = true` — exactly the invisible failure the new core lint is
about. Hold timing therefore belongs on the remote, which has a clock.

**No Zigbee hardware has been involved.** The OpenHC CA1 is still being built, so nothing here has
met an EM357, an NCP, or a real mesh. Everything above is fixtures and contract validation, which
is real but is not the same claim.

Two things worth recording from doing it:

* `kindOf` ordering is load-bearing. A remote is a battery device that also reports voltage, so the
  existing numeric fallback would have classified a Hue Dimmer as a `sensor` and buried its buttons.
  The `action` check has to come first, and there is a test for exactly that.
* Herdsman's `brightness_move_up` means *start ramping* and runs until `brightness_stop`, while
  `brightness_step_up` is one step. Flattening all three to a click turns every hold-to-dim into a
  single nudge, so they map to `hold`/`release`/`click` respectively.

Also fixed while there: `core-zigbee`'s "every kind maps to a proxy core actually has" test compared
against a hand-written list, so it passed for contracts that did not exist and failed for one that
did. It now reads `driver-sdk/proxies/`, and skips when that is not checked out.

## Two paths, and they are not equivalent

**Path A — USB, works today.** The remote on USB CDC, `t2i_mqtt_bridge.py`, into Juno as a
`remote`. No radio, no EM250, no unknowns. It needs the remote tethered, which is wrong for a
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
4. **Then** the Zephyr firmware drives the EM250 with our own ZBX implementation. Reflashing the
   EM250 is not an option — see below.

Writing the decoder before step 2 would mean guessing a wire format. That is the one kind of
code here that cannot be checked, and `zbx.c` only earned its self-test because the STM32-side
format was actually known.
