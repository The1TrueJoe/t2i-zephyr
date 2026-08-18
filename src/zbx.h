#ifndef T2I_ZBX_H
#define T2I_ZBX_H

/*
 * ZBX: the RTI host <-> EM250 link on USART3. Framing and message layer are
 * reverse-engineered from stock; see docs/ZIGBEE-PROTOCOL.md, which carries the
 * full opcode tables and the decompiled sources for everything here.
 *
 * The codec is pure and self-testable, which matters: the bench unit has no
 * radio, so zbx_selftest() is the only thing that can be proven before the code
 * ever meets an EM250. Link-up behaviour is NOT established — see §10 of the doc.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ZBX_SOF 0x81
#define ZBX_EOF 0x82
#define ZBX_ESC 0x80
#define ZBX_MAX_PAYLOAD 0x80

/* Wire encoder. Returns bytes written, or 0 if it would not fit.
 * `out` needs 2 * len + 4 worst case. */
size_t zbx_encode(const uint8_t *payload, size_t len, uint8_t *out, size_t out_sz);

struct zbx_rx {
	uint8_t buf[ZBX_MAX_PAYLOAD];
	uint8_t idx;
	bool in_frame;
	bool esc;
};

/* Feed one byte. Returns payload length (checksum stripped) on a good frame,
 * else 0. Overlong frames are dropped, never truncated. */
size_t zbx_rx_byte(struct zbx_rx *r, uint8_t b);

/* Message layer. dst/hdr/app mirror stock's scatter-gather chunks. */
size_t zbx_build_send(uint8_t dst, const uint8_t *hdr, size_t hdr_len,
		      const uint8_t *app, size_t app_len, uint8_t *msg, size_t msg_sz);
size_t twt_header(uint8_t *out, uint8_t kind, uint8_t session, uint8_t ack);
size_t twtc_syscode(uint8_t *out, uint8_t unit, uint8_t sys, uint16_t cmd, uint8_t seq);

/* Round-trip check of the codec. Returns true on pass. Runs with no radio
 * attached, which is the whole point. */
bool zbx_selftest(void);

/* ---- USART3 transport. Safe to call with no radio populated: PC10/PC11 are
 * otherwise unused, so this just parks two pins in AF7. ---- */
void zbx_uart_init(void);

/* Frame and send. Emits stock's 0x81 0x81 wake burst and inter-frame gap —
 * whether the EM250 actually requires it is unestablished (doc §10). */
bool zbx_send(const uint8_t *payload, size_t len);

/* Drain the UART, returning payload length into *out on a complete frame.
 * Poll-based: there is no radio to be timely for yet. */
size_t zbx_poll(const uint8_t **out);

/* Frames seen / rejected, for bring-up on a unit that has a radio. */
void zbx_stats(uint32_t *rx_frames, uint32_t *rx_bad, uint32_t *rx_bytes);

/* First raw bytes seen on USART3 since boot, unframed. Diagnostic only. */
size_t zbx_raw(uint8_t *out, size_t max);

/* RX line level sampled at init with a pull-down then a pull-up, before USART3 takes the pin.
 * 1/1 = something is driving it (a live radio idles high). 0/1 = floating, nothing there. */
void zbx_rx_line(uint8_t *pd, uint8_t *pu);

/* PCLK1 in Hz, computed from RCC. ZBX_BRR is only correct at 30 MHz. */
uint32_t zbx_pclk1(void);

#endif
