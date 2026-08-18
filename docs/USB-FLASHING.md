# Flashing over USB only — verified procedure

The radio remotes have no SWD. USB is the only way in, so this path has to work
every time. **Verified end-to-end on hardware 2026-08-17:** an image was built
with `FW_VERSION "A"`, flashed over SWD, then a second image with
`FW_VERSION "B-over-usb"` was pushed over USB alone — the remote rebooted and
reported `T2i fw B-over-usb`. No SWD involved in the second step.

```bash
python3 tools/t2i_usb_uploader.py --image build/zephyr/zephyr.bin
```

Takes ~5-10 s for 8062 frames, then the device stages to SPI-NOR, resets, and the
RTI bootloader commits it to `0x08004000`. It re-enumerates about 10 s later.

Confirm what is actually running — this is the whole point of `FW_VERSION`, since
on a USB-only unit there is otherwise no way to tell whether an update committed:

```bash
python3 -c "import serial;s=serial.Serial('/dev/cu.usbmodem1101',115200,timeout=1);[print(s.readline().decode(errors='replace').strip()) for _ in range(8)]"
```

Bump `FW_VERSION` in [src/main.c](../src/main.c) before any update you intend to
verify. An unchanged stamp after an update means the commit did **not** happen.

## STOCK -> ours over USB does NOT work — measured 2026-08-18

Everything above was verified **ours -> ours**: a remote already running our firmware, updated by
our own `updater.c`. Going from **stock RTI** to ours over USB is a different path, and on the
radio unit (`MstrBdrm ID 40`) it **does not commit**.

What was tried, on a unit with no SWD:

1. Uploaded our image. All 8062 frames accepted, device reset. Stock still running.
2. Repeated with `KEY HELD`, watchdog and nRESET fixes. Same.
3. Replayed a **known-good stock image** (`recovered_T2i_1x_fw_from_usb.bin`, 507906 bytes, the
   capture the uploader was reverse-engineered from). **Also did not commit.**

So the transport is fine and the device commits *nothing* — not our image, not even a stock one.
The finalize trigger in our own implementation is `recv_cnt == declared && cksum == 0`, and the
uploader satisfies both by construction, so stock's app is either not reaching its own finalize or
its bootloader is rejecting the staged image for a reason not visible without SWD.

### How to tell which firmware is actually running

Do **not** use the device name. Our `updater.c` answers `0x83` with a **192-byte info blob copied
verbatim from stock**, so "Remote Technologies" proves nothing, and the name reads `Unknown` for
both our firmware and a stock unit whose config has been erased. That ambiguity cost a wrong
conclusion here. Compare the reply against `info_reply[]` byte for byte instead:

```bash
python3 -c "
import re,glob,serial,time
ours=bytes(int(x,16) for x in re.findall(r'0x([0-9a-fA-F]{2})',
    re.search(r'info_reply\[192\]\s*=\s*\{(.*?)\};', open('src/updater.c').read(), re.S).group(1)))
s=serial.Serial(sorted(glob.glob('/dev/cu.usbmodem*'))[0],115200,timeout=2)
s.write(bytes([0x83,0x81,0x80]+[0]*61)); time.sleep(2)
print('ours:', s.read(4096)[:192]==ours)"
```

Offsets 7 and 9 are the tell: stock reports `0x42`/`0x3c`, our captured blob has `0x10`/`0x27`.

### How stock's updater actually commits — decompiled 2026-08-18

Ghidra headless on `stock_flash_backup.bin` (base `0x08000000`). A commit needs **two** things, and
stock writes the second one itself:

| Condition | Where |
|---|---|
| internal marker at `0x0807FFFC` **erased** | so the bootloader does not just chain-load the app |
| `0xCDAB3412` present at **SPI `0x01FFFFFC`** | what tells the bootloader an image is staged |

**Bootloader `FUN_08000978`:**

```c
if (*(0x0807FFFC) == 0xCDAB3412)   // marker intact
    jump 0x08004004;               // chain-load the app, done
else {
    read(&m, 0x01FFFFFC, 4);       // SPI marker
    if (m == 0xCDAB3412)
        copy 0x1F0 * 0x400 bytes   // 507904 = exactly 0x08004000..0x0807FFFF
             from SPI 0x01F84000 -> flash 0x08004000;
}
```

**App finalize `FUN_0801F7F0`** — note it *verifies by read-back* and silently does nothing on
mismatch. No error, no reset, no commit:

```c
m = 0xCDAB3412;
write(&m, 0x01FFFFFC, 4);
flush();
read(&back, 0x01FFFFFC, 4);
if (back == m) {            // ONLY on a successful read-back
    ...
    invalidate(0x0807FFFC);
    reset();
}
```

**Data receiver `FUN_0801F714`** holds the trigger. State struct at `0x20002764`:
`+0x00` cksum8, `+0x01` progress %, `+0x04` SPI write pointer, `+0x08` **declared**,
`+0x0C` received, `+0x10` read-back verify buffer.

```c
last = (declared <= len + received);
if (last) len = declared - received;     // clamp
cksum8 += sum(bytes);                    // running 8-bit sum
... erase on 4K/64K boundary crossings, write, read back, compare ...
if (last && cksum8 == 0)                 // <-- the commit trigger
    FUN_0801F7F0();
```

So stock commits on `received == declared && cksum8 == 0` — **the same rule `updater.c`
implements**, and one `t2i_usb_uploader.py` satisfies by construction. Which means our uploads fail
*upstream* of this: `declared` (`state+0x08`) is never set to what we assume. `FUN_0801F6C8`
resets cksum, write-pointer and received but pointedly does **not** set `declared`, so the `0x82`
start frame handler does — and that handler is the next thing to decompile.

That also explains why replaying a byte-perfect stock image changed nothing: the payload was never
the problem.

#### `declared` is set conditionally, and the start can be refused

`FUN_0801F6D8` is the `0x82` start handler:

```c
uint FUN_0801f6d8(uint declared) {
    if (FUN_0800d0ca() != 0) return 1;      // refused - nothing below runs
    *(uint *)(state + 8) = declared;        // only now is `declared` set
    ...
    FUN_0800d120(state->spi_ptr);           // erase the first sector
    return (erase_ok ? 0 : 1);
}
```

`FUN_0800D0CA` is a SPI-NOR transaction (mutex, chip-select, command `0x00`, read status) in the
same family as `FUN_0800D074` (flush, command `0x8C`), `FUN_0800D120` (erase), `FUN_0800D1BE`
(write) and `FUN_0800D296` (read). If it returns non-zero the update never starts, `declared` stays
stale, and every data frame afterwards is a no-op.

**Our uploader throws that answer away.** `t2i_usb_uploader.py` sends the `0x82` frame and calls
`read_for(fd, 0.3)` without inspecting it, so a refusal is invisible and we send all 8062 frames
regardless. Reading and reporting that reply is the next step, and it is the cheapest way to learn
whether the start is being refused or `declared` is arriving wrong.

**Caution before testing:** `0x82` erases the first sector as part of starting, and
`FUN_0801F714` erases incrementally as the write pointer advances. There is no such thing as a
harmless partial update — that is what cost `MstrBdrm ID 40` its configuration.

Erasing is incremental, done inside `FUN_0801F714` as the write pointer crosses a 4 KB / 64 KB
boundary — not an upfront wipe. That is why a *partial* transfer still destroys whatever was in the
staging window, which is how `MstrBdrm ID 40` lost its configuration.

### Integration Designer CAN update it; our uploader still cannot — 2026-08-18

ID reloaded the firmware on the radio unit successfully. The `0x83` info reply proves it: bytes 7/9
went from `0x42 0x3c` (the original stock build) to `0x20 0x2b` (the build ID pushed). Ours would
be `0x10 0x27`.

Immediately afterwards, a full `t2i_usb_uploader.py --image` run **still did not commit**.

What that rules out, and it is most of the earlier suspicion:

* the SPI-NOR is healthy, so `FUN_0800D0CA` is not failing and the start is not being refused
* the bootloader commit path works
* the unit is not damaged

So the gap is in **our protocol**, not the hardware. ID does something the uploader does not.

Also established: **stock sends no reply to `0x82`.** `--start-only` returned 0 bytes, so accepted
and refused are indistinguishable from the host, and `FUN_0801F6D8`'s return value never reaches
USB. The original uploader discarding that read was not the bug.

**Next step: capture what ID actually sends.** USBPcap + Wireshark on the Windows VM while ID
performs an update gives the exact frame sequence, and diffing that against `upload()` in
`t2i_usb_uploader.py` ends the guesswork. Static analysis has gone as far as it usefully can here —
`FUN_0801F6D8` has no resolvable callers and no function-pointer table entry, so the USB command
dispatch is indirect in a way Ghidra has not cracked.

### Consequence for deployment

**A radio remote needs SWD once to bootstrap.** The SWD unit only accepts USB updates because it
was first flashed over SWD; USB-only updating works from our firmware onward, not from stock.
Until the stock-side commit is understood, a unit with no SWD cannot be converted.

### Cost incurred

The first upload erased the SPI staging window (`0x01F80000`-`0x02000000`), which on this unit held
RTI's configuration: the handshake name went from `MstrBdrm ID 40` to `Unknown` and did not come
back. The unit still runs stock and still enumerates, but its Integration Designer programming is
gone and would need re-pushing from the `.rti` project.

## What stops a bad image from bricking a remote

The RTI bootloader has **no USB of its own** (verified: sector 0 touches only
SPI3, GPIO and RCC). It can only commit an image already staged in SPI-NOR, and
staging is the application's job. So the one fatal failure is an application that
stops enumerating USB. Three layers exist to prevent that, all in
[src/safety.c](../src/safety.c):

1. **Watchdog** — IWDG at 8 s (PR=6, RLR=1000). A hang becomes a reset, not a
   dead remote. It cannot be stopped once started, so every long operation must
   feed it, including the multi-second SPI-NOR erase and the boot rail-cycle.
2. **Boot counter** — in RAM at `0x2001FF80`, survives warm resets, incremented
   every boot and cleared once the app proves healthy.
3. **Safe mode** — after 3 failed boots the next one comes up USB-and-nothing-
   else, so whatever is crashing (display, LVGL, touch, **radio**) cannot take
   the update path down with it.

A power cycle clears the counter, which is intended: pulling the battery gives a
clean slate.

## The boundary, measured 2026-08-17

`BRICK_TEST 2` hangs in `while (1)` immediately after `safety_boot_check()` —
i.e. **before** `updater_init()`. Result: the CDC port **never enumerated**,
checked every 10 s for a full minute. The watchdog reset the remote endlessly and
no boot ever brought USB up, so safe mode was unreachable. Recovered over SWD,
which writes to `0x08004000` and never touches the RTI bootloader at
`0x08000000`.

So the boundary is exactly where it looks:

| Fails where | Recoverable over USB? |
|---|---|
| After `updater_init()` — application code, drivers you start yourself, radio | **Yes** — hard fault included, verified |
| Before `updater_init()` — `reset_hook.c`, kernel init, Zephyr driver init | **No** — SWD only |

**Practical rule for new subsystems, including the radio: initialise it from
`main`, after `updater_init()`.** Never from a Zephyr driver-init hook or
`SYS_INIT` at `POST_KERNEL`, because those run before USB exists and put you in
the bottom row of that table.

### Attempted and reverted: deferring the display

Moving `lcd_hw_init()` after `updater_init()` via `zephyr,deferred-init` +
`device_init()` looked like the obvious way to shrink the pre-USB window — it is
the largest piece of our own code that runs there, and it holds PC12 low for 3 s.
**It broke boot** (remote came up silent in safe mode, no banner). The Zephyr LVGL
module has its own pre-`main` init that acquires the display device, so deferring
the display pulls the rug out from under it. Reverted. Anyone retrying this has to
defer the LVGL module too.

### Residual risk, stated honestly

A fault **before** `safety_boot_check()` runs is not covered by any of the above:
`reset_hook.c`, Zephyr kernel init, and driver init that Zephyr runs before
`main` — which includes `lcd_hw_init()`. Keep that path boring.

`lcd_hw_init()` is the one to watch: it holds PC12 low for 3 s on a warm reset to
clear the latched backlight (see [BACKLIGHT.md](BACKLIGHT.md) §4b). Every step in
it is bounded and it feeds the watchdog across the delay, but anything added
there runs before the safety net exists.

## Recovery from safe mode — verified 2026-08-17

The last link in the chain, exercised on hardware rather than assumed:

1. Built with `FORCE_UNHEALTHY 1`, which resets ~1.5 s into `main`, before
   `safety_mark_healthy()` is ever reached. Flashed over SWD.
2. The remote reset-looped 3 times and engaged safe mode on the 4th boot.
   Observed: the CDC port stayed enumerated and went **silent** — no boot banner,
   because nothing past `updater_init()` runs there.
3. Pushed a good image over **USB alone**, no SWD.
4. The remote came back reporting `T2i fw recovered-via-safe-mode`.

So a firmware that cannot survive its own boot is still recoverable over USB with
no SWD, which is the whole basis for working on a radio remote.

Re-run this after any change to boot order, USB init, or the watchdog: flip
`FORCE_UNHEALTHY` to 1 in [src/main.c](../src/main.c), flash, confirm the port
goes silent but stays present, then push a good image back.

### Harder case: a real hard fault — verified 2026-08-17

`FORCE_UNHEALTHY` is a *tidy* reset, which is the easy case. `BRICK_TEST 1`
executes `udf #0` — a guaranteed UsageFault -> hard fault — placed after the
safe-mode branch, exactly where a broken subsystem would sit. Result: three
faulting boots, safe mode on the fourth, port live and silent, good image pushed
over USB, remote came back as `T2i fw recovered-from-hardfault`. **A hard fault
in application code is fully recoverable with no SWD.**

Note for anyone repeating this: a store to an unmapped address is *not* a
reliable fault on this part. The first attempt used `0xFFFFFFF0`, which sits in
the vendor/PPB region — the write is simply ignored and the firmware ran on
normally. Use `udf #0`.

### Before experimenting with the radio

Radio code is exactly the class of thing safe mode exists for. Bump
`FW_VERSION`, keep the SWD bench unit as the first target for anything new, and
verify the stamp after each USB update.
