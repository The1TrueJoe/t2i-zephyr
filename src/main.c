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

/* How often to report the radio link over USB. */
#define ZBX_REPORT_MS 10000
#define ZBX_PROBE 0

/* Reflash the EM250 with RTI's ZB-Pro coordinator image, once, on the first pass after boot.
 * Set 0 once the radio already runs Telegesis — re-running just reflashes the same image
 * needlessly. */
#define EM250_FLASH 0

/* Drive the Telegesis join state machine: join the CA-1's network and heartbeat-unicast to it. */
#define AT_JOIN 1

/* Synthetic key events, so the firmware -> USB -> bridge -> CA-1 path can be proven end to end
 * with nobody holding the remote. Real presses still report normally alongside these. */
/* Synthetic keypresses so the button->RF->CA-1 path can be proven with nobody at the keypad. Each
 * simulated DOWN queues its code for the RF unicast exactly as a real press does. Set 0 for a
 * build that only forwards real presses. */
#define KEY_SIM     1
#define KEY_SIM_MS  4000

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

/* Dump anything the EM250 says, as hex, over USB CDC.
 *
 * The radio has never been talked to: src/zbx.c passes its own codec self-test but has never met
 * an EM250. Everything here is READ-ONLY by choice — 0x60 and 0x62 take no payload and the 0x6x
 * family is strictly paired (reply opcode = request + 1), so a reply proves the link end to end.
 * The network-shaped opcodes (0x20/0x21/0x26 config, 0x30-0x32, 0x40 send) are deliberately not
 * sent: this remote is joined to a live RTI system and a probe must not change that. */
static void zbx_report(const uint8_t *p, size_t n, const char *tag)
{
	char line[96];
	int w = snprintf(line, sizeof(line), "ZBX %s", tag);

	for (size_t i = 0; i < n && w < (int)sizeof(line) - 4; i++) {
		w += snprintf(line + w, sizeof(line) - (size_t)w, " %02x", p[i]);
	}
	updater_emit(line);
}

/* Drain the radio and report whatever arrives. The EM250 sends unsolicited indications —
 * 0x07 stack status (0x90 = joined), 0x05 network info — so this is worth running even if
 * nothing is ever transmitted. */
static void zbx_pump(void)
{
	const uint8_t *frame;
	size_t n = zbx_poll(&frame);

	if (n) {
		zbx_report(frame, n, "rx");
	}
}

/* Printable-only dump, for the bootloader's banner and menu — emit_hex truncates too early
 * to show which option uploads an image. */
/* Log a Telegesis AT reply as one line, control chars shown as ~. */
static void at_log(const char *tag, char *reply)
{
	char line[160];

	for (char *p = reply; *p; p++) {
		if (*p == '\r' || *p == '\n') {
			*p = '~';
		}
	}
	snprintf(line, sizeof line, "AT %s -> %s", tag, reply[0] ? reply : "(silent)");
	updater_emit(line);
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

static void emit_ascii(const char *tag, const uint8_t *b, size_t n)
{
	char line[200];
	size_t k = (size_t)snprintf(line, sizeof line, "%s: ", tag);

	for (size_t i = 0; i < n && k + 2 < sizeof line; i++) {
		if (b[i] == '\r' || b[i] == '\n') {
			line[k++] = '~';
		} else {
			line[k++] = (b[i] >= 0x20 && b[i] < 0x7f) ? (char)b[i] : '.';
		}
	}
	line[k] = 0;
	updater_emit(line);
}

static void emit_hex(const char *tag, const uint8_t *b, size_t n)
{
	char line[160];
	size_t m = n < 32 ? n : 32;
	size_t k = (size_t)snprintf(line, sizeof line, "%s [%u]: ", tag, (unsigned)n);

	for (size_t i = 0; i < m && k + 4 < sizeof line; i++) {
		k += (size_t)snprintf(&line[k], sizeof line - k, "%02x ", b[i]);
	}
	if (k + 2 < sizeof line) {
		line[k++] = '|';
		for (size_t i = 0; i < m && k + 2 < sizeof line; i++) {
			line[k++] = (b[i] >= 0x20 && b[i] < 0x7f) ? (char)b[i] : '.';
		}
		line[k++] = '|';
	}
	line[k] = 0;
	updater_emit(line);
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
	/* A keypress waiting to go out over RF. Set by the key handler, drained by the AT join
	 * state machine once the radio is on the CA-1's network. */
	static volatile uint8_t rf_pending_key = KEY_NONE;
	int64_t sim_at = 0;
	unsigned sim_i = 0, sim_phase = 0;
	int last_led = -1, led_pending = -1;
	int64_t key_down_at = 0;
	bool key_held_sent = false;
	int64_t led_since = 0;
	int als_step = -1, als_pending = -1;
	int64_t als_since = 0;
	int32_t als_acc = -1;   /* EMA accumulator, value << ALS_SMOOTH */
	int64_t zbx_last_report = 0;
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

		zbx_pump();

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

			/* Probe DISABLED for this test. Every raw byte we have seen is 0x81, arriving at
			 * exactly this 10s cadence — and 0x81 is the SOF our own wake burst sends. If the
			 * bytes stop when we stop transmitting, we are hearing ourselves (TX bleeding into
			 * RX), not the radio, and no amount of framing work would ever have helped. */
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

			/* Telegesis join state machine, at the confirmed 19200 baud.
			 *
			 * The radio answers AT. Walk it onto the CA-1's network: confirm alive,
			 * read status, join (the CA-1 hands over the network key in the clear),
			 * then once joined unicast a heartbeat to the coordinator at 0x0000 so
			 * the CA-1's incomingMessageHandler proves the RF path end to end.
			 *
			 * Every reply is logged. This never touches updater.c, so USB recovery
			 * stays intact whatever this does. */
			if (AT_JOIN) {
				static int st_at;
				static int joined;
				static unsigned beat;
				char reply[160];
				const uint32_t B = 0x061A;   /* 19200 */

				switch (st_at) {
				case 0:   /* confirm the radio */
					em250_at("ATI", B, reply, sizeof reply, 500);
					at_log("ATI", reply);
					st_at = 1;
					break;
				case 1:   /* trust-centre link key = ZigBeeAlliance09 (HA default) */
					em250_at("ATS09=5A6967426565416C6C69616E63653039:password",
						 B, reply, sizeof reply, 800);
					at_log("S09 linkkey", reply);
					st_at = 2;
					break;
				case 2:   /* channel mask = channel 15 only (bit 4) */
					em250_at("ATS00=0010", B, reply, sizeof reply, 600);
					at_log("S00 chan15", reply);
					st_at = 3;
					break;
				case 3:   /* main function: use preconfigured TC link key (bit 8) */
					em250_at("ATS0A=0100:password", B, reply, sizeof reply, 600);
					at_log("S0A preconf", reply);
					st_at = 4;
					break;
				case 4:   /* network status: already joined? */
					em250_at("AT+N", B, reply, sizeof reply, 800);
					at_log("AT+N", reply);
					if (strstr(reply, "+N=") && !strstr(reply, "NoPAN")) {
						joined = 1;
						st_at = 6;
					} else {
						st_at = 5;
					}
					break;
				case 5:   /* join — result streams seconds after OK */
					em250_at_wait("AT+JN", B, reply, sizeof reply, 9000);
					at_log("AT+JN", reply);
					if (strstr(reply, "JPAN")) {
						updater_emit("AT *** JOINED CA-1 ***");
						joined = 1;
						st_at = 6;
					} else {
						st_at = 4;   /* recheck / retry */
					}
					break;
				case 6:   /* joined: unicast a pressed button to the coordinator (0x0000) */
					if (joined && rf_pending_key != KEY_NONE) {
						uint8_t k = rf_pending_key;
						char cmd[48];

						rf_pending_key = KEY_NONE;
						/* Payload "K<code> <name>" — the code drives the Juno
						 * remote mapping, the name keeps the CA-1 log readable. */
						snprintf(cmd, sizeof cmd, "AT+UCAST:0000=K%u %s",
							 k, keypad_name(k));
						em250_at_wait(cmd, B, reply, sizeof reply, 3000);
						at_log("UCAST key", reply);
					} else if (joined && (beat++ % 30) == 0) {
						/* Occasional keepalive so a long-idle link stays proven. */
						em250_at_wait("AT+UCAST:0000=IDLE", B, reply,
							      sizeof reply, 3000);
						at_log("UCAST idle", reply);
					}
					break;
				}
				safety_watchdog_feed();
			}

			/* Display state, repeated rather than printed once at boot: the boot banner is
			 * unobservable on a USB-only unit (the CDC port stalls ~25s on open), which is
			 * exactly the situation a dark screen leaves you in. */
			snprintf(line, sizeof(line), "DISP panel=0x%04x bl=%d%% als=%d~%d",
				 hx8347_panel_id(), hx8347_backlight_pct(), st.als, st.als_avg);
			updater_emit(line);

			/* Probe DISABLED for this test. Every raw byte we have seen is 0x81, arriving at
			 * exactly this 10s cadence — and 0x81 is the SOF our own wake burst sends. If the
			 * bytes stop when we stop transmitting, we are hearing ourselves (TX bleeding into
			 * RX), not the radio, and no amount of framing work would ever have helped. */
			if (ZBX_PROBE) {
				/* Every one of these takes no payload and cannot change the network.
				 * The 0x6x family is strictly paired (reply = request + 1), so any
				 * answer at all proves the link. Cycling because a silent radio and a
				 * radio that ignores one particular opcode look identical. */
				/* Network-shaped now, deliberately: this remote is not in service and the
				 * whole point is to get RF off it so the CA-1's sniffer can see something.
				 *
				 * 0x20 layout from FUN_08020770 (docs/ZIGBEE-PROTOCOL.md): epan[0..7],
				 * pan_lo, mode, chanmask big-endian u32. Channel 15 only (bit 15) so the
				 * sniffer knows exactly where to listen. `mode` is not decoded, so it is
				 * cycled — a form and a join look different on air and either proves the
				 * link. 0x30/0x31/0x32 take no payload and are the other plausible
				 * "start the stack" triggers. */
				/* mode is NOT free: FUN_08019E40 only ever emits 2, 3 or 4 for 0x20
				 * (param 1 -> 2, 2 -> 3, 0x13 -> 4). We first tried 0 and 1, which is
				 * precisely why the radio answered status 0x01. chanmask covers 11-26
				 * (0x07FFF800) so it may pick any channel and the sniffer sweeps. */
				/* epan = the radio's own EUI, as reported in the 0x05 network-info frame
				 * (c4 0b c1 05 00 6f 0d 00). An all-zero extended PAN was refused with
				 * status 0x01, and the frame layout is byte-correct against FUN_08020770,
				 * so the argument is what it objects to. */
				static const uint8_t net0[] = {
					0x20, 0x0E, 0xc4,0x0b,0xc1,0x05,0x00,0x6f,0x0d,0x00,
					0x00, 0x02, 0x07,0xFF,0xF8,0x00 };
				static const uint8_t net1[] = {
					0x20, 0x0E, 0xc4,0x0b,0xc1,0x05,0x00,0x6f,0x0d,0x00,
					0x00, 0x03, 0x07,0xFF,0xF8,0x00 };
				static const uint8_t net2[] = {
					0x20, 0x0E, 0xc4,0x0b,0xc1,0x05,0x00,0x6f,0x0d,0x00,
					0x00, 0x04, 0x07,0xFF,0xF8,0x00 };
				static const uint8_t s30[] = { 0x30, 0x00 };
				static const uint8_t s31[] = { 0x31, 0x00 };
				static const uint8_t q60[] = { 0x60, 0x00 };
				/* Ordered as a sequence, not a grab-bag. The radio answers queries but
				 * refuses 0x20/0x30 with status 0x01, which matches the documented host
				 * state machine (0 uninit, 1 opened, 2 query network, 3 router init, ...)
				 * — we were commanding a stack that had never been opened. 0x02 and 0x04
				 * are the two zero-payload TX opcodes that plausibly do that, so they run
				 * first and the network command follows. */
				static const uint8_t s02[] = { 0x02, 0x00 };
				static const uint8_t s04[] = { 0x04, 0x00 };
				static const struct { const uint8_t *p; uint8_t n; } queries[] = {
					{ s02, 2 }, { s04, 2 },
					{ net0, sizeof net0 }, { net1, sizeof net1 },
					{ net2, sizeof net2 }, { s30, 2 }, { s31, 2 },
					{ q60, 2 },
				};
				static uint8_t qi;

				snprintf(line, sizeof(line), "ZBX tx 0x%02x", queries[qi].p[0]);
				updater_emit(line);
				zbx_send(queries[qi].p, queries[qi].n);

				/* Drain hard, immediately. USART3 here is polled with no RX interrupt and
				 * no DMA, so the data register holds exactly one byte: at 115200 a reply
				 * byte lands every ~87us, while the main loop comes round every 10ms. Every
				 * reply we have "not received" was almost certainly received and then
				 * overrun, leaving the single byte we kept seeing. */
				for (int w = 0; w < 200; w++) {
					const uint8_t *fr;
					size_t fl = zbx_poll(&fr);

					if (fl) {
						zbx_report(fr, fl, "reply");
					}
					k_busy_wait(500);
				}
				qi = (qi + 1) % (sizeof(queries) / sizeof(queries[0]));
			}
		}

		st.key = keypad_scan(&st.key_row, &st.key_col);
		st.key_rows = keypad_rows();
		st.key_name = keypad_name(st.key);

		/* Synthetic key events. The pipeline (firmware -> USB -> bridge -> CA-1 ->
		 * Juno remote contract) has to be provable with nobody holding the remote,
		 * so KEY_SIM walks the real key table emitting the same DOWN/HELD/UP lines
		 * a finger would. Same code path, same format — set KEY_SIM 0 for a build
		 * that only reports real presses. */
		if (KEY_SIM && k_uptime_get() - sim_at >= KEY_SIM_MS) {
			static const uint8_t sim_keys[] = {
				138,   /* Vol + */
				139,   /* Vol - */
				140,   /* Ch +  */
				141,   /* Ch -  */
				135,   /* OK    */
				143,   /* Menu  */
				142,   /* Guide */
				129,   /* Mute  */
			};
			char ev[48];
			uint8_t k = sim_keys[sim_i % (sizeof sim_keys)];

			sim_at = k_uptime_get();
			switch (sim_phase) {
			case 0:
				rf_pending_key = k;   /* feed the RF path, as a real press does */
				snprintf(ev, sizeof(ev), "KEY DOWN %u %s r%d c%d",
					 k, keypad_name(k), 0, 0);
				updater_emit(ev);
				sim_phase = (sim_i % 3 == 0) ? 1 : 2;   /* every third is a hold */
				break;
			case 1:
				snprintf(ev, sizeof(ev), "KEY HELD %u %s", k, keypad_name(k));
				updater_emit(ev);
				sim_phase = 2;
				break;
			default:
				snprintf(ev, sizeof(ev), "KEY UP %u %s", k, keypad_name(k));
				updater_emit(ev);
				sim_phase = 0;
				sim_i++;
				break;
			}
		}

		/* Report key transitions to the host over USB CDC. This is the path to
		 * publishing button presses without the radio: tools/t2i_mqtt_bridge.py
		 * reads these lines and forwards them to MQTT. */
		if (st.key != last_reported_key) {
			char ev[48];

			if (st.key != KEY_NONE) {
				beep_click();   /* stock clicks on every key, not just some */
				rf_pending_key = st.key;   /* queue it for the RF unicast */
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
