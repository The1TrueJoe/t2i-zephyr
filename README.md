# t2i-zephyr — custom firmware for the RTI T2i remote

Custom [Zephyr](https://zephyrproject.org/) firmware for the RTI T2i handheld
remote (STM32F205VET6). Chain-loads from the stock RTI bootloader at
`0x08004000`, so the bootloader (sector 0) is never touched and the remote can
always be returned to stock.

## Status

- ✅ Custom Zephyr firmware boots and runs on the T2i.
- ✅ **USB CDC** works (the remote appears as a serial port).
- ✅ **On-device update receiver**: the firmware speaks the reverse-engineered RTI
  update protocol over USB, stages an incoming image to the external SPI-NOR
  (S25FL256S on SPI3), and lets the stock bootloader commit it — so a remote is
  **re-flashable over USB with no pins/SWD** (restore stock, or push a new build).
- ✅ Verified: restored the stock RTI app to a remote entirely over USB.
- ⏳ Hardware bring-up (buttons, LCD, speaker, front-panel LEDs, IR) — in progress.

## Build

```bash
cd ~/Documents/GitHub/t2i-zephyr
export ZEPHYR_SDK_INSTALL_DIR=$HOME/zephyr-sdk-1.0.1 ZEPHYR_TOOLCHAIN_VARIANT=zephyr
source $HOME/zephyrproject/zephyr/zephyr-env.sh
west build -b t2i . -- -DBOARD_ROOT=$HOME/Documents/GitHub/t2i-zephyr
```

Board definition is in `boards/st/t2i/`. Output: `build/zephyr/zephyr.bin`.

## Flashing (`tools/t2i_flash.py`)

**We do not distribute RTI's firmware.** The flasher always backs up the current
image first, so your restore image is your own copy, made from your own remote.

```bash
# Save a full backup of the current flash (keep this to restore later!)
python3 tools/t2i_flash.py backup

# Install custom firmware (auto-backs-up first, over SWD)
python3 tools/t2i_flash.py install build/zephyr/zephyr.bin

# Restore a saved backup over USB — NO pins (via the on-device receiver)
python3 tools/t2i_flash.py restore backups/t2i_backup_XXXX.bin
#   ...or over SWD:  python3 tools/t2i_flash.py restore backup.bin --swd
```

`tools/t2i_usb_uploader.py` is the lower-level USB uploader the receiver speaks to.

## TODO

- **Integration Designer compatibility** — let RTI's *official* software restore
  stock firmware directly (so a non-technical owner needs no tools). ID identifies
  a T2i by its exact USB descriptors; our firmware currently uses Zephyr's stock
  CDC-ACM layout, which ID rejects ("connect your remote"). The genuine descriptor
  set is captured (device class 0x02, DATA=interface 0, bulk EP 0x03/0x81, int EP
  0x82, bcdDevice 0x0100) — implementing a custom USB class that reproduces it,
  plus accepting ID's `0x82` page-count encoding, is the remaining work.
- Peripheral drivers: buttons, LCD, speaker, front-panel LEDs, IR emitters.

## Safety / legal

The stock RTI bootloader in sector 0 is never modified, so a remote can always be
recovered via SWD. Do not redistribute RTI firmware images; back up and restore
only your own.
