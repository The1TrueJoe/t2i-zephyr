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

### Residual risk, stated honestly

A fault **before** `safety_boot_check()` runs is not covered by any of the above:
`reset_hook.c`, Zephyr kernel init, and driver init that Zephyr runs before
`main` — which includes `lcd_hw_init()`. Keep that path boring.

`lcd_hw_init()` is the one to watch: it holds PC12 low for 3 s on a warm reset to
clear the latched backlight (see [BACKLIGHT.md](BACKLIGHT.md) §4b). Every step in
it is bounded and it feeds the watchdog across the delay, but anything added
there runs before the safety net exists.

### Before experimenting with the radio

Radio code is exactly the class of thing safe mode exists for. Bump
`FW_VERSION`, keep the SWD bench unit as the first target for anything new, and
verify the stamp after each USB update.
