/*
 * LIS3DH accelerometer on I2C2 @0x18 — wake-on-motion source for the T2i.
 *
 * Register values are copied verbatim from stock RTI (FUN_0801acda), which is
 * what makes the interrupt behave like the original remote:
 *   CTRL_REG2 = 0x09 puts the high-pass filter in front of INT1, so gravity is
 *   subtracted and only *changes* in acceleration trip it — without that a
 *   remote lying still on a table sits permanently above the threshold.
 *
 * INT1 is wired to PE5 (EXTI line 5) on the hardware. We don't use the pin: the
 * interrupt is latched (LIR_INT1) so polling INT1_SRC over I2C catches every
 * event and clears it. That avoids EXTI/NVIC plumbing entirely, and is plenty
 * for screen-off sleep. Wiring PE5 to EXTI only becomes necessary for STM32 STOP
 * mode, where the CPU must be woken by a pin rather than by polling.
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include "accel.h"

#define ACCEL_ADDR   0x18

#define REG_WHO_AM_I 0x0F
#define REG_CTRL1    0x20
#define REG_INT1_CFG 0x30
#define REG_REFERENCE 0x26
#define REG_INT1_SRC 0x31
#define REG_INT1_THS 0x32
#define REG_INT1_DUR 0x33
#define REG_OUT_X_L  0x28
#define AUTO_INC     0x80   /* multi-byte reads need bit7 set in the sub-address */

#define WHO_AM_I_LIS3DH 0x33
#define INT1_SRC_IA     (1u << 6)   /* an interrupt was generated */

static const struct device *i2c;
static bool ready;
static uint8_t last_src;   /* last INT1_SRC, exposed for bring-up diagnostics */

/* stock RTI init, in RTI's own order (FUN_0801acda) */
static const uint8_t init_seq[][2] = {
	{ 0x20, 0x57 },   /* CTRL_REG1: 100 Hz, X/Y/Z enabled, normal mode */
	/* CTRL_REG2: HP_IA1 only. Stock writes 0x09, which also sets FDS — that
	 * routes high-pass-filtered data into the OUTPUT registers, so a stationary
	 * remote reads ~0 on every axis with no gravity and just noise on top.
	 * (That is the "jitters around 0, no steady 1g" symptom.) RTI only cares
	 * about motion; we want readable orientation too, so clear FDS and keep the
	 * filter on the interrupt generator, where it belongs. */
	{ 0x21, 0x01 },
	{ 0x22, 0x40 },   /* CTRL_REG3: IA1 routed to the INT1 pin */
	{ 0x23, 0x20 },   /* CTRL_REG4: +/-8g  (0x08 here was the old scaling bug) */
	{ 0x24, 0x08 },   /* CTRL_REG5: latch INT1 until INT1_SRC is read */
	/* INT1_THS is 7-bit; stock writes 0x8a with bit7 (reserved) set. Taken at
	 * face value that is ~8.5g at +/-8g — a threshold nothing short of a whack
	 * will cross, which is why motion never woke us. 1 LSB = 62mg here, so
	 * 0x06 is ~0.4g: enough to catch the remote being picked up, high enough
	 * that a still remote never trips it. Raise if it wakes in your pocket. */
	{ 0x32, 0x06 },
	{ 0x33, 0x01 },   /* INT1_DURATION */
	{ 0x30, 0x2a },   /* INT1_CFG: X/Y/Z high events, OR'd */
};

bool accel_init(void)
{
	uint8_t who = 0;

	i2c = DEVICE_DT_GET(DT_NODELABEL(i2c2));
	if (!device_is_ready(i2c)) {
		return false;
	}
	if (i2c_reg_read_byte(i2c, ACCEL_ADDR, REG_WHO_AM_I, &who) != 0 ||
	    who != WHO_AM_I_LIS3DH) {
		return false;   /* wrong part, or the bus never answered */
	}

	for (unsigned i = 0; i < ARRAY_SIZE(init_seq); i++) {
		if (i2c_reg_write_byte(i2c, ACCEL_ADDR, init_seq[i][0], init_seq[i][1]) != 0) {
			return false;
		}
	}

	/* Reading REFERENCE resets the high-pass filter. Without this the filter can
	 * sit with a stale offset after configuration and never produce a value that
	 * crosses the threshold. */
	(void)i2c_reg_read_byte(i2c, ACCEL_ADDR, REG_REFERENCE, &who);
	/* clear any interrupt latched while we were configuring */
	(void)i2c_reg_read_byte(i2c, ACCEL_ADDR, REG_INT1_SRC, &who);

	ready = true;
	return true;
}

uint8_t accel_last_src(void)
{
	return last_src;
}

bool accel_read(int *x, int *y, int *z)
{
	uint8_t b[6];

	if (!ready ||
	    i2c_burst_read(i2c, ACCEL_ADDR, REG_OUT_X_L | AUTO_INC, b, sizeof(b)) != 0) {
		return false;
	}

	/* CTRL_REG1 LPen=0 + CTRL_REG4 HR=0 = normal mode: 10 significant bits,
	 * left-justified in 16. Shift down so the value is a real 10-bit signed
	 * reading (~64 counts per g at +/-8g) rather than 256x too large. */
	int16_t rx = (int16_t)(b[0] | (b[1] << 8));
	int16_t ry = (int16_t)(b[2] | (b[3] << 8));
	int16_t rz = (int16_t)(b[4] | (b[5] << 8));

	if (x) { *x = rx >> 6; }
	if (y) { *y = ry >> 6; }
	if (z) { *z = rz >> 6; }
	return true;
}

bool accel_motion(void)
{
	uint8_t src = 0;

	if (!ready ||
	    i2c_reg_read_byte(i2c, ACCEL_ADDR, REG_INT1_SRC, &src) != 0) {
		return false;
	}
	last_src = src;
	return (src & INT1_SRC_IA) != 0;   /* the read itself clears the latch */
}
