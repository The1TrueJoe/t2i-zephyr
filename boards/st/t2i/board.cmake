# SPDX-License-Identifier: Apache-2.0
#
# Flashing is done with t2i_usb_uploader.py over USB, or openocd over SWD directly.
# (No west-flash runner wired up — the RTI bootloader owns sector 0.)
board_runner_args(openocd)
include(${ZEPHYR_BASE}/boards/common/openocd.board.cmake)
