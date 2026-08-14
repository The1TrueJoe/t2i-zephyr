#ifndef T2I_TOUCH_H
#define T2I_TOUCH_H

/* 4-wire resistive touch (PA3-6 via ADC2) + beeper (TIM3_CH3/PB0). */
void touch_init(void);
/* Returns 1 if the screen is being touched, and fills raw ADC x,y (0..4095). */
int  touch_read(int *x, int *y);

void beep_init(void);
void beep_click(void);
void beep_test(void);   /* boot self-test: two audible beeps */

#endif /* T2I_TOUCH_H */
