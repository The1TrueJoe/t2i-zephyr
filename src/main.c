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

extern const lv_image_dsc_t juno_logo;   /* src/juno_logo.c */

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

static uint32_t taps;

static void btn_clicked(lv_event_t *e)
{
	lv_obj_t *label = lv_event_get_user_data(e);
	char b[24];

	snprintf(b, sizeof(b), "taps: %u", ++taps);
	lv_label_set_text(label, b);
	MARK(0x48, taps);
	beep_click();
}

int main(void)
{
	const struct device *disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	if (!device_is_ready(disp)) {
		return 0;
	}
	display_blanking_off(disp);
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

	for (int i = 0; i < 60; i++) {
		lv_timer_handler();
		k_busy_wait(50000);
	}
	lv_obj_delete(logo);

	touch_init();
	beep_init();
	MARK(0x00, 2);
	beep_test();   /* two beeps at boot — tells us if the speaker path works */
	MARK(0x00, 3);

	lv_indev_t *indev = lv_indev_create();
	lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
	lv_indev_set_read_cb(indev, touch_lv_read);
	MARK(0x00, 4);

	MARK(0x00, 5);

	lv_obj_t *title = lv_label_create(scr);
	lv_label_set_text(title, "Tap the button");
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

	lv_obj_t *count = lv_label_create(scr);
	lv_label_set_text(count, "taps: 0");
	lv_obj_align(count, LV_ALIGN_TOP_MID, 0, 28);

	MARK(0x00, 6);
	lv_obj_t *btn = lv_button_create(scr);
	lv_obj_set_size(btn, 140, 70);
	lv_obj_center(btn);
	lv_obj_add_event_cb(btn, btn_clicked, LV_EVENT_CLICKED, count);
	lv_obj_t *btn_label = lv_label_create(btn);
	lv_label_set_text(btn_label, "TAP ME");
	lv_obj_center(btn_label);

	/* calibration readout — drag the corners and copy these into X_LO..Y_HI */
	lv_obj_t *info = lv_label_create(scr);
	lv_label_set_text(info, "raw: --");
	lv_obj_align(info, LV_ALIGN_BOTTOM_MID, 0, -6);
	MARK(0x00, 7);

	uint32_t beat = 0;
	while (1) {
		char b[64];
		MARK(0x04, ++beat);   /* heartbeat — proves the render loop is alive */
		snprintf(b, sizeof(b), "%s %d,%d z%d\nX %d-%d  Y %d-%d",
			 last_pressed ? "raw" : "up ", last_rx, last_ry, last_z,
			 minX, maxX, minY, maxY);
		lv_label_set_text(info, b);
		MARK(0x34, last_pressed);

		lv_timer_handler();
		/* pace without sleeping — never WFI, so the CPU stays SWD-catchable */
		k_busy_wait(8000);
		k_yield();
	}
	return 0;
}
