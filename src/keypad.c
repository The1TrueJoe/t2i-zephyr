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


/* Stock RTI key code (128..180) -> button name.
 *
 * Two independent sources agree 52/52: the firmware's own "Keypad Test" diag
 * page (DiagHandleHardkeyPress at 0x080046a8 dispatches code-128 through a
 * 53-entry table, each case painting one labelled cell) and Integration
 * Designer's programmable-hardkey list (length-prefixed UTF-16 at file offset
 * 0xfc1588, entry i == code 128+i). Only 180 (Backlight) is absent from ID,
 * being a local hardware function rather than a programmable key.
 */
static const char *const key_names[53] = {
	/* 128 */ "Exit",            /* r2 c4  PE2 /PC4 */
	/* 129 */ "Mute",            /* r0 c7  PE0 /PC7 */
	/* 130 */ "Soft Lft Cntr",   /* r0 c3  PE0 /PC3  ID: Softkey 2 */
	/* 131 */ "Up",              /* r1 c4  PE1 /PC4 */
	/* 132 */ "Left",            /* r1 c5  PE1 /PC5 */
	/* 133 */ "Right",           /* r1 c7  PE1 /PC7 */
	/* 134 */ "Down",            /* r2 c2  PE2 /PC2 */
	/* 135 */ "OK",              /* r5 c1  PE14/PC1 */
	/* 136 */ "Soft Lft",        /* r0 c4  PE0 /PC4  ID: Softkey 1 */
	/* 137 */ "Soft Rht",        /* r0 c1  PE0 /PC1  ID: Softkey 4 */
	/* 138 */ "Vol +",           /* r1 c6  PE1 /PC6 */
	/* 139 */ "Vol -",           /* r2 c1  PE2 /PC1 */
	/* 140 */ "Ch +",            /* r6 c7  PE15/PC7 */
	/* 141 */ "Ch -",            /* r2 c3  PE2 /PC3 */
	/* 142 */ "Guide",           /* r2 c7  PE2 /PC7 */
	/* 143 */ "Menu",            /* r2 c6  PE2 /PC6 */
	/* 144 */ "Info",            /* r2 c5  PE2 /PC5 */
	/* 145 */ "Off",             /* r0 c5  PE0 /PC5  ID: Power Off */
	/* 146 */ "Play",            /* r4 c2  PE13/PC2 */
	/* 147 */ "Pause",           /* r3 c6  PE12/PC6 */
	/* 148 */ "Stop",            /* r3 c5  PE12/PC5 */
	/* 149 */ "Record",          /* r3 c4  PE12/PC4 */
	/* 150 */ "<<",              /* r4 c4  PE13/PC4  ID: Scan << */
	/* 151 */ ">>",              /* r4 c3  PE13/PC3  ID: Scan >> */
	/* 152 */ "|<<",             /* r4 c6  PE13/PC6  ID: Skip << */
	/* 153 */ ">>|",             /* r4 c5  PE13/PC5  ID: Skip >> */
	/* 154 */ "1",               /* r5 c3  PE14/PC3 */
	/* 155 */ "2",               /* r5 c2  PE14/PC2 */
	/* 156 */ "3",               /* r4 c7  PE13/PC7 */
	/* 157 */ "4",               /* r5 c6  PE14/PC6 */
	/* 158 */ "5",               /* r5 c5  PE14/PC5 */
	/* 159 */ "6",               /* r5 c4  PE14/PC4 */
	/* 160 */ "7",               /* r6 c2  PE15/PC2 */
	/* 161 */ "8",               /* r6 c6  PE15/PC6 */
	/* 162 */ "9",               /* r5 c7  PE14/PC7 */
	/* 163 */ "0",               /* r6 c4  PE15/PC4 */
	/* 164 */ "-/.",             /* r6 c5  PE15/PC5 */
	/* 165 */ "Enter",           /* r6 c3  PE15/PC3 */
	/* 166 */ "Scroll Up",       /* r3 c0  PE12/PC0  ID: Joystick Up */
	/* 167 */ "Scroll Click",    /* r2 c0  PE2 /PC0  ID: Joystick Click */
	/* 168 */ "Scroll Down",     /* r1 c0  PE1 /PC0  ID: Joystick Down */
	/* 169 */ "Scroll Left",     /* r0 c0  PE0 /PC0  ID: Joystick Left */
	/* 170 */ "Scroll Right",    /* r4 c0  PE13/PC0  ID: Joystick Right */
	/* 171 */ "On",              /* r0 c6  PE0 /PC6  ID: Power On */
	/* 172 */ "List",            /* r1 c2  PE1 /PC2 */
	/* 173 */ "Red",             /* r3 c2  PE12/PC2 */
	/* 174 */ "Green",           /* r3 c7  PE12/PC7 */
	/* 175 */ "Yellow",          /* r3 c3  PE12/PC3 */
	/* 176 */ "Blue",            /* r3 c1  PE12/PC1 */
	/* 177 */ "Soft Rht Cntr",   /* r0 c2  PE0 /PC2  ID: Softkey 3 */
	/* 178 */ "Prev",            /* r1 c3  PE1 /PC3 */
	/* 179 */ "Back",            /* r1 c1  PE1 /PC1 */
	/* 180 */ "Backlight",       /* r6 c1  PE15/PC1  (not exposed by ID) */
};

const char *keypad_name(uint8_t code)
{
	/* One unsigned range test covers KEY_NONE (0xFF) and anything unexpected. */
	if ((unsigned)(code - 128u) >= 53u) {
		return "?";
	}
	return key_names[code - 128];
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
