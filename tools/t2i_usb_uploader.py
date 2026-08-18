#!/usr/bin/env python3
"""
t2i_usb_uploader.py  — push a firmware image to an RTI T2i over USB.

FULLY reverse-engineered (2026-08-12). The T2i is a plain USB CDC-ACM device; the
firmware-update protocol runs over its CDC DATA endpoints (EP 0x03 OUT / 0x81 IN),
which macOS exposes as the /dev/cu.usbmodem* serial port. There is NO separate
"enter update mode" command — just open the port and speak the protocol.

Protocol (each frame = one 64-byte USB packet; byte0 = type):
  0x83 0x81 0x80                 -> init/handshake; device replies on IN with info
  0x82 0x00 <declared u32LE>      -> start download; byte1 is a SUBCOMMAND (0x00 = update),
                                     declared is read from frame offset 2 (stock: ldr.w r0,[r4,#2])
  0x80 + 63 firmware bytes       -> data block (payload is chunked 63B/frame)
The device stages the image to external SPI, then reboots and the bootloader
commits it to internal flash (app region 0x08004000..0x0807FFFF).

Checksum (reversed): running 8-bit sum, seed 0; the 8-bit sum of the first
`declared` bytes of the payload must be 0. build_payload() sets the last summed
byte to satisfy that. Firmware is PLAINTEXT, no signature.

Firmware image must be linked for base 0x08004000 and padded to the full app
region. total_size defaults to the captured 507906 (= 8062 x 63) bytes.

  # transport/protocol test: replay the known-good recovered image
  python3 t2i_usb_uploader.py --replay recovered_T2i_1x_fw_from_usb.bin

  # push your own firmware (linked for 0x08004000)
  python3 t2i_usb_uploader.py --image my_fw.bin

Test on the SWD-connected unit first (SWD verifies the commit / un-bricks).
"""
import os, sys, glob, struct, time, termios, select, signal, argparse

END_MAGIC = bytes.fromhex("1234abcd")
# Per-frame delay, measured from a real Integration Designer update captured over ETW:
# 8064 frames in 16.0 s = 1.98 ms/frame. Going faster is not a speed win, it is a reset: at
# 0.6 ms/frame the remote rebooted around frame 6300 every time, mid-transfer.
PACE_S = 0.002

# Where the remote stops listening. FUN_0801F714 erases a 64 KB SPI sector whenever the staging
# write pointer crosses a 0x10000 boundary, and a sector erase takes on the order of a second.
# During it the device drains no USB, the host CDC buffer fills, and macOS returns ENXIO and throws
# the queued bytes away -- which is why uploads died at a random frame and never finalized.
# Staging starts at SPI 0x01F84000 (bootloader literal DAT_08000A74).
SPI_STAGE_BASE = 0x01F84000
ERASE_PAUSE_S = 1.5
FRAME = 64
DATA_PER_FRAME = 63

def build_payload(firmware: bytes, total_size: int) -> bytes:
    """[firmware | zero pad | END_MAGIC | adjust]  (declared bytes sum to 0)  + trailing byte."""
    declared = total_size - 1
    body = bytearray(total_size)
    if len(firmware) > declared - 6:
        raise SystemExit("firmware too big for total_size")
    body[:len(firmware)] = firmware
    body[declared-5:declared-1] = END_MAGIC
    s = sum(body[:declared-1]) & 0xFF
    body[declared-1] = (-s) & 0xFF           # last summed byte -> sum(0:declared)==0
    body[declared] = 0xCC                     # trailing byte (past declared, ignored)
    assert (sum(body[:declared]) & 0xFF) == 0
    return bytes(body)

def find_port(explicit=None):
    if explicit: return explicit
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        raise SystemExit("No /dev/cu.usbmodem* found — is the T2i plugged into this Mac?")
    return ports[0]

def open_serial(dev):
    print(f"opening {dev} (first open can stall ~25s while the CDC line settles)...")
    t0 = time.time()
    fd = os.open(dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)  # don't block on carrier
    print(f"  opened in {time.time()-t0:.1f}s")
    a = termios.tcgetattr(fd)
    a[0] = 0; a[1] = 0
    a[2] = a[2] | termios.CLOCAL | termios.CREAD
    a[2] = (a[2] & ~termios.CSIZE) | termios.CS8
    a[2] = a[2] & ~termios.PARENB & ~termios.CSTOPB
    a[3] = 0
    a[6][termios.VMIN] = 0; a[6][termios.VTIME] = 0
    a[4] = a[5] = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW, a)
    termios.tcflush(fd, termios.TCIOFLUSH)
    return fd

def frame(type_byte, body=b""):
    b = bytearray(FRAME); b[0] = type_byte
    b[1:1+len(body)] = body[:FRAME-1]
    return bytes(b)

def write_all(fd, data, retries=200):
    """Write, tolerating a device that stops accepting while it erases.

    Stock erases a 64 KB SPI sector as the write pointer crosses a boundary, which takes on the
    order of a second. During that it stops draining USB, macOS's CDC buffer fills, and a blind
    write returns ENXIO ("Device not configured") rather than blocking — which killed an upload
    around frame 6000 every time. Waiting for writability and retrying rides it out."""
    mv = memoryview(data); off = 0
    while off < len(mv):
        try:
            off += os.write(fd, mv[off:])
        except BlockingIOError:
            select.select([], [fd], [], 1.0)
        except OSError as e:
            if e.errno != 6 or retries <= 0:
                raise
            retries -= 1
            select.select([], [fd], [], 0.05)
    return


def _write_all_unused(fd, data):
    mv = memoryview(data); off = 0
    while off < len(mv):
        try:
            off += os.write(fd, mv[off:])
        except BlockingIOError:
            select.select([], [fd], [], 1.0)

def read_for(fd, secs):
    dl = time.time() + secs; buf = bytearray()
    while time.time() < dl:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            try: c = os.read(fd, 4096)
            except BlockingIOError: continue
            except OSError: break
            if c: buf += c
    return bytes(buf)

def upload(dev, payload, declared):
    if len(payload) % DATA_PER_FRAME != 0:
        print(f"warning: payload {len(payload)} not a multiple of {DATA_PER_FRAME}")
    fd = open_serial(dev)
    try:
        # BLOCKING writes on purpose. With O_NONBLOCK, macOS's CDC driver returns ENXIO
        # ("Device not configured") the moment its buffer fills while the remote is busy erasing
        # a 64 KB sector — and the bytes already queued are lost, so the device's byte count and
        # checksum end up short and it silently never finalizes. Blocking makes the kernel wait
        # for the device instead of throwing the transfer away.
        os.set_blocking(fd, True)
        # 1) init
        write_all(fd, frame(0x83, bytes([0x81, 0x80])))
        info = read_for(fd, 1.5)
        print(f"init reply ({len(info)}B): {info[:48].hex()}")
        if b"Remote Technologies" in info:
            print("  -> device responded with its info; we're speaking the protocol. ✓")
        elif not info:
            print("  -> no reply. If this is the running app it should still accept the data;"
                  " continuing.")
        # 2) start + declared size
        write_all(fd, frame(0x82, bytes([0x00]) + struct.pack("<I", declared)))
        read_for(fd, 0.3)
        # 3) data blocks
        n = len(payload) // DATA_PER_FRAME
        print(f"sending {len(payload)} bytes in {n} data frames...")
        t0 = time.time()
        reconnects = 0
        i = 0
        while i < n:
            try:
                write_all(fd, frame(0x80, payload[i*DATA_PER_FRAME:(i+1)*DATA_PER_FRAME]))
            except OSError as e:
                # ENXIO here is the HOST's CDC endpoint giving up, not the remote dying: it still
                # answers 0x83 afterwards. The device stays in its receive state, so reopening the
                # port and continuing from the same frame resumes the transfer rather than
                # restarting it. Without this the upload dies at a random frame every time.
                if e.errno != 6 or reconnects >= 40:
                    raise
                reconnects += 1
                print(f"\n  CDC dropped at frame {i}/{n}; reopening ({reconnects})...", flush=True)
                try:
                    os.close(fd)
                except OSError:
                    pass
                time.sleep(1.0)
                for _ in range(60):
                    try:
                        fd = open_serial(find_port(None))
                        os.set_blocking(fd, False)
                        break
                    except (SystemExit, OSError):
                        time.sleep(1.0)
                else:
                    raise
                continue
            i += 1
            # Pace to roughly what the device can absorb. Without this we outrun its SPI writes
            # and overflow the host-side buffer long before the transfer ends.
            if PACE_S:
                time.sleep(PACE_S)
            # About to cross a 64 KB sector boundary: give the device time to erase it.
            a0 = SPI_STAGE_BASE + i * DATA_PER_FRAME
            if (a0 >> 16) != ((a0 + DATA_PER_FRAME) >> 16):
                print(f"  ... 64K boundary at 0x{a0 + DATA_PER_FRAME:08X}, pausing for erase",
                      flush=True)
                time.sleep(ERASE_PAUSE_S)
            if i % 1000 == 0:
                read_for(fd, 0.0)  # drain any acks
                print(f"  frame {i}/{n}", flush=True)
        print(f"done in {time.time()-t0:.1f}s. Draining final reply...")
        print("final reply:", read_for(fd, 2.0)[:48].hex())
        print("Device should now stage->finalize->reset->commit. VERIFY over SWD.")
    finally:
        os.close(fd)

def main():
    ap = argparse.ArgumentParser()
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--replay", metavar="FILE", help="send a file that already includes the "
                   "footer/checksum (e.g. recovered_T2i_1x_fw_from_usb.bin) — transport test")
    g.add_argument("--image", metavar="FILE", help="raw custom firmware (linked for 0x08004000); "
                   "a valid footer/checksum is appended")
    g.add_argument("--probe", action="store_true", help="SAFE: send only the 0x83 init and print "
                   "the reply (no erase/commit) — confirms we're speaking the protocol")
    g.add_argument("--start-only", action="store_true",
                   help="DESTRUCTIVE: send 0x83 then 0x82 (start) and print the reply, sending NO "
                        "data frames. Stock's 0x82 handler (FUN_0801F6D8) refuses the update and "
                        "returns 1 when its SPI check fails, and the normal upload path throws "
                        "that answer away — this is how you see it. It still erases the first "
                        "sector, so it is not a safe probe; see docs/USB-FLASHING.md.")
    ap.add_argument("--dev", help="serial device (default: first /dev/cu.usbmodem*)")
    ap.add_argument("--size", type=lambda x: int(x, 0), default=507906,
                    help="total payload size (default 507906 = 8062*63, from capture)")
    a = ap.parse_args()
    dev = find_port(a.dev)
    if a.probe:
        fd = open_serial(dev)
        try:
            os.set_blocking(fd, False)
            write_all(fd, frame(0x83, bytes([0x81, 0x80])))
            info = read_for(fd, 2.0)
            print(f"init reply ({len(info)}B): {info[:64].hex()}")
            if b"Remote Technologies" in info:
                print("  ✓ device replied with its info — the protocol works over the serial port.")
            elif info:
                print("  got a reply (decode above); protocol likely working.")
            else:
                print("  no reply — see notes; may need pyusb/raw-EP or a different framing.")
        finally:
            os.close(fd)
    elif a.start_only:
        fd = open_serial(dev)
        try:
            os.set_blocking(fd, False)
            write_all(fd, frame(0x83, bytes([0x81, 0x80])))
            info = read_for(fd, 2.0)
            print(f"0x83 info reply: {len(info)}B")
            declared = a.size - 1
            print(f"sending 0x82 start, declared={declared} (0x{declared:08X})")
            write_all(fd, frame(0x82, bytes([0x00]) + struct.pack("<I", declared)))
            reply = read_for(fd, 3.0)
            print(f"0x82 reply: {len(reply)}B  {reply[:64].hex() or '(nothing)'}")
            if not reply:
                print("  -> no answer. Either the start was accepted silently, or the reply "
                      "rides a channel we are not reading.")
            print("NO data frames were sent. The first sector has been erased regardless.")
        finally:
            os.close(fd)
    elif a.replay:
        payload = open(a.replay, "rb").read()
        upload(dev, payload, len(payload) - 1)
    else:
        fw = open(a.image, "rb").read()
        upload(dev, build_payload(fw, a.size), a.size - 1)

if __name__ == "__main__":
    main()
