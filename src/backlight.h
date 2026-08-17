#ifndef T2I_BACKLIGHT_H
#define T2I_BACKLIGHT_H

/* Keypad backlight (PC8 = TIM8_CH3), 0..100%.
 *
 * Stock RTI's curve (FUN_0801ab1e): period 300, pulse = pct*2 + 75, so its
 * usable PWM range floors around 25%. The 0 and 100 endpoints are driven as
 * static GPIO with the timer stopped — LOW for off, HIGH for full, the same
 * sense as the LCD backlight (stock uses GPIO_ResetBits for 0, SetBits for 100).
 *
 * The LCD backlight is not exposed here; it goes through the Zephyr display API
 * (display_set_brightness / display_blanking_on|off).
 */
void keypad_backlight_set(int pct);

#endif /* T2I_BACKLIGHT_H */
