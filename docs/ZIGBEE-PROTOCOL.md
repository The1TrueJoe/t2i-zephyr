# Zigbee Protocol B

## Why the radio refuses network commands — 2026-08-18

Measured on hardware: queries work, network commands are refused.

```
0x04 -> 05 1c ...          get network info   (this is what 0x04 does)
0x60 -> 61 0f ...          query, fine
0x02 -> 0x03 event 0x02 -> 0x0b
0x20 -> 01 03 20 01 00     status 0x01  REFUSED
0x30 -> 01 03 30 01 00     status 0x01  REFUSED
0x31 -> 01 03 31 07 00     status 0x07
```

**It is not the arguments.** Three things were tried and all were wrong guesses:

* `mode` 0/1 — invalid, `FUN_08019E40` only ever emits 2, 3 or 4 for `0x20`
  (arg 1 -> 2, 2 -> 3, 0x13 -> 4). Sending 2/3/4 changed nothing.
* an all-zero extended PAN — replacing it with the radio's own EUI changed nothing.
* the frame layout — `FUN_08020770` confirms ours is byte-exact: `epan[8]`, `pan_lo`,
  `mode`, `chanmask` big-endian.

The decisive observation is that **`0x30` carries no payload at all and is refused identically**.
So it is a precondition, not a parameter.

`FUN_080194F4` (the RX dispatcher) shows what the precondition is:

```c
if (*(char *)(zbx + 0x20) == 0) goto bail;     // state 0 = uninit: ignore everything
...
if (cmd == 0x20) {
    log("ZbxRx router init cmd resp: state: %d", state);
    if (state == 3) {                          // 3 = router init
        if (status != 0x00 && status != 0x0B)  -> failure, retry after 1000ms
        else if (obj[0x22] == 2) { ...; timer 40000; advance }
        else if (obj[0x22] == 1) { ...; timer 20000; advance }
```

So: **`0x00` and `0x0B` are the success statuses**, `0x01` is a real rejection, and the radio only
accepts `0x20` once the host has walked it into router-init through the preceding sequence — with
20-40 second timers between steps and a retry path. State lives at `zbx+0x20`, a sub-mode at
`zbx+0x22`, chanmask at `zbx+0x34`, the TX scratch at `zbx+0x48`.

Replicating that ordered sequence was tried and **did not help**. Implemented stock's own path —
`0x32` (leave/reset) -> `0x04` (query network, which does return the `0x05` info) -> `0x20` — with
watchdog-safe waits between steps. Still `01 03 20 01 00`.

The full elimination, all measured on hardware:

| varied | result |
|---|---|
| `mode` 0,1,2,3,4 (only 2/3/4 are legal per `FUN_08019E40`) | status `01` |
| extended PAN: all zeros, then the radio's own EUI | status `01` |
| `pan_lo`: `0x00`, then `0x35` (PAN 0 is not a valid ZigBee PAN) | status `01` |
| chanmask: single channel, then all of 11-26 (`0x07FFF800`) | status `01` |
| ordering: one-shot, then stock's `0x32` -> `0x04` -> `0x20` | status `01` |
| frame layout | byte-exact against `FUN_08020770` |

Never varies, including for `0x30`, which takes no arguments at all. So it is not the arguments and
not the ordering.

**RESOLVED — it is an end-device image, and `0x20` is ROUTER init.**

The security-material hypothesis was wrong. The clue was in the log string all along:
`ZbxRx **router** init cmd resp`. This is a battery-powered handheld, so its EM250 runs an
end-device image, and an end-device image refuses router init whatever the arguments — which is
exactly what argument-free `0x30` being refused identically meant. No parameter could ever have
fixed it.

`0x21` is the sibling init with the end-device shape (`FUN_080207c0`: `pan_lo`, `epan[0..7]`,
`mode`, `cfg`). In one run, on one radio, seconds apart:

```
ZBX seq: 0x20 router init (control)   ->  01 03 20 01 00    status 0x01  REFUSED
ZBX seq: 0x26 rejoin                  ->  01 03 26 00 00    status 0x00  ACCEPTED
ZBX 0x21 mode=02 cfg=00               ->  01 03 21 00 00    status 0x00  ACCEPTED
```

After the first accepted `0x21` the radio stops answering the rest of a sweep: it is busy scanning.

### The `0x05` network info, decoded

```
05 1c ff c4 0b c1 05 00 6f 0d 00 ff fe 01 07 15 20 09 50 10 00 02 00 ...
      ^^ channel 0xff = none
         ^^^^^^^^^^^^^^^^^^^^^^^ EUI c4:0b:c1:05:00:6f:0d:00
                                 ^^^^^ node id 0xfffe = unassigned
                                                      ^^ 2.0   ^^ 5.0  (nibble-split versions)
                                          extended PAN (b[0x16..0x1d]) = all zeros
```

Channel `0xff`, node `0xfffe`, extended PAN zero: the radio is on no network. Worth noting the
capture filter that hid this for so long — it matched `ZBX rx 01/03/07` and silently dropped
`ZBX rx 05`, so the one reply that described the radio's actual state was never printed.

### Confirmed on air

`0x21` accepted makes the radio scan, and the scan is visible from the CA-1's mfglib sniffer as
802.15.4 beacon requests — FCF `0x0803`, dst PAN and address `ffff`, MAC command `0x07`:

```
01:47:58.354 PACKET ch11 lqi=211 rssi=10  03083bffffffff07cdc3
01:48:01.029 PACKET ch11 lqi=212 rssi=10  03084bffffffff07cc06
01:48:02.957 PACKET ch11 lqi=212 rssi=10  03085bffffffff077c44
```

The band is busy with the house's own network, so the attribution is a differential rather than an
assertion — same sniffer, same channel, back-to-back builds differing only in `ZBX_PROBE`:

| T2i join | beacon requests captured |
|---|---|
| disabled (`ZBX_PROBE 0`) | **0** |
| enabled (`ZBX_PROBE 1`) | **9** |

**The remote transmits, and the CA-1 receives it.** It has not joined anything yet — there is no
coordinator with permit-join open on a PAN it will accept, so the scan finds nothing and retries.
### Associated with the CA-1

`tools/ca1_form.js` turns the CA-1 into a coordinator over EZSP — stack profile 2, security level 5,
`setInitialSecurityState`, `formNetwork`, `permitJoining(0xFF)`:

```
extendedPan = 5432690000000001   panId = 0x0035   channel = 15   nodeType = 1 (coordinator)
```

Pointing the T2i's `0x21` at it gets a **real MAC association**. From the coordinator's own EZSP
callbacks — `0x23` childJoinHandler and `0x24` trustCenterJoinHandler — carrying the T2i's EUI:

```
0x23 index=00 joining=01 child=5fed c40bc105006f0d00 type=05    associates, sleepy end device
0x24         node=5fed  c40bc105006f0d00 ...                    trust centre sees it
0x23 index=00 joining=00 child=5fed c40bc105006f0d00 type=05    leaves again
```

A fresh short address every retry (`5fed`, `b75b`, `7c96`, `b903`, ...): it associates, fails, and
tries again.

### `cfg` = 3 is the secured join

Sweeping `cfg` 0..3 against a live network is unambiguous — `cfg` 0/1/2 make no attempt at all, and
only `cfg = 3` produces one, reproducibly ~5 s later:

```
23:29:52  0x21 join epan=as-is cfg=3
23:29:57  ZBX rx 07 01 af        <- stack status indication
23:31:12  0x21 join epan=as-is cfg=3
23:31:17  ZBX rx 07 01 af        <- again, same delay
```

`0x07` is the stack status indication and `0xAF` is `EMBER_PRECONFIGURED_KEY_REQUIRED`: the EM250
will not accept a network key sent in the clear. That is the one remaining blocker, and it is a
key we do not have — RTI's preconfigured link key is baked into the EM250 image, which we cannot
read.

### The join gets all the way to key delivery

Correcting an earlier reading in this document: `0xAF` is **not** "wrong key". Sweeping the trust
centre's join policy showed the remote reaching every stage of a real join:

| trust centre | remote answers |
|---|---|
| `securityLevel 0` (no key at all) | `0xAD NO_NETWORK_KEY_RECEIVED` |
| key **in the clear** (`setPolicy` decision 0) | `0xAC RECEIVED_KEY_IN_THE_CLEAR` |
| key **encrypted**, any of 4 candidate keys | `0xAF PRECONFIGURED_KEY_REQUIRED` |

`0xAC` means the network key *arrived* and the remote refused it for being unencrypted. So the
EM250 has `EMBER_REQUIRE_ENCRYPTED_KEY` set and no key to decrypt with. Both halves are baked in.

Two `setPolicy` gotchas cost real time and are worth recording: policies are **reset by
`formNetwork`**, so setting one before forming silently does nothing (it still returns `00`); and
`setPolicy` takes an `EzspDecisionId` while the callback reports an `EmberJoinDecision` — different
enums. Measured mapping: `EzspDecisionId` 0 -> `SEND_KEY_IN_THE_CLEAR`, 1 -> `USE_PRECONFIGURED_KEY`,
2 and 3 -> `DENY_JOIN`.

### Everything swept

Remote side: `mode` 0-5; `cfg` 0-7 **and** every high bit (`0x08`,`0x10`,`0x20`,`0x40`,`0x80`,`0xFF`)
— all accepted, all still `0xAC`; extended PAN in both byte orders; `pan_lo`; channel mask.
Provisioning from the host is impossible: `0x22`/`0x23`/`0x24` return `0x01` from all six reachable
states (after `0x02`, after `0x04`, after `0x32`, after `0x30`, after `0x31`, mid-`0x21`-join).
`0x50` is accepted (`00`) but does nothing visible; `0x51`/`0x52` never answer.

Coordinator side: stack profile 1 (no join attempt at all — **profile 2 is required**) and 2;
security level 0 and 5; bitmask `0x0000`/`0x0200`/`0x0204`/`0x0304`/`0x0B04`; join decisions 0-4;
preconfigured keys `ZigBeeAlliance09`, all-zero, all-`FF`, `00..0F`, and the remote's own EUI.

### What RTI's own tooling says

`ZBConfig.exe`, carved out of the Integration Designer install (Windows VM disk image, PE resource
`name#1096/130/1033`), embeds a **complete EM250 EmberZNet image** — `xap2b-em250-em250-ZBPro`,
98 KB, saved as `zbpro_em250.ebl`. Its strings describe RTI's scheme:

```
Failed to generate random link key.        Failed to generate random NWK key.
Failed to set policy for requesting TC link keys.
Trust Center no longer allowing joins.     RX [CPNCT_MSG_TYPE_EPID_REQ]
app\ZBProNCT\ZBProNCT.c                    app\util\bootload\bootload-utils.c
```

So the ZB-Pro **generates its keys randomly** and lets joined devices *request* the trust-centre
link key. `ZigBeeAlliance09` does not appear anywhere in the image, and the constant pool ahead of
the string block holds only bit tables and address vectors — no 16-byte key. Whatever the T2i
bootstraps with is not a well-known constant sitting in RTI's tool.

### The radio's bootloader IS reachable — opcode 0x08

Static analysis of stock's radio init (`FUN_0800BFB2`) settles the pin question. Its literal pool is
`0x40004800` (USART3), `0x40020800` (GPIOC), `0x20006320` (the zbx object), and the only pins it
touches are:

| pin | role | evidence |
|---|---|---|
| PC10 | USART3 TX | `GPIO_PinAFConfig(GPIOC, 10, AF7)` at `0x800c31e` |
| PC11 | USART3 RX | `GPIO_PinAFConfig(GPIOC, 11, AF7)` at `0x800c312` |
| PC13 | nRESET | pin mask `0x2000` as output, then low / 1.4 ms / high |

**Three signals, no boot-mode pin.** Nothing else is driven, so the EM250's bootloader activation
line is not under software control. The stock image also contains no radio-bootload strings at all
(the only match is `T2i Bootloader Version`, the STM32's own) -- RTI never bootloads the radio from
the host; the ZB-Pro does it over the air.

The remaining software route would be the radio app launching its own bootloader, so the last
unprobed opcodes were tested. None does:

| opcode | result |
|---|---|
| `0x06` | status `0x00` accepted, radio keeps running |
| `0x74`, `0x75` | status `0x01` refused |
| `0x80` | returns *data*, alternating `14 8a` / `12 01` -- a query, not a status |
| `0x50` | status `0x00`, no visible effect |
| `0x51`, `0x52` | never answer |

Byte counters confirm it: every probe returned exactly one framed reply (`bad+0`, `bytes+8`), never
the ASCII banner a bootloader would emit.

**That conclusion was wrong, and the mistake is worth naming:** the opcode list above came from
stock's *encoder* table -- commands the host knows how to build. The radio's receive side accepts
commands stock never sends. Sweeping all 256 opcodes with a zero-length payload found one that is
not in that table at all and that stops the radio speaking ZBX:

```
ZBX opcode sweep 0x00-0xff, zero payload
ZBX op 08: SILENT after this - mode change!
ZBX op 06: status 00      ZBX op 26: status 00     ZBX op 50/51/52: status 00
ZBX op 21: status 06      ZBX op 31: status 00     ZBX op 32: status 05
ZBX op 40: status 02      ZBX op 80: status 12     ZBX op 87: status 14
```

**`0x08` launches the EM250's standalone bootloader.** Send it, then CR+LF:

```
after 0x08: 81 01 03 08 00 00 f4 82        (acked, status 00)
MENU:  ~~EM250 Bootloader. v20 b09~~~~1. upload ebl~~2. run~~3. ebl info~~BL > .
```

The same Ember standalone bootloader as the CA-1's EM357, reached over USART3 with **no bootmode
pin and no hardware access** -- and `2. run` returns to the application, which was confirmed by ZBX
coming straight back with a `0x05` network-info reply. So flashing the radio is **reversible**.

Reproducing it needs the exact cadence: `0x02` open, then `0x08`, then **CR+LF** rounds about a
second apart. A bare CR, or longer gaps, produce nothing at all.

What remains is a suitable image. `3. ebl info` will describe the one currently installed, and
`zbpro_em250.ebl` (RTI's own, carved from ZBConfig.exe) is a known-good EM250 image to compare
against -- though it is the ZB-Pro *coordinator* app, not an NCP.

### What the bootloader can and cannot do

Entry is reliable but fussy, so `bl_enter()` verifies the banner and retries rather than assuming —
a sweep run against a radio that never entered looks exactly like a sweep that found nothing, and
that cost a wasted run.

Sweeping every printable character at the `BL >` prompt found **no undocumented commands**: only
`3` responds, with `xap2b-em250-em250-TXBZB` (so the installed image is RTI's T2i radio build, as
against the ZB-Pro's `-ZBPro`). There is no read or dump, so **the EM250's flash cannot be
extracted** and RTI's link key cannot be lifted out of it.

`0x80` looked like a memory read — 5-byte payload, two data bytes back, and XAP2b is 16-bit
word-addressed — but it returns `1201` at every address probed (`0000`, `0001`, `0002`, `0100`,
`1000`, `4000`, `8000`, `f000`, three reads each). It is a fixed status word. `0x87` answers once
and then stops. No memory read exists.

`0x26` acks `00` for all 60 combinations of addr x mode x cfg and produces **no stack status at
all** — it is accepted but starts no join. Ruled out.

### The one-way door

Flashing the radio is **not** symmetric with flashing the CA-1's EM357, and the reason is worth
stating plainly: opcode `0x08` is a command of the **TXBZB application**. It is the only route into
the bootloader — there is no bootmode pin and the bootloader does not persist across a reset on its
own. Overwrite TXBZB with an image that has no equivalent command and bootloader access is gone for
good, with no copy of TXBZB anywhere to restore (Integration Designer does not ship one; the only
`xap2b` images on the whole VM disk are the ZB-Pro's, inside `ZBConfig.exe`).

If it is ever done, the upload protocol is known: ZBConfig drives the same bootloader over FTDI and
its strings are pure XMODEM-CRC (`Got CRC16...`, `Got NAK...`, `Got CAN...`, `Sending EOT...`) —
send `1`, wait for `C`, stream 128-byte CRC blocks, exactly as the EM357 was flashed.

The relevant strings mark the path: `ZbxRx router init cmd resp`, `ZbxRx: correct or no network`,
`ZbxRx: wrong network`, `ZbxRx stack stat ind`, `ZIGBEE UNI FAILURE - %x`.

## LINK UP ON HARDWARE — 2026-08-18

The host <-> EM250 link works from our firmware. Valid frames, checksums passing, `bad=0`:

```
ZBX rx     03 01 02                                     unsolicited event (app event 7)
ZBX reply  61 0f 00 00 0b 03 ff ff 05 02 00 00 0a ...   0x60 -> 0x61, 15-byte payload
ZBX reply  63 08 00 00 00 00 00 00 00 00                0x62 -> 0x63
ZBX reply  05 1c ff c4 0b c1 05 00 6f 0d 00 ff fe ...   0x05 NETWORK INFO, 28 bytes
```

Two things had to be right, and both were wrong at first:

1. **PC13 is the EM250's nRESET** and nothing was driving it. After an MCU reset the pin is an
   input, so the radio sat whereever it happened to be. `zbx_uart_init()` now pulses it low and
   releases it, as stock does in `FUN_0800BFB2` (low, `FUN_080113A4(0x578)` = 1.4 ms, high).
2. **RX must be interrupt-driven.** USART3 here has no FIFO: the data register holds ONE byte, a
   byte lands every ~87 us at 115200, and the main loop comes round every 10 ms. Polling kept
   exactly one byte of every reply and overran the rest — which looked like a dead radio for
   hours. The radio had been answering correctly the whole time.

Diagnostics worth keeping (all in `zbx.c`, reported over USB):

* `zbx_rx_line()` samples PC11 with a pull-down then a pull-up **before** USART3 takes the pin.
  `1/1` = something is driving it, so the radio is present and powered; `0/1` = floating. This is
  what ruled out "no module fitted" without opening the case.
* `zbx_pclk1()` derives PCLK1 from RCC instead of assuming it. `ZBX_BRR` is stock's literal
  `0x0104`, which is only 115200 if PCLK1 is 30 MHz — measured 30 MHz, baud 115384, 0.16% off.
* `zbx_raw()` keeps the first raw bytes unframed, which is how the single-byte overrun was spotted.


> Auto-saved from overnight research agent. Static analysis only — nothing here was tested on hardware.

**Summary:** RTI's host↔EM250 link on USART3 is fully reversed at the byte level. UART is 115200 8N1, no flow control, TX+RX, RXNE interrupt, TX by DMA1 Stream3/Ch4 (USART_Init call at 0x08018DB0 driven from FUN_0800bfb2; BRR computes to 0x0104 at PCLK1=30 MHz, which I re-derived from the stock PLL config). The wire format is a byte-stuffed frame `0x81 <escaped payload> <escaped checksum> 0x82`, escape byte 0x80 emitted literally before any 0x80/0x81/0x82, checksum = two's complement of the 8-bit sum so sum(payload)+cksum ≡ 0. Encoder is FUN_08018A88, decoder FUN_08018B70 — they are exact mirrors. Inside the frame every message is `[opcode][payload_len][payload…]`; I enumerated the complete TX opcode set from the length table in FUN_080206F6 plus all 19 encoders, and the complete RX opcode set from the FUN_080194F4 dispatch plus all 14 parsers. A button press reaches the network only when the key's programmed command format is "System TW" (two-way): keypad task → CommandQ → _HandleHardkeyPress (FUN_08010450) → _ExecuteCmds (FUN_080105DC) → FUN_0801CDAA → "TwTc Client - SendSysCode" (FUN_0801D11E) → TWT session layer → FUN_0801944A → ZBX command 0x40. Plain IR and legacy "Sys RF" keys never touch the EM250 — FUN_08016830 is a bit-banged one-way RF transmitter, not ZigBee.

**Open questions:**
- Is the `0x81 0x81` wake-up burst plus ~1.4 ms gap an EM250 requirement, or RTI belt-and-braces? A Zephyr port could try omitting it, but that needs the radio remote on a scope or a working link to confirm.
- What is the time unit of `FUN_080113a4`? The divisor at `DAT_080114FC` was not read, so the 1393-tick delay is only assumed to be microseconds. Reading that literal would pin the pre-reset and inter-frame delays exactly.
- What does `msg[2]` of command `0x40` address? It is the low byte of a 16-bit value from `session+0x1C` (or `+0x17C` on the retransmit path). Endpoint, short-address table slot, or link index is undetermined, and it matters for talking to more than one peer.
- What do the RX indications `0x61`, `0x63`, `0x65`, `0x67`, `0x69`, `0x6B`, `0x6D`, `0x8E` carry? Field offsets are extracted but the meanings are unknown; they feed app events 0-6 and 0x0D through `zbx->event_cb`.
- Are commands `0x06`, `0x22`-`0x24`, `0x74`, `0x75`, `0x80` dead length-table entries, or used by a different firmware revision? The Integration Designer install may contain a newer image worth diffing.
- How do Integration Designer's UI fields map onto the TwTc SysCode triple (unit id from `FUN_0800e830`, system code, 16-bit command)? Cross-referencing a saved `.rti` project against the config blob in flash would settle it.
- Byte 10 of command `0x20` and byte 2 of `0x21`/`0x26` take only the low half of a 16-bit field. Is the upper byte genuinely unused, or is this a stock truncation bug like the `0x6C` dropped argument?
- Does the EM250 side tolerate a receiver that ignores the wake burst and sends frames back-to-back? Relevant if the Zephyr port wants to use interrupt-driven TX instead of the stock DMA-plus-busy-poll.

---

## RTI T2i host ↔ EM250 protocol on USART3

All addresses are flash offsets in `stock_flash_backup.bin` loaded at `0x08000000`. Verified by static analysis only — no hardware in the loop.

> **Vector table caveat:** the table at `0x08000000` belongs to a bootloader and its peripheral slots are weak `b .` stubs. The application vector table is at `0x08004000` (`SCB->VTOR` written at `0x08000CB2` from the literal at `0x08000CD4`). `USART3_IRQHandler` = `0x0800BF39`, found at `0x080040DC` (= `0x08004000 + 55*4`).

### 1. Physical layer

| Item | Value | Where |
|---|---|---|
| Peripheral | USART3 @ `0x40004800` | literal at `0x0800C450` |
| Baud | 115200 (`0x1C200`) | `FUN_0800bfb2`, init struct `+0x00` |
| Format | 8 data, no parity, 1 stop | struct `+0x04/+0x06/+0x08` all zero |
| Mode | `0x0C` = Rx \| Tx | struct `+0x0a` |
| Flow control | none | struct `+0x0c` = 0 |
| `USART3->BRR` | **`0x0104`** (derived) | computed at runtime in `FUN_08018db0` |
| RX | RXNE interrupt, NVIC IRQ 39, preempt 1 | `FUN_08019050(USART3,0x525,1)`, `FUN_0800decc` |
| TX | DMA1 Stream3 Channel 4, blocking poll | `FUN_0800c154`, literals `0x40026058` / `0x40004804` |
| PC10 | USART3_TX, AF7, 50 MHz, PP, pull-up | `FUN_0800c2ea` |
| PC11 | USART3_RX, AF7, same; also EXTI11 for wake | `FUN_0800c2ea`, `FUN_0800bfaa` |
| PC13 | EM250 **nRESET**, active low | low → 1.4 ms → high in `FUN_0800bfb2` |

`FUN_08018db0` is ST's `USART_Init` (assert string `"@\CPU\ST\STM32\src\stm32f2xx_usart.c"` at `0x08019163`). Its BRR math is the stock formula and selects PCLK2 only for USART1/USART6, so USART3 runs off PCLK1.

PCLK1 re-derived from `SystemInit` (`0x08000C6A`–`0x08000CB6`): `PLLCFGR = 0x05403C19` → PLLM 25, PLLN 240, PLLP /2, HSE source, PLLQ 5; with a 25 MHz crystal SYSCLK = 120 MHz. `CFGR |= 0x1400` sets PPRE1 = `0b101` = /4 → **PCLK1 = 30 MHz** (PPRE2 = /2 → 60 MHz).

```
intdiv  = 25 * 30000000 / (4 * 115200) = 1627
mant    = 1627 / 100 = 16          -> 0x100
frac    = (27*16 + 50) / 100 = 4   -> 0x004
BRR     = 0x0104   (115384.6 baud, +0.16 %)
```

Zephyr side needs nothing exotic:

```dts
&usart3 {
    pinctrl-0 = <&usart3_tx_pc10 &usart3_rx_pc11>;
    pinctrl-names = "default";
    current-speed = <115200>;
    /* 8N1, no flow control = Zephyr defaults */
    status = "okay";
};
/* PC13 = EM250 nRESET, active low */
```

### 2. Wire framing

Encoder `FUN_08018A88`, decoder `FUN_08018B70`. Exact mirrors.

```
+------+---------------------------+----------+------+
| 0x81 | payload bytes (escaped)   | cksum    | 0x82 |
+------+---------------------------+----------+------+
```

* `0x81` = SOF. On receive it also resynchronises: seeing `0x81` mid-frame resets the index and keeps going.
* `0x82` = EOF.
* `0x80` = escape. Emitted **before** any payload or checksum byte equal to `0x80`, `0x81`, or `0x82`. The escaped byte follows **verbatim** — this is *not* the EZSP/ASH XOR-`0x20` scheme.
* `cksum` = two's complement of the 8-bit sum of the *unescaped* payload. The checksum itself is escaped if needed. On receive: sum every stored byte (payload **and** checksum); a valid frame sums to 0 mod 256.
* The receive callback gets `len = index - 1` — the checksum is stripped, the payload is not.
* Escape handling is tested before SOF/EOF, so escaped `0x81`/`0x82` are correctly data.

Encoder core, transcribed:

```c
/* ctx: +0 phase, +1 byte, +4 dst, +8 *idx, +0xc maxlen, +0x10 *cksum */
if (phase == 0) { *idx = 0; *cksum = 0; dst[(*idx)++] = 0x81; }
*cksum += byte;
if (byte == 0x80 || byte == 0x81 || byte == 0x82) dst[(*idx)++] = 0x80;
dst[(*idx)++] = byte;
if (phase == 2) {
    *cksum = (uint8_t)(~*cksum + 1);
    if (*cksum == 0x80 || *cksum == 0x81 || *cksum == 0x82) dst[(*idx)++] = 0x80;
    dst[(*idx)++] = *cksum;
    dst[(*idx)++] = 0x82;
    return *idx;                    /* then idx = cksum = 0 */
}
```

`phase` is 0 for the first byte of a frame, 2 for the last, 1 in between — which is how the three scatter-gather chunks in `FUN_0800c154` become one frame (`FUN_08018a24` with class 0 / 1 / 2, or `FUN_080189c0` for a single chunk). The checksum accumulator is shared across chunks; the output index is per-chunk with the destination pointer advanced by the caller.

**Wake-up burst.** Every transmission in `FUN_0800c154` is two DMA transfers:

1. raw bytes `81 81` (BufferSize = 2), poll to completion;
2. `FUN_080113a4(0x578)` — a TIM-based blocking delay, ≈1.39 ms if the tick is 1 µs;
3. the encoded frame.

So the line sees `81 81 <~1.4 ms gap> 81 …payload… cksum 82`. Total unescaped payload across all chunks must be `<= 0x80`, else `"ZBComm Send Length too long"` (`0x0800C4AC`).

### 3. Message layer

Inside the frame:

```
byte 0 : opcode
byte 1 : payload length (bytes after this header)
byte 2+: payload
```

`FUN_080206F6` writes both header bytes and returns the total frame length. On **receive**, byte 1 is never read — every parser validates against the de-framed length instead. A reimplementation should do the same and treat the RX length byte as untrusted.

#### Host → EM250 (transmit)

| Opcode | Payload len | Total | Encoder | Payload layout |
|---|---|---|---|---|
| `0x02` | 0 | 2 | `FUN_08020768` | — |
| `0x04` | 0 | 2 | `FUN_0802076c` | — |
| `0x06` | 3 | 5 | *(no encoder in this build)* | |
| `0x20` | 0x0E | 0x10 | `FUN_08020770` | `epan[0..7]`, `pan_lo`, `mode`, `chanmask` big-endian u32 |
| `0x21` | 0x0B | 0x0D | `FUN_080207c0` | `pan_lo`, `epan[0..7]`, `mode`, `cfg` |
| `0x22`–`0x24` | 8 | 10 | *(none)* | |
| `0x26` | 5 | 7 | `FUN_080207fe` | `pan_lo`, `addr[0..1]`, `mode`, `cfg` |
| `0x30` | 0 | 2 | `FUN_08020824` | — |
| `0x31` | 0 | 2 | `FUN_08020828` | — |
| `0x32` | 0 | 2 | `FUN_0802082c` | — |
| **`0x40`** | **1 (+appended)** | **3 (+n)** | **`FUN_08020830`** | **`dst`, then chunks 1 and 2 appended; byte 1 = `1 + n1 + n2`** |
| `0x50` | 0 | 2 | `FUN_0802084e` | — |
| `0x51` | 3 | 5 | `FUN_08020852` | `b`, `u16_hi`, `u16_lo` |
| `0x52` | 0 | 2 | `FUN_08020872` | — |
| `0x60` | 0 | 2 | `FUN_08020876` | — |
| `0x62` | 0 | 2 | `FUN_0802087a` | — |
| `0x64` | 1 | 3 | `FUN_0802087e` | `arg` |
| `0x66` | 1 | 3 | `FUN_08020892` | `arg` |
| `0x68` | 1 | 3 | `FUN_080208a6` | `arg` |
| `0x6A` | 1 | 3 | `FUN_080208ba` | `arg` |
| `0x6C` | 0 ⚠ | 2 | `FUN_080208ce` | writes `buf[2]` that is **never sent** — firmware bug |
| `0x74`,`0x75` | 8 | 10 | *(none)* | |
| `0x80` | 5 | 7 | *(none)* | |

`FUN_08019d38(id, arg)` is the dispatcher for the `0x6x` / `0x04` family (case 0→`0x60`, 1→`0x62`, 2→`0x64`, 3→`0x66`, 4→`0x68`, 5→`0x6A`, 6→`0x6C`, 10→`0x04`; case 0x0B returns cached network info to the app without transmitting).

#### EM250 → host (receive)

| Opcode | Parser | Min len | Meaning / dispatch |
|---|---|---|---|
| `0x01` | `FUN_080208e2` | 5 | Command response. `b[2]` = echoed command, `b[3]` = status, `b[4]` = extra |
| `0x03` | `FUN_080208fc` | 3 | `b[2]`; app event 7 |
| `0x05` | `FUN_0802090e` | 30 | Network info: two 8-byte blocks (`b[3..10]`, `b[0x16..0x1D]`), big-endian u16 at `b[0x0B..0x0C]`, nibble-split `b[0x10]`/`b[0x12]`. App event 10 |
| `0x07` | `FUN_080209a2` | 3 | Stack status ind, `b[2]` → obj+0x23. `0x90` = up/joined. App event 8 |
| `0x09` | — | — | Low buffers; sets low-buffer flag. App event 0x0C |
| `0x0A` | — | — | Normal buffers; clears flag. App event 0x0C |
| `0x41` | `FUN_080209b4` | 5 | **Send complete.** `b[2]` = status (0 = OK), `b[3]` |
| `0x42` | `FUN_080209ca` | 5 | Incoming data: `b[2]`,`b[3]`,`b[4]`, payload = `b[5..]`, len − 5 |
| `0x43` | — | — | counted only |
| `0x61` | `FUN_080209fc` | 17 | app event 0 |
| `0x63` | `FUN_08020a48` | 10 | app event 1 |
| `0x65` | `FUN_08020a76` | 5 | app event 2 |
| `0x67` | `FUN_08020a92` | 12 | app event 3 |
| `0x69` | `FUN_08020ac8` | 17 | app event 4 |
| `0x6B` | `FUN_08020b14` | 9 | app event 5 |
| `0x6D` | `FUN_08020b42` | 7 | app event 6 |
| `0x8E` | `FUN_08020b66` | 7 | app event 0x0D |

Two acknowledgement styles coexist:

* **`0x6x` family** — strict pairing, response opcode = request opcode + 1.
* **`0x2x`/`0x3x`/`0x4x`/`0x5x`** — acknowledged by opcode `0x01` with `b[2]` echoing the command. `0x40` additionally gets an asynchronous `0x41` completion.

Multi-byte fields are big-endian throughout (`CONCAT11(low_addr, high_addr)` in Ghidra output = first byte on the wire is the MSB).

### 4. Sending a button press

`FUN_0801944a` @ `0x0801944A` is the only producer of opcode `0x40`. It is reached through a function pointer (`0x0801944B`, stored at `0x0801DD58`, registered by `FUN_0801e270` at `0x0801DCB2`).

```c
void ZbxSendData(req) {            /* FUN_0801944a */
    lock(zbx + 0x38);              /* "ZBXport CS" */
    if (zbx->state != 9)      { twt_fail(); return; }   /* 9 = idle/ready */
    if (zbx->low_buffers == 1){ twt_fail(); return; }
    hdr.dst = req->u16_at_0;                    /* only the low byte reaches the wire */
    hdr.n   = (u8)req->len1 + (u8)req->len2;
    zbx->len[0] = ZbxEncode40(zbx + 0x48, &hdr);/* 3 -> [0x40][1+n][dst] */
    zbx->p[0]   = zbx + 0x48;
    zbx->p[1]   = req->ptr1;  zbx->len[1] = req->len1;
    zbx->p[2]   = req->ptr2;  zbx->len[2] = req->len2;
    (*zbx->send)(zbx);                          /* obj+0x2c -> FUN_0800c154 */
    zbx->state = 0x0B;  start_timer(100);
    unlock(zbx + 0x38);
}
```

Request struct (built by `FUN_0801eb04`): `+0x00` u16 destination, `+0x04` chunk1 ptr, `+0x08` chunk1 len, `+0x0c` chunk2 ptr, `+0x10` chunk2 len.

Chunk 1 is the TWT transport header (`FUN_0802155a`):

```
byte 0 : kind   (1, 2, 3=data, 5, 6=data-ack, 7; parser rejects > 7)
byte 1 : session id
byte 2 : ack / fragment    -- present only when kind == 3 or 6
         length = 3 for kinds 3 and 6, otherwise 2
```

Chunk 2 is the application payload. For a two-way system code (`FUN_080211bc`):

```
byte 0 : 0x02          -- TwTc "SysCode"
byte 1 : unit id       -- FUN_0800e830()
byte 2 : system code   -- the "%02bX" in "Sys Out: %02bX %04X"
byte 3 : cmd high      -- the "%04X"
byte 4 : cmd low
byte 5 : sequence      -- rec[0x14], incremented per send
         length = 6
```

`FUN_080211e6` builds the shorter `[0x04][seq]`, length 2.

#### Assembled frame

```
message : 40 0A dd  kk ss ff  02 uu cc ch cl qq
          |  |  |   \______/  \_______________/
          |  |  |    TWT hdr    app payload (6)
          |  |  +-- destination low byte
          |  +----- 1 + 3 + 6 = 0x0A
          +-------- opcode 0x40
```

Synthetic example (invented dst / session / codes — this is **not** a capture):

```
message  : 40 0A 01 03 11 FF 02 07 03 00 1F 05
sum      : 0x18E -> 0x8E
checksum : 0x72
wire     : 81 40 0A 01 03 11 FF 02 07 03 00 1F 05 72 82
preceded by: 81 81  <~1.4 ms>
```

No byte here is `0x80`/`0x81`/`0x82`, so no escapes are inserted.

#### Response sequence

```
host -> 0x40                        state 0x09 -> 0x0B, 100 ms timeout
EM   -> 0x01 [.. 0x40 status ..]
          status 0x00 : state 0x0B -> 0x0C, 300 ms timeout
          status 0x04 : state -> 0x0D, "!!!ZIGBEE UNI FAILURE! %x"
          other       : state -> 0x09, "!!!ZIGBEE OTHER FAILURE! %x", twt_fail
EM   -> 0x41 [status]               state 0x0C -> 0x09
          status 0x00 : twt_confirm(ok)   FUN_0801e638(1,1)
          else        : twt_confirm(fail) FUN_0801e638(1,2)
```

### 5. Keypad → radio path

```
FUN_080111ec              keypad matrix scan (PC0-7 cols, PE0/1/2/12-15 rows)
  -> FUN_080111b8         debounce: 3 identical reads 15 ms apart
  -> task @0x08011124     "Keypad Task"; 40 ms repeat; release publishes key 0
  -> FUN_08006c30(3,&key) -> FUN_08006e58 -> msg [0x03][keycode] on "CommandQ"
  -> FUN_08010450         "Cmd: _HandleHardkeyPress(): key(0x%X), cmdState(%d)"
  -> FUN_08009146         look up the key's programmed command record
  -> FUN_080105dc         "Cmd: _ExecuteCmds()", switches on (fmt & 0x0F):
        0  Fmt:Dflt   -> default from FUN_0800e8d8()
        9  Fmt:Sys IR -> FUN_08016830(0, ...)   bit-banged IR, NOT ZigBee
        10 Fmt:Sys RF -> FUN_08016830(1, ...)   bit-banged legacy RF, NOT ZigBee
        3  Fmt:Sys DC -> FUN_08010950
        3  Fmt:Sys TW -> FUN_0801cdaa           <-- the only ZigBee route
        else Fmt:Stand -> IR blast + FUN_0801b060 "MacroRun"
  -> FUN_0801cdaa         fill TwTc client record, kick FUN_0801ce8e
  -> FUN_0801d11e         "TwTc Client - SendSysCode"; FUN_080211bc builds the 6-byte payload
  -> FUN_0801e702         TWT session send ("TWT CS")
  -> FUN_0801eb04         transport pump; calls iface+0x08
  -> FUN_0801944a         ZBX command 0x40
  -> FUN_0800c154         framing + DMA over USART3
```

**Only "System TW" keys reach the EM250.** `FUN_08016830` looks like a radio send but is a bit-banged one-way transmitter emitting `id, ~id, code, ~code, cmd_hi, ~cmd_hi, cmd_lo, ~cmd_lo` repeatedly via `FUN_0801692a`, gating a GPIO (pin 14) around the burst. That is RTI's legacy 418/433 MHz path, not ZigBee.

Two special keys are intercepted before dispatch in `FUN_08010450`: `0xB4` (arms a 4 s timer, `cmdState = 4`) and `0x87` when the previous key was `0xB4` (a chord). Keycodes are the stock `128..180` values from the keymap at `0x08011344`.

### 6. Receive path

```
USART3 IRQ (0x0800BF38)
  reads DR, tail-calls FUN_08018b70(port=1, byte)     de-framer + checksum
  -> FUN_0800c400(ptr, len)                           registered rx callback
       buf = pool_alloc(ZBComm)          /* 70-byte block */
       memcpy(buf, ptr, len);            /* UNBOUNDED - see caveat */
       post msg [0x0D][len][ptr_be32] to queue 13
  -> ZBComm RX task @0x0800C3BC
       FUN_0800ffa6 parses the message, then FUN_080194f4(ptr, len)
       FUN_08018d08 frees the block
```

### 7. Object layouts

**ZBComm object @ `0x20006320`** (`DAT_0800c454`):

| Offset | Field |
|---|---|
| `+0x00` | pool base = obj + `0x650` |
| `+0x04` | u16 pool size = `0x2BE` (702) |
| `+0x06` | u16 block size = `0x46` (70) |
| `+0x08` | u8 bitmap bytes = 2 → exactly 10 blocks |
| `+0x14`..`+0x1C` | 9 bytes network info, memset `0xFF` |
| `+0x20` | radio-present flag (set from event 8, byte `0x90`) |
| `+0x24` | "ZBComm RX Q" handle (returned by `FUN_0800c148`) |
| `+0x50` | RX task stack, `0x180` bytes |
| `+0x650` | pool memory |
| `+0x910` | 8-byte EUI / extended PAN |
| `+0x918` | `0x80`-byte UART RX buffer |

**UART port struct** (single instance, `DAT_08018c4c`, stride `0x14`): `+0x00` id, `+0x04` buf, `+0x08` size, `+0x09` idx, `+0x0A` in_frame, `+0x0B` esc_pending, `+0x10` rx callback (`FUN_0800c400`).

**Zbx object @ `0x2000B064`** (both `DAT_08019e3c` and `DAT_0801a04c`):

| Offset | Field |
|---|---|
| `+0x00`/`04`/`08` | `p[0..2]` scatter-gather pointers |
| `+0x0C`/`0E`/`10` | `len[0..2]` |
| `+0x14`..`+0x1C` | network info from RX `0x05` |
| `+0x20` | state |
| `+0x21` | low-buffer flag |
| `+0x22` | role (0 or 4, from init) |
| `+0x23` | last stack status |
| `+0x26` | u16 PAN |
| `+0x28` / `+0x2A` | main / low-buffer timer handles |
| `+0x2C` | send callback = `0x0800C154` |
| `+0x30` | event callback = `0x0800C42C` |
| `+0x34` | u32 channel mask (`0xFFFFFFFF`) |
| `+0x38` | mutex "ZBXport CS" |
| `+0x40`..`+0x47` | 8-byte extended PAN ID |
| `+0x48` | TX frame scratch (= `0x2000B0AC`) |

State values at `+0x20`: `0` uninit, `1` opened, `2` query network, `3` router init, `4`–`8` join/leave sequence, `9` idle/ready, `0x0B` awaiting `0x01`/`0x40` response, `0x0C` awaiting `0x41`, `0x0D` failure hold.

### 8. Two stock-firmware bugs — do not replicate

1. **RX overflow.** `FUN_08018b70` delivers up to 127 bytes (buffer size `0x80`, `len = idx − 1`); `FUN_0800c400` then `memcpy`s that many bytes into a **70-byte** pool block with no bound check. Nothing in the chain clamps it — the de-framer only checks against `0x80`, and the message layer's own length byte is never read.
2. **Dropped argument on `0x6C`.** `FUN_080208ce` writes an argument at `buf[2]`, but `FUN_080206f6` gives `0x6C` the default payload length of 0, so the frame is 2 bytes and the argument never leaves the chip.

### 9. Reference implementation

```c
/* ZBX framing for the RTI T2i host <-> EM250 link on USART3, 115200 8N1.
 * Frame: 0x81 <payload, 0x80/81/82 escaped by a literal 0x80> <escaped cksum> 0x82
 * cksum = -sum8(payload), so sum8(payload) + cksum == 0.
 * Precede every frame with 0x81 0x81 and ~1.4 ms of silence (stock behaviour). */

#define ZBX_SOF 0x81
#define ZBX_EOF 0x82
#define ZBX_ESC 0x80
#define ZBX_MAX_PAYLOAD 0x80

static inline bool zbx_needs_esc(uint8_t b)
{
    return b == ZBX_ESC || b == ZBX_SOF || b == ZBX_EOF;
}

/* Worst case out size: 1 + 2*len + 2 + 1 */
size_t zbx_encode(const uint8_t *payload, size_t len, uint8_t *out, size_t out_sz)
{
    size_t n = 0;
    uint8_t ck = 0;

    if (len == 0 || len > ZBX_MAX_PAYLOAD || out_sz < 2 * len + 4) {
        return 0;
    }
    out[n++] = ZBX_SOF;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = payload[i];
        ck += b;
        if (zbx_needs_esc(b)) out[n++] = ZBX_ESC;
        out[n++] = b;
    }
    ck = (uint8_t)(~ck + 1);
    if (zbx_needs_esc(ck)) out[n++] = ZBX_ESC;
    out[n++] = ck;
    out[n++] = ZBX_EOF;
    return n;
}

struct zbx_rx {
    uint8_t buf[ZBX_MAX_PAYLOAD];
    uint8_t idx;
    bool in_frame;
    bool esc;
};

/* Feed one received byte. Returns payload length (checksum stripped) on a good
 * frame, else 0. Overlong frames are dropped, not truncated -- unlike stock,
 * which memcpy'd up to 127 bytes into a 70-byte pool block. */
size_t zbx_rx_byte(struct zbx_rx *r, uint8_t b)
{
    if (b == ZBX_ESC && !r->esc) { r->esc = true; return 0; }

    if (r->esc) {
        r->esc = false;
        if (r->in_frame) {
            if (r->idx >= sizeof r->buf) { r->in_frame = false; r->idx = 0; return 0; }
            r->buf[r->idx++] = b;
        }
        return 0;
    }
    if (b == ZBX_SOF) { r->in_frame = true; r->idx = 0; return 0; }  /* also resyncs */

    if (b != ZBX_EOF) {
        if (r->in_frame) {
            if (r->idx >= sizeof r->buf) { r->in_frame = false; r->idx = 0; return 0; }
            r->buf[r->idx++] = b;
        }
        return 0;
    }
    /* EOF */
    size_t out = 0;
    if (r->in_frame && r->idx >= 2) {
        uint8_t sum = 0;
        for (uint8_t i = 0; i < r->idx; i++) sum += r->buf[i];
        if (sum == 0) out = r->idx - 1u;      /* drop the trailing checksum */
    }
    r->in_frame = false;
    r->idx = 0;
    return out;
}

/* --- message layer --- */

/* Payload length for each command, from FUN_080206F6. */
static uint8_t zbx_cmd_len(uint8_t cmd)
{
    switch (cmd) {
    case 0x20:                     return 0x0E;
    case 0x21:                     return 0x0B;
    case 0x22: case 0x23: case 0x24:
    case 0x74: case 0x75:          return 0x08;
    case 0x06: case 0x51:          return 0x03;
    case 0x26: case 0x80:          return 0x05;
    case 0x40: case 0x64: case 0x66:
    case 0x68: case 0x6A:          return 0x01;
    default:                       return 0x00;
    }
}

/* Build a ZBX command 0x40 (send data) with the TWT header and app payload.
 * hdr/app are the two chunks stock firmware passes as separate scatter entries. */
size_t zbx_build_send(uint8_t dst,
                      const uint8_t *hdr, size_t hdr_len,
                      const uint8_t *app, size_t app_len,
                      uint8_t *msg, size_t msg_sz)
{
    size_t n = 3 + hdr_len + app_len;

    if (n > ZBX_MAX_PAYLOAD || n > msg_sz) return 0;
    msg[0] = 0x40;
    msg[1] = (uint8_t)(1 + hdr_len + app_len);
    msg[2] = dst;
    memcpy(msg + 3, hdr, hdr_len);
    memcpy(msg + 3 + hdr_len, app, app_len);
    return n;
}

/* TWT transport header: kinds 3 and 6 carry the ack/fragment byte. */
size_t twt_header(uint8_t *out, uint8_t kind, uint8_t session, uint8_t ack)
{
    out[0] = kind;
    out[1] = session;
    if (kind == 3 || kind == 6) { out[2] = ack; return 3; }
    return 2;
}

/* TwTc "SysCode" payload -- what a two-way button press actually carries. */
size_t twtc_syscode(uint8_t *out, uint8_t unit, uint8_t sys, uint16_t cmd, uint8_t seq)
{
    out[0] = 0x02;
    out[1] = unit;
    out[2] = sys;
    out[3] = (uint8_t)(cmd >> 8);          /* big-endian on the wire */
    out[4] = (uint8_t)cmd;
    out[5] = seq;
    return 6;
}
```

Round-trip self-check:

```c
static void zbx_selftest(void)
{
    uint8_t msg[ZBX_MAX_PAYLOAD], wire[2 * ZBX_MAX_PAYLOAD + 4], hdr[3], app[6];
    struct zbx_rx rx = {0};

    /* trivial frame: 81 30 00 D0 82 */
    const uint8_t p[] = { 0x30, 0x00 };
    size_t n = zbx_encode(p, sizeof p, wire, sizeof wire);
    assert(n == 5);
    assert(wire[0] == 0x81 && wire[3] == 0xD0 && wire[4] == 0x82);

    /* checksum property holds for the escaped case too */
    const uint8_t e[] = { 0x81, 0x01 };          /* payload contains a SOF byte */
    n = zbx_encode(e, sizeof e, wire, sizeof wire);
    assert(wire[1] == 0x80 && wire[2] == 0x81);  /* escaped, verbatim */

    /* a keypress round-trips through the decoder */
    size_t hl = twt_header(hdr, 3, 0x11, 0xFF);
    size_t al = twtc_syscode(app, 0x07, 0x03, 0x001F, 0x05);
    size_t ml = zbx_build_send(0x01, hdr, hl, app, al, msg, sizeof msg);
    assert(ml == 12 && msg[1] == 0x0A);
    n = zbx_encode(msg, ml, wire, sizeof wire);

    size_t got = 0;
    for (size_t i = 0; i < n; i++) got = zbx_rx_byte(&rx, wire[i]);
    assert(got == ml && memcmp(rx.buf, msg, ml) == 0);

    /* a corrupted byte must be rejected */
    memset(&rx, 0, sizeof rx);
    wire[3] ^= 0x01;
    got = 0;
    for (size_t i = 0; i < n; i++) got = zbx_rx_byte(&rx, wire[i]);
    assert(got == 0);
}
```

### 10. What is *not* established

* Whether the `0x81 0x81` wake burst and the 1.4 ms gap are an EM250 requirement or RTI belt-and-braces. Untestable without the radio remote.
* The exact time unit of `FUN_080113a4` — the divisor `DAT_080114FC` was not read, so "1.4 ms" assumes a 1 µs tick.
* Semantics of `msg[2]` in command `0x40`: it is the low byte of a 16-bit value from the TWT session (`session+0x1C`). Endpoint vs. short-address slot vs. link-table index is unresolved.
* Payload semantics of RX indications `0x61`, `0x63`, `0x65`, `0x67`, `0x69`, `0x6B`, `0x6D`, `0x8E` — field offsets are known, meanings are not.
* Meaning of the config getters `FUN_0800e830`, `FUN_0800e8ce`, `FUN_0800e8d4`, `FUN_0800e8d8`, `FUN_0800e8e0` (unit id, PAN, EUI64, default format, node index respectively — inferred from use sites, not proven).
* Byte 10 of command `0x20` and byte 2 of `0x21`/`0x26` is the low byte of a 16-bit field whose upper half is discarded. Whether that field is a PAN id, a channel, or a node type is unresolved.
* How Integration Designer's UI maps onto the `unit`/`sys`/`cmd16` triple in the TwTc SysCode payload.
* Commands `0x06`, `0x22`–`0x24`, `0x74`, `0x75`, `0x80` have length-table entries but no encoder in this build — either dead entries or used by a firmware variant.

### 11. Tooling used

A helper lives at `/private/tmp/claude-501/-Users-joeytelaak-Documents-GitHub-t2i-zephyr/41e801e3-bd38-4548-8d42-0df4fd7cc779/scratchpad/gz.py` (session scratchpad, will not survive). Modes: `d`/`dc` decompile (± callers), `c` callers, `x`/`xd` xrefs, `a` disassemble range, `fl` list functions in range, `dr` decompile range, `lit` scan the raw image for a 32-bit literal. Run it from the decomp directory with `$HOME/zephyrproject/.venv/bin/python`. The Ghidra project takes an exclusive lock, so serialise invocations — concurrent runs fail with `LockException`.
