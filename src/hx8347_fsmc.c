/*
 * Zephyr display driver for the RTI T2i panel: Himax HX8347 on an 8-bit FSMC
 * parallel bus. Reverse-engineered from stock RTI and verified on hardware
 * (color bars). Exposes the Zephyr display API so LVGL can render to it.
 *
 *   command byte -> *(0x60000000), data byte -> *(0x60040000)  (RS/DC = A18 = PD13)
 *   FSMC bank1/NE1, 8-bit; data PD14/15/0/1 + PE7-10; RD=PD4 WR=PD5 CS=PD7 RS=PD13
 *   PD6 = LCD reset. Backlight = TIM2_CH2 PWM on PA1 (dimming input to the driver IC).
 *
 * Register names live in t2i_regs.h.
 */
#define DT_DRV_COMPAT rti_hx8347_fsmc

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <string.h>
#include <stdint.h>
#include "t2i_regs.h"
#include "backlight.h"

#define PANEL_W 240
#define PANEL_H 320

/* Backlight PWM frequency is chosen dynamically from the duty cycle.
 *
 * Two conflicting constraints:
 *   - the driver IC needs an ON pulse of roughly >=5us. Below that it produces
 *     no light and can drop into a shutdown only a power cycle clears. Stock
 *     RTI avoids this by running at 2 kHz (FUN_0801a9fa: 6 MHz timer / 3000),
 *     where even 1% duty is a 5us pulse.
 *   - 2 kHz is audible, and the boost converter whines at it.
 *
 * Since pulse = duty x period, a low frequency is only *needed* at low duty.
 * So: pick the highest frequency that still keeps the pulse >= BL_MIN_PULSE_US,
 * capped at an inaudible 30 kHz. Normal brightness therefore runs silently at
 * 30 kHz, and only the very dim end drops toward stock's 2 kHz — where the LED
 * current, and hence the whine, is smallest anyway.
 *
 *   duty 50%  -> 30 kHz (16us pulse)      duty 5%  -> 10 kHz (5us)
 *   duty 15%  -> 30 kHz (5us)             duty 1%  ->  2 kHz (5us)
 */
#define BL_MIN_PULSE_US 5
#define BL_MAX_HZ       30000    /* inaudible */
#define BL_MIN_HZ       2000     /* stock RTI */
#define BL_TIMER_HZ     60000000 /* TIM2 clock with PSC = 0 */

static void pin_af(int port, int pin, int af)
{
	GPIO_MODER(port) = (GPIO_MODER(port) & ~(3u << (pin * 2))) | (GPIO_MODE_AF << (pin * 2));
	GPIO_OSPEEDR(port) |= (3u << (pin * 2));
	if (pin < 8) GPIO_AFRL(port) = (GPIO_AFRL(port) & ~(0xFu << (pin * 4))) | ((uint32_t)af << (pin * 4));
	else         GPIO_AFRH(port) = (GPIO_AFRH(port) & ~(0xFu << ((pin - 8) * 4))) | ((uint32_t)af << ((pin - 8) * 4));
}
static void pin_out(int port, int pin, int val)
{
	GPIO_MODER(port) = (GPIO_MODER(port) & ~(3u << (pin * 2))) | (GPIO_MODE_OUTPUT << (pin * 2));
	GPIO_OSPEEDR(port) |= (3u << (pin * 2));
	GPIO_BSRR(port) = val ? (1u << pin) : (1u << (pin + 16));
}

static inline void lcd_cmd(uint8_t c) { LCD_CMD_REG = c; }
static inline void lcd_dat(uint8_t d) { LCD_DAT_REG = d; }
static inline void lcd_reg(uint8_t c, uint8_t d) { LCD_CMD_REG = c; LCD_DAT_REG = d; }

/* HX8347 windowed access: cols (X) = regs 0x02-0x05, rows (Y) = regs 0x06-0x09. */
static void lcd_window(int x0, int y0, int x1, int y1)
{
	lcd_reg(0x02, x0 >> 8); lcd_reg(0x03, x0 & 0xFF);
	lcd_reg(0x04, x1 >> 8); lcd_reg(0x05, x1 & 0xFF);
	lcd_reg(0x06, y0 >> 8); lcd_reg(0x07, y0 & 0xFF);
	lcd_reg(0x08, y1 >> 8); lcd_reg(0x09, y1 & 0xFF);
}

static void panel_init(void)
{
	lcd_reg(0xEA,0x00); lcd_reg(0xEB,0x20); lcd_reg(0xEC,0x0C); lcd_reg(0xED,0xC4);
	lcd_reg(0xE8,0x40); lcd_reg(0xE9,0x38); lcd_reg(0xF1,0x01); lcd_reg(0xF2,0x10);
	lcd_reg(0x27,0xA3);
	lcd_reg(0x40,0x01); lcd_reg(0x41,0x07); lcd_reg(0x42,0x06); lcd_reg(0x43,0x0A);
	lcd_reg(0x44,0x0C); lcd_reg(0x45,0x3D); lcd_reg(0x46,0x02); lcd_reg(0x47,0x43);
	lcd_reg(0x48,0x07); lcd_reg(0x49,0x14); lcd_reg(0x4A,0x19); lcd_reg(0x4B,0x1A);
	lcd_reg(0x4C,0x1E);
	lcd_reg(0x50,0x02); lcd_reg(0x51,0x33); lcd_reg(0x52,0x35); lcd_reg(0x53,0x39);
	lcd_reg(0x54,0x38); lcd_reg(0x55,0x3E); lcd_reg(0x56,0x3C); lcd_reg(0x57,0x7D);
	lcd_reg(0x58,0x01); lcd_reg(0x59,0x05); lcd_reg(0x5A,0x06); lcd_reg(0x5B,0x0B);
	lcd_reg(0x5C,0x18); lcd_reg(0x5D,0xFF);
	lcd_reg(0x1B,0x1B); lcd_reg(0x1A,0x01); lcd_reg(0x24,0x45); lcd_reg(0x25,0x1F);
	lcd_reg(0x23,0x8A); lcd_reg(0x18,0x36); lcd_reg(0x19,0x01); lcd_reg(0x01,0x00);
	lcd_reg(0x1F,0x88); k_msleep(5); lcd_reg(0x1F,0x80); k_msleep(5);
	lcd_reg(0x1F,0x90); k_msleep(5); lcd_reg(0x1F,0xD0); k_msleep(5);
	lcd_reg(0x16,0x40); lcd_reg(0x17,0x05); lcd_reg(0x36,0x02);
	lcd_reg(0x28,0x38); k_msleep(40); lcd_reg(0x28,0x3C);
	lcd_window(0, 0, PANEL_W - 1, PANEL_H - 1);
}

/* LCD backlight on PA1 = TIM2_CH2. pct 0..100. PWM mode 2, active-low (RTI). */
/* LCD backlight on PA1 = TIM2_CH2, following stock RTI's FunLight backlight
 * setter (FUN_0801a9fa) exactly, because the endpoints matter:
 *
 *   0    -> TIM2 DISABLED, PA1 driven LOW as plain GPIO  (genuinely off)
 *   100  -> TIM2 DISABLED, PA1 driven HIGH as plain GPIO (full brightness)
 *   else -> PWM mode 2 on PA1
 *
 * Leaving the timer running while parking the pin, or restoring duty without
 * re-initialising the output compare, does not reliably bring the light back —
 * which is why "off" previously became "off permanently". Stock disables the
 * timer at both endpoints and rebuilds the PWM on the way out, so we do too.
 */
static void backlight_set(int pct)
{
	if (pct < 0) pct = 0; if (pct > 100) pct = 100;
	RCC_APB1ENR |= (1u << 0);                /* TIM2EN */

	if (pct == 0 || pct == 100) {
		TIM2_CR1 = 0;                    /* stop the timer first */
		pin_out(GPIO_PORT_A, 1, pct ? 1 : 0);
		return;
	}

	/* highest frequency whose ON pulse still clears BL_MIN_PULSE_US */
	uint32_t hz = (uint32_t)pct * (1000000u / BL_MIN_PULSE_US) / 100u;
	if (hz > BL_MAX_HZ) { hz = BL_MAX_HZ; }
	if (hz < BL_MIN_HZ) { hz = BL_MIN_HZ; }

	uint32_t arr = BL_TIMER_HZ / hz;       /* TIM2 is 32-bit, so this always fits */

	TIM2_PSC  = 0;
	TIM2_ARR  = arr;
	TIM2_CCR2 = (uint32_t)pct * arr / 100u;
	TIM2_CCMR1 = 0x7800;                     /* OC2M = PWM mode 2 + OC2PE */
	TIM2_CCER  = 0x30;                       /* CC2E + CC2P (active-low) */
	TIM2_EGR   = TIM_EGR_UG;                 /* latch preload */
	pin_af(GPIO_PORT_A, 1, 1);               /* PA1 -> AF1 = TIM2_CH2 */
	TIM2_CR1   = TIM_CR1_CEN;
}

/* Keypad backlight, following stock RTI (FUN_0801ab1e): TIM8 period 300 with
 * pulse = pct*2 + 75, and the 0/100 endpoints driven as static GPIO rather than
 * PWM — the same shape as the LCD backlight. */
void keypad_backlight_set(int pct)
{
	if (pct < 0) pct = 0; if (pct > 100) pct = 100;
	RCC_APB2ENR |= (1u << 1);                /* TIM8EN */

	if (pct == 0 || pct == 100) {
		TIM8_CR1 = 0;
		/* active-low: high = off, low = on */
		pin_out(GPIO_PORT_C, 8, pct ? 0 : 1);
		return;
	}

	pin_af(GPIO_PORT_C, 8, 3);               /* PC8 -> AF3 = TIM8_CH3 */
	TIM8_PSC = 9; TIM8_ARR = 300;
	TIM8_CCR3 = (uint32_t)pct * 2 + 75;      /* stock curve */
	TIM8_CCMR2 = (6u << 4) | (1u << 3);      /* OC3M = PWM1 + OC3PE */
	TIM8_CCER  = (1u << 8) | (1u << 9);      /* CC3E + CC3P */
	TIM8_BDTR  = TIM_BDTR_MOE;               /* advanced timer needs MOE */
	TIM8_CR1   = TIM_CR1_CEN;
}

static void lcd_hw_init(void)
{
	RCC_AHB1ENR |= RCC_AHB1ENR_GPIO(GPIO_PORT_A) | RCC_AHB1ENR_GPIO(GPIO_PORT_C)
		     | RCC_AHB1ENR_GPIO(GPIO_PORT_D) | RCC_AHB1ENR_GPIO(GPIO_PORT_E);
	RCC_AHB3ENR |= (1u << 0);                /* FSMC clock */

	/* Kill the LCD backlight before anything else: the RTI bootloader hands off
	 * with TIM2 already driving it, so the panel can be lit (showing power-up
	 * noise) from the instant we start. Turning it off first, and only raising
	 * it once the app has drawn a frame, is what removes the RGB grid. */
	backlight_set(0);

	/* PC12 = backlight/boost rail enable. Stock drives it high in FUN_08021354
	 * (output PP + pull-up). Without it both backlights stay dark even though
	 * the PWMs run and the panel logic answers on the FSMC bus. */
	pin_out(GPIO_PORT_C, 12, 1);

	keypad_backlight_set(90);

	int dpins[] = {0,1,4,5,7,13,14,15};
	for (unsigned i = 0; i < sizeof(dpins)/sizeof(dpins[0]); i++) pin_af(GPIO_PORT_D, dpins[i], 12);
	int epins[] = {7,8,9,10};
	for (unsigned i = 0; i < sizeof(epins)/sizeof(epins[0]); i++) pin_af(GPIO_PORT_E, epins[i], 12);

	pin_out(GPIO_PORT_D, 6, 1);              /* PD6 = LCD reset (idle high) */

	FSMC_BTR1 = FSMC_BTR1_CFG;
	FSMC_BCR1 = FSMC_BCR1_CFG;
	FSMC_BCR1 |= 0x1u;                        /* MBKEN */

	pin_out(GPIO_PORT_D, 6, 0); k_msleep(20);
	pin_out(GPIO_PORT_D, 6, 1); k_msleep(150);

	panel_init();

	/* Clear GRAM to black. The panel powers up holding noise, and the RTI
	 * bootloader may already have raised the backlight before we got here — so
	 * blanking alone is not enough to guarantee nothing is ever displayed.
	 * Wiping the framebuffer means there is simply nothing to show. */
	lcd_window(0, 0, PANEL_W - 1, PANEL_H - 1);
	lcd_cmd(0x22);
	for (uint32_t i = 0; i < (uint32_t)PANEL_W * PANEL_H; i++) {
		lcd_dat(0x00);
		lcd_dat(0x00);
	}
}

static int hx8347_write(const struct device *dev, uint16_t x, uint16_t y,
			const struct display_buffer_descriptor *desc, const void *buf)
{
	ARG_UNUSED(dev);
	const uint16_t *pixels = buf;
	uint16_t w = desc->width, h = desc->height;
	uint16_t pitch = desc->pitch ? desc->pitch : w;

	lcd_window(x, y, x + w - 1, y + h - 1);
	lcd_cmd(0x22);
	for (uint16_t row = 0; row < h; row++) {
		const uint16_t *line = &pixels[row * pitch];
		for (uint16_t col = 0; col < w; col++) {
			uint16_t p = line[col];
			lcd_dat(p >> 8);
			lcd_dat(p & 0xFF);
		}
	}
	return 0;
}

static void hx8347_get_caps(const struct device *dev, struct display_capabilities *caps)
{
	ARG_UNUSED(dev);
	memset(caps, 0, sizeof(*caps));
	caps->x_resolution = PANEL_W;
	caps->y_resolution = PANEL_H;
	caps->supported_pixel_formats = PIXEL_FORMAT_RGB_565;
	caps->current_pixel_format = PIXEL_FORMAT_RGB_565;
	caps->current_orientation = DISPLAY_ORIENTATION_NORMAL;
}

/* Blanking parks PA1 as a plain GPIO driven low, rather than leaving TIM2
 * running at 0% duty.
 *
 * Two reasons. First, a permanently-low *PWM* input makes the backlight driver
 * latch into shutdown such that restoring duty alone does not bring the light
 * back (observed: dim-to-3% recovers, 0% does not) — reconfiguring the pin from
 * scratch on the way out avoids that. Second, TIM2 stops in STOP mode, so a pin
 * left under timer control holds whatever level it happened to stop at; driving
 * it as GPIO makes "off" deterministic while the CPU is stopped.
 *
 * Note we do NOT touch PC12 here: despite being the backlight/boost enable, it
 * is a shared rail — dropping it lights the low-battery indicator and cuts the
 * panel's logic supply, so the HX8347 loses its init and no amount of backlight
 * brings the image back. */
/* Panel power down/up — stock RTI's exact sequence, verified in the decomp
 * (the LCD control switch at FUN_0800fb2c, cases 6 and 5, both gated on the
 * panel ID reading 0x47 as ours does).
 *
 * This is how stock gets a genuinely black screen in sleep. It is NOT just the
 * display-on bit: it walks the HX8347's power control down (0x1F) and stops the
 * panel oscillator (0x19), then reverses that on the way back up.
 *
 * Doing it this way also sidesteps the backlight entirely. Driving the backlight
 * to a static 0 blanks the screen but this driver IC does not recover from it
 * without losing power; with the panel powered down there is nothing to see
 * anyway, so the backlight can stay at a low duty and keep receiving pulses.
 */
static int hx8347_blanking_on(const struct device *dev)
{
	ARG_UNUSED(dev);

	lcd_reg(0x28, 0x38); k_msleep(40);   /* display off */
	lcd_reg(0x28, 0x20); k_msleep(5);
	lcd_reg(0x1F, 0xA9); k_msleep(5);    /* power control down */
	lcd_reg(0x19, 0x00);                 /* oscillator off */
	backlight_set(1);                    /* minimum, but never static */
	return 0;
}

static int hx8347_blanking_off(const struct device *dev)
{
	ARG_UNUSED(dev);

	lcd_reg(0x19, 0x01); k_msleep(6);    /* oscillator on */
	lcd_reg(0x1F, 0xAC); k_msleep(5);
	lcd_reg(0x1F, 0xA4); k_msleep(5);
	lcd_reg(0x1F, 0xB4); k_msleep(5);
	lcd_reg(0x1F, 0xF4); k_msleep(5);
	lcd_reg(0x1F, 0xD4); k_msleep(5);    /* power control back up */
	lcd_reg(0x28, 0x38); k_msleep(5);
	lcd_reg(0x28, 0x3C);                 /* display on */
	backlight_set(50);
	return 0;
}

/* Backlight level, 0..255 -> 0..100% duty on TIM2_CH2. Lets the app dim rather
 * than only blank, which is both a real feature for a battery remote and the
 * only way to observe the sleep path without a working debug probe. */
static int hx8347_set_brightness(const struct device *dev, uint8_t brightness)
{
	ARG_UNUSED(dev);
	backlight_set((int)brightness * 100 / 255);
	return 0;
}

static int hx8347_set_pixel_format(const struct device *dev, enum display_pixel_format pf)
{
	ARG_UNUSED(dev);
	return (pf == PIXEL_FORMAT_RGB_565) ? 0 : -ENOTSUP;
}

static int hx8347_init(const struct device *dev)
{
	ARG_UNUSED(dev);
	lcd_hw_init();
	return 0;
}

static const struct display_driver_api hx8347_api = {
	.blanking_on = hx8347_blanking_on,
	.blanking_off = hx8347_blanking_off,
	.set_brightness = hx8347_set_brightness,
	.write = hx8347_write,
	.get_capabilities = hx8347_get_caps,
	.set_pixel_format = hx8347_set_pixel_format,
};

DEVICE_DT_INST_DEFINE(0, hx8347_init, NULL, NULL, NULL,
		      POST_KERNEL, CONFIG_DISPLAY_INIT_PRIORITY, &hx8347_api);
