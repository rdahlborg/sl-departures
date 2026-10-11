#pragma once
#include <stdint.h>

/* Improv Wi-Fi over serial (https://www.improv-wifi.com/serial/), which lets
 * the ESP Web Tools installer ask for Wi-Fi credentials right after flashing.
 * Called from serial.c with a complete packet whose checksum is verified. */
void improv_handle_packet(uint8_t type, const uint8_t *data, uint8_t len);
