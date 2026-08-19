/*
 * Telegesis ETRX2 EM250 radio image, embedded for the on-device reflash (src/em250.c).
 *
 * The image is NOT committed as a C array — it is generated at build time from tools/rti/etrx2.ebl
 * so it can be replaced by swapping that .ebl (a newer Telegesis build, or a different EM250 NCP).
 * The CMake generate_inc_file_for_target rule turns it into etrx2_ebl.inc during the build.
 */
#include <stddef.h>
#include <stdint.h>

const unsigned char etrx2_ebl_raw[] = {
#include "etrx2_ebl.inc"
};
const unsigned int etrx2_ebl_raw_len = sizeof etrx2_ebl_raw;
