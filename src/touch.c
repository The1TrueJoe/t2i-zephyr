/*
 * 4-wire resistive touchscreen + beeper for the RTI T2i, reverse-engineered
 * from stock RTI (touch driver FUN_08015928, tone FUN_08009a78).
 *
 *   Touch electrodes: PA3=X+  PA4=Y+  PA5=X-  PA6=Y-  (ADC2 IN3..IN6)
 *   Beeper: PB0 = TIM3_CH3 (AF2), PWM tone.
 *
 * Read strategy: PENIRQ-style detect (drive X- low, pull-up Y+, sense Y+ IDR),
 * then two ADC passes (drive a plate, sample the perpendicular wire).
 */
#include <zephyr/kernel.h>
#include "t2i_regs.h"
#include "touch.h"

#define A GPIO_PORT_A
#define XP 3   /* PA3 = X+  (ADC2 IN3) */
#define YP 4   /* PA4 = Y+  (ADC2 IN4) */
#define XM 5   /* PA5 = X-  (ADC2 IN5) */
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
static void pa_hiz(int pin)     { pa_mode(pin, GPIO_MODE_INPUT, 0); }
static void pa_analog(int pin)  { pa_mode(pin, GPIO_MODE_ANALOG, 0); }
static void pa_in_pullup(int pin){ pa_mode(pin, GPIO_MODE_INPUT, 1); }

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

int touch_read(int *x, int *y)
{
	/* --- detect: X- low, Y+ input pull-up; touched pulls Y+ low --- */
	pa_hiz(XP); pa_out(XM, 0); pa_hiz(YM); pa_in_pullup(YP);
	k_busy_wait(50);
	int pressed = !((GPIO_IDR(A) >> YP) & 1u);
	if (!pressed) {
		return 0;
	}

	/* --- read X: drive X plate (X+ high, X- low), sample Y+ --- */
	pa_out(XP, 1); pa_out(XM, 0); pa_hiz(YM); pa_analog(YP);
	k_busy_wait(50);
	int rx = adc2_read(YP);          /* IN4 */

	/* --- read Y: drive Y plate (Y+ high, Y- low), sample X+ --- */
	pa_out(YP, 1); pa_out(YM, 0); pa_hiz(XM); pa_analog(XP);
	k_busy_wait(50);
	int ry = adc2_read(XP);          /* IN3 */

	*x = rx;
	*y = ry;
	return 1;
}

/* ---- beeper: TIM3_CH3 on PB0 (AF2), ~3 kHz tone burst ---- */
void beep_init(void)
{
	RCC_AHB1ENR |= RCC_AHB1ENR_GPIO(GPIO_PORT_B);
	RCC_APB1ENR |= (1u << 1);       /* TIM3EN */
	/* PB0 -> AF2 = TIM3_CH3 */
	GPIO_MODER(GPIO_PORT_B) = (GPIO_MODER(GPIO_PORT_B) & ~(3u << 0)) | (GPIO_MODE_AF << 0);
	GPIO_OSPEEDR(GPIO_PORT_B) |= (3u << 0);
	GPIO_AFRL(GPIO_PORT_B) = (GPIO_AFRL(GPIO_PORT_B) & ~(0xFu << 0)) | (2u << 0);
	TIM3_PSC  = 0;
	TIM3_ARR  = 20000;              /* 60 MHz / 20000 = 3 kHz */
	TIM3_CCR3 = 10000;              /* 50% */
	TIM3_CCMR2 = (6u << 4) | (1u << 3);  /* OC3M = PWM1, OC3PE */
	TIM3_CCER  = (1u << 8);         /* CC3E */
}

static void beep_tone(int ms)
{
	TIM3_CR1 = TIM_CR1_CEN;         /* tone on */
	k_msleep(ms);
	TIM3_CR1 = 0;                   /* tone off */
}

void beep_click(void) { beep_tone(30); }

/* boot self-test: two clearly-audible beeps if the speaker path works at all */
void beep_test(void)
{
	beep_tone(180);
	k_msleep(120);
	beep_tone(180);
}
