/* SL metro departure board on a Waveshare ESP32-C6-LCD-1.47. */
#include <stdlib.h>
#include <time.h>
#include "esp_lvgl_port.h"
#include "board.h"
#include "controls.h"
#include "i18n.h"
#include "metro_data.h"
#include "ota.h"
#include "serial.h"
#include "sl_api.h"
#include "ui.h"
#include "web.h"
#include "wifi_setup.h"

static int saved_group;
static uint16_t saved_site;

/* Runs once the station list is loaded or fetched. */
static void stations_ready(void)
{
    sl_start(saved_group, saved_site);
}

void app_main(void)
{
    /* SL's API returns local times, so mktime() must use Stockholm time. */
    setenv("TZ", CONFIG_CLOCK_TZ, 1);
    tzset();

    /* Initialises NVS, which controls_load() reads from. */
    board_wifi_init();
    /* Early, so a new version's health check starts right away. */
    ota_init();
    /* Early, so the web installer's Improv query gets an answer quickly. */
    serial_init();

    controls_load(&saved_group, &saved_site);
    i18n_load();
    /* Before the UI, which reads SL state. */
    sl_init();

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
    metro_start(stations_ready);
    ota_start();
}
