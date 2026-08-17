/*
 * USB update receiver — see updater.h for why this is the anti-brick path.
 *
 * Speaks the reverse-engineered RTI update protocol over USB CDC, stages the
 * incoming image to the external SPI-NOR (raw SPI, 4-byte ops), then lets the
 * untouched RTI bootloader commit it — so a sealed remote is re-flashable over
 * USB with no pins (restore stock RTI, or push a new build).
 *
 * Protocol (64-byte frames, byte0 = type):
 *   0x83 -> reply with the genuine device-info blob
 *   0x82 [00 01] <declaredLE32> -> start: erase staging, reset counters
 *   0x80 + 63 bytes             -> data: sum + write to SPI staging
 * When `declared` bytes are summed and the 8-bit sum == 0, the SPI marker is
 * already in the staged image at 0x01FFFFFC; we invalidate the internal marker
 * (erase sector 7) and SYSRESETREQ -> the bootloader commits SPI@0x01F84000 to
 * flash@0x08004000.
 *
 * Ported out of main.c (commit f72d08d) into its own module + thread so that a
 * fault or hang in the UI cannot take the update path down with it.
 *
 * Debug: progress in high-RAM markers (read over SWD), since CDC is the
 * protocol channel: 0x2001FF20=state 24=recv 28=declared 2C=result.
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/sys/ring_buffer.h>
#include <stdint.h>
#include <string.h>
#include "updater.h"
#include "safety.h"


#define DBG(n)   (*(volatile uint32_t *)(0x2001FF20U + ((n) * 4U)))  /* 0..3 */

/* ---- staging geometry (decoded from bootloader) ---- */
#define SPI_IMAGE_BASE   0x01F84000u          /* staged image start */
#define SPI_MARKER_ADDR  0x01FFFFFCu          /* SPI commit marker  */
#define APP_REGION_SIZE  0x0007C000u          /* 496 KB (bytes committed) */

/* ---- USB device: impersonate the genuine RTI T2i so Integration Designer
 *      recognizes it and can restore stock firmware over USB ---- */
USBD_DEVICE_DEFINE(t2i_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)), 0x13BD, 0x1028);
USBD_DESC_LANG_DEFINE(t2i_lang);
USBD_DESC_MANUFACTURER_DEFINE(t2i_mfr, "Remote Technologies");
USBD_DESC_PRODUCT_DEFINE(t2i_product, "T2i");
USBD_DESC_CONFIG_DEFINE(t2i_cfg_desc, "FS Config");
static const uint8_t attributes = USB_SCD_SELF_POWERED;
USBD_CONFIGURATION_DEFINE(t2i_fs_config, attributes, 125, &t2i_cfg_desc);

static const struct device *const cdc = DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart0));
static const struct spi_dt_spec flash_spi =
	SPI_DT_SPEC_GET(DT_NODELABEL(extflash), SPI_WORD_SET(8) | SPI_TRANSFER_MSB, 0);

/* ---- raw SPI-NOR ops (S25FL256S, 4-byte addressing) ---- */
static void spi_send(const uint8_t *b0, size_t n0, const uint8_t *b1, size_t n1)
{
	struct spi_buf bufs[2] = { {.buf = (void *)b0, .len = n0},
				   {.buf = (void *)b1, .len = n1} };
	struct spi_buf_set set = {.buffers = bufs, .count = n1 ? 2 : 1};
	(void)spi_write_dt(&flash_spi, &set);
}
static void spi_wren(void) { uint8_t c = 0x06; spi_send(&c, 1, NULL, 0); }
static uint8_t spi_rdsr(void)
{
	uint8_t tx[2] = {0x05, 0}, rx[2] = {0};
	struct spi_buf tb = {.buf = tx, .len = 2}, rb = {.buf = rx, .len = 2};
	struct spi_buf_set ts = {.buffers = &tb, .count = 1}, rs = {.buffers = &rb, .count = 1};
	(void)spi_transceive_dt(&flash_spi, &ts, &rs);
	return rx[1];
}
/* A 64KB sector erase on this part can take seconds and the staging erase does
 * eight of them — far longer than the watchdog period, so feed it here or the
 * remote resets in the middle of taking an update. */
static void spi_wait(void)
{
	while (spi_rdsr() & 0x01) {
		safety_watchdog_feed();
		k_yield();
	}
}
static void spi_erase64k(uint32_t a)
{
	uint8_t c[5] = {0xDC, a >> 24, a >> 16, a >> 8, a};
	spi_wren(); spi_send(c, 5, NULL, 0); spi_wait();
}
static void spi_pp(uint32_t a, const uint8_t *d, size_t n)
{
	uint8_t h[5] = {0x12, a >> 24, a >> 16, a >> 8, a};
	spi_wren(); spi_send(h, 5, d, n); spi_wait();
}
/* Clear any latched erase/program error (else WIP stays stuck) and remove the
 * power-on block protection so the staging area is erasable/writable. */
static void spi_unprotect(void)
{
	uint8_t clsr = 0x30; spi_send(&clsr, 1, NULL, 0);   /* CLSR: clear E_ERR/P_ERR + WIP */
	spi_wren();
	uint8_t wrr[2] = {0x01, 0x00};                       /* WRR: SR1=0 -> BP=0, SRWD=0 */
	spi_send(wrr, 2, NULL, 0);
	spi_wait();
}

/* ---- internal flash: erase sector 7 to invalidate marker @0x0807FFFC ---- */
#define FLASH_KEYR (*(volatile uint32_t *)0x40023C04)
#define FLASH_SR   (*(volatile uint32_t *)0x40023C0C)
#define FLASH_CR   (*(volatile uint32_t *)0x40023C10)
static void invalidate_internal_marker(void)
{
	FLASH_KEYR = 0x45670123; FLASH_KEYR = 0xCDEF89AB;   /* unlock */
	while (FLASH_SR & (1u << 16)) { }
	FLASH_CR = (1u << 1) | (7u << 3) | (2u << 8);        /* SER, SNB=7, PSIZE=x32 */
	FLASH_CR |= (1u << 16);                              /* STRT */
	while (FLASH_SR & (1u << 16)) { }
	FLASH_CR = (1u << 31);                               /* clear SER, LOCK */
}

/* ---- staging state ---- */
static uint32_t declared, recv_cnt, stage_addr;
static uint8_t  cksum, page[256];
static uint16_t page_len;
static bool     active;

static void stage_flush(void)
{
	if (page_len) { spi_pp(stage_addr, page, page_len); stage_addr += page_len; page_len = 0; }
}

static void do_finalize(void)
{
	stage_flush();
	DBG(3) = (cksum == 0) ? 0x600D0000u : (0xBAD00000u | cksum);
	if (cksum == 0) {
		invalidate_internal_marker();
		DBG(3) = 0x600DF00Du;
		*(volatile uint32_t *)0xE000ED0C = 0x05FA0004u;   /* SYSRESETREQ */
	}
	active = false;
}

static void start(const uint8_t *f)
{
	declared = f[3] | (f[4] << 8) | (f[5] << 16) | ((uint32_t)f[6] << 24);
	recv_cnt = 0; stage_addr = SPI_IMAGE_BASE; cksum = 0; page_len = 0; active = true;
	DBG(1) = 0; DBG(2) = declared;
	spi_unprotect();   /* clear power-on block protection so staging is erasable */
	/* erase the 512 KB staging window (8 x 64 KB sectors) covering the app image */
	for (uint32_t a = 0x01F80000u; a < 0x02000000u; a += 0x10000u) { spi_erase64k(a); }
	DBG(0) = 0x82000000u;
}

static void data(const uint8_t *d, size_t n)
{
	if (!active) { return; }
	for (size_t i = 0; i < n; i++) {
		if (recv_cnt < declared) { cksum += d[i]; }
		if ((stage_addr - SPI_IMAGE_BASE) + page_len < APP_REGION_SIZE) {
			page[page_len++] = d[i];
			if (page_len == 256) { stage_flush(); }
		}
		recv_cnt++;
		if (recv_cnt == declared) { do_finalize(); return; }
	}
	DBG(1) = recv_cnt;
}

/* 0x83 reply — the genuine 192-byte T2i info blob captured from stock RTI,
 * so Integration Designer identifies this as a real T2i. */
static const uint8_t info_reply[192] = {
	0x80, 0x88, 0x88, 0x4b, 0x00, 0x01, 0x00, 0x10, 0x01, 0x27, 0x00, 0x00,
	0x00, 0x01, 0x00, 0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x55, 0x6e,
	0x6b, 0x6e, 0x6f, 0x77, 0x6e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x13, 0x52, 0x65, 0x6d, 0x6f, 0x74,
	0x65, 0x20, 0x54, 0x65, 0x63, 0x68, 0x6e, 0x6f, 0x6c, 0x6f, 0x67, 0x69,
	0x65, 0x73, 0x03, 0x54, 0x80, 0x32, 0x69, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* Interrupt-driven RX into a ring buffer (poll_in isn't supported on CDC-ACM).
 * When the ring is full we stop draining the CDC FIFO so the USB OUT endpoint
 * NAKs and the host paces itself (needed during the multi-second staging erase). */
RING_BUF_DECLARE(rx_rb, 8192);

static void cdc_isr(const struct device *dev, void *ud)
{
	ARG_UNUSED(ud);
	while (uart_irq_update(dev), uart_irq_rx_ready(dev)) {
		uint8_t buf[64];
		if (ring_buf_space_get(&rx_rb) < sizeof(buf)) {
			uart_irq_rx_disable(dev);
			break;
		}
		int n = uart_fifo_read(dev, buf, sizeof(buf));
		if (n <= 0) { break; }
		ring_buf_put(&rx_rb, buf, n);
	}
}

/* read exactly n bytes from the CDC (blocking) */
static void cdc_read(uint8_t *b, size_t n)
{
	size_t got = 0;
	while (got < n) {
		uint32_t k = ring_buf_get(&rx_rb, b + got, n - got);
		got += k;
		uart_irq_rx_enable(cdc);   /* re-arm if the ISR disabled it when full */
		if (!k) {
			/* Sleep rather than k_yield(): yielding busy-spins whenever no
			 * other thread of >= priority is ready, which starves the rest
			 * of the firmware while we wait for a host that may never
			 * send anything. */
			k_msleep(2);
		}
	}
}

static void usb_bringup(void)
{
	(void)usbd_add_descriptor(&t2i_usbd, &t2i_lang);
	(void)usbd_add_descriptor(&t2i_usbd, &t2i_mfr);
	(void)usbd_add_descriptor(&t2i_usbd, &t2i_product);
	(void)usbd_add_configuration(&t2i_usbd, USBD_SPEED_FS, &t2i_fs_config);
	(void)usbd_register_all_classes(&t2i_usbd, USBD_SPEED_FS, 1, NULL);
	usbd_device_set_code_triple(&t2i_usbd, USBD_SPEED_FS, USB_BCC_MISCELLANEOUS, 0x02, 0x01);
	(void)usbd_init(&t2i_usbd);
	(void)usbd_enable(&t2i_usbd);
}


bool updater_busy(void)      { return active; }
uint32_t updater_received(void) { return recv_cnt; }
uint32_t updater_declared(void) { return declared; }

static void updater_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	DBG(0) = 0xAA000000u;   /* receiver alive */
	usb_bringup();
	k_msleep(1500);

	uart_irq_callback_user_data_set(cdc, cdc_isr, NULL);
	uart_irq_rx_enable(cdc);

	uint8_t frame[64];
	while (1) {
		cdc_read(frame, sizeof(frame));
		switch (frame[0]) {
		case 0x83:
			for (size_t i = 0; i < sizeof(info_reply); i++) {
				uart_poll_out(cdc, info_reply[i]);
			}
			DBG(0) = 0x83000000u;
			break;
		case 0x82:
			start(frame);
			break;
		case 0x80:
			data(frame + 1, 63);
			break;
		default:
			break;
		}
	}
}

/* Preemptible, deliberately. A cooperative priority here starves main: this
 * thread waits for CDC data by polling, and a cooperative thread that yields
 * with nothing higher-priority ready simply gets the CPU straight back — the
 * UI never advanced past its splash. Preemptible + a real sleep in the poll
 * loop lets both make progress. */
K_THREAD_STACK_DEFINE(updater_stack, 2048);
static struct k_thread updater_tcb;

void updater_init(void)
{
	k_thread_create(&updater_tcb, updater_stack, K_THREAD_STACK_SIZEOF(updater_stack),
			updater_thread, NULL, NULL, NULL,
			K_PRIO_PREEMPT(5), 0, K_NO_WAIT);
	k_thread_name_set(&updater_tcb, "usb-updater");
}
