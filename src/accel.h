#ifndef T2I_ACCEL_H
#define T2I_ACCEL_H

#include <stdbool.h>
#include <stdint.h>

/* LIS3DH on I2C2 @0x18, configured exactly as stock RTI (FUN_0801acda):
 * 100 Hz, +/-8g, high-pass filtered wake-on-motion latched on INT1 (PE5). */
bool accel_init(void);

/* Raw 10-bit acceleration (normal mode, sign-extended). NULL args are skipped. */
bool accel_read(int *x, int *y, int *z);

/* True once per motion event that crossed the INT1 threshold. Reading the
 * source register clears the latch, so each event is reported exactly once. */
bool accel_motion(void);

/* Raw INT1_SRC from the last accel_motion() call — bring-up diagnostics. */
uint8_t accel_last_src(void);

#endif /* T2I_ACCEL_H */
