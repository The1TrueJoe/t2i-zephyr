/*
 * RTI T2i firmware — top level.
 *
 * Boot order is chosen for recoverability, not convenience. The radio-equipped
 * remote has no SWD, and the RTI bootloader has no USB of its own, so the
 * application enumerating USB *is* the only way back into that unit. Anything
 * that could stop it is therefore started after it, and guarded:
 *
 *   1. safety_boot_check()  count this boot; go USB-only if the last few failed
 *   2. updater_init()       USB update receiver, in its own thread
 *   3. watchdog             a hang now becomes a reset, not a dead remote
 *   4. everything else      display, LVGL, touch, keypad, accelerometer
 *   5. safety_mark_healthy()once the loop has genuinely run for a while
 *
 * Modules: updater.c (USB), ui.c (LVGL), power.c (sleep policy), wake.c (EXTI),
 * lowpower.c (STOP), safety.c (watchdog + boot counting), keypad/accel/touch,
 * hx8347_fsmc.c (display driver).
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <stdio.h>
#include "status.h"
#include "ui.h"
#include "power.h"
#include "updater.h"
#include "safety.h"
#include "touch.h"
#include "accel.h"
#include "keypad.h"
#include "battery.h"
#include "wake.h"
#include "lowpower.h"

#define SPLASH_MS 3000

/* Run this long without incident before declaring the boot good. Long enough to
 * be past init and a few hundred render passes. */
#define HEALTHY_AFTER_MS 10000

/* Hold the debug key (stock code 180) this long to arm the watchdog self-test:
 * feeding stops, and the remote should reset itself ~8s later with the reason
 * shown as "rst WATCHDOG" on the next boot. Proving the safety net works beats
 * assuming it does. */
#define DEBUG_HOLD_MS 3000

#define MARK(off, v) (*(volatile uint32_t *)(0x2001FF00 + (off)) = (uint32_t)(v))

/* USB-only safe mode: the previous boots failed, so run the update receiver and
 * absolutely nothing else. Whatever was crashing — display, LVGL, touch, radio
 * — is not started here, so the host can always push a working image. */
static void safe_mode(void)
{
	MARK(0x00, 0x5AFE);

	/* Clear the counter on the way in, so safe mode is one-shot: the next boot
	 * tries the real firmware again. Without this, anything that resets before
	 * the healthy mark — including a developer's st-flash --reset — latches the
	 * remote into safe mode permanently. If the firmware really is broken it
	 * simply fails three more times and lands back here, which is the
	 * self-recovering behaviour we want. */
	safety_mark_healthy();

	while (1) {
		safety_watchdog_feed();
		k_msleep(100);
	}
}

int main(void)
{
	bool unsafe_boot = safety_boot_check();

	/* USB first, always: it is the recovery path and must not depend on
	 * anything below it surviving. */
	updater_init();
	MARK(0x00, 1);

	/* Only now arm the watchdog — USB is up, so a later hang resets into a
	 * boot that still enumerates. */
	safety_watchdog_start();

	if (unsafe_boot) {
		safe_mode();   /* never returns */
	}

	const struct device *disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

	if (!device_is_ready(disp)) {
		safe_mode();   /* no screen, but USB is live — still recoverable */
	}

	ui_init(disp, SPLASH_MS);
	MARK(0x00, 2);

	touch_init();
	beep_init();
	keypad_init();

	/* Hold any key while powering on: never sleep. Gives a deterministic
	 * always-attachable target for SWD flashing on the bench unit. */
	bool recovery = keypad_any();

	ui_touch_indev_init();
	battery_init();
	bool accel_ok = accel_init();
	bool wake_ok = wake_init();
	power_init(disp, recovery);
	MARK(0x00, (accel_ok ? 7 : 0x70) | (wake_ok ? 0 : 0x700));

	struct t2i_status st = {
		.accel_ok = accel_ok,
		.recovery = recovery,
		.woke_by = "-",
		.clk = "?",
		.boot_attempts = safety_boot_attempts(),
		.reset_cause = safety_reset_cause(),
	};
	uint32_t beat = 0;
	uint8_t last_reported_key = KEY_NONE;
	int64_t debug_held_since = 0;
	bool healthy = false;
	int64_t started = k_uptime_get();

	while (1) {
		MARK(0x04, ++beat);
		safety_watchdog_feed();

		if (!healthy && k_uptime_get() - started > HEALTHY_AFTER_MS) {
			safety_mark_healthy();
			healthy = true;
			st.healthy = true;
		}

		st.key = keypad_scan(&st.key_row, &st.key_col);
		st.key_rows = keypad_rows();
		st.key_name = keypad_name(st.key);

		/* Report key transitions to the host over USB CDC. This is the path to
		 * publishing button presses without the radio: tools/t2i_mqtt_bridge.py
		 * reads these lines and forwards them to MQTT. */
		if (st.key != last_reported_key) {
			char ev[48];

			if (st.key != KEY_NONE) {
				snprintf(ev, sizeof(ev), "KEY DOWN %u %s r%d c%d",
					 st.key, st.key_name, st.key_row, st.key_col);
			} else {
				snprintf(ev, sizeof(ev), "KEY UP %u %s", last_reported_key,
					 keypad_name(last_reported_key));
			}
			updater_emit(ev);
			last_reported_key = st.key;
		}

		/* debug key: hold to arm the watchdog self-test */
		if (st.key == KEY_DEBUG) {
			if (debug_held_since == 0) {
				debug_held_since = k_uptime_get();
			}
			st.debug_hold_ms = (uint32_t)(k_uptime_get() - debug_held_since);
			if (st.debug_hold_ms >= DEBUG_HOLD_MS && !st.wdt_test_armed) {
				safety_watchdog_selftest();
				st.wdt_test_armed = true;
			}
		} else {
			debug_held_since = 0;
			st.debug_hold_ms = 0;
		}

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

		bool was_asleep = power_asleep();

		if (power_tick(activity, source)) {
			continue;   /* asleep: power_tick already blocked for us */
		}

		if (was_asleep) {
			/* Just woke: the panel was powered down, so its framebuffer is
			 * gone and LVGL would otherwise repaint nothing. */
			ui_invalidate();
		}

		accel_read(&st.accel_x, &st.accel_y, &st.accel_z);
		st.batt_raw = battery_raw();
		st.batt_low = battery_low();
		st.charger = battery_charger_present();
		st.charge_state = battery_charge_state();
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
