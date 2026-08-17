#ifndef T2I_BATTERY_H
#define T2I_BATTERY_H

#include <stdbool.h>
#include <stdint.h>

/* Battery sense on PA7 = ADC1 IN7, and the charger status GPIOs.
 *
 * Stock has NO volts-or-percent conversion: it compares raw 12-bit counts
 * against fixed thresholds with hysteresis (2460 trip / 2731 clear) after a
 * self-calibrating offset. We expose raw counts plus that same low/normal
 * decision, and deliberately do not invent a voltage — the divider ratio is not
 * in the firmware, so counts cannot be converted without measuring the pack.
 */
void battery_init(void);

/* Raw 12-bit ADC counts, or -1 if the conversion did not complete. */
int battery_raw(void);

/* Stock's hysteresis: low below 2460, clears above 2731. */
bool battery_low(void);

/* Charger lines: PC9 = charger present, PC14/PC15 = 2-bit status. */
bool battery_charger_present(void);
uint8_t battery_charge_state(void);

#endif /* T2I_BATTERY_H */
