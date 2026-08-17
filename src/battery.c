/*
 * Battery + charger sensing — see battery.h.
 *
 * Reversed from stock: FUN_0801b9ac / FUN_0801bd58 configure ADC1 with PA7
 * analog and ADC_RegularChannelConfig(ADC1, 7, 1, 480 cycles). The charger GPIOs
 * (PC9 present, PC14/PC15 status, all input pull-up) are read by FUN_0800e214,
 * which also drives PC12 LOW to light the low-battery indicator.
 *
 * NOTE PC12: our display driver holds it HIGH and must keep doing so — that is
 * the indicator being *off*. Do not drive it low unless you mean "low battery";
 * doing so also browns out the panel (see docs/BACKLIGHT.md).
 */
#include <zephyr/kernel.h>
#include "t2i_regs.h"
#include "battery.h"

#define ADC1_BASE  0x40012000u
#define ADC1_SR    REG32(ADC1_BASE + 0x00)
#define ADC1_CR1   REG32(ADC1_BASE + 0x04)
#define ADC1_CR2   REG32(ADC1_BASE + 0x08)
#define ADC1_SMPR2 REG32(ADC1_BASE + 0x10)
#define ADC1_SQR1  REG32(ADC1_BASE + 0x2C)
#define ADC1_SQR3  REG32(ADC1_BASE + 0x34)
#define ADC1_DR    REG32(ADC1_BASE + 0x4C)

#define ADC_SR_EOC_BIT   (1u << 1)
#define ADC_CR2_ADON_BIT (1u << 0)
#define ADC_CR2_SWSTART_BIT (1u << 30)

#define BATT_CHANNEL 7                  /* PA7 = ADC1 IN7 */

/* Stock's thresholds, raw 12-bit counts (FUN_0801bd58 region). */
#define BATT_LOW_TRIP  2460
#define BATT_LOW_CLEAR 2731

static bool low_latched;

void battery_init(void)
{
	RCC_AHB1ENR |= RCC_AHB1ENR_GPIO(GPIO_PORT_A) | RCC_AHB1ENR_GPIO(GPIO_PORT_C);
	RCC_APB2ENR |= (1u << 8);       /* ADC1EN */

	/* PA7 analog */
	GPIO_MODER(GPIO_PORT_A) |= (3u << (7 * 2));

	/* Charger sense: PC9 present, PC14/PC15 status — inputs with pull-ups. */
	for (int pin = 9; pin <= 15; pin++) {
		if (pin != 9 && pin != 14 && pin != 15) {
			continue;
		}
		GPIO_MODER(GPIO_PORT_C) &= ~(3u << (pin * 2));               /* input */
		GPIO_PUPDR(GPIO_PORT_C) = (GPIO_PUPDR(GPIO_PORT_C) & ~(3u << (pin * 2)))
					  | (1u << (pin * 2));               /* pull-up */
	}

	ADC1_CR1  = 0;                  /* 12-bit, no scan */
	ADC1_SQR1 = 0;                  /* one conversion */
	ADC1_SMPR2 = 0x3FFFFFFF;        /* 480-cycle sample, ch0..9 */
	ADC1_SQR3 = BATT_CHANNEL;
	ADC1_CR2  = ADC_CR2_ADON_BIT;
	k_busy_wait(20);
}

int battery_raw(void)
{
	ADC1_SQR3 = BATT_CHANNEL;
	ADC1_SR = 0;
	ADC1_CR2 |= ADC_CR2_SWSTART_BIT;

	/* bounded: a stalled ADC must never hang the remote */
	for (int i = 0; i < 200000 && !(ADC1_SR & ADC_SR_EOC_BIT); i++) {
		__asm__ volatile("nop");
	}
	if (!(ADC1_SR & ADC_SR_EOC_BIT)) {
		return -1;
	}
	return (int)(ADC1_DR & 0xFFF);
}

bool battery_low(void)
{
	int v = battery_raw();

	if (v < 0) {
		return low_latched;   /* no reading: keep the last decision */
	}
	if (v < BATT_LOW_TRIP) {
		low_latched = true;
	} else if (v > BATT_LOW_CLEAR) {
		low_latched = false;
	}
	return low_latched;
}

bool battery_charger_present(void)
{
	return ((GPIO_IDR(GPIO_PORT_C) >> 9) & 1u) == 0u;   /* pull-up: low = present */
}

uint8_t battery_charge_state(void)
{
	uint32_t idr = GPIO_IDR(GPIO_PORT_C);

	return (uint8_t)(((idr >> 14) & 1u) | (((idr >> 15) & 1u) << 1));
}
