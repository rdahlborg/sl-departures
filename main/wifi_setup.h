#pragma once
#include <stdbool.h>
#include "board.h"

/* Wi-Fi setup from a phone: the board starts a hotspot, shows a QR code to
 * join it, and serves a captive portal page for choosing the home network. */

typedef enum {
    SETUP_OFF,
    SETUP_STARTING,     /* scanning for networks, starting the hotspot */
    SETUP_WAITING,      /* hotspot up, no phone yet */
    SETUP_PHONE,        /* a phone has joined the hotspot */
    SETUP_CONNECTING,   /* trying the credentials from the page */
    SETUP_DONE,         /* connected; closes by itself shortly */
} setup_state_t;

typedef struct {
    setup_state_t state;
    char ap_ssid[33];
    char ap_password[16];
    char target[33];        /* network being tried or last tried */
    bool last_failed;       /* the last attempt failed */
} setup_status_t;

/* Address of the setup page on the hotspot. */
#define WIFI_SETUP_PORTAL_URL "http://192.168.4.1"

/* Non-blocking; safe to call from the button handler. */
void wifi_setup_start(void);
void wifi_setup_stop(void);
bool wifi_setup_active(void);
void wifi_setup_get_status(setup_status_t *out);

/* Try these credentials (from the setup page). Non-blocking. */
void wifi_setup_submit(const char *ssid, const char *password);

/* Networks found when setup started, strongest first. */
int wifi_setup_networks(const board_ap_t **out);
