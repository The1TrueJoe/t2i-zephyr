/*
 * ZBX framing + USART3 transport for the RTI T2i host <-> EM250 link.
 *
 * Framing, message layer and the command-length table are taken from
 * docs/ZIGBEE-PROTOCOL.md, which reverses encoder FUN_08018A88 and decoder
 * FUN_08018B70 (exact mirrors) plus the dispatch tables around them.
 *
 *   frame = 0x81 <payload, 0x80/81/82 escaped by a literal 0x80> <esc cksum> 0x82
 *   cksum = -sum8(payload), so sum8(payload) + cksum == 0
 *
 * The escape is a literal prefix and the escaped byte follows verbatim. This is
 * NOT the EZSP/ASH XOR-0x20 scheme, and getting that wrong is the easiest way to
 * produce frames that look right and are not.
 *
 * Two stock bugs are deliberately NOT reproduced (doc §8): the de-framer here
 * drops overlong frames instead of letting 127 bytes be memcpy'd into a 70-byte
 * block, and nothing truncates silently.
 *
 * Nothing below has met an EM250. The codec is proven by zbx_selftest(); link-up
 * is not established (doc §10) and needs a unit that has the radio.
 */
#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <string.h>
#include "t2i_regs.h"
#include "zbx.h"

/* ---- wire codec ---- */

static inline bool zbx_needs_esc(uint8_t b)
{
	return b == ZBX_ESC || b == ZBX_SOF || b == ZBX_EOF;
}

size_t zbx_encode(const uint8_t *payload, size_t len, uint8_t *out, size_t out_sz)
{
	size_t n = 0;
	uint8_t ck = 0;

	if (len == 0 || len > ZBX_MAX_PAYLOAD || out_sz < 2 * len + 4) {
		return 0;
	}
	out[n++] = ZBX_SOF;
	for (size_t i = 0; i < len; i++) {
		uint8_t b = payload[i];

		ck += b;
		if (zbx_needs_esc(b)) {
			out[n++] = ZBX_ESC;
		}
		out[n++] = b;
	}
	ck = (uint8_t)(~ck + 1);
	if (zbx_needs_esc(ck)) {
		out[n++] = ZBX_ESC;
	}
	out[n++] = ck;
	out[n++] = ZBX_EOF;
	return n;
}

size_t zbx_rx_byte(struct zbx_rx *r, uint8_t b)
{
	/* Escape is tested before SOF/EOF, so an escaped 0x81/0x82 stays data. */
	if (b == ZBX_ESC && !r->esc) {
		r->esc = true;
		return 0;
	}
	if (r->esc) {
		r->esc = false;
		if (r->in_frame) {
			if (r->idx >= sizeof r->buf) {
				r->in_frame = false;
				r->idx = 0;
				return 0;
			}
			r->buf[r->idx++] = b;
		}
		return 0;
	}
	if (b == ZBX_SOF) {          /* also resyncs a partial frame */
		r->in_frame = true;
		r->idx = 0;
		return 0;
	}
	if (b != ZBX_EOF) {
		if (r->in_frame) {
			if (r->idx >= sizeof r->buf) {
				r->in_frame = false;
				r->idx = 0;
				return 0;
			}
			r->buf[r->idx++] = b;
		}
		return 0;
	}

	size_t out = 0;

	if (r->in_frame && r->idx >= 2) {
		uint8_t sum = 0;

		for (uint8_t i = 0; i < r->idx; i++) {
			sum += r->buf[i];
		}
		if (sum == 0) {
			out = r->idx - 1u;   /* strip the trailing checksum */
		}
	}
	r->in_frame = false;
	r->idx = 0;
	return out;
}

/* ---- message layer ---- */

size_t zbx_build_send(uint8_t dst, const uint8_t *hdr, size_t hdr_len,
		      const uint8_t *app, size_t app_len, uint8_t *msg, size_t msg_sz)
{
	size_t n = 3 + hdr_len + app_len;

	if (n > ZBX_MAX_PAYLOAD || n > msg_sz) {
		return 0;
	}
	msg[0] = 0x40;                                        /* send data */
	msg[1] = (uint8_t)(1 + hdr_len + app_len);
	msg[2] = dst;
	memcpy(msg + 3, hdr, hdr_len);
	memcpy(msg + 3 + hdr_len, app, app_len);
	return n;
}

size_t twt_header(uint8_t *out, uint8_t kind, uint8_t session, uint8_t ack)
{
	out[0] = kind;
	out[1] = session;
	if (kind == 3 || kind == 6) {
		out[2] = ack;
		return 3;
	}
	return 2;
}

size_t twtc_syscode(uint8_t *out, uint8_t unit, uint8_t sys, uint16_t cmd, uint8_t seq)
{
	out[0] = 0x02;
	out[1] = unit;
	out[2] = sys;
	out[3] = (uint8_t)(cmd >> 8);          /* big-endian on the wire */
	out[4] = (uint8_t)cmd;
	out[5] = seq;
	return 6;
}

/* ---- self-check ---- */

#define CHECK(c) do { if (!(c)) { return false; } } while (0)

bool zbx_selftest(void)
{
	uint8_t msg[ZBX_MAX_PAYLOAD], wire[2 * ZBX_MAX_PAYLOAD + 4], hdr[3], app[6];
	struct zbx_rx rx = {0};

	/* trivial frame: 81 30 00 D0 82 */
	static const uint8_t p[] = { 0x30, 0x00 };
	size_t n = zbx_encode(p, sizeof p, wire, sizeof wire);

	CHECK(n == 5);
	CHECK(wire[0] == ZBX_SOF && wire[3] == 0xD0 && wire[4] == ZBX_EOF);

	/* a payload byte equal to SOF is escaped and follows verbatim */
	static const uint8_t e[] = { 0x81, 0x01 };

	n = zbx_encode(e, sizeof e, wire, sizeof wire);
	CHECK(n == 6);
	CHECK(wire[1] == ZBX_ESC && wire[2] == 0x81);

	/* a keypress round-trips through the decoder byte for byte */
	size_t hl = twt_header(hdr, 3, 0x11, 0xFF);
	size_t al = twtc_syscode(app, 0x07, 0x03, 0x001F, 0x05);
	size_t ml = zbx_build_send(0x01, hdr, hl, app, al, msg, sizeof msg);

	CHECK(ml == 12 && msg[1] == 0x0A);
	n = zbx_encode(msg, ml, wire, sizeof wire);

	size_t got = 0;

	for (size_t i = 0; i < n; i++) {
		got = zbx_rx_byte(&rx, wire[i]);
	}
	CHECK(got == ml && memcmp(rx.buf, msg, ml) == 0);

	/* a single corrupted byte must be rejected, not delivered */
	memset(&rx, 0, sizeof rx);
	wire[3] ^= 0x01;
	got = 0;
	for (size_t i = 0; i < n; i++) {
		got = zbx_rx_byte(&rx, wire[i]);
	}
	CHECK(got == 0);

	/* oversize input is refused rather than overrunning `out` */
	CHECK(zbx_encode(p, sizeof p, wire, 4) == 0);
	return true;
}

/* ---- USART3 transport ---- */

#define USART3_BASE 0x40004800u
#define USART3_SR   REG32(USART3_BASE + 0x00)
#define USART3_DR   REG32(USART3_BASE + 0x04)
#define USART3_BRR  REG32(USART3_BASE + 0x08)
#define USART3_CR1  REG32(USART3_BASE + 0x0C)

#define CR1_RXNEIE (1u << 5)
#define SR_ORE  (1u << 3)
#define SR_RXNE (1u << 5)
#define SR_TXE  (1u << 7)
#define SR_TC   (1u << 6)

/* Stock's BRR at PCLK1 = 30 MHz. 0x0104 = 260 -> 115385 baud, 0.16% off 115200. */
#define ZBX_BRR 0x0104

static struct zbx_rx rx_state;
static uint32_t st_frames, st_bad, st_bytes;

/* Last raw bytes off the wire, before any framing. The frame counters alone cannot tell
 * "the radio is silent" from "the radio is talking and we are misframing it", and those need
 * completely different fixes. */
#define ZBX_RAW_KEEP 24
static uint8_t raw_ring[ZBX_RAW_KEEP];
static uint8_t raw_n;

static uint8_t rx_idle_pd, rx_idle_pu;

void zbx_rx_line(uint8_t *pd, uint8_t *pu) { *pd = rx_idle_pd; *pu = rx_idle_pu; }

/* What PCLK1 actually is, derived from RCC rather than assumed.
 *
 * ZBX_BRR is stock's literal 0x0104, which only yields 115200 if PCLK1 is 30 MHz. If our Zephyr
 * clock tree differs the baud is wrong, and a wrong baud looks exactly like what we are seeing:
 * the radio is plainly alive (its TX idles high) yet only a single framing-error byte ever
 * arrives per burst. */
#define RCC_CFGR_REG REG32(0x40023800u + 0x08)
#define RCC_PLLCFGR  REG32(0x40023800u + 0x04)

uint32_t zbx_pclk1(void)
{
	uint32_t cfgr = RCC_CFGR_REG, pll = RCC_PLLCFGR;
	uint32_t src = (cfgr >> 2) & 3u;
	uint32_t sysclk;

	if (src == 2u) {                      /* PLL */
		uint32_t m = pll & 0x3F, n = (pll >> 6) & 0x1FF;
		uint32_t pdiv = (((pll >> 16) & 3u) + 1u) * 2u;
		uint32_t in = (pll & (1u << 22)) ? HSE_VALUE : 16000000u;

		sysclk = m ? (in / m) * n / pdiv : 0;
	} else {
		sysclk = (src == 1u) ? HSE_VALUE : 16000000u;
	}
	uint32_t hpre = (cfgr >> 4) & 0xF;
	uint32_t ahb = sysclk >> ((hpre & 8u) ? ((hpre & 7u) + 1u + ((hpre == 0xC) ? 1u : 0u)) : 0u);
	uint32_t ppre1 = (cfgr >> 10) & 7u;

	return (ppre1 & 4u) ? (ahb >> ((ppre1 & 3u) + 1u)) : ahb;
}

size_t zbx_raw(uint8_t *out, size_t max)
{
	size_t n = raw_n < max ? raw_n : max;

	memcpy(out, raw_ring, n);
	return n;
}

static void pc_af7(int pin)
{
	GPIO_MODER(GPIO_PORT_C) = (GPIO_MODER(GPIO_PORT_C) & ~(3u << (pin * 2)))
				  | (GPIO_MODE_AF << (pin * 2));
	GPIO_OSPEEDR(GPIO_PORT_C) |= (3u << (pin * 2));
	/* AFRH covers pins 8..15 */
	GPIO_AFRH(GPIO_PORT_C) = (GPIO_AFRH(GPIO_PORT_C) & ~(0xFu << ((pin - 8) * 4)))
				 | (7u << ((pin - 8) * 4));
}

/* Frames completed by the ISR, waiting for the main loop. */
#define ZBX_Q 4
static struct { uint8_t buf[ZBX_MAX_PAYLOAD]; uint8_t len; } done_q[ZBX_Q];
static volatile uint8_t q_head, q_tail;

/* USART3 RX interrupt.
 *
 * Polling cannot work here and that cost real time to see: the data register holds ONE byte, a
 * byte lands every ~87us at 115200, and the main loop comes round every 10ms. The radio was
 * replying correctly the whole time and we were keeping one byte of each answer and overrunning
 * the rest. */
static void zbx_isr(const void *arg)
{
	ARG_UNUSED(arg);

	while (USART3_SR & (SR_RXNE | SR_ORE)) {
		uint8_t b = (uint8_t)(USART3_DR & 0xFF);   /* reading DR also clears ORE */
		size_t len = zbx_rx_byte(&rx_state, b);

		st_bytes++;
		if (raw_n < ZBX_RAW_KEEP) {
			raw_ring[raw_n++] = b;
		}
		if (len) {
			uint8_t nh = (uint8_t)((q_head + 1u) % ZBX_Q);

			if (nh != q_tail) {
				memcpy(done_q[q_head].buf, rx_state.buf, len);
				done_q[q_head].len = (uint8_t)len;
				q_head = nh;
				st_frames++;
			} else {
				st_bad++;      /* queue full: the main loop is not draining */
			}
		}
	}
}

void zbx_uart_init(void)
{
	RCC_AHB1ENR |= RCC_AHB1ENR_GPIO(GPIO_PORT_C);
	RCC_APB1ENR |= (1u << 18);              /* USART3EN */

	/* Is anything actually driving RX?
	 *
	 * A powered EM250 holds its TX (our PC11) HIGH when idle, so it reads 1 whichever way we
	 * pull it. A line nobody drives just follows the pull. Worth knowing before blaming framing:
	 * every byte we have received so far is a single 0x81 arriving exactly when WE transmit,
	 * which is what a floating input next to a switching pin looks like. */
	GPIO_MODER(GPIO_PORT_C) &= ~(3u << (11 * 2));                 /* input */
	GPIO_PUPDR(GPIO_PORT_C) = (GPIO_PUPDR(GPIO_PORT_C) & ~(3u << (11 * 2)))
				  | (2u << (11 * 2));                 /* pull-DOWN */
	k_busy_wait(2000);
	rx_idle_pd = (GPIO_IDR(GPIO_PORT_C) >> 11) & 1u;
	GPIO_PUPDR(GPIO_PORT_C) = (GPIO_PUPDR(GPIO_PORT_C) & ~(3u << (11 * 2)))
				  | (1u << (11 * 2));                 /* pull-UP */
	k_busy_wait(2000);
	rx_idle_pu = (GPIO_IDR(GPIO_PORT_C) >> 11) & 1u;
	GPIO_PUPDR(GPIO_PORT_C) &= ~(3u << (11 * 2));                 /* back to no pull */

	pc_af7(10);                             /* PC10 = TX (host -> radio) */
	pc_af7(11);                             /* PC11 = RX (radio -> host) */

	USART3_BRR = ZBX_BRR;
	USART3_CR1 = (1u << 13) | (1u << 3) | (1u << 2);   /* UE | TE | RE, 8N1 */
	memset(&rx_state, 0, sizeof rx_state);

	IRQ_CONNECT(39, 5, zbx_isr, NULL, 0);              /* USART3_IRQn on STM32F2 */
	irq_enable(39);
	USART3_CR1 |= CR1_RXNEIE;

	/* Release the radio from reset.
	 *
	 * PC13 is the EM250's nRESET, active low (docs/ZIGBEE-PROTOCOL.md; stock pulses it low for
	 * ~1.4ms in FUN_0800bfb2). After an MCU reset this pin is an input, so nothing is driving
	 * it — the radio is left wherever it happened to be, which for a first attempt at talking to
	 * it is indistinguishable from a dead link. Drive it explicitly, always. */
	GPIO_MODER(GPIO_PORT_C) = (GPIO_MODER(GPIO_PORT_C) & ~(3u << (13 * 2)))
				  | (GPIO_MODE_OUTPUT << (13 * 2));
	GPIO_BSRR(GPIO_PORT_C) = 1u << (13 + 16);   /* nRESET low */
	k_msleep(5);                                /* stock uses ~1.4ms; longer is free */
	GPIO_BSRR(GPIO_PORT_C) = 1u << 13;          /* release */

	/* The EM250 has to boot its own stack before it can answer. Stock releases reset and then
	 * goes on to configure the network, so it never measures this; 500ms was a guess and the
	 * link stayed silent, so give it well past anything plausible. */
	k_msleep(2000);
	memset(&rx_state, 0, sizeof rx_state);
}

static void tx_byte(uint8_t b)
{
	/* Bounded: a missing or unclocked USART must not wedge the caller. */
	for (int i = 0; i < 100000 && !(USART3_SR & SR_TXE); i++) {
		__asm__ volatile("nop");
	}
	USART3_DR = b;
}

bool zbx_send(const uint8_t *payload, size_t len)
{
	uint8_t wire[2 * ZBX_MAX_PAYLOAD + 4];
	size_t n = zbx_encode(payload, len, wire, sizeof wire);

	if (n == 0) {
		return false;
	}

	/* Stock precedes every frame with 0x81 0x81 and ~1.4ms of silence. Whether
	 * the EM250 needs it is unestablished (doc §10) — kept because matching
	 * stock is the only behaviour we have any evidence for. */
	tx_byte(ZBX_SOF);
	tx_byte(ZBX_SOF);
	k_busy_wait(1400);

	for (size_t i = 0; i < n; i++) {
		tx_byte(wire[i]);
	}
	for (int i = 0; i < 100000 && !(USART3_SR & SR_TC); i++) {
		__asm__ volatile("nop");
	}
	return true;
}

size_t zbx_poll(const uint8_t **out)
{
	if (q_tail != q_head) {
		size_t n = done_q[q_tail].len;

		if (out) {
			*out = done_q[q_tail].buf;
		}
		q_tail = (uint8_t)((q_tail + 1u) % ZBX_Q);
		return n;
	}
	return 0;
}

void zbx_stats(uint32_t *rx_frames, uint32_t *rx_bad, uint32_t *rx_bytes)
{
	if (rx_frames) { *rx_frames = st_frames; }
	if (rx_bad)    { *rx_bad = st_bad; }
	if (rx_bytes)  { *rx_bytes = st_bytes; }
}
