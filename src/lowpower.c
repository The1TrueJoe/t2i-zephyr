/*
 * STM32F2 STOP mode entry/exit — see lowpower.h.
 *
 * Sequence matches stock RTI's PWR_EnterSTOPMode (0x0801C260): clear PDDS so we
 * stop rather than standby, set LPDS so the regulator drops to low-power, set
 * SLEEPDEEP, WFI. On wake we clear SLEEPDEEP and rebuild the clock tree.
 *
 * The clock restore is the part that is easy to get wrong: STOP always exits on
 * HSI (16 MHz) with HSE and the PLL switched off, so without this the whole
 * system silently runs 7.5x slow afterwards. RCC register *contents* survive
 * STOP, so the PLL factors Zephyr programmed at boot are still there and we only
 * have to restart the oscillators and re-select the PLL. FLASH_ACR likewise
 * survives, so the 3 wait states needed at 120 MHz are still set.
 */
#include <zephyr/kernel.h>
#include "t2i_regs.h"
#include "lowpower.h"

#define PWR_CR        REG32(0x40007000)
#define SCB_SCR       REG32(0xE000ED10)
#define RCC_CR        REG32(RCC_BASE + 0x00)
#define RCC_CFGR      REG32(RCC_BASE + 0x08)

#define PWR_CR_LPDS   (1u << 0)   /* regulator in low-power mode during stop */
#define PWR_CR_PDDS   (1u << 1)   /* 0 = STOP, 1 = STANDBY */
#define SCR_SLEEPDEEP (1u << 2)

#define RCC_CR_HSEON   (1u << 16)
#define RCC_CR_HSERDY  (1u << 17)
#define RCC_CR_PLLON   (1u << 24)
#define RCC_CR_PLLRDY  (1u << 25)
#define RCC_APB1ENR_PWREN (1u << 28)

#define CFGR_SW_MASK   0x3u
#define CFGR_SW_PLL    0x2u
#define CFGR_SWS_SHIFT 2

/* Bounded spins: a dead oscillator must not hang the remote forever. If HSE or
 * the PLL never comes back we keep running on HSI — slow, but alive and still
 * able to take a firmware update. */
#define CLK_TIMEOUT 1000000

static uint32_t stops;

uint32_t lowpower_sysclk_src(void)
{
	return (RCC_CFGR >> CFGR_SWS_SHIFT) & CFGR_SW_MASK;
}

uint32_t lowpower_stop_count(void)
{
	return stops;
}

void lowpower_stop(void)
{
	stops++;
	RCC_APB1ENR |= RCC_APB1ENR_PWREN;   /* PWR_CR is unwritable without this */

	PWR_CR = (PWR_CR & ~PWR_CR_PDDS) | PWR_CR_LPDS;
	SCB_SCR |= SCR_SLEEPDEEP;

	__asm__ volatile ("dsb" ::: "memory");
	__asm__ volatile ("wfi");
	__asm__ volatile ("isb" ::: "memory");

	SCB_SCR &= ~SCR_SLEEPDEEP;

	/* --- back on HSI @16 MHz: restore HSE -> PLL -> 120 MHz --- */
	RCC_CR |= RCC_CR_HSEON;
	for (int i = 0; i < CLK_TIMEOUT && !(RCC_CR & RCC_CR_HSERDY); i++) {
		__asm__ volatile("nop");
	}
	if (!(RCC_CR & RCC_CR_HSERDY)) {
		return;   /* no crystal — stay on HSI rather than hang */
	}

	RCC_CR |= RCC_CR_PLLON;
	for (int i = 0; i < CLK_TIMEOUT && !(RCC_CR & RCC_CR_PLLRDY); i++) {
		__asm__ volatile("nop");
	}
	if (!(RCC_CR & RCC_CR_PLLRDY)) {
		return;
	}

	RCC_CFGR = (RCC_CFGR & ~CFGR_SW_MASK) | CFGR_SW_PLL;
	for (int i = 0; i < CLK_TIMEOUT &&
	     ((RCC_CFGR >> CFGR_SWS_SHIFT) & CFGR_SW_MASK) != CFGR_SW_PLL; i++) {
		__asm__ volatile("nop");
	}
}
