/*
 * IR transmit, reverse-engineered from stock RTI (see docs/IR-BUZZER.md).
 *
 *   PB0  = TIM3_CH3 (AF2) — 50%-duty carrier, idles LOW = off
 *   PB15 = SPI2_MOSI (AF5) — the mark/space envelope, clocked out by DMA
 *   PB14 = RF path enable, active high — must be LOW for IR
 *
 * Stock clocks SPI2 at PCLK1/64 = 468.75 kHz, so one byte on MOSI is exactly
 * 8 / 468750 = 17.0667 us. That byte time is the IR time quantum: 0xFF is a
 * quantum of mark, 0x00 a quantum of space. Stock re-arms one DMA burst per
 * mark/space with memory-increment off; we build the whole frame in RAM and let
 * one increment-on transfer clock it out, which is the same waveform with a
 * fraction of the bookkeeping.
 *
 * Doing this in software instead would mean holding interrupts off for a whole
 * ~70 ms frame to keep the timing, which would drop USB. The DMA does it for
 * free.
 */
#include <zephyr/kernel.h>
#include <string.h>
#include "t2i_regs.h"
#include "ir.h"

#define B GPIO_PORT_B

/* 8 bits at PCLK1/64 (30 MHz / 64 = 468.75 kHz) */
#define IR_QUANTUM_NS 17067

/* Longest frame we will emit. NEC full frame is ~68 ms = ~4000 quanta. */
#define IR_MAX_QUANTA 4608
static uint8_t ir_buf[IR_MAX_QUANTA];

static void pb_af(int pin, int af)
{
	GPIO_MODER(B) = (GPIO_MODER(B) & ~(3u << (pin * 2))) | (GPIO_MODE_AF << (pin * 2));
	GPIO_OSPEEDR(B) |= (3u << (pin * 2));
	if (pin < 8) {
		GPIO_AFRL(B) = (GPIO_AFRL(B) & ~(0xFu << (pin * 4))) | ((uint32_t)af << (pin * 4));
	} else {
		GPIO_AFRH(B) = (GPIO_AFRH(B) & ~(0xFu << ((pin - 8) * 4)))
			       | ((uint32_t)af << ((pin - 8) * 4));
	}
}

/* Park MOSI as a plain low output. SPI leaves the pin wherever the last bit put
 * it, and a stuck-high envelope would gate the carrier on indefinitely. */
static void mosi_idle(void)
{
	GPIO_MODER(B) = (GPIO_MODER(B) & ~(3u << (15 * 2))) | (GPIO_MODE_OUTPUT << (15 * 2));
	GPIO_BSRR(B) = 1u << (15 + 16);
}

void ir_init(void)
{
	RCC_AHB1ENR |= RCC_AHB1ENR_GPIO(B) | (1u << 21);   /* GPIOB, DMA1 */
	RCC_APB1ENR |= (1u << 1) | (1u << 14);             /* TIM3, SPI2 */

	/* PB14 low: select the IR path, not the RF one. */
	GPIO_MODER(B) = (GPIO_MODER(B) & ~(3u << (14 * 2))) | (GPIO_MODE_OUTPUT << (14 * 2));
	GPIO_BSRR(B) = 1u << (14 + 16);

	pb_af(0, 2);                                       /* PB0 -> TIM3_CH3 */
	TIM3_PSC   = 0;
	TIM3_CCMR2 = (6u << 4) | (1u << 3);                /* OC3M = PWM1, OC3PE */
	TIM3_CCER  = (1u << 8);                            /* CC3E */
	TIM3_CR1   = 0;                                    /* carrier off, pin low */

	mosi_idle();
}

static void carrier(int hz)
{
	if (hz <= 0) {
		TIM3_CR1 = 0;
		return;
	}
	uint32_t arr = (60000000u / (uint32_t)hz) - 1u;

	TIM3_ARR  = arr;
	TIM3_CCR3 = arr / 2;
	TIM3_EGR  = TIM_EGR_UG;
	TIM3_CR1  = TIM_CR1_CEN;
}

void ir_send(const uint16_t *us, int n, int carrier_hz)
{
	int q = 0;

	/* Round each duration to whole quanta, accumulating the remainder so a long
	 * frame does not drift: rounding each of ~70 entries independently would
	 * cost up to 8 us apiece, which is what breaks marginal receivers. */
	int64_t want_ns = 0, done_ns = 0;

	for (int i = 0; i < n; i++) {
		want_ns += (int64_t)us[i] * 1000;
		int cnt = (int)((want_ns - done_ns + IR_QUANTUM_NS / 2) / IR_QUANTUM_NS);

		if (cnt < 1) {
			cnt = 1;
		}
		if (q + cnt > IR_MAX_QUANTA) {
			cnt = IR_MAX_QUANTA - q;   /* clamp: a truncated frame beats a stomped one */
		}
		memset(&ir_buf[q], (i & 1) ? 0x00 : 0xFF, (size_t)cnt);
		q += cnt;
		done_ns += (int64_t)cnt * IR_QUANTUM_NS;
		if (q >= IR_MAX_QUANTA) {
			break;
		}
	}
	if (q == 0) {
		return;
	}

	pb_af(15, 5);                                      /* PB15 -> SPI2_MOSI */
	/* master, TX-only (BIDI out), software NSS high, BR = fPCLK/64 */
	SPI2_CR1 = (1u << 15) | (1u << 14) | (5u << 3) | (1u << 9) | (1u << 8) | (1u << 2);
	SPI2_CR2 = (1u << 1);                              /* TXDMAEN */
	SPI2_CR1 |= (1u << 6);                             /* SPE */

	DMA1_S4CR = 0;
	while (DMA1_S4CR & 1u) {
	}
	DMA1_HIFCR  = 0x3Fu << 0;                          /* clear stream-4 flags */
	DMA1_S4PAR  = SPI2_BASE + 0x0C;
	DMA1_S4M0AR = (uint32_t)ir_buf;
	DMA1_S4NDTR = (uint32_t)q;
	DMA1_S4FCR  = 0;
	/* ch0, mem->periph, MINC, byte/byte, no circular */
	DMA1_S4CR = (0u << 25) | (1u << 6) | (1u << 10) | 1u;

	carrier(carrier_hz);

	/* Bounded: the frame cannot outlast its own quanta by much, and a hung DMA
	 * must not wedge the caller with the carrier stuck on. */
	int64_t deadline = k_uptime_get() + ((int64_t)q * IR_QUANTUM_NS) / 1000000 + 50;

	while (!(DMA1_HISR & DMA_HISR_TCIF4) && k_uptime_get() < deadline) {
		k_msleep(1);
	}
	while ((SPI2_SR & (1u << 7)) && k_uptime_get() < deadline) {   /* BSY */
	}

	carrier(0);
	DMA1_S4CR = 0;
	SPI2_CR1  = 0;
	SPI2_CR2  = 0;
	mosi_idle();
}

/* NEC: 9ms mark, 4.5ms space, 32 bits LSB-first (addr, ~addr, cmd, ~cmd) as a
 * 560us mark plus a 560us (0) or 1690us (1) space, then a 560us stop mark. */
void ir_send_nec(uint8_t addr, uint8_t cmd)
{
	uint16_t f[68];
	uint32_t bits = (uint32_t)addr | ((uint32_t)(uint8_t)~addr << 8)
			| ((uint32_t)cmd << 16) | ((uint32_t)(uint8_t)~cmd << 24);
	int i = 0;

	f[i++] = 9000;
	f[i++] = 4500;
	for (int b = 0; b < 32; b++) {
		f[i++] = 560;
		f[i++] = (bits & (1u << b)) ? 1690 : 560;
	}
	f[i++] = 560;
	ir_send(f, i, 38000);
}
