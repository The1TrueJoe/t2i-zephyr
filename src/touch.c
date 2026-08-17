/*
 * 4-wire resistive touchscreen + beeper for the RTI T2i, reverse-engineered
 * from stock RTI (touch driver FUN_08015928, tone FUN_08009a78).
 *
 *   Touch electrodes (ADC2 IN3..IN6), plate pairing per stock:
 *     X plate = PA4 (X+) / PA3 (X-)      Y plate = PA5 (Y+) / PA6 (Y-)
 *   Stock FUN_08015928: PA4 high, PA3 low, PA6 analog, sample ch5 (PA5).
 *   Stock FUN_08015a0c: PA5 high, PA6 low, PA3 analog, sample ch4 (PA4).
 *   Beeper: PB0 = TIM3_CH3 (AF2), PWM tone.
 *
 * Read strategy: PENIRQ-style detect (drive X- low, pull-up Y+, sense Y+ IDR),
 * then two ADC passes (drive a plate, sample the perpendicular wire).
 */
#include <zephyr/kernel.h>
#include "t2i_regs.h"
#include "touch.h"

/* Press threshold on the Z (pressure) reading, 0..4095. Lower = more sensitive.
 * Tune against the "z" figure shown on the demo screen: pick a value comfortably
 * above what an untouched panel reads and below a light fingertip press. */
#define TOUCH_Z_MIN 12

#define A GPIO_PORT_A
#define XM 3   /* PA3 = X-  (ADC2 IN3) */
#define XP 4   /* PA4 = X+  (ADC2 IN4) */
#define YP 5   /* PA5 = Y+  (ADC2 IN5) */
#define YM 6   /* PA6 = Y-  (ADC2 IN6) */

static void pa_mode(int pin, uint32_t mode, uint32_t pull)
{
	GPIO_MODER(A) = (GPIO_MODER(A) & ~(3u << (pin * 2))) | (mode << (pin * 2));
	GPIO_PUPDR(A) = (GPIO_PUPDR(A) & ~(3u << (pin * 2))) | (pull << (pin * 2));
}
static void pa_out(int pin, int val)
{
	pa_mode(pin, GPIO_MODE_OUTPUT, 0);
	GPIO_BSRR(A) = val ? (1u << pin) : (1u << (pin + 16));
}
static void pa_analog(int pin)  { pa_mode(pin, GPIO_MODE_ANALOG, 0); }

static uint16_t adc2_read(int ch)
{
	ADC2_SQR3 = ch;                 /* single conversion of channel `ch` */
	ADC2_SR  = 0;                   /* clear stale EOC/STRT */
	ADC2_CR2 |= ADC_CR2_SWSTART;
	/* bounded wait — a stalled conversion must never hang the whole app */
	for (int i = 0; i < 200000 && !(ADC2_SR & ADC_SR_EOC); i++) {
		__asm__ volatile("nop");
	}
	return (uint16_t)(ADC2_DR & 0xFFF);
}

void touch_init(void)
{
	RCC_AHB1ENR |= RCC_AHB1ENR_GPIO(A);
	RCC_APB2ENR |= (1u << 9);       /* ADC2EN */
	ADC_CCR   = (ADC_CCR & ~(3u << 16)) | (1u << 16);  /* ADC clk = PCLK2/4 */
	ADC2_CR1  = 0;                  /* 12-bit, no scan */
	ADC2_SQR1 = 0;                  /* 1 conversion in the sequence */
	ADC2_SMPR2 = 0x3FFFFFFF;        /* 480-cycle sample on ch0..9 (high-Z panel) */
	ADC2_CR2  = ADC_CR2_ADON;       /* enable ADC2 */
	k_busy_wait(20);
}

/* median of 3 — a bare resistive panel jitters enough to misfire LVGL clicks */
static int median3(int a, int b, int c)
{
	if (a > b) { int t = a; a = b; b = t; }
	if (b > c) { b = c; }
	return a > b ? a : b;
}

static int read_axis(int hi_pin, int lo_pin, int sense_pin, int off_pin)
{
	pa_out(hi_pin, 1); pa_out(lo_pin, 0);
	pa_analog(off_pin); pa_analog(sense_pin);
	k_busy_wait(50);
	int a = adc2_read(sense_pin);
	int b = adc2_read(sense_pin);
	int c = adc2_read(sense_pin);
	return median3(a, b, c);
}

/* Pressure (Z): drive Y+ high and X- low, then sample X+. Untouched there is no
 * current path and X+ sits at ~0; pressing bridges the plates and pulls X+ up,
 * harder press -> higher reading. An analog threshold beats the old digital
 * pull-up test, which needed a hard press just to cross the logic-low level. */
static int touch_z(void)
{
	pa_out(YP, 1); pa_out(XM, 0);
	pa_analog(YM); pa_analog(XP);
	k_busy_wait(50);
	return median3(adc2_read(XP), adc2_read(XP), adc2_read(XP));
}

int touch_read(int *x, int *y, int *z)
{
	int zv = touch_z();

	if (z) {
		*z = zv;   /* always reported so TOUCH_Z_MIN can be tuned on-screen */
	}
	if (zv < TOUCH_Z_MIN) {
		return 0;
	}

	/* drive the X plate, sample Y+ (ch5) — stock FUN_08015928 */
	int xv = read_axis(XP, XM, YP, YM);
	/* drive the Y plate, sample X+ (ch4) — stock FUN_08015a0c */
	int yv = read_axis(YP, YM, XP, XM);

	if (x) { *x = xv; }
	if (y) { *y = yv; }
	return 1;
}

/* ---- beeper: TIM4_CH2 on PB7 (AF2), ~400 Hz square burst ----
 *
 * This was previously on TIM3_CH3/PB0, which is WRONG — PB0 is the IR carrier
 * (docs/IR-BUZZER.md). Stock's beeper is a plain 50% square wave on PB7 gated on
 * for 20ms per key click by its "Buzzer Task": TIM4 tick = 60MHz/120 = 500kHz,
 * ARR 1249 -> 400 Hz. There is no DAC and no sample playback anywhere in stock.
 */
/* Stock clicks at 400 Hz, but 20 ms of 400 Hz is only 8 cycles and on this unit
 * it reads as a rattle rather than a click. The transducer's own resonance is
 * what decides how it sounds, so leave the knob: BEEP_HZ / BEEP_MS are the two
 * numbers to turn. TIM4 ticks at 60 MHz / 120 = 500 kHz. */
#define BEEP_HZ 2700        /* stock: 400. most small buzzers peak near 2.7 kHz */
#define BEEP_MS 20          /* stock: 20 ms per key click */
#define BEEP_TICK 500000
#define BEEP_PSC 119

void beep_init(void)
{
	RCC_AHB1ENR |= RCC_AHB1ENR_GPIO(GPIO_PORT_B);
	RCC_APB1ENR |= (1u << 2);       /* TIM4EN */

	/* PB7 -> AF2 = TIM4_CH2 */
	GPIO_MODER(GPIO_PORT_B) = (GPIO_MODER(GPIO_PORT_B) & ~(3u << (7 * 2)))
				  | (GPIO_MODE_AF << (7 * 2));
	GPIO_OSPEEDR(GPIO_PORT_B) |= (3u << (7 * 2));
	GPIO_AFRL(GPIO_PORT_B) = (GPIO_AFRL(GPIO_PORT_B) & ~(0xFu << (7 * 4)))
				 | (2u << (7 * 4));

	TIM4_PSC  = BEEP_PSC;
	beep_set_hz(BEEP_HZ);
	TIM4_CCMR1 = (6u << 12) | (1u << 11);   /* OC2M = PWM1, OC2PE */
	TIM4_CCER  = (1u << 4);                 /* CC2E */
	TIM4_EGR   = TIM_EGR_UG;
}

void beep_set_hz(int hz)
{
	uint32_t arr = (hz > 0) ? (BEEP_TICK / (uint32_t)hz) - 1u : 1249u;

	TIM4_ARR  = arr;
	TIM4_CCR2 = arr / 2;                    /* 50% */
	TIM4_EGR  = TIM_EGR_UG;
}

static void beep_tone(int ms)
{
	TIM4_CR1 = TIM_CR1_CEN;
	k_msleep(ms);
	TIM4_CR1 = 0;
}

void beep_click(void) { beep_tone(BEEP_MS); }

void beep_test(void)
{
	beep_tone(180);
	k_msleep(120);
	beep_tone(180);
}
