/*
 * Sleep/wake policy for the T2i — see power.h.
 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/display.h>
#include "power.h"
#include "wake.h"
#include "lowpower.h"
#include "backlight.h"

/* Power-path markers, readable over SWD while the screen is dark:
 *   0x2001FF8C  asleep flag
 *   0x2001FF90  last brightness applied
 *   0x2001FF94  wake count
 *   0x2001FF98  activity seen this tick (bit0) */
#define PMARK(off, v) (*(volatile uint32_t *)(0x2001FF8C + (off)) = (uint32_t)(v))

/* Idle this long with no touch, key or motion before sleeping. */
#define SLEEP_AFTER_MS 30000

/* Backlight levels, 0..255.
 *
 * Never drop PC12 to save power: that rail also feeds the panel logic and the
 * battery monitor, so taking it low browns out the display and lights the
 * low-battery indicator. Backlight off is done at the timer/pin instead. */
#define BRIGHT_AWAKE  128

/* Keypad backlight while awake, 0..100 (stock's curve floors around 25). */
#define KEYPAD_AWAKE 90
/* "Off" is the smallest non-zero duty, not 0.
 *
 * Stock RTI drives its backlight fully off (TIM2 disabled, PA1 low) and we
 * copied that exactly — but on this unit it does not come back: afterwards the
 * registers read correct (PA1 in AF, 50% duty, PC12 high) and yet no light is
 * produced. A duty that never goes static always recovers. 3 -> 1% duty, which
 * is visually off in a lit room while still giving the driver IC edges to run
 * its charge pump on. Costs ~1% of backlight power versus a true off. */
#define BRIGHT_ASLEEP 3

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

	PMARK(0x0C, activity ? 1 : 0);

	if (asleep) {
		if (activity) {
			woke_by = source ? source : "?";
			wake_count_total++;
			display_blanking_off(display);   /* panel back on */
			keypad_backlight_set(KEYPAD_AWAKE);
			display_set_brightness(display, BRIGHT_AWAKE);
			PMARK(0x04, BRIGHT_AWAKE);
			asleep = false;
			PMARK(0x00, 0);
			PMARK(0x08, wake_count_total);
			return false;
		}
	} else if (!never_sleep &&
		   k_uptime_get() - last_active > SLEEP_AFTER_MS) {
		display_blanking_on(display);    /* panel off = truly black, like stock */
		keypad_backlight_set(0);         /* the keys should not glow while asleep */
		PMARK(0x04, 0);
		asleep = true;
		PMARK(0x00, 1);
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
