/*
 * T2i LVGL app: renders a live X/Y/Z accelerometer chart on the HX8347 LCD.
 * Display comes from the hx8347_fsmc Zephyr driver (chosen zephyr,display);
 * LVGL auto-inits against it. Accelerometer = LIS3DH on I2C2, slave 0x18.
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/i2c.h>
#include <lvgl.h>
#include <stdio.h>
#include <stdint.h>

#define ACC_ADDR 0x18
static const struct device *i2c2;

static int acc_wr(uint8_t reg, uint8_t val)
{
	return i2c_reg_write_byte(i2c2, ACC_ADDR, reg, val);
}
static int acc_rd(uint8_t reg, uint8_t *buf, uint8_t n)
{
	return i2c_burst_read(i2c2, ACC_ADDR, (n > 1) ? (reg | 0x80) : reg, buf, n);
}

int main(void)
{
	const struct device *disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	if (!device_is_ready(disp)) {
		return 0;
	}
	display_blanking_off(disp);

	/* accelerometer bring-up */
	i2c2 = DEVICE_DT_GET(DT_NODELABEL(i2c2));
	bool acc_ok = device_is_ready(i2c2);
	if (acc_ok) {
		acc_wr(0x20, 0x57);   /* CTRL_REG1: 100 Hz, X/Y/Z enabled */
		acc_wr(0x23, 0x08);   /* CTRL_REG4: high-resolution */
	}

	/* --- LVGL UI --- */
	lv_obj_t *scr = lv_scr_act();

	lv_obj_t *title = lv_label_create(scr);
	lv_label_set_text(title, "T2i accelerometer");
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

	lv_obj_t *chart = lv_chart_create(scr);
	lv_obj_set_size(chart, 224, 220);
	lv_obj_align(chart, LV_ALIGN_CENTER, 0, 0);
	lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
	lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, -160, 160);
	lv_chart_set_point_count(chart, 60);
	lv_chart_series_t *sx = lv_chart_add_series(chart, lv_color_hex(0xFF3030), LV_CHART_AXIS_PRIMARY_Y);
	lv_chart_series_t *sy = lv_chart_add_series(chart, lv_color_hex(0x30FF30), LV_CHART_AXIS_PRIMARY_Y);
	lv_chart_series_t *sz = lv_chart_add_series(chart, lv_color_hex(0x4080FF), LV_CHART_AXIS_PRIMARY_Y);

	lv_obj_t *val = lv_label_create(scr);
	lv_label_set_text(val, "X:-- Y:-- Z:--");
	lv_obj_align(val, LV_ALIGN_BOTTOM_MID, 0, -8);

	while (1) {
		if (acc_ok) {
			uint8_t d[6];
			if (acc_rd(0x28, d, 6) == 0) {
				int16_t x = (int16_t)(d[0] | (d[1] << 8));
				int16_t y = (int16_t)(d[2] | (d[3] << 8));
				int16_t z = (int16_t)(d[4] | (d[5] << 8));
				lv_chart_set_next_value(chart, sx, x);
				lv_chart_set_next_value(chart, sy, y);
				lv_chart_set_next_value(chart, sz, z);
				char b[40];
				snprintf(b, sizeof(b), "X:%d  Y:%d  Z:%d", x, y, z);
				lv_label_set_text(val, b);
			}
		}
		lv_timer_handler();
		k_msleep(30);
	}
	return 0;
}
