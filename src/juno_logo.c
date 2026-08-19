/*
 * Juno logo splash. The pixel data is NOT committed as a C array — it is generated at build time
 * from assets/juno_logo.rgb565 (220x60, RGB565), so the logo can be replaced by swapping that
 * binary. Produce a new one with tools/make_logo.sh; the CMake generate_inc_file_for_target rule
 * turns it into juno_logo.inc during the build.
 */
#include <lvgl.h>

static const uint8_t juno_logo_map[] = {
#include "juno_logo.inc"
};

const lv_image_dsc_t juno_logo = {
    .header = {
        .magic  = LV_IMAGE_HEADER_MAGIC,
        .cf     = LV_COLOR_FORMAT_RGB565,
        .flags  = 0,
        .w      = 220,
        .h      = 60,
        .stride = 440,
    },
    .data_size = sizeof(juno_logo_map),
    .data      = juno_logo_map,
};
