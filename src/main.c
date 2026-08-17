/*
 * RTI T2i firmware — top level.
 *
 * Deliberately thin: bring the subsystems up in a safe order, then gather
 * inputs, feed the sleep policy, and render. Everything substantial lives in a
 * module:
 *
 *   updater.c   USB update receiver  (anti-brick path — started first)
 *   ui.c        LVGL screens, splash, touch->pointer binding
 *   power.c     sleep/wake policy
 *   wake.c      EXTI wake sources (keypad rows, accel INT1)
 *   lowpower.c  STM32 STOP mode primitive
 *   keypad.c    8x7 matrix        accel.c  LIS3DH
 *   touch.c     4-wire resistive  hx8347_fsmc.c  display driver
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include "status.h"
#include "ui.h"
#include "power.h"
#include "updater.h"
#include "touch.h"
#include "accel.h"
#include "keypad.h"
#include "wake.h"
#include "lowpower.h"

#define SPLASH_MS 3000

#define MARK(off, v) (*(volatile uint32_t *)(0x2001FF00 + (off)) = (uint32_t)(v))

int main(void)
{
	const struct device *disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

	/* The USB update receiver comes up FIRST and in its own thread. It is the
	 * only way back into a remote without SWD, so it must not depend on the
	 * display, LVGL, or anything else below surviving. */
	updater_init();
	MARK(0x00, 1);

	if (!device_is_ready(disp)) {
		/* No screen, but USB is already live — still recoverable. */
		while (1) { k_msleep(1000); }
	}

	ui_init(disp, SPLASH_MS);
	MARK(0x00, 2);

	touch_init();
	beep_init();
	keypad_init();

	/* Recovery: hold any key while powering on and the remote never sleeps,
	 * giving a deterministic always-attachable target for SWD flashing. */
	bool recovery = keypad_any();

	ui_touch_indev_init();
	bool accel_ok = accel_init();
	bool wake_ok = wake_init();
	power_init(disp, recovery);
	MARK(0x00, (accel_ok ? 7 : 0x70) | (wake_ok ? 0 : 0x700));

	struct t2i_status st = {
		.accel_ok = accel_ok,
		.recovery = recovery,
		.woke_by = "-",
		.clk = "?",
	};
	uint32_t beat = 0;

	while (1) {
		MARK(0x04, ++beat);

		st.key = keypad_scan(&st.key_row, &st.key_col);
		st.key_rows = keypad_rows();

		bool motion = accel_motion();
		if (motion) {
			st.motion_events++;
		}

		/* While asleep LVGL is not running, so its read callback never fires
		 * and ui_touch_down() would go stale — read the panel directly. */
		bool touched = power_asleep() ? touch_read(NULL, NULL, NULL)
					      : ui_touch_down();
		bool activity = touched || motion || st.key != KEY_NONE;
		const char *source = touched ? "touch"
					     : (st.key != KEY_NONE ? "KEY" : "motion");

		if (power_tick(activity, source)) {
			continue;   /* asleep: power_tick already blocked for us */
		}

		accel_read(&st.accel_x, &st.accel_y, &st.accel_z);
		ui_touch_raw(&st.touch_x, &st.touch_y, &st.touch_z);
		ui_touch_range(&st.touch_min_x, &st.touch_max_x,
			       &st.touch_min_y, &st.touch_max_y);
		st.touch_down = touched;
		st.asleep = power_asleep();
		st.wakes = power_wakes();
		st.woke_by = power_woke_by();
		st.wake_irqs = wake_count();
		st.stops = lowpower_stop_count();
		st.clk = lowpower_sysclk_src() == 2 ? "PLL120"
			: (lowpower_sysclk_src() == 1 ? "HSE" : "HSI16");
		st.usb_busy = updater_busy();
		st.usb_received = updater_received();
		st.usb_declared = updater_declared();

		ui_render(&st);
		k_msleep(10);   /* idle between frames; keeps SWD able to halt us */
	}
	return 0;
}
