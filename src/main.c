/*
 * Milestone 1 (debuggable variant): bring up USB CDC-ACM on the new usbd stack,
 * but do it explicitly from main() with a stage marker after each step so a
 * plain SWD read of t2i_stage tells us exactly how far it got — our only
 * "console" until USB actually works.
 *
 *   t2i_signature == 0x5A5AF00D  => main() was reached (crash, if any, is in-main)
 *   t2i_stage                    => last usbd step completed (0x10..0x18)
 *   t2i_heartbeat                => main loop is spinning (USB fully up)
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/usb/usbd.h>
#include <stdint.h>

/* Boot-progress markers in high RAM (see reset_hook.c) to bisect the crash. */
#define BOOTMARK(n)  (*(volatile unsigned int *)(0x2001FF00U + ((n) * 4U)))

static int mark_pre_kernel(void)  { BOOTMARK(2) = 0xB0070003U; return 0; }
static int mark_post_kernel(void) { BOOTMARK(3) = 0xB0070004U; return 0; }
SYS_INIT(mark_pre_kernel, PRE_KERNEL_1, 0);
SYS_INIT(mark_post_kernel, POST_KERNEL, 99);

volatile uint32_t t2i_signature;
volatile uint32_t t2i_heartbeat;
volatile uint32_t t2i_stage;

USBD_DEVICE_DEFINE(t2i_usbd,
		   DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
		   0x1209, 0x5432);

USBD_DESC_LANG_DEFINE(t2i_lang);
USBD_DESC_MANUFACTURER_DEFINE(t2i_mfr, "jttelaak");
USBD_DESC_PRODUCT_DEFINE(t2i_product, "T2i Custom Firmware");
USBD_DESC_CONFIG_DEFINE(t2i_fs_cfg_desc, "FS Config");

static const uint8_t attributes = USB_SCD_SELF_POWERED;
USBD_CONFIGURATION_DEFINE(t2i_fs_config, attributes, 125, &t2i_fs_cfg_desc);

int main(void)
{
	BOOTMARK(4) = 0xB0070005U;   /* main() reached */
	t2i_signature = 0x5A5AF00D;
	t2i_stage = 0x10;

	(void)usbd_add_descriptor(&t2i_usbd, &t2i_lang);     t2i_stage = 0x11;
	(void)usbd_add_descriptor(&t2i_usbd, &t2i_mfr);      t2i_stage = 0x12;
	(void)usbd_add_descriptor(&t2i_usbd, &t2i_product);  t2i_stage = 0x13;
	(void)usbd_add_configuration(&t2i_usbd, USBD_SPEED_FS, &t2i_fs_config);
	t2i_stage = 0x14;
	(void)usbd_register_all_classes(&t2i_usbd, USBD_SPEED_FS, 1, NULL);
	t2i_stage = 0x15;
	usbd_device_set_code_triple(&t2i_usbd, USBD_SPEED_FS,
				    USB_BCC_MISCELLANEOUS, 0x02, 0x01);
	t2i_stage = 0x16;
	(void)usbd_init(&t2i_usbd);    t2i_stage = 0x17;
	(void)usbd_enable(&t2i_usbd);  t2i_stage = 0x18;

	uint32_t n = 0;
	while (1) {
		printk("Hello from the T2i! Running my own firmware. count=%u\n", n++);
		t2i_heartbeat++;
		k_msleep(1000);
	}
	return 0;
}
