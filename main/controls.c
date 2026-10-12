/* The BOOT button (GPIO9) is the only user button on the board.
 *
 *   board view       click: next station   double: previous   hold: pick line
 *   line picker      click: next line      double: previous   hold: choose
 *   station picker   click: next station   double: previous   hold: choose
 *   Wi-Fi setup      hold: cancel
 *   phone QR code    any press: close
 *   about screen     click: close          hold: check for updates
 *
 * The line picker ends with "Mobil…", which shows a QR code for the
 * settings page (or starts phone setup when offline), "English" (or
 * "Svenska"), which switches language, and "Om / Uppdatera…".
 *
 * The picker closes without changes after PICKER_TIMEOUT_MS of inactivity.
 */
#include <string.h>
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "nvs.h"
#include "iot_button.h"
#include "button_gpio.h"
#include "sl_api.h"
#include "ui.h"
#include "wifi_setup.h"
#include "ota.h"
#include "board.h"
#include "controls.h"
#include "i18n.h"

#define PIN_BOOT            9
#define PICKER_TIMEOUT_MS   30000
#define DEFAULT_GROUP       0       /* Gröna linjen */
#define DEFAULT_SITE        9109    /* Brommaplan */
#define NVS_NS              "sl"

static const char *TAG = "controls";

typedef enum { MODE_BOARD, MODE_PICK_LINE, MODE_PICK_STATION, MODE_PHONE, MODE_ABOUT } ui_mode_t;

static ui_mode_t mode = MODE_BOARD;
static int pick_group;
static int pick_index;
static esp_timer_handle_t timeout_timer;
/* Room for every station name on the longest line, newline separated. */
static char options[1024];

void controls_load(int *group, uint16_t *site)
{
    *group = DEFAULT_GROUP;
    *site = DEFAULT_SITE;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t g;
        uint16_t s;
        if (nvs_get_u8(h, "group", &g) == ESP_OK && nvs_get_u16(h, "site", &s) == ESP_OK &&
            g < METRO_N_GROUPS) {
            *group = g;
            *site = s;
        }
        nvs_close(h);
    }
}

static void save(int group, uint16_t site)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "group", group);
        nvs_set_u16(h, "site", site);
        nvs_commit(h);
        nvs_close(h);
    }
}

static int station_index(const metro_group_t *g, uint16_t site)
{
    for (int i = 0; i < g->n_stations; i++) {
        if (g->stations[i] == site) {
            return i;
        }
    }
    return -1;
}

void controls_select(int group, uint16_t site)
{
    sl_select(group, site);
    save(group, site);
}

/* Step to the next/previous station on the current line, wrapping. */
static void step_station(int delta)
{
    sl_config_t c;
    sl_get_config(&c);
    const metro_group_t *g = &metro_groups[c.group];
    int i = station_index(g, c.home);
    i = ((i < 0 ? 0 : i) + delta + g->n_stations) % g->n_stations;
    controls_select(c.group, g->stations[i]);
}

/* Entries after the line groups in the line picker. */
#define PICK_PHONE  METRO_N_GROUPS
#define PICK_LANG   (METRO_N_GROUPS + 1)
#define PICK_ABOUT  (METRO_N_GROUPS + 2)
#define PICK_COUNT  (METRO_N_GROUPS + 3)

static void show_line_picker(void)
{
    options[0] = '\0';
    for (int i = 0; i < METRO_N_GROUPS; i++) {
        strlcat(options, metro_group_name(i), sizeof(options));
        strlcat(options, "\n", sizeof(options));
    }
    /* The other language, named in that language. */
    strlcat(options, T("Mobil…\nEnglish\nOm / Uppdatera…", "Phone…\nSvenska\nAbout / Update…"),
            sizeof(options));
    ui_picker_show(T("Välj linje", "Choose line"), options, pick_index,
                   metro_groups[pick_index < METRO_N_GROUPS ? pick_index : 0].color);
}

static void show_station_picker(void)
{
    const metro_group_t *g = &metro_groups[pick_group];
    options[0] = '\0';
    for (int i = 0; i < g->n_stations; i++) {
        if (i) strlcat(options, "\n", sizeof(options));
        strlcat(options, metro_station_name(g->stations[i]), sizeof(options));
    }
    ui_picker_show(metro_group_name(pick_group), options, pick_index, g->color);
}

static int picker_count(void)
{
    return mode == MODE_PICK_LINE ? PICK_COUNT : metro_groups[pick_group].n_stations;
}

typedef enum { EV_NEXT, EV_PREV, EV_HOLD, EV_TIMEOUT } event_t;

/* Runs with the LVGL lock held. */
static void handle(event_t ev)
{
    if (wifi_setup_active()) {
        if (ev == EV_HOLD) {
            wifi_setup_stop();
        }
        return;
    }
    /* The phone QR code closes itself once a station is chosen. */
    if (mode == MODE_PHONE && !ui_phone_visible()) {
        mode = MODE_BOARD;
    }
    if (ev == EV_TIMEOUT) {
        if (mode != MODE_BOARD) {
            ui_picker_hide();
            mode = MODE_BOARD;
        }
        return;
    }
    if (mode != MODE_BOARD) {
        esp_timer_stop(timeout_timer);
        esp_timer_start_once(timeout_timer, PICKER_TIMEOUT_MS * 1000ULL);
    }

    switch (mode) {
    case MODE_PHONE:
        if (ev != EV_TIMEOUT) {
            ui_phone_hide();
            mode = MODE_BOARD;
        }
        break;

    case MODE_ABOUT:
        if (ev == EV_HOLD) {
            ota_check_now();
        } else if (ev != EV_TIMEOUT) {
            ui_about_hide();
            mode = MODE_BOARD;
        }
        break;

    case MODE_BOARD:
        if (ev == EV_NEXT || ev == EV_PREV) {
            if (metro_ready()) {
                step_station(ev == EV_NEXT ? 1 : -1);
            }
        } else {
            sl_config_t c;
            sl_get_config(&c);
            pick_group = c.group;
            pick_index = c.group;
            mode = MODE_PICK_LINE;
            show_line_picker();
            esp_timer_start_once(timeout_timer, PICKER_TIMEOUT_MS * 1000ULL);
        }
        break;

    case MODE_PICK_LINE:
    case MODE_PICK_STATION:
        if (ev == EV_NEXT || ev == EV_PREV) {
            int n = picker_count();
            pick_index = (pick_index + (ev == EV_NEXT ? 1 : -1) + n) % n;
            ui_picker_select(pick_index);
        } else if (mode == MODE_PICK_LINE && pick_index == PICK_PHONE) {
            esp_timer_stop(timeout_timer);
            ui_picker_hide();
            /* Online, the settings page is on the home network; offline,
             * the board's own hotspot serves it. */
            if (board_wifi_connected()) {
                mode = MODE_PHONE;
                ui_phone_show();
            } else {
                mode = MODE_BOARD;
                wifi_setup_start();
            }
        } else if (mode == MODE_PICK_LINE && pick_index == PICK_LANG) {
            /* Switch and go back to the board, which shows the new language. */
            esp_timer_stop(timeout_timer);
            ui_picker_hide();
            mode = MODE_BOARD;
            i18n_set(i18n_lang() == LANG_EN ? LANG_SV : LANG_EN);
        } else if (mode == MODE_PICK_LINE && pick_index == PICK_ABOUT) {
            esp_timer_stop(timeout_timer);
            ui_picker_hide();
            mode = MODE_ABOUT;
            ui_about_show();
        } else if (mode == MODE_PICK_LINE && pick_index < METRO_N_GROUPS && !metro_ready()) {
            /* No stations yet: stay in the list. */
        } else if (mode == MODE_PICK_LINE) {
            pick_group = pick_index;
            /* Start on the current station if it is on this line too. */
            sl_config_t c;
            sl_get_config(&c);
            int i = station_index(&metro_groups[pick_group], c.home);
            pick_index = i < 0 ? 0 : i;
            mode = MODE_PICK_STATION;
            show_station_picker();
        } else {
            esp_timer_stop(timeout_timer);
            ui_picker_hide();
            mode = MODE_BOARD;
            controls_select(pick_group, metro_groups[pick_group].stations[pick_index]);
        }
        break;
    }
}

static void dispatch(event_t ev)
{
    if (lvgl_port_lock(1000)) {
        handle(ev);
        lvgl_port_unlock();
    } else {
        ESP_LOGW(TAG, "UI busy, dropped button event");
    }
}

static void on_click(void *btn, void *arg)  { dispatch(EV_NEXT); }
static void on_double(void *btn, void *arg) { dispatch(EV_PREV); }
static void on_hold(void *btn, void *arg)   { dispatch(EV_HOLD); }
static void on_timeout(void *arg)           { dispatch(EV_TIMEOUT); }

void controls_simulate(char gesture)
{
    switch (gesture) {
    case 'n': dispatch(EV_NEXT); break;
    case 'p': dispatch(EV_PREV); break;
    case 'h': dispatch(EV_HOLD); break;
    }
}

void controls_init(void)
{
    const esp_timer_create_args_t targs = { .callback = on_timeout, .name = "picker_timeout" };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &timeout_timer));

    const button_config_t cfg = { .long_press_time = 800 };
    const button_gpio_config_t gpio_cfg = { .gpio_num = PIN_BOOT, .active_level = 0 };
    button_handle_t btn;
    ESP_ERROR_CHECK(iot_button_new_gpio_device(&cfg, &gpio_cfg, &btn));
    ESP_ERROR_CHECK(iot_button_register_cb(btn, BUTTON_SINGLE_CLICK, NULL, on_click, NULL));
    ESP_ERROR_CHECK(iot_button_register_cb(btn, BUTTON_DOUBLE_CLICK, NULL, on_double, NULL));
    ESP_ERROR_CHECK(iot_button_register_cb(btn, BUTTON_LONG_PRESS_START, NULL, on_hold, NULL));
}
