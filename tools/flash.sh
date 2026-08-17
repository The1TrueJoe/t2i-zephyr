#!/bin/sh
# Build-and-flash the T2i over SWD, reliably, and reboot it afterwards.
#
# Every flag here exists because of a specific failure:
#
#   --flash=512k   st-flash runs its own connect, separate from st-info, and its
#                  flash-size detection intermittently reads 0 -> "Unknown memory
#                  region". Forcing the size skips the detection entirely.
#   st-info first  a probe reliably "warms up" the link; a cold st-flash often
#                  fails where a probe-then-flash succeeds.
#   retries        the ST-Link wedges ("Failed to enter SWD mode" = probe state,
#                  needs a USB replug; "Can not connect" = target asleep, needs a
#                  power-cycle). Retrying rides out the transient cases.
#   reset at end   st-flash --reset is unreliable on this unit (dead NRST), so we
#                  reset explicitly afterwards and report whether it took.
#
# If the target is asleep, hold any key while powering on: recovery mode never
# sleeps, which keeps it attachable indefinitely.
set -eu

HERE=$(cd "$(dirname "$0")/.." && pwd)
BIN=${1:-$HERE/build/zephyr/zephyr.bin}
ADDR=0x08004000
TRIES=${TRIES:-6}

[ -f "$BIN" ] || { echo "no image at $BIN — build first"; exit 1; }
printf 'flashing %s (%s bytes)\n' "$BIN" "$(wc -c < "$BIN" | tr -d ' ')"

n=1
while [ "$n" -le "$TRIES" ]; do
    st-info --probe >/dev/null 2>&1 || true      # warm up the link
    if st-flash --flash=512k --reset write "$BIN" "$ADDR" >/tmp/t2i-flash.log 2>&1; then
        echo "flashed on attempt $n"
        break
    fi
    why=$(grep -oE 'Failed to enter SWD mode|Can not connect|Unknown memory region' \
          /tmp/t2i-flash.log | head -1 || true)
    echo "attempt $n failed: ${why:-unknown}"
    case "$why" in
      "Failed to enter SWD mode")
        echo "  -> the ST-Link itself is wedged: unplug and replug its USB" ;;
      "Can not connect")
        echo "  -> target unreachable (asleep?): power-cycle, or hold a key while powering on" ;;
    esac
    n=$((n + 1))
    sleep 2
done
[ "$n" -le "$TRIES" ] || { echo "gave up after $TRIES attempts"; tail -3 /tmp/t2i-flash.log; exit 1; }

# Reboot into the new image. st-flash --reset already tried; do it explicitly and
# say plainly whether it worked, rather than leaving you guessing why the old
# firmware still seems to be running.
if st-flash reset >/dev/null 2>&1; then
    echo "reset issued — new firmware running"
else
    echo "reset FAILED (SYSRESETREQ is unreliable here) — power-cycle the remote"
fi
