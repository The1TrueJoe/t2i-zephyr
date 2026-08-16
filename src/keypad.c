/*
 * T2i keypad — 8x7 matrix, reverse-engineered from stock RTI (FUN_080111ec).
 *
 *   Columns: PC0..PC7  (push-pull outputs, driven low one at a time)
 *   Rows:    PE0/1/2/12/13/14/15  (inputs, pulled up — a pressed key reads 0)
 *
 * Stock parks every column low between scans, which makes "is any key held?" a
 * single read of the row bits with no scanning at all — that is what wakes the
 * remote from sleep.
 */
#include <zephyr/kernel.h>
#include "t2i_regs.h"
#include "keypad.h"

#define C GPIO_PORT_C
#define E GPIO_PORT_E

static const uint8_t row_pin[7] = { 0, 1, 2, 12, 13, 14, 15 };

/* Stock RTI key codes, indexed [row][col] (extracted from 0x08011344).
 * 0xFF marks the three unpopulated positions in the 8x7 grid. */
static const uint8_t keymap[7][8] = {
	{ 169, 137, 177, 130, 136, 145, 171, 129 },
	{ 168, 179, 172, 178, 131, 132, 138, 133 },
	{ 167, 139, 134, 141, 128, 144, 143, 142 },
	{ 166, 176, 173, 175, 149, 148, 147, 174 },
	{ 170, 255, 146, 151, 150, 153, 152, 156 },
	{ 255, 135, 155, 154, 159, 158, 157, 162 },
	{ 255, 180, 160, 165, 163, 164, 161, 140 },
};

static void pc_out(int pin)
{
	GPIO_MODER(C) = (GPIO_MODER(C) & ~(3u << (pin * 2))) | (GPIO_MODE_OUTPUT << (pin * 2));
}

static void pe_in_pullup(int pin)
{
	GPIO_MODER(E) = (GPIO_MODER(E) & ~(3u << (pin * 2))) | (GPIO_MODE_INPUT << (pin * 2));
	GPIO_PUPDR(E) = (GPIO_PUPDR(E) & ~(3u << (pin * 2))) | (1u << (pin * 2));
}

/* drive the given column low, all others high; mask of 0xFF parks all low */
static void cols_drive(int only_col)
{
	if (only_col < 0) {
		GPIO_BSRR(C) = 0xFFu << 16;             /* every column low */
		return;
	}
	/* Every other column high, this one low. The set half of BSRR must exclude
	 * this column: when a bit appears in both halves the set wins, so the
	 * obvious `0xFF | (1 << (col+16))` leaves every column high and nothing
	 * ever reads as pressed. */
	GPIO_BSRR(C) = (0xFFu & ~(1u << only_col)) | (1u << (only_col + 16));
}

static uint32_t rows_read(void)
{
	uint32_t idr = GPIO_IDR(E), bits = 0;

	for (int r = 0; r < 7; r++) {
		if (!((idr >> row_pin[r]) & 1u)) {
			bits |= 1u << r;                /* active low: 0 = pressed */
		}
	}
	return bits;
}

void keypad_init(void)
{
	RCC_AHB1ENR |= RCC_AHB1ENR_GPIO(C) | RCC_AHB1ENR_GPIO(E);

	for (int c = 0; c < 8; c++) {
		pc_out(c);
	}
	for (int r = 0; r < 7; r++) {
		pe_in_pullup(row_pin[r]);
	}
	cols_drive(-1);          /* park low, as stock does between scans */
	k_busy_wait(50);
}

uint8_t keypad_rows(void)
{
	cols_drive(-1);
	k_busy_wait(20);
	return (uint8_t)rows_read();
}

bool keypad_any(void)
{
	cols_drive(-1);
	k_busy_wait(20);         /* let the pull-ups settle after the drive change */
	return rows_read() != 0;
}

uint8_t keypad_scan(int *row, int *col)
{
	for (int c = 0; c < 8; c++) {
		cols_drive(c);
		k_busy_wait(20);

		uint32_t bits = rows_read();
		if (!bits) {
			continue;
		}
		for (int r = 0; r < 7; r++) {
			if (bits & (1u << r)) {
				cols_drive(-1);
				if (row) { *row = r; }
				if (col) { *col = c; }
				return keymap[r][c];
			}
		}
	}
	cols_drive(-1);
	if (row) { *row = -1; }
	if (col) { *col = -1; }
	return KEY_NONE;
}
