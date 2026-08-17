#ifndef T2I_KEYPAD_H
#define T2I_KEYPAD_H

#include <stdbool.h>
#include <stdint.h>

#define KEY_NONE 0xFF

/* Stock RTI code 180 (row 6, col 1). Reserved as our debug key: held down it
 * triggers diagnostics rather than any normal remote function. */
#define KEY_DEBUG 180

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

#endif /* T2I_KEYPAD_H */
