/* Improv Wi-Fi serial protocol, device side.
 *
 * Packet: "IMPROV" | version | type | length | data | checksum
 * where checksum is the low byte of the sum of all preceding bytes.
 */
#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_log.h"
#include "board.h"
#include "improv.h"

#define IMPROV_VERSION      1

#define TYPE_STATE          0x01
#define TYPE_ERROR          0x02
#define TYPE_RPC            0x03
#define TYPE_RPC_RESULT     0x04

#define STATE_READY         0x02
#define STATE_PROVISIONING  0x03
#define STATE_PROVISIONED   0x04

#define ERR_NONE            0x00
#define ERR_INVALID_RPC     0x01
#define ERR_UNKNOWN_RPC     0x02
#define ERR_UNABLE_CONNECT  0x03

#define CMD_WIFI_SETTINGS   0x01
#define CMD_CURRENT_STATE   0x02
#define CMD_DEVICE_INFO     0x03
#define CMD_SCAN            0x04

#define PROVISION_TIMEOUT_MS 20000
#define MAX_APS             20

static const char *TAG = "improv";

static void send_packet(uint8_t type, const uint8_t *data, uint8_t len)
{
    uint8_t buf[6 + 3 + 255 + 2];
    memcpy(buf, "IMPROV", 6);
    buf[6] = IMPROV_VERSION;
    buf[7] = type;
    buf[8] = len;
    memcpy(buf + 9, data, len);
    uint8_t sum = 0;
    for (int i = 0; i < 9 + len; i++) {
        sum += buf[i];
    }
    buf[9 + len] = sum;
    buf[10 + len] = '\n';
    /* One write so log output from other tasks can't land mid-packet. */
    fwrite(buf, 1, 11 + len, stdout);
    fflush(stdout);
}

static void send_byte(uint8_t type, uint8_t value)
{
    send_packet(type, &value, 1);
}

/* RPC result: command | data length | (string length | string)* */
static void send_result(uint8_t cmd, const char *const *strs, int n)
{
    uint8_t data[255];
    int pos = 2;
    for (int i = 0; i < n; i++) {
        size_t l = strlen(strs[i]);
        if (pos + 1 + l > sizeof(data)) {
            break;
        }
        data[pos++] = l;
        memcpy(data + pos, strs[i], l);
        pos += l;
    }
    data[0] = cmd;
    data[1] = pos - 2;
    send_packet(TYPE_RPC_RESULT, data, pos);
}

/* The settings page on the home network. ESP Web Tools shows it as a
 * "Visit device" button, where line and station are chosen. */
static void send_url_result(uint8_t cmd)
{
    char url[32];
    snprintf(url, sizeof(url), "http://%s/", board_wifi_ip());
    const char *urls[] = { url };
    send_result(cmd, urls, board_wifi_ip()[0] ? 1 : 0);
}

static void send_state(void)
{
    if (board_wifi_connected()) {
        send_byte(TYPE_STATE, STATE_PROVISIONED);
        send_url_result(CMD_CURRENT_STATE);
    } else {
        send_byte(TYPE_STATE, STATE_READY);
    }
}

static void wifi_settings(const uint8_t *p, uint8_t len)
{
    /* ssid length | ssid | password length | password */
    char ssid[33], pass[65];
    if (len < 2 || p[0] > 32 || 1 + p[0] + 1 > len) {
        send_byte(TYPE_ERROR, ERR_INVALID_RPC);
        return;
    }
    uint8_t sl = p[0], pl = p[1 + sl];
    if (pl > 64 || 2 + sl + pl > len) {
        send_byte(TYPE_ERROR, ERR_INVALID_RPC);
        return;
    }
    memcpy(ssid, p + 1, sl);
    ssid[sl] = '\0';
    memcpy(pass, p + 2 + sl, pl);
    pass[pl] = '\0';

    ESP_LOGI(TAG, "Provisioning Wi-Fi \"%s\"", ssid);
    send_byte(TYPE_ERROR, ERR_NONE);
    send_byte(TYPE_STATE, STATE_PROVISIONING);
    if (board_wifi_provision(ssid, pass, PROVISION_TIMEOUT_MS)) {
        send_byte(TYPE_STATE, STATE_PROVISIONED);
        send_url_result(CMD_WIFI_SETTINGS);
    } else {
        send_byte(TYPE_ERROR, ERR_UNABLE_CONNECT);
        send_byte(TYPE_STATE, STATE_READY);
    }
}

static void device_info(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const char *info[] = { "SL Departures", app->version, "ESP32-C6", "SL Departures" };
    send_result(CMD_DEVICE_INFO, info, 4);
}

static void scan(void)
{
    static board_ap_t aps[MAX_APS];
    int n = board_wifi_scan(aps, MAX_APS);
    for (int i = 0; i < n; i++) {
        char rssi[8];
        snprintf(rssi, sizeof(rssi), "%d", aps[i].rssi);
        const char *row[] = { aps[i].ssid, rssi, aps[i].secure ? "YES" : "NO" };
        send_result(CMD_SCAN, row, 3);
    }
    send_result(CMD_SCAN, NULL, 0);   /* end of list */
}

void improv_handle_packet(uint8_t type, const uint8_t *data, uint8_t len)
{
    if (type != TYPE_RPC) {
        return;
    }
    /* command | payload length | payload */
    if (len < 2 || 2 + data[1] > len) {
        send_byte(TYPE_ERROR, ERR_INVALID_RPC);
        return;
    }
    const uint8_t *payload = data + 2;
    switch (data[0]) {
    case CMD_WIFI_SETTINGS: wifi_settings(payload, data[1]); break;
    case CMD_CURRENT_STATE: send_state(); break;
    case CMD_DEVICE_INFO:   device_info(); break;
    case CMD_SCAN:          scan(); break;
    default:                send_byte(TYPE_ERROR, ERR_UNKNOWN_RPC); break;
    }
}
