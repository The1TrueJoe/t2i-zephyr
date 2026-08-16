/*
 * Interrupt-driven wake for the T2i.
 *
 * Polling the keypad and accelerometer kept the CPU spinning at 120 MHz doing
 * nothing, which is the opposite of what a battery remote wants. Every wake
 * source we have is already an EXTI-capable pin, so the sleep path can block on
 * a semaphore instead: the core sits in WFI until hardware raises an edge.
 *
 * The keypad rows and the accelerometer INT1 line happen not to collide — rows
 * are EXTI 0,1,2,12-15 and INT1 is EXTI 5 — which matters because an EXTI line
 * can only be mapped to one port at a time. (The touch electrodes are PA3-6, so
 * a future PENIRQ-style touch interrupt must avoid EXTI 5: use PA4 or PA6.)
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include "wake.h"

#define ACCEL_INT1_PIN 5

static const gpio_pin_t row_pins[] = { 0, 1, 2, 12, 13, 14, 15 };

static const struct device *gpioe;
static struct gpio_callback cb;
static uint32_t irqs;

static K_SEM_DEFINE(wake_sem, 0, 1);

static void on_wake(const struct device *dev, struct gpio_callback *c, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(c);
	ARG_UNUSED(pins);

	irqs++;
	k_sem_give(&wake_sem);
}

bool wake_init(void)
{
	uint32_t mask = 0;

	gpioe = DEVICE_DT_GET(DT_NODELABEL(gpioe));
	if (!device_is_ready(gpioe)) {
		return false;
	}

	for (unsigned i = 0; i < ARRAY_SIZE(row_pins); i++) {
		if (gpio_pin_configure(gpioe, row_pins[i], GPIO_INPUT | GPIO_PULL_UP) != 0 ||
		    gpio_pin_interrupt_configure(gpioe, row_pins[i], GPIO_INT_EDGE_FALLING) != 0) {
			return false;
		}
		mask |= BIT(row_pins[i]);
	}

	/* LIS3DH INT1 idles low and is driven high on an event (and latched, so it
	 * stays high until INT1_SRC is read) — the edge we want is rising. */
	if (gpio_pin_configure(gpioe, ACCEL_INT1_PIN, GPIO_INPUT) != 0 ||
	    gpio_pin_interrupt_configure(gpioe, ACCEL_INT1_PIN, GPIO_INT_EDGE_RISING) != 0) {
		return false;
	}
	mask |= BIT(ACCEL_INT1_PIN);

	gpio_init_callback(&cb, on_wake, mask);
	return gpio_add_callback(gpioe, &cb) == 0;
}

int wake_wait(k_timeout_t timeout)
{
	return k_sem_take(&wake_sem, timeout);
}

uint32_t wake_count(void)
{
	return irqs;
}
