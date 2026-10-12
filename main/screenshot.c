/* Serial-triggered screenshots, see screenshot.h. */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "lvgl.h"

#define LV_HOR_RES_MAX_GUESS 320
#include "screenshot.h"

/* A full-screen snapshot needs more contiguous RAM than is free, so the
 * screen is redrawn instead and each strip is dumped on its way to the panel.
 * It's written straight to USB: printf also goes out the 115200 baud UART,
 * which made a screenshot take 20 seconds. */
static lv_display_flush_cb_t panel_flush;

static void out(const char *s, size_t len)
{
    usb_serial_jtag_write_bytes(s, len, pdMS_TO_TICKS(500));
}

static void outf(const char *fmt, int a, int b, int c, int d)
{
    char buf[48];
    out(buf, snprintf(buf, sizeof(buf), fmt, a, b, c, d));
}

static void capture_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    static const char hex[] = "0123456789abcdef";
    static char line[6 + LV_HOR_RES_MAX_GUESS * 4 + 2];
    int w = lv_area_get_width(area);
    outf("SNAP-AREA %d %d %d %d\n", area->x1, area->y1, area->x2, area->y2);
    for (int y = 0; y < lv_area_get_height(area); y++) {
        const uint8_t *row = px + y * w * 2;
        char *p = line + sprintf(line, "SNAP:");
        for (int i = 0; i < w * 2 && p - line < (int)sizeof(line) - 3; i++) {
            *p++ = hex[row[i] >> 4];
            *p++ = hex[row[i] & 15];
        }
        *p++ = '\n';
        out(line, p - line);
    }
    /* Let the idle task feed the watchdog. */
    vTaskDelay(1);
    panel_flush(disp, area, px);
}

void screenshot_dump(void)
{
    if (!lvgl_port_lock(2000)) {
        out("SNAP-ERR busy\n", 14);
        return;
    }
    lv_display_t *disp = lv_display_get_default();
    outf("SNAP-BEGIN %d %d\n", lv_display_get_horizontal_resolution(disp),
         lv_display_get_vertical_resolution(disp), 0, 0);
    panel_flush = lv_display_get_flush_cb(disp);
    lv_display_set_flush_cb(disp, capture_flush);
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(disp);
    lv_display_set_flush_cb(disp, panel_flush);
    lvgl_port_unlock();
    out("SNAP-END\n", 9);
}
