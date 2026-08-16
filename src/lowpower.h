#ifndef T2I_LOWPOWER_H
#define T2I_LOWPOWER_H

#include <stdint.h>

/*
 * STM32F2 STOP mode. Zephyr has no power.c for stm32f2x (F1/G0/G4/L1/WB do), so
 * CONFIG_PM cannot do this for us and it is hand-rolled — following the same
 * sequence stock RTI used (PWR_CR LPDS/PDDS + SLEEPDEEP + WFI, disassembled at
 * 0x0801C260).
 *
 * Blocks until an EXTI line wakes the CPU (keypad rows or accelerometer INT1),
 * then restores the 120 MHz PLL before returning — STOP always exits running on
 * the 16 MHz HSI.
 *
 * SAFETY: SWD cannot attach while the core is stopped unless DBGMCU_CR has
 * DBG_STOP set. reset_hook.c sets it. With a dead NRST, clearing that bit and
 * shipping a sleep bug means the only way back in is the boot flash-window.
 */
void lowpower_stop(void);

/* Current SYSCLK source from RCC_CFGR SWS: 0=HSI(16MHz), 1=HSE, 2=PLL(120MHz).
 * After a STOP/wake cycle this must read 2 — if the PLL restore silently failed
 * everything still "works" but runs 7.5x slow, which is easy to miss. */
uint32_t lowpower_sysclk_src(void);

/* How many times we have entered STOP. */
uint32_t lowpower_stop_count(void);

#endif /* T2I_LOWPOWER_H */
