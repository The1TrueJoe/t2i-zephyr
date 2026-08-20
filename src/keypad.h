#ifndef T2I_KEYPAD_H
#define T2I_KEYPAD_H

#include <stdbool.h>
#include <stdint.h>

#define KEY_NONE 0xFF

/* Stock RTI code 180 (row 6, col 1) is the Backlight button. Held, it opens the
 * on-device menu (and lights the screen); it is never sent over RF. */
#define KEY_BACKLIGHT 180
#define IR_TEST_KEY   149   /* Record */
#define KEY_INFO      144

/* Codes used to navigate the on-device menu (see main.c). */
#define KEY_EXIT   128
#define KEY_UP     131
#define KEY_LEFT   132
#define KEY_RIGHT  133
#define KEY_DOWN   134
#define KEY_SELECT 135
#define KEY_BACK   179

/* 8x7 matrix: columns PC0-7 (driven), rows PE0/1/2/12-15 (sensed, active low). */
void keypad_init(void);

/* Cheap "is anything held?" — all columns low, any row low. Used to wake. */
bool keypad_any(void);

/* Raw row bits with all columns parked low — bring-up diagnostics. */
uint8_t keypad_rows(void);

/* Full scan. Returns the stock RTI key code (128..180), or KEY_NONE.
 * `row`/`col` (may be NULL) get the matrix position, which is what you want
 * while working out which physical button is which. */
uint8_t keypad_scan(int *row, int *col);

/* Human name for a stock key code, or "?" if unknown. See src/keypad.c. */
const char *keypad_name(uint8_t code);

#endif /* T2I_KEYPAD_H */
