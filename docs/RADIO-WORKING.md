# The radio works: button presses reach the CA-1 over the air

**Summary:** The remote's EM250 was reflashed from RTI's locked TXBZB image to the **Telegesis ETRX2
AT-command firmware**, which turns the radio into an open, documented ZigBee end device driven by
plain `AT` commands over USART3. It joins the CA-1 (EM357/EZSP) as a secure end device using the
standard `ZigBeeAlliance09` link key — no RTI key needed — and unicasts button codes to the
coordinator, which resolves them to remote-contract commands. This is the end-to-end path that was
blocked all along by RTI's radio demanding a preconfigured key we could never obtain.

## Why this path

Every other avenue was a dead end, each for a concrete reason (see `ZIGBEE-PROTOCOL.md` and
`EM250-REFLASH.md`):

- **RTI TXBZB (stock radio):** an end device that only joins an RTI controller with RTI's
  preconfigured link key. The key lives only in the EM250 flash, is not a known default, and cannot
  be read out (no bootloader dump command, no memory-read opcode, and SIF probing is not available
  on this hardware). Blocked at the key exchange (`0xAC`/`0xAF`) from every reachable state.
- **RTI ZBPro (coordinator):** flashable, but a coordinator never sleeps — wrong for a battery
  remote — and it speaks RTI's proprietary CPNCT protocol. Abandoned.
- **Custom EM250 firmware:** needs the long-EOL EmberZNet 4.x XAP2b toolchain (bundled compiler +
  licensed stack libs), which is not obtainable. There is no EM250 NCP image, so EZSP is out too.

Telegesis ETRX2 is the escape: a **real** EM250 EmberZNet image (`xap2b-em250-em250-ETRX2`,
R3.0.8) that ships a friendly AT interface, joins any standard ZigBee network, lets us set the key,
and can run as a sleepy end device. It flashes over the same standalone bootloader (opcode `0x08`,
then XMODEM-CRC) that we already drive.

## How it flashes

`0x08` launches the EM250 standalone bootloader on USART3; `em250_flash_ebl()` uploads the image in
128-byte XMODEM-CRC blocks (`src/em250.c`). **This cannot brick the radio** — the image's program
records write only `0x02800` upward, leaving the bootloader's reserved `0x0000-0x2800` untouched, so
a failed upload just leaves an invalid app with the bootloader still resident. It is **not
undoable**, though: RTI's TXBZB exists in no file and cannot be read back.

Images are archived in `tools/rti/`:
- `etrx2.ebl` — Telegesis ETRX2 R3.0.8 (what's flashed now)

## The AT link

The Telegesis app runs at **19200 baud 8N1** on USART3 (the STM32 talks 115200 to the bootloader,
19200 to the app — `zbx_set_brr()` switches). `em250_at()` / `em250_at_wait()` in `src/em250.c`
send an AT line and collect the reply.

Bring-up, from `main.c`'s AT state machine:

```
ATI                                          -> R308X, EUI 000D6F0005C10BC4
ATS09=<ZigBeeAlliance09>:password            -> trust-centre link key (HA default)
ATS00=0010                                   -> channel mask: channel 15 only (bit 4)
ATS0A=0100:password                          -> use preconfigured TC link key
AT+JN                                        -> join; streams "JPAN:15,1A2B,..." on success
AT+UCAST:0000=K<code> <name>                 -> unicast the button to the coordinator (0x0000)
```

Telegesis's default link key already **is** `ZigBeeAlliance09`, so the join works even before S09 is
set; setting it explicitly makes it deterministic. Channel-locking to 15 (S00) makes the join fast
and sticky instead of scanning all channels and rejoining.

## The CA-1 side

`tools/ca1_coord.js` drives the EM357 over EZSP: form a network (channel 15, PAN `0x1a2b`, EPAN
`5432690000000001`) with `HAVE_PRECONFIGURED_KEY` and TC link key `ZigBeeAlliance09`, permit
joining, and print every `trustCenterJoinHandler` (a node joining) and `incomingMessageHandler`
(data received). It maps the `K<code>` payload to the remote-contract command (`volume_up`, `menu`,
…) exactly as `juno-driver` does — so the log reads `BUTTON code 138 -> remote::volume_up`.

## Proven on hardware

```
CA-1: NODE JOINED nodeId=0x8d7d eui=000d6f0005c10bc4        <- the remote's EM250 joins over RF
CA-1: MESSAGE from 0x8d7d : "BEAT0" ... "BEAT7"             <- ACKed unicasts arrive intact
remote: AT+UCAST:0000=BEAT2 -> SEQ:BC -> OK -> ACK:BC       <- reliable delivery confirmed
```

## What's left

- Wire the CA-1 receiver into Juno's `core-zigbee` / the `remote` contract so button commands route
  to rooms (the driver's mapping table already exists; this connects it to the live RF feed).
- Configure the Telegesis as a **sleepy** end device (S0A device-type bits) and gate the radio on
  keypress for battery life — the transport is proven; this is power tuning.
- The **AT passthrough over USB** (host sends arbitrary AT to the radio) is designed but deferred:
  it modifies `updater.c`, the anti-brick path, so it must be proven on the SWD unit and approved
  before touching the USB-only unit.
