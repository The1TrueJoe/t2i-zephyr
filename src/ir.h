#ifndef T2I_IR_H
#define T2I_IR_H
#include <stdint.h>

void ir_init(void);

/* Send one IR frame. `us` is alternating mark/space durations in microseconds,
 * starting with a mark; `n` is how many entries. Blocks for the frame length. */
void ir_send(const uint16_t *us, int n, int carrier_hz);

/* NEC frame for (addr, cmd) — the one thing that can be checked against a real
 * receiver without an IR code database. */
void ir_send_nec(uint8_t addr, uint8_t cmd);

#endif
