/*
 * T2i touch demo: the resistive panel is registered as a real LVGL pointer
 * input device, so ordinary LVGL widgets are clickable. The bottom label shows
 * live raw ADC + the running min/max, which is what you drag the four corners
 * against to fill in the calibration constants below.
 *
 * Display via the hx8347_fsmc Zephyr driver + LVGL; touch + beep via
 * src/touch.c (ADC2 4-wire + TIM3/PB0).
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <lvgl.h>
#include <stdio.h>
#include <stdint.h>
#include "touch.h"
#include "accel.h"
#include "keypad.h"
#include "wake.h"

extern const lv_image_dsc_t juno_logo;   /* src/juno_logo.c */

/* Blank the screen after this long with no activity. Any button press, touch or
 * motion resets the countdown. */
#define SLEEP_AFTER_MS 30000

/* Backlight levels (0..255). Sleep dims rather than blanking outright while we
 * are still proving the wake path: a faintly lit screen tells you whether the
 * loop is alive and whether a keypress restores brightness. Set BRIGHT_ASLEEP
 * to 0 for real power behaviour once wake is trusted. */
#define BRIGHT_AWAKE  128
#define BRIGHT_ASLEEP 8

#define MARK(off, v) (*(volatile uint32_t *)(0x2001FF00 + (off)) = (uint32_t)(v))

/* --- calibration knobs -----------------------------------------------------
 * Measured on-device: top-left reads raw (3333, 546), bottom-right (800, 3354).
 * X is inverted (left reads high), so its LO/HI are deliberately reversed —
 * map_axis handles lo > hi. Re-measure by dragging the corners and reading the
 * min/max off the bottom label. Set SWAP_XY if the axes ever come out rotated.
 */
#define X_LO 3333
#define X_HI 800
#define Y_LO 546
#define Y_HI 3354
#define SWAP_XY 0

static int map_axis(int raw, int lo, int hi, int span)
{
	int v = (raw - lo) * span / (hi - lo);   /* lo > hi inverts the axis */
	return v < 0 ? 0 : (v >= span ? span - 1 : v);
}

/* last sample, stashed by the LVGL read callback for the on-screen readout */
static int last_rx, last_ry, last_z, last_pressed;
static int minX = 4095, maxX, minY = 4095, maxY;

static void touch_lv_read(lv_indev_t *indev, lv_indev_data_t *data)
{
	ARG_UNUSED(indev);
	int rx = 0, ry = 0, rz = 0;

	if (!touch_read(&rx, &ry, &rz)) {
		last_z = rz;
		last_pressed = 0;
		data->state = LV_INDEV_STATE_RELEASED;
		return;   /* keep data->point at its last value, as LVGL expects */
	}
	last_z = rz;

	last_pressed = 1;
	last_rx = rx;
	last_ry = ry;
	if (rx < minX) { minX = rx; }
	if (rx > maxX) { maxX = rx; }
	if (ry < minY) { minY = ry; }
	if (ry > maxY) { maxY = ry; }
	MARK(0x30, (uint16_t)rx | ((uint32_t)(uint16_t)ry << 16));
	MARK(0x40, (uint16_t)minX | ((uint32_t)(uint16_t)maxX << 16));
	MARK(0x44, (uint16_t)minY | ((uint32_t)(uint16_t)maxY << 16));

#if SWAP_XY
	data->point.x = map_axis(ry, Y_LO, Y_HI, 240);
	data->point.y = map_axis(rx, X_LO, X_HI, 320);
#else
	data->point.x = map_axis(rx, X_LO, X_HI, 240);
	data->point.y = map_axis(ry, Y_LO, Y_HI, 320);
#endif
	data->state = LV_INDEV_STATE_PRESSED;
}

int main(void)
{
	const struct device *disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	if (!device_is_ready(disp)) {
		return 0;
	}
	/* Leave the backlight OFF for now. The panel powers up with whatever noise
	 * is in GRAM, and lighting it before the first frame is drawn shows an RGB
	 * grid of uninitialised memory. Blanking stays on until the splash has been
	 * rendered below. */
	display_blanking_on(disp);
	MARK(0x00, 1);   /* stage marker — read 0x2001FF00 over SWD to see how far boot got */

	/* Splash: the Juno logo on black for the duration of the boot flash-window.
	 * That window exists so SWD/st-flash can always catch this CPU (NRST is dead
	 * on this unit, and a sleeping CPU can't be grabbed) — showing the logo just
	 * puts the otherwise-idle 3s to use. */
	lv_obj_t *scr = lv_scr_act();
	lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

	lv_obj_t *logo = lv_image_create(scr);
	lv_image_set_src(logo, &juno_logo);
	lv_obj_center(logo);

	/* Draw the whole first frame (black background + logo) before any light
	 * reaches the panel, then switch the backlight on. */
	lv_refr_now(NULL);
	display_blanking_off(disp);
	display_set_brightness(disp, BRIGHT_AWAKE);

	for (int i = 0; i < 60; i++) {
		lv_timer_handler();
		k_busy_wait(50000);
	}
	lv_obj_delete(logo);

	/* The splash left the screen black; keep it (easier on the eyes than a white
	 * panel) but make text white so the readouts are legible. Text colour is an
	 * inherited style, so setting it on the screen covers every label. */
	lv_obj_set_style_text_color(scr, lv_color_white(), 0);

	touch_init();
	beep_init();
	keypad_init();
	MARK(0x00, 2);
	beep_test();   /* two beeps at boot — tells us if the speaker path works */
	MARK(0x00, 3);

	lv_indev_t *indev = lv_indev_create();
	lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
	lv_indev_set_read_cb(indev, touch_lv_read);
	MARK(0x00, 4);

	MARK(0x00, 5);

	lv_obj_t *title = lv_label_create(scr);
	lv_label_set_text(title, "T2i bring-up");
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

	MARK(0x00, 6);

	/* live readout: touch, accelerometer, keypad */
	lv_obj_t *info = lv_label_create(scr);
	lv_label_set_text(info, "starting...");
	lv_obj_align(info, LV_ALIGN_TOP_LEFT, 6, 40);

	bool accel_ok = accel_init();
	bool wake_ok = wake_init();
	MARK(0x00, (accel_ok ? 7 : 0x70) | (wake_ok ? 0 : 0x700));   /* 0x70 = no accel, 0x700 = no wake IRQ */

	uint32_t beat = 0, motion_events = 0, src_seen = 0;
	const char *woke_by = "-";
	uint32_t wakes = 0;
	bool asleep = false;
	int64_t last_active = k_uptime_get();

	while (1) {
		char b[128];
		MARK(0x04, ++beat);   /* heartbeat — proves the render loop is alive */

		int krow = -1, kcol = -1;
		uint8_t key = keypad_scan(&krow, &kcol);
		bool motion = accel_motion();
		if (motion) {
			motion_events++;
		}
		/* Sticky diagnostics: a motion event is easy to miss when polling RAM
		 * over SWD, because our own read clears the latch within ~10ms. Keep a
		 * count, and OR together every INT1_SRC bit ever seen. */
		src_seen |= accel_last_src();
		MARK(0x18, motion_events);
		MARK(0x1C, src_seen);

		/* While asleep lv_timer_handler never runs, so the LVGL read callback
		 * never fires and last_pressed goes stale — read the panel directly
		 * instead. That was why touch could not wake it. */
		bool touched = asleep ? touch_read(NULL, NULL, NULL) : (last_pressed != 0);
		bool active = touched || motion || key != KEY_NONE;

		/* wake-source diagnostics: bit0 touch, bit1 motion, bit2 key,
		 * byte1 = raw INT1_SRC, byte2 = row bits seen by the last scan */
		MARK(0x10, (touched ? 1u : 0u) | (motion ? 2u : 0u) |
			   (key != KEY_NONE ? 4u : 0u) |
			   ((uint32_t)accel_last_src() << 8) |
			   ((uint32_t)keypad_rows() << 16));

		if (active) {
			last_active = k_uptime_get();
		}

		if (asleep) {
			if (active) {
				woke_by = touched ? "touch" : (key != KEY_NONE ? "KEY" : "motion");
				wakes++;
				display_set_brightness(disp, BRIGHT_AWAKE);
				asleep = false;
				MARK(0x08, 0);
			}
			/* Falls through to the wait below, which parks the core in WFI
			 * until a keypad row or the accelerometer raises an EXTI edge. */
		} else if (k_uptime_get() - last_active > SLEEP_AFTER_MS) {
			display_set_brightness(disp, BRIGHT_ASLEEP);
			asleep = true;
			MARK(0x08, 1);
		}

		if (asleep) {
			MARK(0x20, wake_count());
			wake_wait(K_MSEC(250));
			continue;   /* nothing to redraw while dimmed */
		}

		int ax = 0, ay = 0, az = 0;
		accel_read(&ax, &ay, &az);
		MARK(0x0C, (uint32_t)key);
		snprintf(b, sizeof(b),
			 "touch %s\n raw %d,%d z%d\n X %d-%d\n Y %d-%d\n\n"
			 "accel %s\n x%d y%d z%d\n\nkey %d  r%d c%d\n"
			 "rows 0x%02x\n\n%s  wakes %u\nirqs %u  motions %u\nwoke by %s",
			 last_pressed ? "DOWN" : "up", last_rx, last_ry, last_z,
			 minX, maxX, minY, maxY,
			 accel_ok ? "ok" : "NOT FOUND", ax, ay, az,
			 key == KEY_NONE ? -1 : key, krow, kcol, keypad_rows(),
			 asleep ? "ASLEEP" : "awake", wakes,
			 wake_count(), motion_events, woke_by);
		lv_label_set_text(info, b);
		MARK(0x34, last_pressed);

		lv_timer_handler();
		if (asleep) {
			/* block in WFI until an EXTI edge or the poll timeout */
			MARK(0x20, wake_count());
			wake_wait(K_MSEC(250));
		} else {
			k_busy_wait(8000);
			k_yield();
		}
	}
	return 0;
}
