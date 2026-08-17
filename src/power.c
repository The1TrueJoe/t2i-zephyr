/*
 * Sleep/wake policy for the T2i — see power.h.
 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/display.h>
#include "power.h"
#include "wake.h"
#include "lowpower.h"

/* Idle this long with no touch, key or motion before sleeping. */
#define SLEEP_AFTER_MS 30000

/* Backlight levels, 0..255.
 *
 * ASLEEP is deliberately a few percent rather than 0: this panel's backlight
 * driver latches into shutdown when its PWM input sits low, and does not
 * restart from restoring the duty cycle or reconfiguring the pin. Cycling its
 * enable (PC12) would restart it, but that rail also feeds the panel logic and
 * the battery monitor — dropping it browns out the display and lights the
 * low-battery indicator. So a low duty is the real floor for "off" here. */
#define BRIGHT_AWAKE  128
#define BRIGHT_ASLEEP 8

/* STM32 STOP mode instead of WFI while asleep: ~0.5mA against ~15-25mA, but a
 * stopped CPU has not proven attachable over SWD on this board even with
 * DBGMCU DBG_STOP set — and with NRST dead that means a power-cycle per flash.
 * Off by default; turn on deliberately for power measurement, ideally with
 * recovery mode (hold a key at boot) available as the escape hatch. */
#define USE_STOP_MODE 0

/* Never STOP this soon after boot, so the flash-window stays catchable. */
#define STOP_NOT_BEFORE_MS 10000

static const struct device *display;
static bool never_sleep;
static bool asleep;
static int64_t last_active;
static uint32_t wake_count_total;
static const char *woke_by = "-";

void power_init(const struct device *disp, bool recovery)
{
	display = disp;
	never_sleep = recovery;
	last_active = k_uptime_get();
}

const char *power_woke_by(void) { return woke_by; }
uint32_t power_wakes(void)      { return wake_count_total; }
bool power_asleep(void)         { return asleep; }

bool power_tick(bool activity, const char *source)
{
	if (activity) {
		last_active = k_uptime_get();
	}

	if (asleep) {
		if (activity) {
			woke_by = source ? source : "?";
			wake_count_total++;
			display_set_brightness(display, BRIGHT_AWAKE);
			asleep = false;
			return false;
		}
	} else if (!never_sleep &&
		   k_uptime_get() - last_active > SLEEP_AFTER_MS) {
		display_set_brightness(display, BRIGHT_ASLEEP);
		asleep = true;
	}

	if (!asleep) {
		return false;
	}

	/* Asleep: block until a wake interrupt. Nothing to draw, so the caller
	 * skips rendering entirely. */
	if (USE_STOP_MODE && k_uptime_get() > STOP_NOT_BEFORE_MS) {
		lowpower_stop();
	} else {
		wake_wait(K_MSEC(250));
	}
	return true;
}
