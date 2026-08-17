/*
 * Anti-brick machinery — see safety.h.
 */
#include <zephyr/kernel.h>
#include "safety.h"

/* Boot counter in RAM above every linked section, like the debug markers in
 * reset_hook.c. Survives warm/watchdog resets (which is exactly the loop we are
 * protecting against) and is cleared by a real power-cycle. */
#define SAFE_BASE   0x2001FF80U
#define SAFE_MAGIC  0x5AFEB007U

/* Boot this many times without reaching safety_mark_healthy() and the next boot
 * comes up USB-only. Three gives a genuine glitch room to recover while still
 * reaching safe mode quickly. */
#define MAX_BOOT_ATTEMPTS 3

struct safety_state {
	uint32_t magic;
	uint32_t attempts;
};

static volatile struct safety_state *const state =
	(volatile struct safety_state *)SAFE_BASE;

/* IWDG — clocked from the ~32 kHz LSI, independent of the main clock tree, so
 * it still bites if the PLL or the whole application dies. */
#define IWDG_KR   (*(volatile uint32_t *)0x40003000U)
#define IWDG_PR   (*(volatile uint32_t *)0x40003004U)
#define IWDG_RLR  (*(volatile uint32_t *)0x40003008U)
#define IWDG_SR   (*(volatile uint32_t *)0x4000300CU)

#define IWDG_KEY_FEED   0xAAAAU
#define IWDG_KEY_ACCESS 0x5555U
#define IWDG_KEY_START  0xCCCCU

/* /256 prescaler -> ~125 Hz; reload 1000 -> about 8 seconds. Long enough that
 * the 3s splash and a slow SPI erase do not trip it, short enough that a hung
 * remote recovers on its own while someone is still holding it. */
#define IWDG_PRESCALER 6
#define IWDG_RELOAD    1000

#define RCC_CSR (*(volatile uint32_t *)0x40023874U)
#define RCC_CSR_RMVF (1u << 24)

static const char *reset_cause = "?";
static bool starve_watchdog;

static void capture_reset_cause(void)
{
	uint32_t csr = RCC_CSR;

	/* Order matters: watchdog and software resets are the interesting ones, and
	 * a power-on also sets BOR/PIN, so check the specific causes first. */
	if (csr & (1u << 29)) {
		reset_cause = "WATCHDOG";
	} else if (csr & (1u << 30)) {
		reset_cause = "wwdg";
	} else if (csr & (1u << 28)) {
		reset_cause = "software";
	} else if (csr & (1u << 31)) {
		reset_cause = "lowpower";
	} else if (csr & (1u << 27)) {
		reset_cause = "power-on";
	} else if (csr & (1u << 26)) {
		reset_cause = "nrst";
	} else if (csr & (1u << 25)) {
		reset_cause = "brownout";
	}
	RCC_CSR |= RCC_CSR_RMVF;   /* clear, so the next boot reports its own cause */
}

const char *safety_reset_cause(void)
{
	return reset_cause;
}

void safety_watchdog_selftest(void)
{
	starve_watchdog = true;
}

bool safety_boot_check(void)
{
	capture_reset_cause();
	if (state->magic != SAFE_MAGIC) {
		/* cold boot (or first run of this firmware) */
		state->magic = SAFE_MAGIC;
		state->attempts = 0;
	}
	state->attempts++;
	return state->attempts > MAX_BOOT_ATTEMPTS;
}

void safety_mark_healthy(void)
{
	state->attempts = 0;
}

uint32_t safety_boot_attempts(void)
{
	return state->magic == SAFE_MAGIC ? state->attempts : 0;
}

#define DBGMCU_APB1_FZ (*(volatile uint32_t *)0xE0042008U)
#define DBGMCU_FZ_WDG  (3u << 11)

void safety_watchdog_start(void)
{
	/* Re-assert here as well as in reset_hook: a debugger attaching *after*
	 * boot re-sets these, and a frozen watchdog is a safety net that silently
	 * does nothing. */
	DBGMCU_APB1_FZ &= ~DBGMCU_FZ_WDG;

	/* Order matters and is easy to get wrong: the IWDG is clocked by the LSI,
	 * and the LSI only starts when the START key is written. Configuring first
	 * and waiting on IWDG_SR before starting therefore spins forever, because
	 * nothing is clocking the registers that would clear those bits. Start it,
	 * then configure. */
	IWDG_KR = IWDG_KEY_START;    /* starts the watchdog and the LSI */
	IWDG_KR = IWDG_KEY_ACCESS;   /* unlock PR/RLR */
	IWDG_PR = IWDG_PRESCALER;
	IWDG_RLR = IWDG_RELOAD;

	/* Bounded, never infinite: a watchdog that hangs the CPU while trying to
	 * protect it is worse than no watchdog. If the bits do not clear we simply
	 * run with the default period. */
	for (int i = 0; i < 100000 && (IWDG_SR & 0x3U); i++) {
		__asm__ volatile("nop");
	}
	IWDG_KR = IWDG_KEY_FEED;
}

void safety_force_reset(void)
{
	/* Shorten the watchdog to ~0.25ms so the fallback is immediate rather than
	 * an 8 second wait, then stop feeding it. */
	IWDG_KR = IWDG_KEY_START;    /* ensure it is running even if never started */
	IWDG_KR = IWDG_KEY_ACCESS;
	IWDG_PR = 0;                 /* /4 */
	IWDG_RLR = 1;
	starve_watchdog = true;

	/* Preferred path: a clean system reset request. */
	*(volatile uint32_t *)0xE000ED0CU = 0x05FA0004U;
	__asm__ volatile ("dsb" ::: "memory");

	/* If that was ignored — which happens on this unit — the starved watchdog
	 * takes us down regardless. */
	for (;;) {
		__asm__ volatile("nop");
	}
}

void safety_watchdog_feed(void)
{
	if (starve_watchdog) {
		return;   /* self-test in progress: let it bite */
	}
	IWDG_KR = IWDG_KEY_FEED;
}
