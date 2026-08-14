#!/usr/bin/env python3
"""
t2i_flash.py — backup / install / restore firmware on an RTI T2i.

SAFETY: `install` ALWAYS saves a full backup of the current flash first, so you
can always put the original back. We do NOT ship RTI's firmware — your backup is
your own copy, made from your own remote.

Commands:
  backup [FILE]              SWD-dump the full 512 KB internal flash to FILE
                             (default: backups/t2i_backup_<UTC>.bin).
  install CUSTOM.bin         SWD: auto-backup, then flash CUSTOM.bin at 0x08004000
                             (linked for 0x08004000) and mark it valid.
  restore BACKUP.bin         Put a saved image back. Default is over USB with NO
                             pins (via the on-device receiver); use --swd to use
                             the ST-Link instead.

Notes:
  * SWD ops need an ST-Link on the J4 SWD header (bench/installer side).
  * `restore` over USB needs the remote to be running our receiver firmware
    (which speaks the RTI update protocol). It restores the app region
    (0x08004000..0x0807FFFF); the RTI bootloader in sector 0 is never touched.
"""
import argparse, os, sys, subprocess, time, datetime, struct, glob

HERE = os.path.dirname(os.path.abspath(__file__))
APP_BASE = 0x08004000
FLASH_BASE = 0x08000000
FLASH_SIZE = 0x80000           # 512 KB
APP_OFF = APP_BASE - FLASH_BASE # 0x4000
APP_SIZE = FLASH_SIZE - APP_OFF # 0x7C000 = 496 KB
END_MAGIC = bytes.fromhex("1234abcd")   # 0xCDAB3412 app-valid marker

OCD = ["openocd", "-f", "interface/stlink.cfg", "-f", "target/stm32f2x.cfg",
       "-c", "adapter speed 480",
       "-c", "reset_config srst_only connect_assert_srst srst_nogate"]

def openocd(cmds, retries=12, need="", timeout=120):
    """Run an OpenOCD command list; retry (SWD examine is flaky on a live/asleep
    CPU) until `need` appears in the output or we run out of tries."""
    full = OCD + ["-c", "init", "-c", "halt"] + sum(([ "-c", c] for c in cmds), []) + ["-c", "exit"]
    last = ""
    for i in range(retries):
        r = subprocess.run(full, capture_output=True, text=True, timeout=timeout)
        out = r.stdout + r.stderr
        last = out
        if (need and need in out) or (not need and "Error" not in out):
            return out
        time.sleep(0.3)
    print(last, file=sys.stderr)
    raise SystemExit("OpenOCD failed after %d tries" % retries)

def do_backup(path=None):
    if not path:
        os.makedirs(os.path.join(HERE, "..", "backups"), exist_ok=True)
        ts = datetime.datetime.utcnow().strftime("%Y%m%d_%H%M%S")
        path = os.path.abspath(os.path.join(HERE, "..", "backups", f"t2i_backup_{ts}.bin"))
    path = os.path.abspath(path)
    print(f"[backup] dumping {FLASH_SIZE} bytes @0x{FLASH_BASE:08X} -> {path}")
    openocd([f'dump_image "{path}" 0x{FLASH_BASE:08X} 0x{FLASH_SIZE:X}'], need="dumped")
    sz = os.path.getsize(path)
    if sz != FLASH_SIZE:
        raise SystemExit(f"backup wrong size ({sz}) — aborting")
    print(f"[backup] OK ({sz} bytes) -> {path}")
    return path

def swd_flash(image, base=APP_BASE, sectors="1 3"):
    img = os.path.abspath(image)
    print(f"[swd] flashing {img} @0x{base:08X}")
    openocd([
        "mww 0x40023808 0",                 # HSI clock so the flash loader is happy
        "flash probe 0",
        f"flash write_image erase \"{img}\" 0x{base:08X} bin",
        f"flash fillw 0x0807FFFC 0x{int.from_bytes(END_MAGIC[::-1],'big'):08X} 1",
        "mww 0xE000ED0C 0x05FA0004",        # SYSRESETREQ -> boot it
    ], need="wrote")
    print("[swd] done + reset")

def build_payload(fw, total=507906):
    """Pad a raw app image, place the marker + zero-sum checksum (receiver format)."""
    declared = total - 1
    body = bytearray(total)
    if len(fw) > declared - 6 and len(fw) != APP_SIZE:
        raise SystemExit("image too big for payload")
    body[:len(fw)] = fw[:total]
    body[declared-5:declared-1] = END_MAGIC
    s = sum(body[:declared-1]) & 0xFF
    body[declared-1] = (-s) & 0xFF
    body[declared] = 0xCC
    assert (sum(body[:declared]) & 0xFF) == 0
    return bytes(body)

def usb_restore(backup):
    """Push the app region of a saved full-flash image over USB via the receiver."""
    data = open(backup, "rb").read()
    app = data[APP_OFF:APP_OFF+APP_SIZE] if len(data) >= FLASH_SIZE else data
    # append checksum + trailing so declared-byte sum == 0 (full-size app image)
    s = sum(app) & 0xFF
    payload = app + bytes([(-s) & 0xFF, 0xCC])
    sys.path.insert(0, HERE)
    import t2i_usb_uploader as up
    dev = up.find_port()
    print(f"[usb] restoring {len(app)} B app region via {dev} (receiver stages -> bootloader commits)")
    up.upload(dev, payload, len(payload) - 1)
    print("[usb] sent; device should stage -> reset -> commit -> boot the restored image")

def main():
    ap = argparse.ArgumentParser(description="Backup/install/restore RTI T2i firmware")
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("backup"); b.add_argument("file", nargs="?")
    i = sub.add_parser("install"); i.add_argument("custom")
    r = sub.add_parser("restore"); r.add_argument("backup"); r.add_argument("--swd", action="store_true")
    a = ap.parse_args()

    if a.cmd == "backup":
        do_backup(a.file)
    elif a.cmd == "install":
        print("[install] backing up current firmware first (safety) ...")
        bk = do_backup()
        print(f"[install] backup saved: {bk}\n[install] now flashing custom firmware ...")
        swd_flash(a.custom)
        print(f"[install] done. Keep {bk} — it's how you restore this remote.")
    elif a.cmd == "restore":
        if a.swd:
            swd_flash(a.backup, base=FLASH_BASE, sectors="0 7")  # full-flash restore
        else:
            usb_restore(a.backup)

if __name__ == "__main__":
    main()
