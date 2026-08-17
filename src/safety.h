#ifndef T2I_SAFETY_H
#define T2I_SAFETY_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Anti-brick machinery for a remote whose only way in is USB.
 *
 * The RTI bootloader has no USB of its own (verified: sector 0 touches SPI3,
 * GPIO and RCC only). It can only commit an image already staged in SPI-NOR,
 * and staging is the application's job — so if the application stops
 * enumerating USB, a remote without SWD is unrecoverable. Everything here
 * exists to make that impossible:
 *
 *   1. WATCHDOG   a hang becomes a reset instead of a dead remote
 *   2. BOOT COUNT kept in RAM that survives warm resets; incremented every
 *                 boot, cleared once the app proves itself healthy
 *   3. SAFE MODE  after MAX_BOOT_ATTEMPTS failures, boot USB-and-nothing-else,
 *                 so the update path cannot be taken down by whatever is
 *                 crashing (display, LVGL, touch, radio...)
 *
 * The counter deliberately lives in RAM, not flash: it must survive the
 * watchdog-reset loop we are protecting against (it does), it costs no flash
 * wear, and a real power-cycle clearing it is the behaviour we want — a user
 * pulling the battery gets a clean slate.
 *
 * Residual risk, stated honestly: a fault *before* safety_boot() runs (in
 * reset_hook or kernel init) is not covered by any of this. Keep that path
 * boring.
 */

/* Call FIRST in main, before touching any hardware. Increments the boot
 * counter. Returns true if the previous boots failed and the firmware should
 * come up in USB-only safe mode. */
bool safety_boot_check(void);

/* The app got far enough to be trusted — clear the counter so the next boot
 * starts fresh. Call once the main loop has run successfully for a while. */
void safety_mark_healthy(void);

/* How many consecutive boots have not reached safety_mark_healthy(). */
uint32_t safety_boot_attempts(void);

/* Independent watchdog. Once started it cannot be stopped, so every long
 * operation must feed it — including the multi-second SPI-NOR erase in the
 * updater. */
void safety_watchdog_start(void);
void safety_watchdog_feed(void);

/* Why the remote last reset, captured from RCC_CSR at boot (the flags are
 * cleared afterwards so the next boot reports its own cause). Knowing a unit
 * rebooted because the watchdog bit — rather than a user pulling power — is the
 * difference between "it's fine" and "something is hanging in the field". */
const char *safety_reset_cause(void);

/* Deliberately stop feeding the watchdog, to prove it actually resets us.
 * A safety net nobody has seen work is not a safety net. */
void safety_watchdog_selftest(void);

#endif /* T2I_SAFETY_H */
