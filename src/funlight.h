#ifndef T2I_FUNLIGHT_H
#define T2I_FUNLIGHT_H

#include <stdbool.h>
#include <stdint.h>

/* Front-panel RGB indicator ("FunLight"), single-wire on PA10.
 *
 * Not a bus: no clock, no chip select, no slave address. A frame is a count of
 * pulses for the register, a 300us gap, then a count of pulses for the value.
 * Reversed from stock FUN_0801bf50 / FUN_0801be26.
 */
void funlight_init(void);

/* Three channels, each on/off, at `pct` brightness (0..100).
 * Stock's own uses: on battery = blue, charging = red, charge complete = green. */
void funlight_set(bool ch0, bool ch1, bool ch2, int pct);

/* Stock holds PA10 low for the duration of sleep (FUN_0801bf02). */
void funlight_sleep(void);

#endif /* T2I_FUNLIGHT_H */
