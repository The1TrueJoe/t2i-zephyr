#ifndef T2I_WAKE_H
#define T2I_WAKE_H

#include <stdbool.h>
#include <zephyr/kernel.h>

/* Interrupt-driven wake sources, so sleeping costs no CPU time.
 *
 *   keypad rows PE0/1/2/12-15 -> EXTI 0,1,2,12-15  (falling: columns are
 *                                                   parked low, a press pulls
 *                                                   its row down)
 *   accel INT1  PE5           -> EXTI 5            (rising: LIS3DH drives it
 *                                                   high and latches)
 *
 * Call after keypad_init(), which sets the row pin modes.
 */
bool wake_init(void);

/* Block (the CPU idles in WFI) until a wake interrupt fires or `timeout`
 * elapses. The timeout exists because the touchscreen has no interrupt line of
 * its own — it is a bare resistive panel — so it still has to be polled. */
int wake_wait(k_timeout_t timeout);

/* Number of wake interrupts seen — bring-up diagnostics. */
uint32_t wake_count(void);

#endif /* T2I_WAKE_H */
