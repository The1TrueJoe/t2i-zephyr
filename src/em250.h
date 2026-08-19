#ifndef T2I_EM250_H
#define T2I_EM250_H

/*
 * Reflashing the EM250 from the remote itself.
 *
 * Opcode 0x08 launches the radio's Ember standalone bootloader on USART3 — no bootmode pin, no
 * hardware access (docs/ZIGBEE-PROTOCOL.md). The upload is plain XMODEM-CRC, exactly as RTI's own
 * ZBConfig.exe drives a ZB-Pro dongle over FTDI.
 *
 * This cannot brick the radio. The image's 97 program records write only 0x02800-0x19BC8,
 * 0x1C000-0x1CAAE and 0x1DF32-0x1DFFC; bytes 0x0000-0x27FF — the bootloader's reserved area — are
 * never written. A failed or interrupted upload leaves the application invalid, which keeps the
 * bootloader resident rather than losing it.
 *
 * It is NOT undoable, though: there is no copy of RTI's TXBZB image anywhere (not in the STM32
 * flash, not in Integration Designer, not on the VM disk) and no way to read one out of the radio
 * — the bootloader has no dump command and the app has no memory read. Only Ember's SIF debug
 * interface could dump it, and that needs physical probing.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Enter the radio's bootloader and confirm the banner. Retries; PC13 resets between attempts. */
bool em250_bl_enter(void);

/* Read the installed image's platform string (bootloader menu option 3) into out.
 * Returns bytes written. "xap2b-em250-em250-TXBZB" is RTI's stock radio app. */
size_t em250_bl_info(char *out, size_t max);

/* Leave the bootloader by running the application (menu option 2). */
void em250_bl_run(void);

/* XMODEM-CRC an .ebl into the radio. Must already be in the bootloader.
 * progress() is called with (blocks_done, blocks_total) so a long upload is observable. */
bool em250_flash_ebl(const uint8_t *ebl, size_t len,
		     void (*progress)(unsigned done, unsigned total));

extern const uint8_t zbpro_ebl[];
extern const size_t zbpro_ebl_len;

#endif
