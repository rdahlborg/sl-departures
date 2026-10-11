#pragma once
#include <stdbool.h>
#include "lvgl.h"

#define LCD_H_RES 320
#define LCD_V_RES 172

/* Initialise the ST7789 panel, LVGL and the backlight. Returns the LVGL display. */
lv_display_t *board_display_init(void);

/* Connect to Wi-Fi and start SNTP. Non-blocking. Uses the credentials
 * saved in NVS, falling back to CONFIG_WIFI_SSID for developer builds. */
void board_wifi_init(void);

bool board_wifi_connected(void);
bool board_wifi_configured(void);

/* Save new credentials and try them. Blocks until connected (true) or
 * failed/timed out (false). Used by Improv provisioning. */
bool board_wifi_provision(const char *ssid, const char *password, int timeout_ms);

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
} board_ap_t;

/* Scan for networks. Returns the number written to aps. */
int board_wifi_scan(board_ap_t *aps, int max);
bool board_time_valid(void);
