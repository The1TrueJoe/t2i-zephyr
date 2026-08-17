#ifndef T2I_UPDATER_H
#define T2I_UPDATER_H

#include <stdbool.h>
#include <stdint.h>

/*
 * USB update receiver — the only way to reflash a remote without SWD.
 *
 * The RTI bootloader has NO USB (verified: sector 0 references SPI3, GPIO and
 * RCC, and contains no reference to the OTG-FS block at 0x5000_0000). So the
 * bootloader can only commit an image that is already staged in external
 * SPI-NOR — putting it there is entirely the application's job. If the
 * application cannot enumerate USB, a remote without SWD has no way back.
 *
 * That makes this module the anti-brick mechanism, and it is why it runs in its
 * own thread started before any display/UI work: a hang or fault in the UI must
 * not take the update path down with it.
 */
void updater_init(void);

/* True once a host has started sending an image (0x82 frame seen). */
bool updater_busy(void);

/* Bytes received so far / total declared by the host — for progress display. */
uint32_t updater_received(void);
uint32_t updater_declared(void);

/* Emit a line of text to the host on the CDC.
 *
 * Shares the CDC with the update protocol, which is input-driven — the host only
 * ever receives replies it asked for — so unsolicited event lines are safe while
 * no update is running. Suppressed during an update so the two never interleave.
 */
void updater_emit(const char *line);

#endif /* T2I_UPDATER_H */
