#ifndef T2I_UI_H
#define T2I_UI_H

#include <zephyr/device.h>
#include "status.h"

/*
 * Everything LVGL lives here: the splash, the screen contents, and the
 * touchscreen -> LVGL pointer binding (including the panel calibration, which
 * is a property of mapping raw ADC onto *this* display).
 *
 * The rest of the firmware never calls LVGL directly — it gathers a
 * struct t2i_status and hands it over. Swapping the bring-up readout for a real
 * remote UI is then a change confined to this file.
 */

/* Panel init + splash. Keeps the backlight off until the first frame is drawn,
 * so the panel's power-up noise is never displayed. Holds the logo for
 * `splash_ms`, which doubles as the boot flash-window. */
void ui_init(const struct device *disp, uint32_t splash_ms);

/* Register the resistive panel as an LVGL pointer device. */
void ui_touch_indev_init(void);

/* Force a full repaint. The panel loses its framebuffer when powered down for
 * sleep, so everything must be redrawn on wake — LVGL only paints what it
 * believes changed. */
void ui_invalidate(void);

/* Redraw the readout and pump LVGL. */
void ui_render(const struct t2i_status *st);

/* True while the LVGL indev last saw the panel pressed. */
bool ui_touch_down(void);

/* Raw touch telemetry for the status snapshot. */
void ui_touch_raw(int *x, int *y, int *z);
void ui_touch_range(int *min_x, int *max_x, int *min_y, int *max_y);

#endif /* T2I_UI_H */
