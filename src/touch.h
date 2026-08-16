#ifndef T2I_TOUCH_H
#define T2I_TOUCH_H

/* 4-wire resistive touch (PA3-6 via ADC2) + beeper (TIM3_CH3/PB0). */
void touch_init(void);
/* Returns 1 if the screen is being touched, and fills raw ADC x,y (0..4095).
 * `z` (may be NULL) always receives the raw pressure reading, pressed or not —
 * it is what TOUCH_Z_MIN in touch.c is tuned against. */
int  touch_read(int *x, int *y, int *z);

void beep_init(void);
void beep_click(void);
void beep_test(void);   /* boot self-test: two audible beeps */

#endif /* T2I_TOUCH_H */
