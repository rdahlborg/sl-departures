/* Board support for Waveshare ESP32-C6-LCD-1.47: display, Wi-Fi, SNTP. */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
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

static volatile bool wifi_connected;
static volatile bool wifi_configured;
/* Set while scanning or provisioning so the event handler doesn't fight
 * over the radio by reconnecting. */
static volatile bool wifi_paused;
static volatile bool wifi_hold;
static volatile int fail_count;
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

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (wifi_configured) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_connected = false;
        fail_count++;
        xEventGroupSetBits(wifi_events, BIT_FAILED);
        if (wifi_configured && !wifi_paused && !wifi_hold) {
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        ESP_LOGI(TAG, "Got IP " IPSTR, IP2STR(&ev->ip_info.ip));
        wifi_connected = true;
        fail_count = 0;
        xEventGroupSetBits(wifi_events, BIT_GOT_IP);
        esp_netif_sntp_start();
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
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_events = xEventGroupCreate();
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
    /* The Wi-Fi driver keeps the last set config in NVS. */
    wifi_config_t cfg = { 0 };
    esp_wifi_get_config(WIFI_IF_STA, &cfg);
    if (cfg.sta.ssid[0] == '\0' && CONFIG_WIFI_SSID[0] != '\0') {
        strlcpy((char *)cfg.sta.ssid, CONFIG_WIFI_SSID, sizeof(cfg.sta.ssid));
        strlcpy((char *)cfg.sta.password, CONFIG_WIFI_PASSWORD, sizeof(cfg.sta.password));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    }
    wifi_configured = cfg.sta.ssid[0] != '\0';
    if (!wifi_configured) {
        ESP_LOGW(TAG, "No Wi-Fi configured, waiting for Improv provisioning");
    }
    ESP_ERROR_CHECK(esp_wifi_start());
}

bool board_wifi_connected(void)
{
    return wifi_connected;
}

bool board_wifi_configured(void)
{
    return wifi_configured;
}

bool board_wifi_provision(const char *ssid, const char *password, int timeout_ms)
{
    wifi_config_t cfg = { 0 };
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password));

    /* The driver saves whatever it is given, so keep the old network to
     * put back if the new one doesn't work. */
    wifi_config_t previous = { 0 };
    esp_wifi_get_config(WIFI_IF_STA, &previous);
    bool was_configured = wifi_configured;

    wifi_paused = true;
    esp_wifi_disconnect();
    if (esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK) {
        wifi_paused = false;
        return false;
    }
    wifi_configured = true;
    fail_count = 0;
    xEventGroupClearBits(wifi_events, BIT_GOT_IP | BIT_FAILED);
    esp_wifi_connect();

    /* Retry a few times; a fresh association often fails once. */
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    bool ok = false;
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
    wifi_paused = false;
    if (!ok) {
        ESP_LOGW(TAG, "Could not connect to %s, keeping the previous network", ssid);
        esp_wifi_disconnect();
        esp_wifi_set_config(WIFI_IF_STA, &previous);
        wifi_configured = was_configured;
        if (wifi_configured && !wifi_hold) {
            esp_wifi_connect();
        }
    }
    return ok;
}

int board_wifi_scan(board_ap_t *aps, int max)
{
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
    if (!was_connected && wifi_configured && !wifi_hold) {
        esp_wifi_connect();
    }
    return n;
}

void board_wifi_hold(bool hold)
{
    wifi_hold = hold;
    if (hold && !wifi_connected) {
        esp_wifi_disconnect();
    } else if (!hold && !wifi_connected && wifi_configured) {
        esp_wifi_connect();
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
