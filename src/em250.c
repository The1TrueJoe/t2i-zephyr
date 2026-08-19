#include "em250.h"
#include "zbx.h"

#include <string.h>
#include <zephyr/kernel.h>

#define SOH    0x01
#define EOT    0x04
#define ACK    0x06
#define NAK    0x15
#define CAN    0x18
#define CRCCHR 'C'

/* Discard any ZBX frames the parser assembles. Once the radio is in its bootloader the bytes are
 * not frames at all, but the queue still has to be emptied so it does not back up. */
static void drain(void)
{
	const uint8_t *f;

	while (zbx_poll(&f)) {
	}
}

bool em250_bl_enter(void)
{
	static const uint8_t c02[] = { 0x02, 0x00 };
	static const uint8_t c08[] = { 0x08, 0x00 };
	static const uint8_t crlf[] = { '\r', '\n' };
	uint8_t raw[192];

	for (int attempt = 0; attempt < 4; attempt++) {
		zbx_send(c02, sizeof c02);
		for (int w = 0; w < 30; w++) { drain(); k_msleep(50); }

		zbx_raw_reset();
		zbx_send(c08, sizeof c08);
		for (int w = 0; w < 30; w++) { drain(); k_msleep(50); }

		/* The banner never comes back on the first CR, and a bare CR never works at
		 * all — it wants CR+LF roughly a second apart. Verify rather than assume:
		 * a run against a radio that never entered looks exactly like a run that
		 * found nothing. */
		for (int round = 0; round < 4; round++) {
			size_t n;

			zbx_raw_reset();
			zbx_tx_raw(crlf, sizeof crlf);
			for (int w = 0; w < 20; w++) { drain(); k_msleep(50); }
			n = zbx_raw(raw, sizeof raw);

			for (size_t i = 0; i + 5 <= n; i++) {
				if (memcmp(&raw[i], "EM250", 5) == 0) {
					return true;
				}
			}
		}
		zbx_uart_init();          /* PC13 reset, then try again */
		k_msleep(500);
	}
	return false;
}

size_t em250_bl_info(char *out, size_t max)
{
	static const uint8_t three[] = { '3' };
	uint8_t raw[192];
	size_t n, k = 0;

	zbx_raw_reset();
	zbx_tx_raw(three, sizeof three);
	for (int w = 0; w < 30; w++) { drain(); k_msleep(50); }
	n = zbx_raw(raw, sizeof raw);

	for (size_t i = 0; i < n && k + 1 < max; i++) {
		if (raw[i] >= 0x20 && raw[i] < 0x7f) {
			out[k++] = (char)raw[i];
		}
	}
	out[k] = 0;
	return k;
}

void em250_bl_run(void)
{
	static const uint8_t two[] = { '2' };

	zbx_tx_raw(two, sizeof two);
	for (int w = 0; w < 40; w++) { drain(); k_msleep(50); }
}

static uint16_t crc16_xmodem(const uint8_t *d, size_t n)
{
	uint16_t c = 0;

	for (size_t i = 0; i < n; i++) {
		c ^= (uint16_t)d[i] << 8;
		for (int b = 0; b < 8; b++) {
			c = (c & 0x8000u) ? (uint16_t)((c << 1) ^ 0x1021u)
					  : (uint16_t)(c << 1);
		}
	}
	return c;
}

bool em250_flash_ebl(const uint8_t *ebl, size_t len,
		     void (*progress)(unsigned done, unsigned total))
{
	static const uint8_t one[] = { '1' };
	static const uint8_t eot[] = { EOT };
	const unsigned total = (unsigned)((len + 127u) / 128u);
	uint8_t blk[133];
	int c;

	/* "1. upload ebl", then the receiver drives the handshake by sending 'C' to ask
	 * for CRC mode. Flush first: the menu text is still sitting in the FIFO. */
	zbx_getc_flush();
	zbx_tx_raw(one, sizeof one);

	for (int i = 0; ; i++) {
		c = zbx_getc(200);
		if (c == CRCCHR) {
			break;
		}
		if (i > 40) {
			return false;      /* no 'C' in ~8 s: not in upload mode */
		}
	}

	for (unsigned b = 0; b < total; b++) {
		size_t off = (size_t)b * 128u;
		size_t n = (len - off) < 128u ? (len - off) : 128u;
		uint16_t crc;
		bool acked = false;

		blk[0] = SOH;
		blk[1] = (uint8_t)((b + 1u) & 0xFFu);
		blk[2] = (uint8_t)(~blk[1]);
		/* The .ebl's own tail padding is 0xFF, so pad the short final block to
		 * match rather than with XMODEM's usual 0x1A. */
		memset(&blk[3], 0xFF, 128);
		memcpy(&blk[3], &ebl[off], n);
		crc = crc16_xmodem(&blk[3], 128);
		blk[131] = (uint8_t)(crc >> 8);
		blk[132] = (uint8_t)(crc & 0xFFu);

		for (int try = 0; try < 6 && !acked; try++) {
			zbx_tx_raw(blk, sizeof blk);
			c = zbx_getc(3000);
			if (c == ACK) {
				acked = true;
			} else if (c == CAN) {
				return false;
			}
			/* NAK or timeout: resend this block. */
		}
		if (!acked) {
			return false;
		}
		/* Called often, not just for display: the upload runs far longer than the
		 * ~8 s watchdog, so this is where the caller gets to feed it. */
		if (progress && (b % 8u) == 0u) {
			progress(b, total);
		}
	}

	for (int try = 0; try < 4; try++) {
		zbx_tx_raw(eot, sizeof eot);
		c = zbx_getc(3000);
		if (c == ACK) {
			if (progress) {
				progress(total, total);
			}
			return true;
		}
	}
	return false;
}
