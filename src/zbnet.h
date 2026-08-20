#ifndef T2I_ZBNET_H
#define T2I_ZBNET_H

#include <stdbool.h>
#include <stdint.h>

/*
 * ZigBee network thread. Owns USART3 for the life of the app: it joins the
 * CA-1's network and then stays joined in the background, so a button press
 * goes out in milliseconds and the radio never blocks the UI thread. See
 * zbnet.c for why this is a thread and not part of the main loop.
 *
 * After this starts, NOTHING on the main thread may touch USART3 (no zbx_*,
 * no em250_at) — the ISR fills the RX rings and this thread is their only
 * reader.
 */

/* Launch the radio thread. Call once at boot, after zbx_uart_init(). */
void zbnet_start(void);

/* Queue a button code to unicast to the coordinator. Thread-safe and returns
 * at once; the press is dropped only if the (deep) queue is full. */
void zbnet_send_key(uint8_t code);

/* True once joined to the coordinator — drives the connectivity screen. */
bool zbnet_joined(void);

#endif /* T2I_ZBNET_H */
