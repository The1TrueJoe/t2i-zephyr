#ifndef T2I_BACKLIGHT_H
#define T2I_BACKLIGHT_H

/* Keypad backlight (PC8 = TIM8_CH3), 0..100%.
 *
 * Stock RTI's curve (FUN_0801ab1e): period 300, pulse = pct*2 + 75, so its
 * usable range is a floor of 25% up to ~92% — not 0..100. The endpoints are
 * special-cased the same way as the LCD backlight: 0 and 100 disable the timer
 * and drive the pin as plain GPIO.
 *
 * The LCD backlight is not exposed here; it is driven through the Zephyr display
 * API (display_set_brightness / display_blanking_on|off).
 */
void keypad_backlight_set(int pct);

#endif /* T2I_BACKLIGHT_H */
