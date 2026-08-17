/*
 * Front-panel RGB indicator on PA10 — see funlight.h.
 *
 * Wire protocol, from stock FUN_0801bf50:
 *
 *     cpsid i
 *     repeat <reg> times:  PA10=0, 25us; PA10=1, 25us
 *     delay 300us                        <- field separator
 *     repeat <val> times:  PA10=0, 25us; PA10=1, 25us
 *     delay 300us                        <- inter-frame gap
 *     cpsie i
 *
 * Each pulse ends high, so the line idles high between frames. A value of 0 is
 * unsendable by construction (no pulses = no field), which is why "off" is 0x20
 * rather than 0: levels are INVERTED, 0x20 = off .. 0x01 = brightest, and stock
 * computes them as 0x20 - pct*0x1F/100 (FUN_0801be26).
 *
 * Registers are two banks of three channels — 0x10/0x11/0x12 and 0x13/0x14/0x15
 * — written in pairs. Whether that is two physical LEDs or two halves of one is
 * not established (no part number appears anywhere near this code).
 *
 * COST: interrupts are locked for the whole frame, as stock does — the timing is
 * a pulse count, so a preemption mid-frame corrupts the value. A worst-case
 * frame (0x21 = 33 pulses) is ~3ms. Only call this on state changes, never per
 * render pass.
 */
#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include "t2i_regs.h"
#include "funlight.h"

#define FL_PIN 10                       /* PA10 */
#define FL_PULSE_US 25
#define FL_GAP_US   300

/* Inverted level scale: 0x20 = off, 0x01 = brightest. */
#define FL_OFF 0x20

/* Sent alone at init and on wake, with no value field. Most likely a reset or
 * re-sync; inferred from call position only. */
#define FL_CMD_RESYNC 0x21

static void fl_pulse(unsigned int count)
{
	for (unsigned int i = 0; i < count; i++) {
		GPIO_BSRR(GPIO_PORT_A) = 1u << (FL_PIN + 16);   /* low */
		k_busy_wait(FL_PULSE_US);
		GPIO_BSRR(GPIO_PORT_A) = 1u << FL_PIN;          /* high */
		k_busy_wait(FL_PULSE_US);
	}
}

/* reg alone (val == 0) sends a bare command, as stock does for 0x21. */
static void fl_write(uint8_t reg, uint8_t val)
{
	unsigned int key = irq_lock();

	fl_pulse(reg);
	k_busy_wait(FL_GAP_US);
	if (val) {
		fl_pulse(val);
	}
	k_busy_wait(FL_GAP_US);
	irq_unlock(key);
}

void funlight_init(void)
{
	RCC_AHB1ENR |= RCC_AHB1ENR_GPIO(GPIO_PORT_A);

	/* Stock drives the line low first, then configures output PP, 2MHz,
	 * pull-up (FUN_0801bf0c). */
	GPIO_BSRR(GPIO_PORT_A) = 1u << (FL_PIN + 16);
	GPIO_MODER(GPIO_PORT_A) = (GPIO_MODER(GPIO_PORT_A) & ~(3u << (FL_PIN * 2)))
				  | (GPIO_MODE_OUTPUT << (FL_PIN * 2));
	GPIO_PUPDR(GPIO_PORT_A) = (GPIO_PUPDR(GPIO_PORT_A) & ~(3u << (FL_PIN * 2)))
				  | (1u << (FL_PIN * 2));
	GPIO_BSRR(GPIO_PORT_A) = 1u << FL_PIN;          /* idle high */

	fl_write(FL_CMD_RESYNC, 0);
}

void funlight_set(bool ch0, bool ch1, bool ch2, int pct)
{
	if (pct < 0) pct = 0; if (pct > 100) pct = 100;

	/* stock's curve, inverted: 100% -> 1, 0% -> 0x20 */
	uint8_t lvl = (uint8_t)(FL_OFF - (pct * 0x1F) / 100);
	if (lvl == 0) {
		lvl = 1;
	}

	uint8_t v0 = ch0 ? lvl : FL_OFF;
	uint8_t v1 = ch1 ? lvl : FL_OFF;
	uint8_t v2 = ch2 ? lvl : FL_OFF;

	/* Both banks, in stock's order (FUN_0801be26). */
	fl_write(0x15, v1); fl_write(0x12, v1);
	fl_write(0x14, v0); fl_write(0x11, v0);
	fl_write(0x13, v2); fl_write(0x10, v2);
}

void funlight_sleep(void)
{
	GPIO_BSRR(GPIO_PORT_A) = 1u << (FL_PIN + 16);   /* hold low, as stock does */
}
