/*
 * T2i touch demo: draws a pin where you touch the resistive screen and plays a
 * click on the beeper. Display via the hx8347_fsmc Zephyr driver + LVGL;
 * touch + beep via src/touch.c (ADC2 4-wire + TIM3/PB0).
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <lvgl.h>
#include <stdio.h>
#include <stdint.h>
#include "touch.h"

#define MARK(off, v) (*(volatile uint32_t *)(0x2001FF00 + (off)) = (uint32_t)(v))

/* Raw-ADC -> screen calibration, measured on-device (drag all four corners).
 * X raw 2700..3827 -> 0..239 ; Y raw 2315..3535 -> 0..319. Flip a MIN/MAX pair
 * if that axis comes out mirrored. */
#define X_MIN 2700
#define X_MAX 3827
#define Y_MIN 2315
#define Y_MAX 3535
static int clampi(int v, int hi) { return v < 0 ? 0 : (v > hi ? hi : v); }
static int map_x(int raw) { return clampi((raw - X_MIN) * 240 / (X_MAX - X_MIN), 239); }
static int map_y(int raw) { return clampi((raw - Y_MIN) * 320 / (Y_MAX - Y_MIN), 319); }

int main(void)
{
	const struct device *disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	if (!device_is_ready(disp)) {
		return 0;
	}
	display_blanking_off(disp);

	/* Boot flash-window: stay fully awake ~3s so SWD/st-flash can always catch
	 * this CPU. NRST is dead on this unit, so a sleeping (WFI) CPU can't be
	 * grabbed to flash — keeping it awake makes every future flash trivial. */
	for (int i = 0; i < 60; i++) { k_busy_wait(50000); }

	touch_init();
	beep_init();
	beep_test();   /* two beeps at boot — tells us if the speaker path works */

	lv_obj_t *scr = lv_scr_act();

	lv_obj_t *title = lv_label_create(scr);
	lv_label_set_text(title, "Touch the screen");
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

	lv_obj_t *info = lv_label_create(scr);
	lv_label_set_text(info, "raw: --");
	lv_obj_align(info, LV_ALIGN_BOTTOM_MID, 0, -6);

	/* the "pin": a small red circle, hidden until touched */
	lv_obj_t *pin = lv_obj_create(scr);
	lv_obj_set_size(pin, 18, 18);
	lv_obj_set_style_radius(pin, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(pin, lv_color_hex(0xFF3040), 0);
	lv_obj_set_style_border_width(pin, 2, 0);
	lv_obj_set_style_border_color(pin, lv_color_hex(0xFFFFFF), 0);
	lv_obj_add_flag(pin, LV_OBJ_FLAG_HIDDEN);

	int was_pressed = 0;
	uint32_t n = 0, presses = 0;
	int minX = 9999, maxX = 0, minY = 9999, maxY = 0;   /* calibration capture */
	while (1) {
		int rx = 0, ry = 0;
		int pressed = touch_read(&rx, &ry);
		MARK(0x30, (uint16_t)rx | ((uint32_t)(uint16_t)ry << 16));   /* raw X | Y<<16 */
		MARK(0x34, pressed);
		MARK(0x38, ++n);

		if (pressed) {
			if (rx < minX) minX = rx;  if (rx > maxX) maxX = rx;
			if (ry < minY) minY = ry;  if (ry > maxY) maxY = ry;
			MARK(0x40, (uint16_t)minX | ((uint32_t)(uint16_t)maxX << 16));
			MARK(0x44, (uint16_t)minY | ((uint32_t)(uint16_t)maxY << 16));
			MARK(0x48, ++presses);
			int sx = map_x(rx);
			int sy = map_y(ry);
			lv_obj_set_pos(pin, sx - 9, sy - 9);
			lv_obj_clear_flag(pin, LV_OBJ_FLAG_HIDDEN);
			char b[48];
			snprintf(b, sizeof(b), "raw %d,%d  -> %d,%d", rx, ry, sx, sy);
			lv_label_set_text(info, b);
			if (!was_pressed) {
				beep_click();   /* click on touch-down */
			}
		} else {
			lv_obj_add_flag(pin, LV_OBJ_FLAG_HIDDEN);
		}
		was_pressed = pressed;

		lv_timer_handler();
		/* pace without sleeping — never WFI, so the CPU stays SWD-catchable */
		k_busy_wait(8000);
		k_yield();
	}
	return 0;
}
