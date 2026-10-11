/* Serial-triggered screenshots, see screenshot.h. */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "lvgl.h"

#define LV_HOR_RES_MAX_GUESS 320
#include "screenshot.h"

/* A full-screen snapshot needs more contiguous RAM than is free, so the
 * screen is redrawn instead and each strip is dumped on its way to the panel. */
static lv_display_flush_cb_t panel_flush;

static void capture_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    static const char hex[] = "0123456789abcdef";
    static char line[6 + LV_HOR_RES_MAX_GUESS * 4 + 2];
    int w = lv_area_get_width(area);
    printf("SNAP-AREA %d %d %d %d\n", (int)area->x1, (int)area->y1, (int)area->x2, (int)area->y2);
    for (int y = 0; y < lv_area_get_height(area); y++) {
        const uint8_t *row = px + y * w * 2;
        char *p = line + sprintf(line, "SNAP:");
        for (int i = 0; i < w * 2 && p - line < (int)sizeof(line) - 3; i++) {
            *p++ = hex[row[i] >> 4];
            *p++ = hex[row[i] & 15];
        }
        *p++ = '\n';
        *p = '\0';
        fputs(line, stdout);
        /* Printing takes seconds; let the idle task feed the watchdog. */
        vTaskDelay(1);
    }
    panel_flush(disp, area, px);
}

void screenshot_dump(void)
{
    if (!lvgl_port_lock(2000)) {
        printf("SNAP-ERR busy\n");
        return;
    }
    lv_display_t *disp = lv_display_get_default();
    printf("SNAP-BEGIN %d %d\n", (int)lv_display_get_horizontal_resolution(disp),
           (int)lv_display_get_vertical_resolution(disp));
    panel_flush = lv_display_get_flush_cb(disp);
    lv_display_set_flush_cb(disp, capture_flush);
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(disp);
    lv_display_set_flush_cb(disp, panel_flush);
    lvgl_port_unlock();
    printf("SNAP-END\n");
    fflush(stdout);
}
