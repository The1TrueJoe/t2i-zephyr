#ifndef T2I_STATUS_H
#define T2I_STATUS_H

#include <stdbool.h>
#include <stdint.h>

/* One snapshot of everything the firmware knows, gathered once per loop and
 * handed to the UI. Keeping this a plain struct means the UI never reaches into
 * drivers, so screens can be reworked without touching input code. */
struct t2i_status {
	/* touch */
	bool     touch_down;
	int      touch_x, touch_y, touch_z;      /* raw ADC */
	int      touch_min_x, touch_max_x;
	int      touch_min_y, touch_max_y;

	/* accelerometer */
	bool     accel_ok;
	int      accel_x, accel_y, accel_z;      /* ~64 counts per g */
	uint32_t motion_events;

	/* keypad */
	uint8_t  key;                            /* stock RTI code, 0xFF = none */
	int      key_row, key_col;
	uint8_t  key_rows;                       /* raw row bitmap */
	const char *key_name;                    /* human name for `key` */

	/* battery / charger */
	int      batt_raw;                       /* 12-bit counts, -1 = no reading */
	bool     batt_low;
	int      als;                            /* ambient light, raw 12-bit; -1 = no reading */
	int      als_avg;                        /* smoothed ambient light, what drives brightness */
	int      backlight;                      /* LCD backlight, percent */
	bool     charger;
	uint8_t  charge_state;

	/* radio */
	bool     rf_joined;                      /* on the coordinator's ZigBee network */

	/* power / sleep */
	bool     asleep;
	uint8_t  menu;                           /* 0 = main, 1 = connectivity, 2 = settings, 3 = debug */
	uint8_t  beep_vol;                        /* click volume 0..3, shown on the settings page */
	bool     recovery;                       /* held a key at boot: never sleeps */
	uint32_t wakes;
	uint32_t wake_irqs;
	const char *woke_by;
	uint32_t stops;                          /* STOP-mode entries */
	const char *clk;                         /* "PLL120" / "HSI16" / "HSE" */

	/* safety */
	uint32_t boot_attempts;                  /* consecutive boots not yet healthy */
	bool     healthy;                        /* this boot has been declared good */
	const char *reset_cause;                 /* why we last rebooted */
	uint32_t debug_hold_ms;                  /* OK held on the debug page (self-test) */
	bool     wdt_test_armed;                 /* watchdog self-test running */

	/* USB updater */
	bool     usb_busy;
	uint32_t usb_received, usb_declared;
};

#endif /* T2I_STATUS_H */
