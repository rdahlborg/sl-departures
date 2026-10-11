/* Wi-Fi setup from a phone, see wifi_setup.h.
 *
 * The slow parts (scanning, starting the hotspot, trying credentials) run on
 * a small task so the button handler and web server never block on them.
 * The setup page itself is served by web.c.
 * The board's screen shows the result as well as the phone, because the
 * phone usually loses the hotspot while the board switches channel to join
 * the home network.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "dns_server.h"
#include "board.h"
#include "wifi_setup.h"

#define CMD_START       BIT0
#define CMD_STOP        BIT1
#define CMD_CONNECT     BIT2

#define MAX_APS         20
#define CONNECT_TIMEOUT_MS 20000
/* Keep the success message up, and the hotspot alive for the phone's
 * status poll, before closing. */
#define DONE_LINGER_MS  6000
/* Close the hotspot if setup was opened from the menu and nobody used it. */
#define IDLE_TIMEOUT_MS (10 * 60 * 1000)

static const char *TAG = "wifi_setup";

static SemaphoreHandle_t mutex;
static TaskHandle_t task_handle;
static setup_status_t status;
static int clients;
static char pending_ssid[33];
static char pending_pass[65];
static board_ap_t aps[MAX_APS];
static int n_aps;
static dns_server_handle_t dns;
static bool had_wifi;           /* already online when setup started */
static TickType_t started;

static void set_state(setup_state_t s)
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    status.state = s;
    xSemaphoreGive(mutex);
}

/* All DNS queries resolve to the hotspot, so phones find the setup page
 * (served by web.c) whatever address they probe. */
static void start_portal(void)
{
    /* DHCP option 114 tells newer phones the portal address directly. */
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey(BOARD_AP_NETIF_KEY);
    static char uri[] = WIFI_SETUP_PORTAL_URL;
    esp_netif_dhcps_stop(ap);
    esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, uri, strlen(uri));
    esp_netif_dhcps_start(ap);

    dns_server_config_t dns_cfg = DNS_SERVER_CONFIG_SINGLE("*", BOARD_AP_NETIF_KEY);
    dns = start_dns_server(&dns_cfg);
}

static void stop_portal(void)
{
    if (dns) {
        stop_dns_server(dns);
        dns = NULL;
    }
}

/* ---- setup task ----------------------------------------------------- */

static int by_rssi(const void *a, const void *b)
{
    return ((const board_ap_t *)b)->rssi - ((const board_ap_t *)a)->rssi;
}

static void scan_networks(void)
{
    board_ap_t found[MAX_APS];
    int n = board_wifi_scan(found, MAX_APS);
    qsort(found, n, sizeof(found[0]), by_rssi);
    /* The same network often shows up once per access point. */
    n_aps = 0;
    for (int i = 0; i < n; i++) {
        bool dup = false;
        for (int j = 0; j < n_aps && !dup; j++) {
            dup = !strcmp(aps[j].ssid, found[i].ssid);
        }
        if (!dup) {
            aps[n_aps++] = found[i];
        }
    }
}

static void begin(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    /* No 0/O/1/l/i, which are easy to mix up when typed by hand. */
    static const char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
    char pw[9];
    for (int i = 0; i < 8; i++) {
        pw[i] = alphabet[esp_random() % (sizeof(alphabet) - 1)];
    }
    pw[8] = '\0';

    xSemaphoreTake(mutex, portMAX_DELAY);
    memset(&status, 0, sizeof(status));
    status.state = SETUP_STARTING;
    snprintf(status.ap_ssid, sizeof(status.ap_ssid), "SL-Departures-%02X%02X", mac[4], mac[5]);
    strlcpy(status.ap_password, pw, sizeof(status.ap_password));
    xSemaphoreGive(mutex);

    had_wifi = board_wifi_connected();
    started = xTaskGetTickCount();
    board_wifi_hold(true);
    /* Scan before the hotspot is up; scanning later would drop the phone. */
    scan_networks();
    clients = 0;
    board_ap_start(status.ap_ssid, status.ap_password);
    start_portal();
    set_state(SETUP_WAITING);
}

static void end(void)
{
    stop_portal();
    board_ap_stop();
    board_wifi_hold(false);
    set_state(SETUP_OFF);
}

static void try_connect(void)
{
    char ssid[33], pass[65];
    xSemaphoreTake(mutex, portMAX_DELAY);
    strlcpy(ssid, pending_ssid, sizeof(ssid));
    strlcpy(pass, pending_pass, sizeof(pass));
    strlcpy(status.target, ssid, sizeof(status.target));
    status.last_failed = false;
    status.state = SETUP_CONNECTING;
    xSemaphoreGive(mutex);

    if (board_wifi_provision(ssid, pass, CONNECT_TIMEOUT_MS)) {
        ESP_LOGI(TAG, "Connected to %s, closing setup", ssid);
        set_state(SETUP_DONE);
        vTaskDelay(pdMS_TO_TICKS(DONE_LINGER_MS));
        end();
    } else {
        xSemaphoreTake(mutex, portMAX_DELAY);
        status.last_failed = true;
        status.state = clients > 0 ? SETUP_PHONE : SETUP_WAITING;
        xSemaphoreGive(mutex);
    }
}

static void setup_task(void *arg)
{
    while (1) {
        uint32_t cmd = 0;
        xTaskNotifyWait(0, UINT32_MAX, &cmd, pdMS_TO_TICKS(1000));
        setup_state_t s = status.state;
        bool idle = s == SETUP_WAITING || s == SETUP_PHONE;
        /* Wi-Fi was set up another way meanwhile (USB installer). */
        if (idle && !had_wifi && board_wifi_connected()) {
            cmd |= CMD_STOP;
        }
        /* Opened from the menu and left alone. */
        if (idle && had_wifi && clients == 0 &&
            xTaskGetTickCount() - started > pdMS_TO_TICKS(IDLE_TIMEOUT_MS)) {
            cmd |= CMD_STOP;
        }
        if ((cmd & CMD_START) && s == SETUP_STARTING) {
            begin();
            s = status.state;
        }
        if ((cmd & CMD_CONNECT) && s != SETUP_OFF && s != SETUP_STARTING) {
            try_connect();
            s = status.state;
        }
        if ((cmd & CMD_STOP) && s != SETUP_OFF) {
            end();
        }
    }
}

static void ap_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        clients++;
        ESP_LOGI(TAG, "Phone joined the hotspot (%d connected)", clients);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED && clients > 0) {
        clients--;
        ESP_LOGI(TAG, "Phone left the hotspot (%d connected)", clients);
    }
    if (status.state == SETUP_WAITING || status.state == SETUP_PHONE) {
        status.state = clients > 0 ? SETUP_PHONE : SETUP_WAITING;
    }
    xSemaphoreGive(mutex);
}

static void ensure_init(void)
{
    if (mutex) {
        return;
    }
    mutex = xSemaphoreCreateMutex();
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, ap_event, NULL);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, ap_event, NULL);
    xTaskCreate(setup_task, "wifi_setup", 4096, NULL, 3, &task_handle);
}

void wifi_setup_start(void)
{
    ensure_init();
    /* Mark it active right away so the UI and buttons switch over at once. */
    xSemaphoreTake(mutex, portMAX_DELAY);
    bool idle = status.state == SETUP_OFF;
    if (idle) {
        status.state = SETUP_STARTING;
    }
    xSemaphoreGive(mutex);
    if (idle) {
        xTaskNotify(task_handle, CMD_START, eSetBits);
    }
}

void wifi_setup_stop(void)
{
    if (task_handle) {
        xTaskNotify(task_handle, CMD_STOP, eSetBits);
    }
}

bool wifi_setup_active(void)
{
    return mutex && status.state != SETUP_OFF;
}

void wifi_setup_get_status(setup_status_t *out)
{
    if (!mutex) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    *out = status;
    xSemaphoreGive(mutex);
}

void wifi_setup_submit(const char *ssid, const char *password)
{
    if (!wifi_setup_active()) {
        return;
    }
    ESP_LOGI(TAG, "Got network \"%s\" from the phone", ssid);
    xSemaphoreTake(mutex, portMAX_DELAY);
    strlcpy(pending_ssid, ssid, sizeof(pending_ssid));
    strlcpy(pending_pass, password, sizeof(pending_pass));
    xSemaphoreGive(mutex);
    xTaskNotify(task_handle, CMD_CONNECT, eSetBits);
}

int wifi_setup_networks(const board_ap_t **out)
{
    *out = aps;
    return n_aps;
}
