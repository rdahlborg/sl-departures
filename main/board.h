#pragma once
#include <stdbool.h>
#include "lvgl.h"

#define LCD_H_RES 320
#define LCD_V_RES 172

/* Initialise the ST7789 panel, LVGL and the backlight. Returns the LVGL display. */
lv_display_t *board_display_init(void);

/* Connect to Wi-Fi and start SNTP. Non-blocking. Joins the strongest of
 * the saved networks, falling back to CONFIG_WIFI_SSID for developer builds. */
void board_wifi_init(void);

bool board_wifi_connected(void);
bool board_wifi_configured(void);

/* Name of the network we're on; empty when offline. */
const char *board_wifi_ssid(void);

/* Address on the home network, e.g. "192.168.1.167"; empty when offline. */
const char *board_wifi_ip(void);

/* mDNS/DHCP name, e.g. "sl-departures-84d5" (reachable as <name>.local).
 * Known from boot, unlike the IP address. */
const char *board_hostname(void);

/* Try new credentials, and save them to the list of networks if they work.
 * Blocks until connected (true) or failed/timed out (false). Used by Improv
 * provisioning and phone setup. */
bool board_wifi_provision(const char *ssid, const char *password, int timeout_ms);

#define BOARD_MAX_NETWORKS 5

/* Names of the saved networks, most recently used first. Returns how many. */
int board_wifi_saved(char ssids[][33], int max);

/* Remove a saved network. One in use stays connected until it drops.
 * Returns false if it wasn't saved. */
bool board_wifi_forget(const char *ssid);

/* Save a network without trying it, for use when the current one is lost.
 * Replaces the password if it was saved already. */
void board_wifi_add(const char *ssid, const char *password);

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
} board_ap_t;

/* Scan for networks. Returns the number written to aps. */
int board_wifi_scan(board_ap_t *aps, int max);

/* Stop background reconnect attempts, which would make the radio hop
 * channels and drop phones connected to the setup hotspot. */
void board_wifi_hold(bool hold);

/* Setup hotspot (AP+STA). The STA side keeps working alongside it. */
void board_ap_start(const char *ssid, const char *password);
void board_ap_stop(void);
/* "WIFI_AP_DEF" netif key, for the DNS server. */
#define BOARD_AP_NETIF_KEY "WIFI_AP_DEF"
bool board_time_valid(void);
