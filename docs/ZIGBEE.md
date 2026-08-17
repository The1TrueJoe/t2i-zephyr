# ZigBee: getting T2i button presses into zigbee2mqtt

Status: research only. **Nothing in this document has been executed on hardware.**
No flashing, no writes, no resets were performed.

Radio: **Ember/Silicon Labs EM250** (confirmed by the user out-of-band; the STM32-side
software evidence below is consistent with it but does not by itself name the part).

Architecture constraint: the user's own modern ZigBee dongle is the zigbee2mqtt
**coordinator**. The T2i only ever needs to be an end device or router on that network.
No add-on radio hardware; the existing EM250 daughterboard must be used.

---

## What to check first (morning checklist)

Ordered by information-per-minute. Items 1-3 need no soldering and no risk.

1. **Read the chip marking on the desoldered daughterboard.** Confirm EM250 visually and
   photograph the whole module. Look for a *module* part number as well as the die marking —
   if the daughterboard is an off-the-shelf module (Telegesis ETRX2, California Eastern Labs
   ZICM2410, MeshConnect) rather than a bare EM250 on RTI's own PCB, that changes the
   reflashing story completely, because those vendors shipped documented UART firmware-update
   tooling. Also note: pin count, whether there is a crystal, whether there is an external
   PA/LNA, and any 6-8 pin unpopulated header (candidate SIF/programming pads).

2. **Count and label the daughterboard pads.** We need VDD, GND, and the two USART3 lines.
   On the host side these are STM32 **PC10 = TX (host→radio)** and **PC11 = RX (radio→host)**,
   USART3, and the link runs at **115200 8N1, no flow control** (verified, see below).
   Ring out the daughterboard pads to those two STM32 pins on the mainboard to identify which
   daughterboard pad is which.

3. **Talk to the daughterboard from a PC with a USB-UART, before touching the STM32 at all.**
   This is the single highest-value experiment and it is entirely non-destructive. Power the
   module, wire a 3.3 V USB-UART at 115200 8N1, and send the RTI "identify" command:

   ```
   frame payload = 04 00
   on the wire   = 81 81   81 04 00 FC 82
                   ^^^^^   ^^ ^^^^^ ^^ ^^
                   preamble SOF payload cksum EOF
   ```

   (cksum = two's complement of the 8-bit sum of the payload: `-(0x04+0x00) & 0xFF = 0xFC`.)

   If the module answers with a ~30-byte frame starting `81 05 1C ...`, we have a live radio
   and a decoded protocol, and that frame contains **the radio's EUI64, its current extended
   PAN ID, and firmware/stack version fields** (exact offsets in the "Type 0x05" table below).
   That version data is the best available fingerprint of what firmware RTI shipped.

4. **Then try the join experiment** (Path C below). Open permit-join on the user's dongle, and
   send RTI's join command with the *dongle's* channel and extended PAN ID. Watch for a stack
   status indication of `0x90` (EMBER_NETWORK_UP). This either wins outright or returns a
   definitive negative in under an hour. Details and the exact bytes are in Path C.

5. Only if 3-4 fail: the reflash paths, which are much worse. See Path A / Path B.

**Do not** power the daughterboard above 3.3 V and do not assume the daughterboard pads are
5 V tolerant. The EM250 is a 2.1-3.6 V part.

---

## Verified vs inferred

Everything in the "Host-side protocol" section is **VERIFIED** from the stock image at
`/Users/joeytelaak/Library/Mobile Documents/com~apple~CloudDocs/t2i-private/firmware/stock_flash_backup.bin`
(loads at 0x08000000), by decompilation. Addresses are given so every claim is re-checkable.
Anything marked INFERRED is an interpretation, not an observation.

---

## 1. Radio identity from software evidence

The user has confirmed EM250 independently. For the record, here is what the STM32 image does
and does not support, because it bears on the later paths.

### What the image does NOT contain (VERIFIED, by exhaustive search)

| Searched for | Result |
|---|---|
| `EZSP`, `ASH`, `Ember`, `EM250`, `EM357`, `SiLabs`, `XNCP`, `ZNet` as strings | **absent** |
| `ZigBee` / `Zigbee` / `zigbee` as mixed-case strings | **absent** (only the all-caps `ZIGBEE!ROAM`, `!!! ZIGBEE UNI FAILURE - %x`, `!!! ZIGBEE OTHER FAILURE - %x`) |
| ZigBee HA well-known trust-center link key (`ZigBeeAlliance09`, bytes `5A 69 67 42 65 65 41 6C 6C 69 61 6E 63 65 30 39`) | **absent** |
| ZLL master key (`D0 D1 ... DF`) | **absent** |
| `Channel`, `PanId`, `PAN` as strings | **absent** |
| Any chip-ID table, bootloader query string, or expected-radio-version constant | **absent** |
| Any radio firmware update path | **absent** — the only update strings in the image concern the T2i's own STM32 image |

So: the STM32 image **cannot** identify the radio part for us, and it contains no facility for
reflashing the radio. That part of the earlier note in `HARDWARE.md` holds up.

### What the image DOES contain that points at EmberZNet underneath (VERIFIED)

1. **`EMBER_NETWORK_UP` is used as a literal.** The stack-status handler compares the radio's
   reported status byte against **0x90**, which is `EMBER_NETWORK_UP` in EmberZNet's
   `EmberStatus` enumeration, and `0x91` is `EMBER_NETWORK_DOWN`
   ([Silicon Labs status codes](https://docs.silabs.com/d/zigbee-stack-api/7.2.0/status-codes)).
   Two independent sites do this:
   - in `FUN_080194f4` at the type-0x07 branch: `if (*(char *)(iVar2 + 0x23) != -0x70)` — that is
     `!= 0x90`;
   - in the ZBX event callback at `0x0800C42C`, hand-disassembled:
     ```
     0800c42c  cmp   r0, #8          ; event id 8 == stack status indication
     0800c42e  bne   0800c442
     0800c430  ldr   r0, [pc, #0x20] ; -> 0x20006320  (ZBComm state)
     0800c432  ldrb  r1, [r1, #0]    ; status byte from the indication
     0800c434  cmp   r1, #0x90       ; EMBER_NETWORK_UP
     0800c436  ite   eq
     0800c438  movs  r1, #1
     0800c43a  movs  r1, #0
     0800c43c  strb  r1, [r0, #0x20] ; cached "network is up" flag
     0800c440  bx    lr
     ```
2. **An all-zero 8-byte extended PAN ID is treated as "no network".** At `0x0801A0FC` there are
   exactly eight `0x00` bytes, memcmp'd against the extended PAN ID the radio reports; a match
   logs `"ZbxRx: correct or no network"`. An all-zero extended PAN ID is precisely EmberZNet's
   "not on a network" representation.
3. **The join command carries (channel, 8-byte extended PAN ID, node type)** and nothing else —
   the exact argument shape of `emberJoinNetwork(EmberNodeType, EmberNetworkParameters*)`.
4. The debug vocabulary is Ember-shaped: `!!! ZBX LOW BUFFERS` / `!!! ZBX NORMAL BUFFERS`
   mirrors EmberZNet's buffer-low/buffer-normal callbacks.

**Strength of evidence: strong for "the radio runs an EmberZNet ZigBee PRO stack with RTI's
application on top". Zero evidence for the specific part number** — nothing in the image
distinguishes EM250 from EM357 from EM260. The confirmation had to come from the chip marking,
and did.

**Not EZSP.** RTI's host protocol is their own, verified byte-for-byte below. It is not EZSP and
its framing is not ASH (ASH uses 0x7E flag / 0x7D escape with XOR 0x20 / CRC-16; RTI uses
0x81/0x82/0x80 with an 8-bit negated sum).

---

## 2. Host-side protocol on USART3 ("ZBX") — fully reversed

### Link parameters (VERIFIED)

From `FUN_0800bfb2` (the ZBComm task bring-up), which fills an ST-library
`USART_InitTypeDef` on the stack and calls `USART_Init` (`FUN_08018db0`) with
`DAT_0800c450 = 0x40004800` = USART3:

| Field | Value | Meaning |
|---|---|---|
| `BaudRate` | `0x0001C200` | **115200** |
| `WordLength` | `0x0000` | 8 data bits |
| `StopBits` | `0x0000` | 1 stop bit |
| `Parity` | `0x0000` | none |
| `Mode` | `0x000C` | Rx \| Tx |
| `HardwareFlowControl` | `0x0000` | **none** |

`FUN_08018db0` is a verbatim ST `USART_Init` (it validates the struct against the same
0/0x1000/0x2000/0x3000, 0/0x400/0x600, 0/0x100/0x200/0x300 constant sets and computes BRR from
the APB clock), so the field offsets are certain.

**USART3 = 115200 8N1, no flow control.** Pins PC10 (TX) / PC11 (RX), AF7.

Transmission is **DMA1 Stream 3, Channel 4** (`DMA_InitTypeDef` built at `0x0800C194`ff with
peripheral address `0x40004804` = USART3_DR, direction `0x0040` = memory-to-peripheral). The
staging buffer is at `0x20006CB8`.

### Wire framing (VERIFIED — `FUN_08018a88` at 0x08018A88, plus the DMA setup at 0x0800C154)

```
      [0x81 0x81]  0x81   <escaped payload>   <escaped checksum>   0x82
       preamble    SOF                                             EOF
```

* **Preamble**: before each frame the TX path DMA-sends **two bytes of 0x81** as a separate
  transfer (loop at `0x0800C184`, transfer size 2 at `[sp+0x1c]`), waits for transfer-complete,
  then sends the encoded frame. INFERRED: this is a wake/sync burst for the EM250's UART.
* **SOF** = `0x81`, emitted once at the start of encoding.
* **EOF** = `0x82`.
* **Escape** = `0x80`. The three bytes `0x80`, `0x81`, `0x82` are escaped by emitting `0x80`
  followed by the **unmodified** byte (no XOR).
* **Checksum** = 8-bit sum of the *logical* (unescaped) payload bytes, then negated
  (`cksum = (-sum) & 0xFF`), appended before EOF, and itself escaped if it lands on
  0x80/0x81/0x82. Verification rule: `(sum(payload) + cksum) & 0xFF == 0`.
* SOF, EOF, escape bytes and the checksum are **not** included in the checksum.
* **Maximum total logical length is 128 bytes** — `FUN_0800C154` rejects anything where
  `hdr_len + seg1_len + seg2_len >= 0x81` and logs an error.

The TX path takes a three-segment gather descriptor, so a frame can be assembled from a header
plus two payload fragments:

```c
struct zbx_tx_desc {   /* passed to the transport at 0x0800C154 */
    void *hdr;      /* +0x00 */
    void *seg1;     /* +0x04 */
    void *seg2;     /* +0x08 */
    uint16_t hdr_len;   /* +0x0C */
    uint16_t seg1_len;  /* +0x0E */
    uint16_t seg2_len;  /* +0x10 */
};
```

### Payload format (VERIFIED — `FUN_080206f6` at 0x080206F6)

```
payload[0] = opcode
payload[1] = payload length (bytes after this one)
payload[2..] = arguments
total logical length = payload[1] + 2
```

`FUN_080206f6(buf, opcode)` writes `buf[0] = opcode`, `buf[1] = <fixed length for that opcode>`,
and returns `length + 2`. The complete length table, read directly out of that function:

| opcode | `payload[1]` | total | builder |
|---|---|---|---|
| 0x06 | 3 | 5 | (no builder in this module) |
| **0x20** | 0x0E | 16 | `FUN_08020770` |
| **0x21** | 0x0B | 13 | `FUN_080207c0` |
| 0x22 / 0x23 / 0x24 | 8 | 10 | (no builder in this module) |
| **0x26** | 5 | 7 | `FUN_080207fe` |
| **0x40** | 1 + N | 3 + N | `FUN_08020830` (**variable length**) |
| 0x51 | 3 | 5 | `FUN_08020852` |
| 0x74 / 0x75 | 8 | 10 | (no builder in this module) |
| 0x80 | 5 | 7 | (no builder in this module) |
| **all others** | 0 | 2 | see below |

Zero-payload commands with builders: **0x02** (`FUN_08020768`), **0x04** (`FUN_0802076c`),
**0x30** (`FUN_08020824`), **0x31** (`FUN_08020828`), **0x32** (`FUN_0802082c`),
**0x50** (`FUN_0802084e`), **0x52** (`FUN_08020872`), **0x60** (`FUN_08020876`),
**0x62** (`FUN_0802087a`).
Single-argument-byte commands: **0x64** (`FUN_0802087e`), **0x66** (`FUN_08020892`),
**0x68** (`FUN_080208a6`), **0x6A** (`FUN_080208ba`), **0x6C** (`FUN_080208ce`).

INFERRED: the opcodes with a length entry but no builder (0x06, 0x22-0x24, 0x74, 0x75, 0x80)
are the RTI *processor* side of the same shared library — the XP-6/XP-8 speaks the other half of
this protocol. That is consistent with `FUN_08019e40`'s unused "form network" branch (below).

### The critical observation for Path C

**The largest argument block in the entire command table is 14 bytes (opcode 0x20).** There is
no 16-byte payload anywhere. Therefore **the host cannot hand the radio a 128-bit ZigBee network
key or trust-centre link key.** There is no "set security state", "set preconfigured key", or
"set network key" command in RTI's protocol at all. Key material lives entirely on the EM250
side, in its own tokens/NVM. This is central to assessing Path C, and cuts both ways — see there.

### Network join / form command (VERIFIED — `FUN_08019e40` at 0x08019E40)

```c
FUN_08019e40(uint8_t node_type,      /* state+0x22 */
             uint16_t channel,       /* state+0x26 -- only the low byte reaches the wire */
             uint8_t *net_id,        /* state+0x40 -- 8 bytes, or 2 in the short form */
             uint8_t short_form);    /* state+0x24 */
```

Three encodings, selected by `node_type` and `short_form`:

**Opcode 0x21 — join, long form (13 bytes). This is what the T2i actually sends.**
```
21 0B <channel> <epid[0]..epid[7]> <subtype> <flags>
```
* `channel` — one byte.
* `epid[0..7]` — the 8-byte extended PAN ID (or the coordinator EUI64; see below).
* `subtype` — derived from `node_type`: `node_type==3 -> 2`, `node_type==4 -> 3`,
  `5 <= node_type <= 0x11 -> node_type-1`, otherwise `1`.
* `flags` — `state+0x25`, which the T2i sets to **0**.

**Opcode 0x26 — join, short form (7 bytes), used when `short_form != 0`.** The T2i never uses
this path (`short_form` is hard-coded to 0, see the config below).
```
26 05 <channel> <panid_lo> <panid_hi> <subtype> <flags>
```
Note this variant carries a **16-bit PAN ID** instead of the 64-bit extended PAN ID — so the
protocol *does* have a place for both, it just uses the extended form here.

**Opcode 0x20 — form network (16 bytes), only for `node_type` in {1, 2, 0x13}.** Unused by the
T2i.
```
20 0E <epid[0]..epid[7]> <channel> <role> <u32 big-endian>
```
`role` = 2/3/4 for node_type 1/2/0x13. The u32 is `state+0x34`, which the T2i config sets to
`0xFFFFFFFF`. INFERRED: a 32-bit channel mask ("any channel"), which would be the natural
companion to a form/scan operation.

### Where the network parameters come from (VERIFIED)

`FUN_0800bfb2` builds the config struct and calls `FUN_08019198` (the ZBX init):

```c
struct zbx_cfg {                    /* -> copied into the ZBX state at DAT_08019e3c */
    uint8_t  node_type;   /* +0x00  = (FUN_0800e8e0() == 0) ? 4 : 0        -> state+0x22 */
    uint16_t channel;     /* +0x02  = FUN_0800e8ce()                       -> state+0x26 */
    uint8_t *net_id;      /* +0x04  -> 8 bytes copied from FUN_0800e8d4()  -> state+0x40 */
    uint32_t extra;       /* +0x08  = 0xFFFFFFFF                           -> state+0x34 */
    void   (*send)();     /* +0x0C  = 0x0800C155  (the USART3 DMA transport, REQUIRED) */
    void   (*event_cb)(); /* +0x10  = 0x0800C42D  (the event callback shown above) */
    uint8_t  short_form;  /* +0x14  = 0                                    -> state+0x24 */
    uint8_t  flags;       /* +0x15  = 0                                    -> state+0x25 */
};
```

The three getters resolve to a **RAM config block at 0x20003EEC**:

| getter | reads | meaning |
|---|---|---|
| `FUN_0800e8ce` (0x0800E8CE) | `*(uint16_t*)(0x20003EEC + 0x1C)` = `0x20003F08` | ZigBee **channel** |
| `FUN_0800e8d4` (0x0800E8D4) | returns the constant pointer `0x20003F22` | the 8-byte **network identity** |
| `FUN_0800e8e0` (0x0800E8E0) | `*(uint8_t*)(0x20003EEC + 0x64)` = `0x20003F50`, mapped 1→1, 2→3, 4→2, 8→4, else 0 | node role selector |

The string `"General Data"` sits at `0x0800E9F4`, immediately after this pointer group in the
literal pool. INFERRED: `0x20003EEC` is an in-RAM copy of a "General Data" configuration record
loaded from the SPI flash (S25FL256S), i.e. the settings RTI's Integration Designer writes into
the remote. Practical consequence: **channel and extended PAN ID are host-owned parameters, not
baked into the radio's firmware.** In our own Zephyr firmware we simply choose them.

Because `short_form == 0` and `node_type ∈ {0, 4}`, the T2i always takes the **opcode 0x21**
branch with `subtype` = 1 (node_type 0) or 3 (node_type 4).

### Boot / join state machine (VERIFIED)

State lives at `state+0x20` (`FUN_08019e02` is the one-line setter). `FUN_0801a014(ms)` arms a
timeout; `FUN_0801a03e` cancels it.

1. `FUN_08019198` — init, state := **1**.
2. `FUN_08019234` — sends opcode **0x04** (zero payload), 10 s timeout, state := **2**.
3. Radio answers with a **type 0x05** indication (≥30 bytes). In state 2 the handler memcmps the
   reported extended PAN ID against the configured one:
   * match, **or** reported EPID is all-zero (`0x0801A0FC`) → logs
     `"ZbxRx: correct or no network"`, sends the **0x21** join command, 10 s timeout,
     state := **3**.
   * mismatch → logs `"ZbxRx: wrong network"` and first leaves: opcode **0x02** (via
     `FUN_08019e0a`, state := 7) or opcode **0x32** (via `FUN_08019faa`, state := 6), depending
     on `node_type`.
4. Command responses arrive as **frame type 0x01**: `01 03 <acked_opcode> <status> <extra>`
   (parsed by `FUN_080208e2`, which requires length ≥ 5 and extracts `rx[2..4]`). In state 3 a
   `0x21`/`0x26` response with status 0 advances toward joined; non-zero status is retried or
   escalated.
5. A **type 0x07** indication is the stack-status indication: `07 01 <EmberStatus>`
   (`FUN_080209a2`, length ≥ 3, extracts `rx[2]`). Status **0x90** = `EMBER_NETWORK_UP` is what
   states 4, 5 and 13 wait for; on 0x90 in state 13 the machine finally reaches state **11**
   (`0x0B`), which is the "joined and able to send" state (`FUN_0801944a` refuses to transmit
   unless `state == 9`... see the data path below).
6. Terminal failure states log `!!! ZIGBEE UNI FAILURE - %x` (unicast failed, status 0x04),
   `!!! ZIGBEE OTHER FAILURE - %x`, or `ZBX_STATS_GOING_NORMAL_FAIL`.

### Type 0x05 indication — the radio's identity/status report (VERIFIED — `FUN_0802090e`)

Requires length ≥ 30. Field map, using wire offsets into the payload:

| payload offset | goes to | INFERRED meaning |
|---|---|---|
| `[2]` | out[0x12] | status / result code |
| `[3..10]` | out[0..7] | 8 bytes — the radio's own **EUI64** |
| `[0x0B..0x0C]` | out[8..9] as big-endian u16 | 16-bit **node ID** (or PAN ID) |
| `[0x0D..0x0F]` | out[0x13..0x15] | version / ID bytes |
| `[0x10]` | out[0x16] = high nibble, out[0x17] = low nibble | **version, split into major.minor nibbles** |
| `[0x11]` | out[0x18] | version / build byte |
| `[0x12]` | out[0x19] = high nibble, out[0x1A] = low nibble | **second version, major.minor nibbles** |
| `[0x13..0x15]` | out[0x1B..0x1D] | version / ID bytes |
| `[0x16..0x1D]` | out[10..17] | 8 bytes — the **extended PAN ID** (this is the field that gets memcmp'd) |

Nine of those output bytes (`out[0x12..0x1A]`) are cached into the ZBX state at `state+0x14`
through `state+0x1C`, i.e. RTI keeps a persistent "radio info" record. **The two nibble-split
version bytes are the only firmware-version data anywhere in this system**, and they only exist
on the wire — so the answer to "what firmware is on the radio" is obtainable, but only by asking
the radio (checklist item 3), never from the STM32 image.

### Data path (VERIFIED)

`FUN_0801944a` is the send-application-payload entry point. It refuses to transmit unless
`state == 9`, and refuses if the low-buffer flag `state+0x21` is set (the flag driven by the
`!!! ZBX LOW BUFFERS` / `!!! ZBX NORMAL BUFFERS` indications, frame types 0x09 and 0x0A). It
then builds an **opcode 0x40** header via `FUN_08020830`, whose length byte is
`1 + seg1_len + seg2_len` — the variable-length case — and hands the transport a 3-segment
descriptor so the two application fragments are streamed without a copy. Its response is a
type-0x01 frame with `acked_opcode == 0x40`; status 0x04 produces
`!!! ZIGBEE UNI FAILURE`. So **0x40 = send unicast**.

Asynchronous inbound indications, each with its own frame type, are dispatched to the
application callback with these event ids:

| frame type | parser | callback event |
|---|---|---|
| 0x03 | `FUN_080208fc` | 7 |
| 0x05 | `FUN_0802090e` | 10 (0x0A) |
| 0x07 | `FUN_080209a2` | 8 |
| 0x09 / 0x0A | — | 12 (0x0C) — buffers low / normal |
| 0x41 | `FUN_080209b4` | — (join/leave result) |
| 0x42 | `FUN_080209ca` | — (feeds `FUN_0801e364`) |
| 0x43 | — | — |
| 0x61 | `FUN_080209fc` | 0 |
| 0x63 | `FUN_08020a48` | 1 |
| 0x65 | `FUN_08020a76` | 2 |
| 0x67 | `FUN_08020a92` | 3 |
| 0x69 | `FUN_08020ac8` | 4 |
| 0x6B | `FUN_08020b14` | 5 |
| 0x6D | `FUN_08020b42` | 6 |
| 0x8E | `FUN_08020b66` | 13 (0x0D) |

INFERRED: the 0x61-0x6D odd-numbered family are the responses to the 0x60-0x6C even-numbered
commands (0x60→0x61, 0x62→0x63, 0x64→0x65, 0x66→0x67, 0x68→0x69, 0x6A→0x6B, 0x6C→0x6D) — a
group of radio diagnostics/attribute reads dispatched by `FUN_08019d38`. The `ZIGBEE!ROAM`
string at `0x08010CBC` is referenced from `FUN_08010840`, well outside the ZBX module — that is
the application deciding to re-roam, not a protocol command.

---

## 3. The three candidate paths

_(this section is completed below, after the external research)_
