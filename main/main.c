/* SL metro departure board on a Waveshare ESP32-C6-LCD-1.47. */
#include <stdlib.h>
#include <time.h>
#include "esp_lvgl_port.h"
#include "board.h"
#include "controls.h"
#include "serial.h"
#include "sl_api.h"
#include "ui.h"
#include "web.h"
#include "wifi_setup.h"

void app_main(void)
{
    /* SL's API returns local times, so mktime() must use Stockholm time. */
    setenv("TZ", CONFIG_CLOCK_TZ, 1);
    tzset();

    /* Initialises NVS, which controls_load() reads from. */
    board_wifi_init();
    /* Early, so the web installer's Improv query gets an answer quickly. */
    serial_init();

    int group;
    uint16_t site;
    controls_load(&group, &site);
    /* Before the UI, which reads SL state; polling waits for Wi-Fi itself. */
    sl_start(group, site);

    board_display_init();
    lvgl_port_lock(0);
    ui_init();
    lvgl_port_unlock();

    controls_init();
    serial_ui_ready();
    web_start();

    /* First boot: offer phone setup straight away. USB setup through the
     * web installer works at the same time. */
    if (!board_wifi_configured()) {
        wifi_setup_start();
    }
}
