#!/bin/sh
# Watch button presses arriving at the CA-1 over ZigBee, live.
#
# The remote (Telegesis-flashed EM250) is joined to the CA-1 EM357 coordinator and unicasts every
# button press. This tails the coordinator log so you see "BUTTON code 138 -> remote::volume_up"
# as buttons are pressed. Synthetic presses fire every 4s (KEY_SIM=1 in the firmware); real
# presses on the handset appear the same way.
#
# If the coordinator ever stops (reboot, killed), restart it with:
#   ssh root@192.168.1.178 'cd /tmp && node ca1_coord.js 15 6699 28800 > /tmp/coord.log 2>&1 &'
exec sshpass -p openhc ssh -o StrictHostKeyChecking=no root@192.168.1.178 \
  'tail -f /tmp/coord.log | grep --line-buffered -E "BUTTON|NODE JOINED|MESSAGE"'
