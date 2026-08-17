/*
 * SoC reset hook — runs very early in z_arm_reset (before z_prep_c / VTOR
 * relocation). We are chain-loaded by the RTI bootloader, which leaves
 * peripherals/interrupts live; if one fires before Zephyr is ready it vectors
 * through a half-initialized handler and crashes.
 *
 * Close that window immediately:
 *   - point VTOR at our table and quiesce all NVIC IRQs,
 *   - hard-reset the USB OTG-FS peripheral so any endpoint/interrupt state the
 *     RTI bootloader/app left behind is gone before Zephyr's udc driver enables
 *     the USB IRQ (otherwise the USB ISR fires pre-init -> wild branch).
 */
#include <zephyr/toolchain.h>

#define SCB_VTOR     (*(volatile unsigned int *)0xE000ED08)
#define SCB_ICSR     (*(volatile unsigned int *)0xE000ED04)
#define SCB_SHCSR    (*(volatile unsigned int *)0xE000ED24)
#define SYSTICK_CTRL (*(volatile unsigned int *)0xE000E010)
#define SYSTICK_VAL  (*(volatile unsigned int *)0xE000E018)
#define NVIC_ICER    ((volatile unsigned int *)0xE000E180)  /* clear-enable */
#define NVIC_ICPR    ((volatile unsigned int *)0xE000E280)  /* clear-pending */
#define ICSR_PENDSTCLR (1u << 25)   /* clear pending SysTick */
#define ICSR_PENDSVCLR (1u << 27)   /* clear pending PendSV  */

/* Keep the debug port clocked through Sleep/Stop/Standby.
 *
 * WFI puts the core in Sleep and STOP gates almost everything — in both cases
 * the debug port loses its clock and SWD drops the target ("Failed to enter SWD
 * mode"), which on a unit with a dead NRST means the only way back in is the
 * boot flash-window. Setting these three bits costs a little sleep current but
 * keeps st-flash/gdb able to attach at any time, which is the difference
 * between a bad sleep bug being debuggable and being a power-cycle hunt.
 * Clear DBGMCU_ENABLE for a production/battery build. */
#define DBGMCU_ENABLE 1
#define DBGMCU_CR    (*(volatile unsigned int *)0xE0042004)
#define DBGMCU_APB1_FZ (*(volatile unsigned int *)0xE0042008)
#define DBGMCU_SLEEP_STOP_STANDBY 0x7u   /* DBG_SLEEP | DBG_STOP | DBG_STANDBY */
/* st-flash sets these so a watchdog cannot reset the chip while it is being
 * programmed — but it never clears them, which silently disables our watchdog
 * for as long as a probe has ever been attached. Clear them so the IWDG is
 * real on the bench, not just in the field. */
#define DBGMCU_FZ_WDG (3u << 11)         /* DBG_IWDG_STOP | DBG_WWDG_STOP */

#define RCC_AHB2ENR  (*(volatile unsigned int *)0x40023834)
#define RCC_AHB2RSTR (*(volatile unsigned int *)0x40023814)
#define RCC_OTGFS_BIT (1u << 7)                             /* OTGFSEN / OTGFSRST */

/* Boot-progress markers in high RAM (above all linked sections, not touched by
 * z_prep_c bss/data init nor the stack) — read over SWD to bisect the crash. */
#define BOOTMARK(n)  (*(volatile unsigned int *)(0x2001FF00U + ((n) * 4U)))

void soc_reset_hook(void)
{
	BOOTMARK(0) = 0xB0070001U;    /* reached soc_reset_hook entry */
	SCB_VTOR = 0x08004000U;      /* our vector table, right now */

#if DBGMCU_ENABLE
	DBGMCU_CR |= DBGMCU_SLEEP_STOP_STANDBY;
	DBGMCU_APB1_FZ &= ~DBGMCU_FZ_WDG;
#endif

	/* The RTI bootloader uses SysTick and hands off with it running and/or a
	 * SysTick exception pending. SysTick/PendSV are system exceptions, so the
	 * NVIC clear-pending below does NOT cover them: clear them in SCB ICSR and
	 * stop the counter, or a stale SysTick fires before the kernel is up and
	 * vectors through an unready handler. */
	SYSTICK_CTRL = 0U;           /* stop the counter + its interrupt */
	SYSTICK_VAL  = 0U;           /* clear current value (write-any) */
	SCB_ICSR = ICSR_PENDSTCLR | ICSR_PENDSVCLR;  /* drop pending SysTick/PendSV */

	for (unsigned int i = 0; i < 8U; i++) {
		NVIC_ICER[i] = 0xFFFFFFFFU;   /* disable all IRQs */
		NVIC_ICPR[i] = 0xFFFFFFFFU;   /* clear all pending */
	}

	/* Clock the OTG-FS block just long enough to reset it to power-on state. */
	RCC_AHB2ENR  |= RCC_OTGFS_BIT;
	RCC_AHB2RSTR |= RCC_OTGFS_BIT;   /* assert peripheral reset */
	RCC_AHB2RSTR &= ~RCC_OTGFS_BIT;  /* deassert */

	BOOTMARK(1) = 0xB0070002U;    /* soc_reset_hook completed */
	__asm__ volatile("dsb; isb" ::: "memory");
}
