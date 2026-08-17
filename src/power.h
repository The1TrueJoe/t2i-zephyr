#ifndef T2I_POWER_H
#define T2I_POWER_H

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/device.h>

/*
 * Sleep/wake policy. Owns "when do we sleep, how deeply, and what brings us
 * back" so main() stays a plain loop and the depth of sleep can change without
 * touching anything else.
 *
 * Wake sources are EXTI-driven (keypad rows, accelerometer INT1) — see wake.c.
 * The touchscreen is deliberately NOT a wake source: it is a bare resistive
 * panel with no interrupt line, so watching it means periodically powering the
 * plates and running ADC conversions, which is exactly the cost sleeping is
 * meant to avoid.
 */
void power_init(const struct device *disp, bool recovery);

/* Feed each loop. `activity` is true if any input happened. Returns true if the
 * remote is (still) asleep, in which case the caller should skip rendering —
 * power_wait() has already blocked for a sensible interval. */
bool power_tick(bool activity, const char *source);

/* What woke us last, and how many times. */
const char *power_woke_by(void);
uint32_t power_wakes(void);
bool power_asleep(void);

#endif /* T2I_POWER_H */
