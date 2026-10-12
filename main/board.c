/* Board support for Waveshare ESP32-C6-LCD-1.47: display, Wi-Fi, SNTP. */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lvgl_port.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "mdns.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "board.h"

#define LCD_HOST     SPI2_HOST
#define PIN_MOSI     6
#define PIN_SCLK     7
#define PIN_CS       14
#define PIN_DC       15
#define PIN_RST      21
#define PIN_BL       22
/* The ST7789 is 240x320 internally; the 172 px wide glass is centred. */
#define PANEL_GAP    34

static const char *TAG = "board";

#define BIT_GOT_IP      BIT0
#define BIT_FAILED      BIT1
/* Give up on new credentials after this many failed attempts. */
#define PROVISION_TRIES 3
/* Saved networks live next to the station choice (see controls.c). */
#define NVS_NS          "sl"
#define NVS_KEY_NETS    "nets"

static volatile bool wifi_connected;
static volatile bool wifi_configured;
/* Set while scanning or provisioning so the event handler doesn't fight
 * over the radio by reconnecting. */
static volatile bool wifi_paused;
static volatile bool wifi_hold;
static volatile int fail_count;
static char wifi_ip[16];
static char hostname[24];
static EventGroupHandle_t wifi_events;

lv_display_t *board_display_init(void)
{
    gpio_config_t bl = { .pin_bit_mask = 1ULL << PIN_BL, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&bl);
    gpio_set_level(PIN_BL, 0);

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * 40 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_CS,
        .dc_gpio_num = PIN_DC,
        .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &io));

    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
    /* Landscape: rows and columns are swapped, so the gap moves to Y. */
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel, 0, PANEL_GAP));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = LCD_H_RES * 40,
        .double_buffer = true,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy = true,
#if CONFIG_DISPLAY_FLIP
            .mirror_x = false,
            .mirror_y = true,
#else
            .mirror_x = true,
            .mirror_y = false,
#endif
        },
        .flags = {
            .buff_dma = true,
            .swap_bytes = true,
        },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    assert(disp);

    gpio_set_level(PIN_BL, 1);
    return disp;
}

static void time_synced_cb(struct timeval *tv)
{
    char buf[32];
    struct tm tm;
    localtime_r(&tv->tv_sec, &tm);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &tm);
    ESP_LOGI(TAG, "Time synced: %s", buf);
}

/* ---- saved networks ------------------------------------------------- */

typedef struct {
    char ssid[33];
    char password[65];
} saved_net_t;

/* Most recently used first; adding to a full list drops the last one. */
static saved_net_t nets[BOARD_MAX_NETWORKS];
static int n_nets;
static SemaphoreHandle_t nets_lock;

static void nets_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (n_nets > 0) {
        nvs_set_blob(h, NVS_KEY_NETS, nets, n_nets * sizeof(nets[0]));
    } else {
        nvs_erase_key(h, NVS_KEY_NETS);
    }
    nvs_commit(h);
    nvs_close(h);
}

static int nets_find(const char *ssid)
{
    for (int i = 0; i < n_nets; i++) {
        if (!strcmp(nets[i].ssid, ssid)) {
            return i;
        }
    }
    return -1;
}

/* Put net first, replacing the entry with the same name if there is one. */
static void nets_put_first(const saved_net_t *net)
{
    saved_net_t copy = *net;
    int i = nets_find(net->ssid);
    if (i < 0) {
        i = n_nets < BOARD_MAX_NETWORKS ? n_nets++ : BOARD_MAX_NETWORKS - 1;
    }
    memmove(&nets[1], &nets[0], i * sizeof(nets[0]));
    nets[0] = copy;
}

static void nets_add(const char *ssid, const char *password)
{
    saved_net_t net = { 0 };
    strlcpy(net.ssid, ssid, sizeof(net.ssid));
    strlcpy(net.password, password, sizeof(net.password));
    xSemaphoreTake(nets_lock, portMAX_DELAY);
    nets_put_first(&net);
    nets_save();
    wifi_configured = true;
    xSemaphoreGive(nets_lock);
}

/* The network we just joined is tried first next time. */
static void nets_promote(const char *ssid)
{
    xSemaphoreTake(nets_lock, portMAX_DELAY);
    int i = nets_find(ssid);
    if (i > 0) {
        nets_put_first(&nets[i]);
        nets_save();
    }
    xSemaphoreGive(nets_lock);
}

/* Load the list, or start it from the single network older versions
 * left in the driver's own storage. */
static void nets_load(void)
{
    nvs_handle_t h;
    size_t len = sizeof(nets);
    bool found = false;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        found = nvs_get_blob(h, NVS_KEY_NETS, nets, &len) == ESP_OK;
        nvs_close(h);
    }
    if (found) {
        n_nets = len / sizeof(nets[0]);
    } else {
        wifi_config_t cfg = { 0 };
        esp_wifi_get_config(WIFI_IF_STA, &cfg);
        if (cfg.sta.ssid[0] != '\0') {
            ESP_LOGI(TAG, "Moving \"%s\" to the saved networks", (char *)cfg.sta.ssid);
            nets_add((char *)cfg.sta.ssid, (char *)cfg.sta.password);
            /* Clear it there, or forgetting every network would bring it back. */
            wifi_config_t empty = { 0 };
            esp_wifi_set_config(WIFI_IF_STA, &empty);
        }
    }
    if (n_nets == 0 && CONFIG_WIFI_SSID[0] != '\0') {
        strlcpy(nets[0].ssid, CONFIG_WIFI_SSID, sizeof(nets[0].ssid));
        strlcpy(nets[0].password, CONFIG_WIFI_PASSWORD, sizeof(nets[0].password));
        n_nets = 1;
    }
    for (int i = 0; i < n_nets; i++) {
        ESP_LOGI(TAG, "Saved network %d: \"%s\"", i + 1, nets[i].ssid);
    }
}

/* ---- connecting ----------------------------------------------------- */

#define NOTE_KICK       BIT0
#define NOTE_PROMOTE    BIT1
#define SCAN_MAX        20
/* Wait between attempts so a missing network doesn't hog the radio. */
#define RETRY_DELAY_MS  500

/* Skip a network for a while after this many failed attempts, so one
 * with a wrong password doesn't keep the board off the others. */
#define MAX_FAILS       3

/* Held for scans and connection attempts, which can't overlap. */
static SemaphoreHandle_t radio_lock;
static TaskHandle_t connect_task;
static volatile bool wifi_connecting;
static volatile bool attempt_failed;
static char wifi_ssid[33];

/* Failed attempts per network since the board was last online. */
static struct {
    char ssid[33];
    uint8_t n;
} fails[BOARD_MAX_NETWORKS];

static int fail_slot(const char *ssid, bool create)
{
    for (int i = 0; i < BOARD_MAX_NETWORKS; i++) {
        if (!strcmp(fails[i].ssid, ssid)) {
            return i;
        }
    }
    for (int i = 0; create && i < BOARD_MAX_NETWORKS; i++) {
        if (fails[i].ssid[0] == '\0') {
            strlcpy(fails[i].ssid, ssid, sizeof(fails[i].ssid));
            return i;
        }
    }
    return -1;
}

static bool failing(const char *ssid)
{
    int i = fail_slot(ssid, false);
    return i >= 0 && fails[i].n >= MAX_FAILS;
}

/* Ask the connect task to join a network, if we're not on one. */
static void kick(void)
{
    if (connect_task) {
        xTaskNotify(connect_task, NOTE_KICK, eSetBits);
    }
}

/* Choose the saved network to try: the strongest one in range or, when
 * none shows up in a scan (hidden networks don't), the next in turn. */
static bool pick_network(wifi_config_t *cfg)
{
    static int turn;
    saved_net_t list[BOARD_MAX_NETWORKS];
    xSemaphoreTake(nets_lock, portMAX_DELAY);
    int n = n_nets;
    memcpy(list, nets, sizeof(list));
    xSemaphoreGive(nets_lock);
    if (n == 0) {
        return false;
    }

    /* Leave out networks that keep failing, unless that's all of them. */
    bool skip[BOARD_MAX_NETWORKS];
    int usable = 0;
    for (int j = 0; j < n; j++) {
        skip[j] = failing(list[j].ssid);
        usable += !skip[j];
    }
    if (usable == 0) {
        memset(fails, 0, sizeof(fails));
        memset(skip, 0, sizeof(skip));
        usable = n;
    }

    int best = -1;
    if (usable > 1) {
        wifi_scan_config_t sc = { .show_hidden = false };
        if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
            uint16_t count = SCAN_MAX;
            wifi_ap_record_t *recs = calloc(SCAN_MAX, sizeof(*recs));
            if (recs && esp_wifi_scan_get_ap_records(&count, recs) == ESP_OK) {
                int best_rssi = INT_MIN;
                for (int i = 0; i < count; i++) {
                    for (int j = 0; j < n; j++) {
                        if (!skip[j] && !strcmp((char *)recs[i].ssid, list[j].ssid) &&
                            recs[i].rssi > best_rssi) {
                            best = j;
                            best_rssi = recs[i].rssi;
                        }
                    }
                }
            } else {
                esp_wifi_clear_ap_list();
            }
            free(recs);
        }
    }
    if (best < 0) {
        do {
            best = turn++ % n;
        } while (skip[best]);
    }
    memset(cfg, 0, sizeof(*cfg));
    strlcpy((char *)cfg->sta.ssid, list[best].ssid, sizeof(cfg->sta.ssid));
    strlcpy((char *)cfg->sta.password, list[best].password, sizeof(cfg->sta.password));
    return true;
}

static void connect_task_fn(void *arg)
{
    char last_tried[33] = "";
    TickType_t wait = portMAX_DELAY;
    while (1) {
        uint32_t notes = 0;
        if (!xTaskNotifyWait(0, UINT32_MAX, &notes, wait)) {
            notes = NOTE_KICK;
        }
        wait = portMAX_DELAY;
        if (notes & NOTE_PROMOTE) {
            nets_promote(wifi_ssid);
            memset(fails, 0, sizeof(fails));
        }
        if (attempt_failed) {
            attempt_failed = false;
            int i = fail_slot(last_tried, true);
            if (i >= 0 && fails[i].n < UINT8_MAX) {
                fails[i].n++;
            }
        }
        if (!(notes & NOTE_KICK)) {
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
        xSemaphoreTake(radio_lock, portMAX_DELAY);
        wifi_config_t cfg;
        if (!wifi_connected && !wifi_connecting && !wifi_paused && !wifi_hold &&
            pick_network(&cfg)) {
            if (strcmp(last_tried, (char *)cfg.sta.ssid)) {
                ESP_LOGI(TAG, "Joining \"%s\"", (char *)cfg.sta.ssid);
                strlcpy(last_tried, (char *)cfg.sta.ssid, sizeof(last_tried));
            }
            wifi_connecting = true;
            if (esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK || esp_wifi_connect() != ESP_OK) {
                wifi_connecting = false;
                wait = pdMS_TO_TICKS(5000);
            }
        }
        xSemaphoreGive(radio_lock);
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        kick();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        wifi_event_sta_connected_t *ev = data;
        memcpy(wifi_ssid, ev->ssid, ev->ssid_len);
        wifi_ssid[ev->ssid_len] = '\0';
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        /* Log only when the reason changes, retries come back to back. */
        static uint8_t last_reason;
        wifi_event_sta_disconnected_t *ev = data;
        if (ev->reason != last_reason) {
            ESP_LOGW(TAG, "Disconnected from \"%.*s\", reason %d", ev->ssid_len,
                     (char *)ev->ssid, ev->reason);
            last_reason = ev->reason;
        }
        if (wifi_connecting) {
            attempt_failed = true;
        }
        wifi_connected = false;
        wifi_connecting = false;
        wifi_ssid[0] = '\0';
        fail_count++;
        xEventGroupSetBits(wifi_events, BIT_FAILED);
        if (!wifi_paused && !wifi_hold) {
            kick();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        ESP_LOGI(TAG, "Got IP " IPSTR " on \"%s\"", IP2STR(&ev->ip_info.ip), wifi_ssid);
        snprintf(wifi_ip, sizeof(wifi_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        wifi_connected = true;
        wifi_connecting = false;
        fail_count = 0;
        xEventGroupSetBits(wifi_events, BIT_GOT_IP);
        esp_netif_sntp_start();
        if (connect_task) {
            xTaskNotify(connect_task, NOTE_PROMOTE, eSetBits);
        }
    }
}

void board_wifi_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    /* Same suffix as the setup hotspot's name. */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(hostname, sizeof(hostname), "sl-departures-%02x%02x", mac[4], mac[5]);
    esp_netif_set_hostname(sta, hostname);
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(hostname);
        mdns_instance_name_set("SL Departures");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }

    wifi_events = xEventGroupCreate();
    nets_lock = xSemaphoreCreateMutex();
    radio_lock = xSemaphoreCreateMutex();
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    /* SNTP is started from the event handler once we have an IP. */
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    sntp_cfg.start = false;
    sntp_cfg.sync_cb = time_synced_cb;
    ESP_ERROR_CHECK(esp_netif_sntp_init(&sntp_cfg));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    nets_load();
    /* The list is ours; the driver needn't save each network it tries. */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    wifi_configured = n_nets > 0;
    if (!wifi_configured) {
        ESP_LOGW(TAG, "No Wi-Fi configured, waiting for Improv provisioning");
    }
    xTaskCreate(connect_task_fn, "wifi_connect", 3072, NULL, 4, &connect_task);
    ESP_ERROR_CHECK(esp_wifi_start());
}

bool board_wifi_connected(void)
{
    return wifi_connected;
}

const char *board_hostname(void)
{
    return hostname;
}

const char *board_wifi_ip(void)
{
    return wifi_connected ? wifi_ip : "";
}

const char *board_wifi_ssid(void)
{
    return wifi_connected ? wifi_ssid : "";
}

bool board_wifi_configured(void)
{
    return wifi_configured;
}

int board_wifi_saved(char ssids[][33], int max)
{
    xSemaphoreTake(nets_lock, portMAX_DELAY);
    int n = n_nets < max ? n_nets : max;
    for (int i = 0; i < n; i++) {
        strlcpy(ssids[i], nets[i].ssid, 33);
    }
    xSemaphoreGive(nets_lock);
    return n;
}

bool board_wifi_forget(const char *ssid)
{
    xSemaphoreTake(nets_lock, portMAX_DELAY);
    int i = nets_find(ssid);
    if (i >= 0) {
        memmove(&nets[i], &nets[i + 1], (n_nets - i - 1) * sizeof(nets[0]));
        n_nets--;
        nets_save();
        wifi_configured = n_nets > 0;
    }
    xSemaphoreGive(nets_lock);
    if (i < 0) {
        return false;
    }
    /* A network in use stays connected until it drops. */
    ESP_LOGI(TAG, "Forgot \"%s\"", ssid);
    return true;
}

void board_wifi_add(const char *ssid, const char *password)
{
    ESP_LOGI(TAG, "Saved \"%s\" for later", ssid);
    nets_add(ssid, password);
    kick();
}

bool board_wifi_provision(const char *ssid, const char *password, int timeout_ms)
{
    wifi_config_t cfg = { 0 };
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password));

    xSemaphoreTake(radio_lock, portMAX_DELAY);
    wifi_paused = true;
    esp_wifi_disconnect();
    bool ok = false;
    if (esp_wifi_set_config(WIFI_IF_STA, &cfg) == ESP_OK) {
        fail_count = 0;
        xEventGroupClearBits(wifi_events, BIT_GOT_IP | BIT_FAILED);
        esp_wifi_connect();

        /* Retry a few times; a fresh association often fails once. */
        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
        while (xTaskGetTickCount() < deadline) {
            EventBits_t bits = xEventGroupWaitBits(wifi_events, BIT_GOT_IP | BIT_FAILED, pdTRUE,
                                                   pdFALSE, deadline - xTaskGetTickCount());
            if (bits & BIT_GOT_IP) {
                ok = true;
                break;
            }
            if ((bits & BIT_FAILED) && fail_count >= PROVISION_TRIES) {
                break;
            }
            if (bits & BIT_FAILED) {
                esp_wifi_connect();
            }
        }
    }
    if (ok) {
        nets_add(ssid, password);
    } else {
        ESP_LOGW(TAG, "Could not connect to %s, not saving it", ssid);
        esp_wifi_disconnect();
    }
    wifi_paused = false;
    xSemaphoreGive(radio_lock);
    if (!ok) {
        kick();
    }
    return ok;
}

int board_wifi_scan(board_ap_t *aps, int max)
{
    xSemaphoreTake(radio_lock, portMAX_DELAY);
    /* Scanning fails while the driver is busy (re)connecting. */
    bool was_connected = wifi_connected;
    wifi_paused = true;
    if (!was_connected) {
        esp_wifi_disconnect();
    }
    wifi_scan_config_t sc = { .show_hidden = false };
    int n = 0;
    if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
        uint16_t count = max;
        wifi_ap_record_t *recs = calloc(max, sizeof(*recs));
        if (recs && esp_wifi_scan_get_ap_records(&count, recs) == ESP_OK) {
            for (int i = 0; i < count; i++) {
                if (recs[i].ssid[0] == '\0') {
                    continue;
                }
                strlcpy(aps[n].ssid, (char *)recs[i].ssid, sizeof(aps[n].ssid));
                aps[n].rssi = recs[i].rssi;
                aps[n].secure = recs[i].authmode != WIFI_AUTH_OPEN;
                n++;
            }
        }
        free(recs);
    } else {
        esp_wifi_clear_ap_list();
    }
    wifi_paused = false;
    xSemaphoreGive(radio_lock);
    kick();
    return n;
}

void board_wifi_hold(bool hold)
{
    wifi_hold = hold;
    if (hold && !wifi_connected) {
        esp_wifi_disconnect();
    } else if (!hold) {
        kick();
    }
}

void board_ap_start(const char *ssid, const char *password)
{
    wifi_config_t ap = {
        .ap = {
            .max_connection = 2,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strlcpy((char *)ap.ap.ssid, ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(ssid);
    strlcpy((char *)ap.ap.password, password, sizeof(ap.ap.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_LOGI(TAG, "Setup hotspot %s started", ssid);
}

void board_ap_stop(void)
{
    esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(TAG, "Setup hotspot stopped");
}

bool board_time_valid(void)
{
    return time(NULL) > 1700000000;
}
