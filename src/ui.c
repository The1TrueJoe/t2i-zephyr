/*
 * LVGL screen handling for the T2i — see ui.h.
 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/display.h>
#include <lvgl.h>
#include <stdio.h>
#include <string.h>
#include "ui.h"
#include "touch.h"
#include "safety.h"

/* fine-grained boot markers: read 0x2001FF88 over SWD to see where ui_init got */
#define UIMARK(v) (*(volatile uint32_t *)0x2001FF88 = (uint32_t)(v))

extern const lv_image_dsc_t juno_logo;   /* src/juno_logo.c */

#define BRIGHT_AWAKE 128

/* --- touch calibration -----------------------------------------------------
 * Measured on-device: top-left reads raw (3333, 546), bottom-right (800, 3354).
 * X is inverted (left reads high), so its LO/HI are deliberately reversed —
 * map_axis handles lo > hi. Re-measure by dragging the corners and reading the
 * min/max off the screen. Set SWAP_XY if the axes ever come out rotated.
 */
#define X_LO 3333
#define X_HI 800
#define Y_LO 546
#define Y_HI 3354
#define SWAP_XY 0

#define PANEL_W 240
#define PANEL_H 320

static lv_obj_t *scr;
static lv_obj_t *info;

static int last_rx, last_ry, last_z;
static bool last_down;
static int min_x = 4095, max_x, min_y = 4095, max_y;

static int map_axis(int raw, int lo, int hi, int span)
{
	int v = (raw - lo) * span / (hi - lo);   /* lo > hi inverts the axis */
	return v < 0 ? 0 : (v >= span ? span - 1 : v);
}

static void touch_lv_read(lv_indev_t *indev, lv_indev_data_t *data)
{
	ARG_UNUSED(indev);
	int rx = 0, ry = 0, rz = 0;

	if (!touch_read(&rx, &ry, &rz)) {
		last_z = rz;
		last_down = false;
		data->state = LV_INDEV_STATE_RELEASED;
		return;   /* leave data->point alone, as LVGL expects */
	}

	last_z = rz;
	last_down = true;
	last_rx = rx;
	last_ry = ry;
	if (rx < min_x) { min_x = rx; }
	if (rx > max_x) { max_x = rx; }
	if (ry < min_y) { min_y = ry; }
	if (ry > max_y) { max_y = ry; }

#if SWAP_XY
	data->point.x = map_axis(ry, Y_LO, Y_HI, PANEL_W);
	data->point.y = map_axis(rx, X_LO, X_HI, PANEL_H);
#else
	data->point.x = map_axis(rx, X_LO, X_HI, PANEL_W);
	data->point.y = map_axis(ry, Y_LO, Y_HI, PANEL_H);
#endif
	data->state = LV_INDEV_STATE_PRESSED;
}

bool ui_touch_down(void) { return last_down; }

void ui_touch_raw(int *x, int *y, int *z)
{
	if (x) { *x = last_rx; }
	if (y) { *y = last_ry; }
	if (z) { *z = last_z; }
}

void ui_touch_range(int *mnx, int *mxx, int *mny, int *mxy)
{
	if (mnx) { *mnx = min_x; }
	if (mxx) { *mxx = max_x; }
	if (mny) { *mny = min_y; }
	if (mxy) { *mxy = max_y; }
}

void ui_touch_indev_init(void)
{
	lv_indev_t *indev = lv_indev_create();

	lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
	lv_indev_set_read_cb(indev, touch_lv_read);
}

void ui_init(const struct device *disp, uint32_t splash_ms)
{
	/* Backlight stays off until a full frame exists: the panel powers up
	 * holding noise, and lighting it first shows an RGB grid of it. */
	UIMARK(1);
	display_blanking_on(disp);
	UIMARK(2);

	scr = lv_scr_act();
	lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

	UIMARK(3);
	lv_obj_t *logo = lv_image_create(scr);
	lv_image_set_src(logo, &juno_logo);
	lv_obj_center(logo);
	UIMARK(4);

	lv_refr_now(NULL);
	UIMARK(5);
	display_blanking_off(disp);
	display_set_brightness(disp, BRIGHT_AWAKE);
	UIMARK(6);

	/* Hold the splash. This also *is* the boot flash-window — the CPU stays
	 * awake and easy for SWD to catch before any sleeping starts. */
	for (uint32_t i = 0; i < splash_ms / 50; i++) {
		safety_watchdog_feed();   /* 3s with no feed would otherwise trip it */
		lv_timer_handler();
		k_busy_wait(50000);
	}
	UIMARK(7);
	lv_obj_delete(logo);
	UIMARK(8);

	/* Black background suits a remote; text colour is inherited, so setting it
	 * on the screen covers every label. */
	lv_obj_set_style_text_color(scr, lv_color_white(), 0);

	lv_obj_t *title = lv_label_create(scr);
	lv_label_set_text(title, "T2i");
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

	info = lv_label_create(scr);
	lv_label_set_text(info, "starting...");
	lv_obj_align(info, LV_ALIGN_TOP_LEFT, 6, 40);
	UIMARK(9);
}

void ui_invalidate(void)
{
	lv_obj_invalidate(lv_scr_act());
}

void ui_render(const struct t2i_status *st)
{
	char b[288];

	if (st->wdt_test_armed) {
		snprintf(b, sizeof(b),
			 "WATCHDOG SELF-TEST\n\nfeeding stopped\n\nthis remote should\nreset itself within\n~8 seconds, then\nshow: rst WATCHDOG");
		lv_label_set_text(info, b);
		lv_timer_handler();
		return;
	}

	if (st->usb_busy) {
		/* A firmware update is in progress — nothing else matters, and the
		 * host is mid-transfer, so show only that. */
		snprintf(b, sizeof(b), "USB UPDATE\n\n%u / %u bytes\n\ndo not disconnect",
			 st->usb_received, st->usb_declared);
	} else if (!st->debug) {
		/* Everything that was on the bring-up screen is still one Info press
		 * away — it just is not what you want to look at once it all works. */
		snprintf(b, sizeof(b),
			 "key %d %s\n\nbatt %d%s\n\n%s\n\nInfo = debug%s",
			 st->key == 0xFF ? -1 : st->key, st->key_name ? st->key_name : "-",
			 st->batt_raw, st->batt_low ? " LOW" : "",
			 st->asleep ? "ASLEEP" : "awake",
			 st->recovery ? "\nRECOVERY (no sleep)" : "");
	} else {
		snprintf(b, sizeof(b),
			 "touch %s\n raw %d,%d z%d\n X %d-%d\n Y %d-%d\n\n"
			 "accel %s\n x%d y%d z%d\n\n"
			 "key %d %s\n r%d c%d  rows 0x%02x\n"
			 "batt %d%s  chg %d/%d\n\n"
			 "%s  wakes %u\n irqs %u  motions %u\n woke by %s\n"
			 "stops %u  clk %s\nboot %u%s  rst %s%s",
			 st->touch_down ? "DOWN" : "up",
			 st->touch_x, st->touch_y, st->touch_z,
			 st->touch_min_x, st->touch_max_x,
			 st->touch_min_y, st->touch_max_y,
			 st->accel_ok ? "ok" : "NOT FOUND",
			 st->accel_x, st->accel_y, st->accel_z,
			 st->key == 0xFF ? -1 : st->key, st->key_name ? st->key_name : "-",
			 st->key_row, st->key_col, st->key_rows,
			 st->batt_raw, st->batt_low ? " LOW" : "",
			 st->charger, st->charge_state,
			 st->asleep ? "ASLEEP" : "awake", st->wakes,
			 st->wake_irqs, st->motion_events, st->woke_by,
			 st->stops, st->clk,
			 st->boot_attempts, st->healthy ? " ok" : " UNPROVEN",
			 st->reset_cause,
			 st->recovery ? "\nRECOVERY (no sleep)" : "");

	if (st->debug_hold_ms) {
		char h[48];
		snprintf(h, sizeof(h), "\nDEBUG held %u ms", st->debug_hold_ms);
		strncat(b, h, sizeof(b) - strlen(b) - 1);
	}
	}

	lv_label_set_text(info, b);
	lv_timer_handler();
}
