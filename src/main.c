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
#include "funlight.h"
#include "ir.h"
#include "zbx.h"

uint16_t hx8347_panel_id(void);
int hx8347_backlight_pct(void);
void hx8347_backlight_state(uint32_t *out);
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
/* Bumped by hand. On a USB-only remote there is otherwise no way to tell which
 * image is actually running, and "did the update commit?" is the single most
 * important question the update path has to answer. */
#define FW_VERSION "dev"

/* Set to 1 to reset before ever reaching safety_mark_healthy(), so the boot
 * counter climbs and safe mode engages. This is how the recovery path gets
 * re-tested after changes to boot, USB or the watchdog — see docs/USB-FLASHING.md. */
#define FORCE_UNHEALTHY 0

/* Deliberately unbootable firmware, for testing the recovery path for real.
 *   1 = hard fault AFTER updater_init()  — should recover via safe mode
 *   2 = hang BEFORE updater_init()       — expected to be UNRECOVERABLE over USB
 * See docs/USB-FLASHING.md. Leave at 0. */
#define BRICK_TEST 0

#define IR_ENABLE_TEST 0   /* browns the remote out — see below */
#define KEY_HOLD_MS 400  /* a press this long is a hold, not a click */
/* Ambient light -> backlight. Thresholds are MEASURED on this hardware, not scaled:
 *   thumb fully over the sensor   1..6
 *   ordinary lit room             5..20
 *   torch pointed at it           400
 * So the entire indoor span is a couple of dozen counts out of 4095, and anything that treated
 * the reading as a 0..4095 range would sit on the bottom step forever. `above` is the raw count
 * a step needs to beat, walked from brightest down. */
static const struct { int above; int pct; } ALS_STEPS[] = {
	{ 100, 90 },   /* daylight or a torch on it */
	{  20, 70 },
	{  10, 50 },
	{   3, 30 },
	{  -1, 15 },   /* covered, or a dark room */
};

/* How long a light level must hold before the backlight follows it.
 *
 * Long on purpose, and longer than the LED's: a hand passing over the remote, or somebody
 * walking between it and a lamp, must not step the screen. Slow is also what stops a feedback
 * loop if the panel's own backlight reaches the sensor -- a fast loop there would ramp itself
 * to full. */
#define ALS_HOLD_MS 2500

/* EMA shift: the average trails by roughly 2^ALS_SMOOTH samples of the 10ms loop, so ~0.6s. */
#define ALS_SMOOTH 6

/* Set to 0 to pin the backlight at BACKLIGHT_PCT and ignore the sensor. */
#define AUTO_BRIGHTNESS 1

#define LED_HOLD_MS 3000 /* how long a charger state must hold before the LED follows */
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

	if (BRICK_TEST == 2) {
		/* Before USB exists, so no boot can ever enumerate and safe mode can
		 * never be reached. This is the documented gap, made real. */
		while (1) {
		}
	}

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

	if (BRICK_TEST == 1) {
		/* An undefined instruction: a guaranteed UsageFault -> hard fault. A
		 * write to an unmapped address is NOT reliable here — 0xFFFFFFF0 sits in
		 * the vendor/PPB region and the store is simply ignored, which is how
		 * the first attempt at this test silently did nothing.
		 *
		 * Placed after the safe-mode branch, exactly as a broken subsystem would
		 * be: safe mode does not start it, so the remote stays reachable. */
		__asm__ volatile("udf #0");
	}

	if (FORCE_UNHEALTHY) {
		k_msleep(1500);
		safety_force_reset();
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
	funlight_init();

	/* Boot sweep: light each channel in turn, then all three. Which physical
	 * colour each channel drives is not in the decomp, so this is how they get
	 * labelled — the name printed over USB is the channel that is lit. */
	static const char *const fl_ch[3] = { "red", "green", "blue" };

	for (int c = 0; c < 3; c++) {
		funlight_set(c == 0, c == 1, c == 2, 100);
		updater_emit(fl_ch[c]);
		k_msleep(500);
	}
	funlight_set(true, true, true, 100);
	updater_emit("all three");
	k_msleep(500);

	{
		char idb[40];

		updater_emit("T2i fw " FW_VERSION);
		/* Radio init goes here — in main, after updater_init(). NEVER in a
		 * driver-init hook: anything failing before USB is up cannot be
		 * recovered on a remote without SWD (docs/USB-FLASHING.md). */
		zbx_uart_init();
		updater_emit(zbx_selftest() ? "ZBX selftest PASS" : "ZBX selftest FAIL");
		snprintf(idb, sizeof(idb), "PANEL id=0x%04x", hx8347_panel_id());
		updater_emit(idb);
	}
	{
		uint32_t bl[8];
		char blb[128];

		hx8347_backlight_state(bl);
		snprintf(blb, sizeof(blb),
			 "BL cr1=%08x arr=%u ccr2=%u ccer=%04x ccmr1=%04x "
			 "moder_a=%08x afrl_a=%08x idr_c=%08x",
			 bl[0], bl[1], bl[2], bl[3], bl[4], bl[5], bl[6], bl[7]);
		updater_emit(blb);
	}

	ir_init();
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
	int last_led = -1, led_pending = -1;
	int64_t key_down_at = 0;
	bool key_held_sent = false;
	int64_t led_since = 0;
	int als_step = -1, als_pending = -1;
	int64_t als_since = 0;
	int32_t als_acc = -1;   /* EMA accumulator, value << ALS_SMOOTH */
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
				beep_click();   /* stock clicks on every key, not just some */
				snprintf(ev, sizeof(ev), "KEY DOWN %u %s r%d c%d",
					 st.key, st.key_name, st.key_row, st.key_col);
			} else {
				snprintf(ev, sizeof(ev), "KEY UP %u %s", last_reported_key,
					 keypad_name(last_reported_key));
			}
			updater_emit(ev);
			last_reported_key = st.key;

			key_down_at = (st.key != KEY_NONE) ? k_uptime_get() : 0;
			key_held_sent = false;

			/* Info toggles the full bring-up dump. */
			if (st.key == KEY_INFO) {
				st.debug = !st.debug;
			}

			/* IR check, DISABLED: sending a frame browns the remote out. A
			 * NEC header is a 9ms mark, and if the envelope on PB15 is not
			 * actually gating the PB0 carrier the way the decomp reads, that is
			 * ~68ms of continuous 50%-duty LED drive — enough to drop the rail.
			 * Do not re-enable until the gating is confirmed on a scope or the
			 * drive current is measured. */
			if (IR_ENABLE_TEST && st.key == IR_TEST_KEY) {
				ir_send_nec(0x00, 0x15);
				updater_emit("IR sent NEC 00 15");
			}
		}

		/* A held key, reported once when it crosses the threshold.
		 *
		 * This has to happen here and not host-side: a Juno driver runs in a sandbox with no
		 * clock, so it can see DOWN and UP but cannot time the gap between them. Without this
		 * line nothing downstream can tell a tap from a hold, and a hold-to-ramp rule has
		 * nothing to start on. */
		if (st.key != KEY_NONE && !key_held_sent && key_down_at &&
		    k_uptime_get() - key_down_at >= KEY_HOLD_MS) {
			char ev[48];

			snprintf(ev, sizeof(ev), "KEY HELD %u %s", st.key, st.key_name);
			updater_emit(ev);
			key_held_sent = true;
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
			funlight_init();     /* stock re-sends the resync command on wake */
			last_led = -1;       /* force the indicator to be re-applied */
		}

		accel_read(&st.accel_x, &st.accel_y, &st.accel_z);
		st.batt_raw = battery_raw();
		st.batt_low = battery_low();
		st.als = als_raw();

		/* Smooth before deciding anything. The raw reading jitters by several counts, and the
		 * whole indoor range is only a couple of dozen counts wide — so raw samples cross a
		 * threshold constantly, which reset the hold timer below on almost every pass and meant
		 * the backlight never actually moved. */
		if (st.als >= 0) {
			if (als_acc < 0) {
				als_acc = st.als << ALS_SMOOTH;   /* seed, so boot does not ramp from 0 */
			}
			als_acc += st.als - (als_acc >> ALS_SMOOTH);
			st.als_avg = als_acc >> ALS_SMOOTH;
		}
		st.backlight = hx8347_backlight_pct();
		st.charger = battery_charger_present();
		st.charge_state = battery_charge_state();

		/* Auto-brightness. Only written on an actual change: every write reprograms TIM2. */
		if (AUTO_BRIGHTNESS && als_acc >= 0) {
			int step = 0;

			while (ALS_STEPS[step].above >= 0 && st.als_avg <= ALS_STEPS[step].above) {
				step++;
			}
			if (step != als_pending) {
				als_pending = step;
				als_since = k_uptime_get();
			} else if (step != als_step &&
				   k_uptime_get() - als_since >= ALS_HOLD_MS) {
				als_step = step;
				display_set_brightness(disp,
						       (uint8_t)(ALS_STEPS[step].pct * 255 / 100));
			}
		}

		/* Front-panel indicator, following stock's states (FUN_0800e214):
		 * on battery = one colour, charging = another, complete = a third.
		 * Which physical colour each channel drives is NOT yet confirmed —
		 * no part number appears near this code — so these are channel
		 * indices, to be labelled once observed on hardware.
		 * Only written on change: a frame locks interrupts for ~3ms. */
		/* On battery the indicator stays dark unless the pack is actually low.
		 * Stock has a third state here, but a permanently lit LED on a remote
		 * that spends its life asleep is not worth the drain. */
		int led = st.charger ? (st.charge_state == 2 ? 1 : 2) : (st.batt_low ? 0 : -1);

		/* A topped-off pack really does toggle between charging and complete,
		 * and an IR burst draws enough current to nudge the charger IC on its
		 * own — so this is real chatter, not a glitch to filter out. Require a
		 * state to hold for LED_HOLD_MS before the colour follows it. */
		if (led != led_pending) {
			led_pending = led;
			led_since = k_uptime_get();
		}
		if (k_uptime_get() - led_since < LED_HOLD_MS) {
			led = last_led;
		}

		if (led != last_led) {
			funlight_set(led == 1, led == 2, led == 0, 40);
			last_led = led;
		}
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
