# Zigbee Protocol

> Auto-saved from overnight research agent. Static analysis only — nothing here was tested on hardware.

**Summary:** The EM250 question splits cleanly into "can it be reflashed" (yes, almost certainly) and "can we write firmware for it" (no). I found a complete, valid Ember EM250 `.ebl` image embedded as a Windows resource of type `EBL` inside RTI's own `zbconfig.exe`, and fully reversed that tool's flash procedure: it opens the RTI USB dongle via FTDI D2XX at 115200 baud, watches the RX stream for the literal Ember bootloader prompt `BL >`, writes `'1'`, waits up to 3 s for the XMODEM-CRC start char `'C'`, then uploads the embedded `.ebl` in standard 128-byte XMODEM blocks. So the Ember standalone serial bootloader is demonstrably present and reachable on RTI EM250 hardware. The stock STM32 firmware, however, contains no XMODEM, no EBL parser, no CRC-16/XMODEM polynomial and no bootload strings at all — it cannot flash the radio, though it does share RTI's exact host framing codec (`0x81` SOF / `0x82` EOF / `0x80` escape / two's-complement 8-bit sum) with `zbconfig.exe`, byte-for-byte down to identical struct offsets. The blocker is not access, it is the toolchain: the EM250 is a 16-bit XAP2b core (Cambridge Consultants) built only by the proprietary, long-EOL xIDE compiler, with EmberZNet shipped as licensed pre-compiled XAP2b libraries — there is no GCC/LLVM backend and no obtainable SDK. Independently, zigbee2mqtt's `ember` driver requires EZSP v13 / EmberZNet 7.4.x+, EZSP is an EM260/SN260 NCP protocol the EM250 SoC never spoke, and z2m explicitly does not support even Series 0/1 parts — so an EM250 coordinator is impossible regardless. Recommendation: do not touch the radio; bridge button presses over the already-wired USB CDC now, and replace the radio daughterboard with an ESP32-C6 later, which also lets you drop Zigbee and z2m entirely.

**Open questions:**
- Does the T2i remote's own EM250 actually have the Ember standalone bootloader installed in flash bytes 0x0000-0x27FF? Everything proven about `BL >` and XMODEM comes from the ZB-Pro dongle image; the remote's radio flash has never been dumped. Answerable in ~1 month by putting a USB-serial adapter on the radio remote's EM250 UART (or the daughterboard connector's USART3 pins), sending CR at 115200 8N1, and watching for the literal `BL >`.
- Which RTI ZBX command makes the EM250 app launch its bootloader? zbconfig.exe sends 2-byte payloads 0x02 (VA 0x402084, 0x40236B) and 0x04 (VA 0x402229, 0x401FC0) and then watches for `BL >`, but I could not prove which, or whether the remote's app implements it at all. Would need the ZB-Pro dongle in hand, or the remote's EM250 image.
- Is there any surviving copy of the EmberZNet 4.x EM250 SDK plus a working xIDE/XAP2b compiler and license? I found no evidence of one. If the answer is genuinely no — which is my expectation — the EM250 path is closed permanently regardless of bootloader access, and this question can be retired.
- What exactly does `Step 3 Program ZM-24` do? Only one `.ebl` is embedded in zbconfig.exe, so reflashing a ZM-24 repeater would have to use `bootload-utils`' clone mode (`Start cloning ...` is in the image). Not established, and only matters if we ever want to bootload a *second* EM250 over the air from a first.
- Does the radio daughterboard connector carry anything beyond USART3 TX/RX and power — specifically an EM250 nRESET line, a bootloader strap, or the SIF pins (SIF_CLK/SIF_MISO/SIF_MOSI/nSIF_LOAD)? This determines whether an out-of-band bootloader entry or a SIF-based dump is possible, and it also defines the pin budget available to an ESP32-C6 replacement module. Requires opening the radio unit or probing the bench unit's empty connector.
- For the ESP32-C6 endgame: plain MQTT-over-WiFi, or native Zigbee joined to the existing mesh? The former is far less work and removes z2m from the design entirely; the latter only earns its keep if the remote genuinely needs to be a Zigbee device rather than just an MQTT publisher. This is a product decision, not a research one.

---

## Running our own firmware on the EM250 — verdict: don't

**Short answer: the radio is reflashable, but there is nothing we could ever build to flash into it, and even a perfect EM250 coordinator would be unusable by zigbee2mqtt. Abandon this path.**

The blocker is not access. Access is fine — better than expected. The blockers are (a) the XAP2b toolchain and (b) z2m's EZSP floor, and each one is fatal on its own.

---

### 1. Does the EM250 have Ember's standalone serial bootloader?

**Yes on RTI's EM250 hardware — proven, not assumed.** The EM250 datasheet says so directly:

> "When used with the EmberZNet stack, code is loaded into Flash memory over the air or by a serial link using a built-in bootloader in a reserved area of the Flash. Alternatively, code may be loaded via the SIF interface with the assistance of RAM-based utility routines also loaded via SIF."

And RTI's own tooling exercises it. `Integration Designer/zbconfig.exe` ("Wireless Repeater Configuration", `c:\Projects\zbconfig\1.2\Release\ZBConfig.pdb`) ships a **complete Ember EM250 `.ebl` image** as a PE resource and XMODEMs it into the ZB-Pro dongle.

#### The embedded .ebl

| property | value |
|---|---|
| resource | type name `EBL`, id 130, lang 1033 |
| file offset / size | `0x4650`, 98816 B (`0x18200`); real data ends `0x181CE`, rest `0xFF` pad |
| header | tag `0x0000`, len 60. `1420 e250` = version 0x1420, **chip signature 0xE250 = EM250** |
| platform string | `xap2b-em250-em250-ZBPro` (32-byte field at header+0x18) |
| program records | tag `0xFD03`, len 1026 = 2-byte **big-endian word** address + 1024 data bytes |
| end record | tag `0xFC04`, len 4 = `d1 60 4c 9b` |

The word-address reading is self-proving: addresses step `0x200` per 1024 data bytes (so 1 unit = 2 bytes), and `0x10000` words is exactly the EM250's 128 kB — which is right, XAP2b is a word-addressed 16-bit machine.

Reconstructed into a 128 kB image, the .ebl writes only:

```
0x02800 - 0x19BC8   95176 B   code + stack
0x1C000 - 0x1CAAE    2734 B   strings/consts (every ASCII string lands exactly here)
0x1DF32 - 0x1DFFC     202 B   small table
```

**Bytes `0x0000-0x27FF` (10 kB) are deliberately never written** — and that region holds the XAP2b reset vector. That is the datasheet's "reserved area of the Flash". Top `0x1E000-0x20000` (8 kB) is untouched too, consistent with Simulated EEPROM (`sim-eeprom.c` is in the image).

Reproduce:

```bash
cd "$HOME/Library/Mobile Documents/com~apple~CloudDocs/t2i-private/t2i research/Integration Designer"
strings -a -t x zbconfig.exe | grep -iE "xap2b|em250|ebl|bootload"
# EBL blob: dd if=zbconfig.exe bs=1 skip=$((0x4650)) count=98816 of=/tmp/zbpro.ebl
```

#### The entry sequence (fully reversed from zbconfig.exe)

115200 8N1 over the FTDI UART. `FT_Open` (ord 1) → `FT_SetBaudRate(h, 0x1C200)` (ord 7) → `FT_SetTimeouts(h,10,10)` (ord 17). Device found by walking `FT_GetDeviceInfoList` with stride `0x64` (= `sizeof FT_DEVICE_LIST_INFO_NODE`) matching `ID == 0x13BD1020` (USB VID `0x13BD`, PID `0x1020`).

1. **Send a carriage return.** Silabs docs: the bootloader prints its menu (`1. upload ebl` / `2. run` / `3. ebl info`) after receiving a CR, and *"scripts should use only the `BL >` prompt to determine when the bootloader is ready for input."*
2. **Wait for the literal 4 bytes `BL >`.** zbconfig's RX thread does exactly this — `FT_Read(h, buf, 0x80, &n)` then a sequential match against ASCII `"BL >"` at VA `0x404964`:
   ```
   0x004025F0  mov     dl, byte ptr [ebp + edi - 0x84]
   0x004025F7  movsx   eax, byte ptr [esi + 0x404964]   ; "BL >"
   0x00402601  cmp     ecx, eax
   0x00402605  inc     esi
   0x00402606  cmp     esi, 4
   0x0040260B  call    0x402a10                          ; matched -> BL handler
   ```
3. **Write ASCII `'1'`** (menu option 1 = upload ebl). VA `0x40496C` holds `"1"`, `0x404970` holds `"2"`:
   ```
   0x00402A3D  push    ecx            ; &written
   0x00402A3E  push    1              ; len
   0x00402A40  push    0x40496c       ; "1"
   0x00402A46  call    ebx            ; FT_Write
   0x00402A48  call    0x4029a0       ; wait for 'C'
   0x00402A61  call    0x401400       ; XMODEM send
   ```
4. **Wait up to 3000 ms for `'C'`** (`0x4029A0`: `cmp eax, 0xbb8` timeout, `cmp byte ptr [ebp-1], 0x43`).
5. **Upload as XMODEM-CRC**, 128-byte blocks (`0x401400`):
   - start char dispatch, 16 retries at 2000 ms: `0x43` `'C'` → CRC16 mode; `0x15` NAK → 8-bit-checksum mode; `0x18` CAN (twice) → abort. Give up ⇒ send `18 18 18`.
   - frame = `0x01` SOH, `blk` (from 1), `~blk`, 128 data bytes, then CRC-16/XMODEM stored **big-endian**, or one additive checksum byte in NAK mode. Short final block padded with `0x1A` (CTRL-Z).
   - 25 retries per block, then `0x04` EOT (`Sending EOT...`).

Payload source chain is closed — the resource *is* the upload:

```
0x00401941  push    0x4045e4        ; L"EBL"
0x00401946  push    0x82            ; 130
0x0040194C  call    FindResourceW
0x0040195A  call    SizeofResource
0x00401962  mov     dword ptr [0x4063bc], eax     ; length
0x00401967  call    LoadResource -> LockResource
0x0040197C  mov     dword ptr [0x4063c0], eax     ; base
```
and `0x401400` reads exactly `[0x4063C0]` / `[0x4063BC]`. Failure path: MessageBox `ZBConfig executable corrupted.`

**Could the STM32 trigger it?** Mechanically, yes — USART3 (PC10/PC11) is wired to the EM250's SC1 UART (GPIO9 TXD / GPIO10 RXD), and CR + `BL >` + `'1'` + XMODEM is maybe 100 lines. **But entry has to be in-band:** zbconfig delay-imports only ordinals 1, 3, 4, 7, 17, 70, 71 — `FT_Open`, `FT_Read`, `FT_Write`, `FT_SetBaudRate`, `FT_SetTimeouts`, `FT_CreateDeviceInfoList`, `FT_GetDeviceInfoList`. **No `FT_SetBitMode`, no `FT_ResetDevice`, no DTR/RTS.** It cannot strap a pin, so the running EM250 app must be what calls `halLaunchStandaloneBootloader()`. That means we need the remote app's command for it — see Unknowns.

---

### 2. Does stock RTI firmware contain anything that could bootload the radio?

**No. Clean negative, tested four ways.**

- **Strings.** Across `stock_flash_backup.bin` and `recovered_T2i_1x_fw_from_usb.bin`, `grep -iE "xmodem|ebl|bootload|SOH|nak|ack|ember|ezsp|ash|sif|em250|xap"` returns three hits, all about the T2i itself: `T2i Bootloader Version %d.%02d` (`0x0801DF05`), `SPI Flash Init`, `.\CPU\ST\STM32\src\stm32f2xx_flash.c`.
- **CRC.** Polynomial `0x1021` appears **zero** times as a 32-bit literal; reflected `0x8408` zero times; no CRC table. Constant 133 (XMODEM-CRC frame length) zero times.
- **Control bytes.** Thumb `CMP Rn,#imm8` scans: 13 sites for `'C'` 0x43, 6 for NAK 0x15, 7 for CAN 0x18 across 512 kB — noise. Only two weak pairs (`0x08007BE5`/`0x08007C24`, `0x080165AA`/`0x080165F9` — the latter in the SPI2/audio region) and **no site with all three**.
- **Semantics.** Every radio string is application-level: `ZBComm Init`, `ZBComm Send Length too long` (`0x0800C4AC`), `ZBXport CS` (`0x08019208`), `ZbxRx stack stat ind: state: %d`, `ZBX_STATS_GOING_NORMAL_FAIL`, `ZbxRx: wrong network`, `ZIGBEE!ROAM`, `!!!Resetting` (`0x0801A063`).

Nothing here is reusable. An STM32-side uploader would be written from scratch.

#### What we *did* get: the wire format, for free

`FUN_08018B70` in the STM32 is **the same source file** as zbconfig's `0x4012A0` — same logic, same struct offsets (+4 buf, +8 cap, +9 len, +0xA in-frame, +0xB escape-pending, +0x10 callback):

```c
/* FUN_08018B70(port, byte) -- STM32 stock, USART3 RX deframer */
if (param_2 == 0x80 && *(char *)(iVar2 + 0xb) == '\0') { *(iVar2+0xb) = 1; return 0; }  /* escape next */
if (*(char *)(iVar2 + 0xb) == '\x01') { /* append literal */ *(iVar2+0xb) = 0; return 0; }
if (param_2 == 0x81) { *(iVar2+10) = 1; *(iVar2+9) = 0; return 0; }                     /* SOF, len=0 */
if (param_2 != 0x82) { /* append to buf */ return 0; }
/* 0x82 EOF: */ cVar4 = 0; do { cVar4 = *pcVar5 + cVar4; ... } while (uVar6 != 0);
                if (cVar4 != '\0') goto LAB_08018bf4;                                   /* sum must be 0 */
                (**(code **)(iVar2 + 0x10))(*(undefined4 *)(iVar2 + 4), uVar3 - 1 & 0xff);
```

Encoder `FUN_08018A88` (matching zbconfig `0x401200`): emit `0x81`; accumulate `sum += b`; prefix `0x80` before any payload byte in {`0x80`,`0x81`,`0x82`}; at end write `~sum + 1` (escaped the same way) then `0x82`.

**RTI "ZBX" frame:** `0x81 | payload (0x80-escaped) | -sum(payload) | 0x82`, verified by the receiver summing payload+checksum to zero.

zbconfig sends 2-byte payloads — command `0x02` (`0x402084`, `0x40236B`) and command `0x04` (`0x402229`, `0x401FC0`) — then starts watching for `BL >`. Its inbound dispatcher (`0x402692`) handles message types 1..11 via index table `00 04 01 04 02 04 04 04 04 04 03` → handlers `0x402784 / 0x402854 / 0x4026C1 / 0x402821 / 0x402866(default)`.

So we can emit valid frames to the EM250 whenever we want. That is a genuinely useful result — just not for bootloading.

---

### 3. Why our own EM250 firmware is impossible

The EM250 is **not ARM**. It is a 16-bit **XAP2b** core from Cambridge Consultants, 12 MHz, 128 kB flash, 5 kB SRAM.

- The only compiler is **xIDE** ("a C-language compiler, assembler, source-level debugger, and graphical editing environment") — proprietary, Windows-only, EOL with the EM250 (~2011-2013). **No GCC, LLVM or clang backend for XAP2b exists.**
- EmberZNet PRO for the EM250 shipped as **licensed pre-compiled XAP2b libraries**. The extracted image's assert strings confirm it: `hal\micro\xap2b\em250\{flash,micro,token,sim-eeprom}.c`, `mac-em250.c`, `phy.c`, `analogue.c`, `mac-arbiter.c`, `scan.c`, `packet-buffer.c`, `association.c`, `route-discovery.c`, `neighbor.c`, `debug-sif.c`.

Reaching `BL >` is a door into an empty room. We have nothing to put through it.

---

### 4. Why zigbee2mqtt could not use it anyway

Three independent blocks:

1. **z2m's floor.** The `ember` driver supports firmware `7.4.x, 8.0.x, 8.1.x, 8.2.x, 9.0.x, 9.1.x` and hard-fails with *"NCP EZSP protocol version of XX does not match Host version 13"*. It "will only be compatible with EZSP v13 (EmberZNet 7.4.0.0 firmware) and later." `adapter: ezsp` is deprecated. Legacy `bellows` topped out at EZSP v4-v8; the EM250 is EmberZNet **4.x** era.
2. **z2m already refuses newer parts.** Listed under *Not recommended*: "All Series 0/1 based-devices — Older hardware, no longer supported by Silabs in newer firmware versions (>= 8.0.0)." The EM250 predates Series 0.
3. **EZSP was never an EM250 protocol.** The EM250 is a **SoC** — app runs on-chip. EZSP is the host protocol of the **EM260/SN260** network co-processor, "allowing developers to easily add ZigBee networking to their preferred application microprocessor via a fast SPI/UART interface." There is no EM250 NCP image to obtain, let alone build.

Being blunt: **the EM250 is not supportable by modern z2m, and no amount of reflashing changes that.**

---

### 5. Recommendation

**Leave the radio alone. It's inert on the bench unit anyway.**

#### Now — USB CDC bridge (zero new hardware)

USB OTG-FS is already wired (PA11/PA12) and the radio remote is USB-capable *without opening it*. Zephyr ships CDC-ACM; publish keypresses as lines, let a host script relay to MQTT. That's a config change plus a `printk`-shaped write — do not build a protocol.

```conf
# prj.conf
CONFIG_USB_DEVICE_STACK=y
CONFIG_USB_CDC_ACM=y
CONFIG_UART_LINE_CTRL=y
```

```c
/* keypad -> host. That's the whole bridge. */
static const struct device *cdc = DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart0));
static void key_event(uint8_t code, bool pressed)
{
    char line[24];
    int n = snprintk(line, sizeof line, "K %u %u\n", code, pressed);
    for (int i = 0; i < n; i++) uart_poll_out(cdc, line[i]);
}
```

Host side is `mosquitto_pub` in a `while read` loop. Skipped: framing, CRC, acks, a binary protocol — add when the link actually proves lossy.

#### Later — replace the daughterboard with an ESP32-C6

The radio is a daughterboard (absent on the bench unit), so this is a swap, not surgery. Wire it to the same USART3 pins.

And then **question the Zigbee requirement itself**: an ESP32-C6 does WiFi + MQTT natively. Publishing to your broker directly deletes the coordinator problem, the EZSP-version problem, and z2m from the design. If you specifically want the remote to appear as a Zigbee device in an existing mesh, the C6 also does native Zigbee (Zigbee 3.0 / ZCL) and z2m handles it via a small external converter — but that is strictly more work for a handheld that only ever sends button presses.

Rough ranking:

| option | verdict |
|---|---|
| Reflash EM250 with our own firmware | **impossible** — no XAP2b toolchain, no stack |
| Reflash EM250 as an EZSP NCP for z2m | **impossible** — EZSP is EM260-only; z2m needs v13 |
| Keep stock EM250, decode RTI's proprietary clusters in a z2m converter | theoretically possible, but needs the remote's join behaviour reversed and gets us a device on *RTI's* app layer; high effort, low payoff |
| **USB CDC → host → MQTT** | **works today, ~20 lines** |
| **ESP32-C6 daughterboard, MQTT over WiFi** | **best endgame; drops Zigbee entirely** |
| ESP32-C6 daughterboard, native Zigbee + z2m converter | works, more effort, only if mesh membership matters |

---

### Unknowns — stated plainly

- **Whether the T2i remote's own EM250 has the standalone bootloader installed.** Everything above about `BL >` comes from the ZB-Pro *dongle*. The remote's EM250 flash has never been dumped (bench unit has no daughterboard; radio unit can't be opened for ~1 month). RTI clearly built EM250 firmware with `bootload-utils.c` linked and reserved the bottom 10 kB, so it is *likely* the remote matches — but that is inference. It also no longer matters.
- **Which ZBX command launches the bootloader.** zbconfig sends `0x02` and `0x04` before watching for `BL >`. One is plausibly the trigger. Unproven, and the remote's app may not implement it at all.
- **How `Step 3 Program ZM-24` actually reflashes a repeater.** Only one `.ebl` is embedded, so it may use `bootload-utils`' clone mode (`Start cloning ...`). Not established.
- **Exact roles of the two reserved flash regions.** Bottom 10 kB = bootloader is strongly implied (reset vector lives there). Top 8 kB = SimEE is a guess from `sim-eeprom.c` being present.

