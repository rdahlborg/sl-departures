/* Reads the USB serial port and dispatches Improv packets and debug keys. */
#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "board.h"
#include "controls.h"
#include "improv.h"
#include "screenshot.h"
#include "serial.h"

static const char *TAG = "serial";

static const char header[] = "IMPROV";
static volatile bool ui_ready;

void serial_ui_ready(void)
{
    ui_ready = true;
}

/* Single-character debug commands. */
static void command(uint8_t c)
{
    if (!ui_ready) {
        return;
    }
    if (c == 'S') {
        screenshot_dump();
    } else if (c == 'W') {
        static board_ap_t aps[20];
        int n = board_wifi_scan(aps, 20);
        for (int i = 0; i < n; i++) {
            ESP_LOGI(TAG, "%4d dBm %s \"%s\"", aps[i].rssi, aps[i].secure ? "secure" : "open",
                     aps[i].ssid);
            ESP_LOG_BUFFER_HEX(TAG, aps[i].ssid, strlen(aps[i].ssid));
        }
        ESP_LOGI(TAG, "%d networks", n);
    } else {
        controls_simulate(c);
    }
}

static void task(void *arg)
{
    /* "IMPROV" | version | type | length | data | checksum */
    static uint8_t pkt[6 + 3 + 255 + 1];
    int pos = 0;
    uint8_t c;

    while (1) {
        if (usb_serial_jtag_read_bytes(&c, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (pos < 6) {
            if (c == (uint8_t)header[pos]) {
                pkt[pos++] = c;
            } else {
                pos = c == (uint8_t)header[0];
                if (pos) {
                    pkt[0] = c;
                } else {
                    command(c);
                }
            }
            continue;
        }
        pkt[pos++] = c;
        if (pos < 9 || pos < 9 + pkt[8] + 1) {
            continue;
        }
        /* Complete packet: verify checksum, then dispatch. */
        uint8_t sum = 0;
        for (int i = 0; i < pos - 1; i++) {
            sum += pkt[i];
        }
        if (sum == pkt[pos - 1]) {
            improv_handle_packet(pkt[7], pkt + 9, pkt[8]);
        } else {
            ESP_LOGW(TAG, "Bad Improv checksum");
        }
        pos = 0;
    }
}

void serial_init(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.tx_buffer_size = 4096;
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "USB serial driver unavailable");
        return;
    }
    /* Route the console through the driver so output and input share it. */
    usb_serial_jtag_vfs_use_driver();
    /* Screenshots render on this task (lv_refr_now), and Improv provisioning
     * blocks here, so give it room. */
    xTaskCreate(task, "serial", 12 * 1024, NULL, 2, NULL);
}
