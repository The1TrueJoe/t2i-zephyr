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
#include "em250.h"
#include "zbx.h"
#include "zbnet.h"

uint16_t hx8347_panel_id(void);
int hx8347_backlight_pct(void);
void hx8347_backlight_state(uint32_t *out);
#include "wake.h"
#include "lowpower.h"

#define SPLASH_MS 3000

/* Run this long without incident before declaring the boot good. Long enough to
 * be past init and a few hundred render passes. */
#define HEALTHY_AFTER_MS 10000

/* Stamped from git at build time (git describe --always --dirty) — never
 * hand-tagged. On a USB-only remote the boot banner is still the only way to
 * confirm which commit is actually running. */
#ifndef FW_GIT_VERSION
#define FW_GIT_VERSION "nogit"
#endif
#define FW_VERSION FW_GIT_VERSION

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

/* How often to report the radio link over USB. */
#define ZBX_REPORT_MS 10000

/* Reflash the EM250 with RTI's ZB-Pro coordinator image, once, on the first pass after boot.
 * Set 0 once the radio already runs Telegesis — re-running just reflashes the same image
 * needlessly. */
#define EM250_FLASH 0

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

	/* Say so, repeatedly. A one-shot line at boot is unobservable on a USB-only unit: the CDC
	 * port stalls ~25s on first open, so by the time a host is listening the boot is long past.
	 * Silence and a working port look identical, which is exactly the confusion this cost. */
	int64_t said = 0;

	while (1) {
		safety_watchdog_feed();
		if (k_uptime_get() - said >= 2000) {
			said = k_uptime_get();
			updater_emit("SAFE MODE — USB only, previous boots failed");
		}
		k_msleep(100);
	}
}

/* Upload progress. Called every 8 blocks, mostly so the watchdog gets fed — the transfer runs far
 * longer than its ~8 s window. The emit is throttled separately so the log stays readable. */
static void flash_progress(unsigned done, unsigned total)
{
	char line[64];

	safety_watchdog_feed();
	if ((done % 64u) == 0u || done == total) {
		snprintf(line, sizeof line, "EM250: %u/%u blocks", done, total);
		updater_emit(line);
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
	safety_watchdog_feed();

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

	/* Feed across every one of these. They run AFTER safety_watchdog_start(), and together with
	 * the splash they very nearly exhaust the 8s IWDG period on their own — adding a blocking
	 * radio probe here once pushed boot past it, which reset-looped the remote into safe mode.
	 * On a unit with no SWD that is only recoverable because safe mode keeps USB up. */
	for (int c = 0; c < 3; c++) {
		funlight_set(c == 0, c == 1, c == 2, 100);
		updater_emit(fl_ch[c]);
		safety_watchdog_feed();
		k_msleep(500);
	}
	funlight_set(true, true, true, 100);
	updater_emit("all three");
	safety_watchdog_feed();
	k_msleep(500);

	{
		char idb[40];

		updater_emit("T2i fw " FW_VERSION);
		/* Radio init goes here — in main, after updater_init(). NEVER in a
		 * driver-init hook: anything failing before USB is up cannot be
		 * recovered on a remote without SWD (docs/USB-FLASHING.md). */
		safety_watchdog_feed();
		zbx_uart_init();
		safety_watchdog_feed();
		updater_emit(zbx_selftest() ? "ZBX selftest PASS" : "ZBX selftest FAIL");
		{
			uint8_t pd, pu;
			char lb[64];

			zbx_rx_line(&pd, &pu);
			uint32_t pclk = zbx_pclk1();

			snprintf(lb, sizeof(lb), "ZBX pclk1=%u baud=%u", pclk, pclk / 260u);
			updater_emit(lb);
			snprintf(lb, sizeof(lb), "ZBX rx-line pulldown=%u pullup=%u (%s)", pd, pu,
				 (pd && pu) ? "DRIVEN - radio present" : "FLOATING - nothing driving");
			updater_emit(lb);
		}
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
	static bool flashed;
	int last_led = -1, led_pending = -1;
	int64_t key_down_at = 0;
	bool key_held_sent = false;
	int64_t led_since = 0;
	int als_step = -1, als_pending = -1;
	int64_t als_since = 0;
	int32_t als_acc = -1;   /* EMA accumulator, value << ALS_SMOOTH */
	int64_t zbx_last_report = 0;
	int64_t debug_held_since = 0;
	int64_t backlight_held_since = 0;
	bool menu_lit = false;
	bool healthy = false;
	int64_t started = k_uptime_get();

	zbnet_start();   /* radio runs on its own thread from here; main never touches USART3 again */

	while (1) {
		MARK(0x04, ++beat);
		safety_watchdog_feed();

		if (!healthy && k_uptime_get() - started > HEALTHY_AFTER_MS) {
			safety_mark_healthy();
			healthy = true;
			st.healthy = true;
		}

		/* Report the radio periodically, not just at boot. The CDC port stalls ~25s on first
		 * open, so a one-shot boot banner is unobservable on a unit with no SWD — by the time
		 * a host is listening it is long gone. */
		if (k_uptime_get() - zbx_last_report >= ZBX_REPORT_MS) {
			uint32_t f, bad, by;
			char line[72];

			zbx_last_report = k_uptime_get();
			zbx_stats(&f, &bad, &by);
			uint8_t raw[24];
			size_t rn = zbx_raw(raw, sizeof raw);
			int w = snprintf(line, sizeof(line), "ZBX frames=%u bad=%u bytes=%u raw:", f, bad, by);

			for (size_t k = 0; k < rn && w < (int)sizeof(line) - 4; k++) {
				w += snprintf(line + w, sizeof(line) - (size_t)w, " %02x", raw[k]);
			}
			updater_emit(line);

			/* Display state, repeated rather than printed once at boot: the boot banner is
			 * unobservable on a USB-only unit (the CDC port stalls ~25s on open), which is
			 * exactly the situation a dark screen leaves you in. */
			snprintf(line, sizeof(line), "DISP panel=0x%04x bl=%d%% als=%d~%d",
				 hx8347_panel_id(), hx8347_backlight_pct(), st.als, st.als_avg);
			updater_emit(line);

			/* Reflash the radio, once.
			 *
			 * RTI's TXBZB app cannot join any network we can build: it demands the
			 * network key APS-encrypted and holds no preconfigured key to decrypt
			 * it with, and neither end can be talked out of that. The EM250 is a
			 * XAP2b SoC with no obtainable toolchain, and an "EM250 NCP image" does
			 * not exist (EZSP is an EM260/SN260 protocol) — so RTI's own ZB-Pro
			 * coordinator build is the only radio firmware we will ever have.
			 *
			 * Cannot brick it: the image's 97 records write only 0x02800-0x19BC8,
			 * 0x1C000-0x1CAAE and 0x1DF32-0x1DFFC, so the bootloader's reserved
			 * 0x0000-0x27FF is never touched, and a failed upload merely leaves the
			 * app invalid — which keeps the bootloader resident. Not undoable: no
			 * copy of TXBZB exists and the radio has no read-out path. */
			if (EM250_FLASH && !flashed) {
				char info[64];

				flashed = true;
				updater_emit("EM250: entering bootloader");
				if (!em250_bl_enter()) {
					updater_emit("EM250: entry FAILED - radio untouched");
				} else {
					bool ok;

					em250_bl_info(info, sizeof info);
					snprintf(line, sizeof(line), "EM250: before = %s", info);
					updater_emit(line);

					snprintf(line, sizeof(line), "EM250: flashing Telegesis %u bytes",
						 (unsigned)etrx2_ebl_raw_len);
					updater_emit(line);

					ok = em250_flash_ebl(etrx2_ebl_raw, etrx2_ebl_raw_len,
							     flash_progress);
					updater_emit(ok ? "EM250: upload OK" : "EM250: upload FAILED");
					safety_watchdog_feed();

					if (!em250_bl_enter()) {
						updater_emit("EM250: re-entry failed; resetting radio");
						zbx_uart_init();
					} else {
						em250_bl_info(info, sizeof info);
						snprintf(line, sizeof(line), "EM250: after  = %s", info);
						updater_emit(line);
						em250_bl_run();
					}
					safety_watchdog_feed();
				}
			}

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
				/* A volume adjust previews the new level itself, so skip the
				 * click there; every other key clicks. */
				bool vol_adjust = (st.menu == 2 &&
						   (st.key == KEY_LEFT || st.key == KEY_RIGHT));
				if (!vol_adjust) {
					beep_click();   /* stock clicks on every key, not just some */
				}
				if (st.menu) {
					/* The menu is modal: the D-pad navigates it locally and
					 * nothing goes out over RF. Pages: 1 connectivity,
					 * 2 settings, 3 debug. */
					switch (st.key) {
					case KEY_DOWN: if (st.menu < 3) { st.menu++; } break;
					case KEY_UP:   if (st.menu > 1) { st.menu--; } break;
					case KEY_EXIT:
					case KEY_BACK:  st.menu = 0; break;
					case KEY_LEFT:   /* settings: quieter */
						if (st.menu == 2) {
							beep_set_volume(beep_get_volume() - 1);
							beep_click();   /* preview at the new level */
						}
						break;
					case KEY_RIGHT:  /* settings: louder */
						if (st.menu == 2) {
							beep_set_volume(beep_get_volume() + 1);
							beep_click();
						}
						break;
					default: break;
					}
					ui_invalidate();
				} else if (st.key != KEY_BACKLIGHT) {
					/* Backlight is a local key (hold = menu); every other key
					 * is a remote button and is unicast at once by the radio
					 * thread. */
					zbnet_send_key(st.key);
				}
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

		/* Hold Backlight to open the on-device menu at the connectivity page.
		 * The menu forces the screen bright so it is readable in the dark. */
		if (st.key == KEY_BACKLIGHT) {
			if (backlight_held_since == 0) {
				backlight_held_since = k_uptime_get();
			} else if (st.menu == 0 &&
				   k_uptime_get() - backlight_held_since >= KEY_HOLD_MS) {
				st.menu = 1;
				ui_invalidate();
			}
		} else {
			backlight_held_since = 0;
		}

		/* Watchdog self-test now lives on the debug page: hold OK there to arm
		 * it. Off the main path so it cannot fire by accident. */
		if (st.menu == 3 && st.key == KEY_SELECT) {
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
			st.menu = 0;         /* always wake to the main screen */
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

		/* Menu forces the screen bright — you opened it to read it. Leaving the
		 * menu hands brightness back to the ambient-light logic. */
		if (st.menu && !menu_lit) {
			display_set_brightness(disp, 255);
			menu_lit = true;
		} else if (!st.menu && menu_lit) {
			menu_lit = false;
			als_step = -1;   /* force auto-brightness to re-apply below */
		}

		/* Auto-brightness. Only written on an actual change: every write reprograms TIM2. */
		if (AUTO_BRIGHTNESS && !st.menu && als_acc >= 0) {
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
		st.rf_joined = zbnet_joined();
		st.beep_vol = (uint8_t)beep_get_volume();
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
