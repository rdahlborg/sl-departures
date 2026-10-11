#pragma once
#include <stdbool.h>
#include <time.h>

/* Firmware updates over Wi-Fi from the project's GitHub releases.
 * A new version is checked each night and from the "Om / Uppdatera…"
 * screen. A freshly installed version must fetch departures within
 * a few minutes, otherwise the board rolls back to the previous one. */

typedef enum {
    OTA_IDLE,
    OTA_CHECKING,
    OTA_UP_TO_DATE,
    OTA_DOWNLOADING,
    OTA_DONE,           /* restarting into the new version */
    OTA_FAILED,
} ota_state_t;

typedef struct {
    ota_state_t state;
    char latest[24];    /* newest release seen, e.g. "v1.1.0" */
    int progress;       /* percent, while downloading */
    time_t checked;     /* last successful check */
} ota_status_t;

/* Call early: starts the health check after an update. */
void ota_init(void);

/* Start the nightly maintenance task (station list refresh, update check). */
void ota_start(void);

/* Check now and install a newer version if there is one. Non-blocking. */
void ota_check_now(void);

void ota_get_status(ota_status_t *out);

/* Running firmware version, e.g. "v1.0.0". */
const char *ota_version(void);
