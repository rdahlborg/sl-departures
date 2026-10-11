/* Firmware updates from GitHub releases, see ota.h.
 *
 * The latest release is read from the GitHub API; its "sl-departures.bin"
 * asset is the app image. It is written to the other OTA slot while the
 * board keeps running, then the board restarts into it. The bootloader
 * rolls back if the new image crashes before it is marked valid, and the
 * health check below rolls back if it never manages to fetch departures.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "board.h"
#include "metro_data.h"
#include "sl_api.h"
#include "ota.h"

#define API_URL         "https://api.github.com/repos/" CONFIG_OTA_GITHUB_REPO "/releases/latest"
#define ASSET_NAME      "sl-departures.bin"
#define RELEASE_BUF     (24 * 1024)
#define HEALTH_LIMIT_US (10LL * 60 * 1000000)
#define NIGHT_HOUR      3

static const char *TAG = "ota";

static SemaphoreHandle_t mutex;
static TaskHandle_t task_handle;
static ota_status_t status;
static esp_timer_handle_t health_timer;

static void set_state(ota_state_t s)
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    status.state = s;
    xSemaphoreGive(mutex);
}

const char *ota_version(void)
{
    return esp_app_get_description()->version;
}

/* "v1.2.3" or "v1.2.3-4-gabcdef" -> 1002003; -1 if not a release version. */
static long version_number(const char *v)
{
    int a, b, c;
    if (sscanf(v, "v%d.%d.%d", &a, &b, &c) != 3) {
        return -1;
    }
    return a * 1000000L + b * 1000L + c;
}

/* ---- health check after an update ----------------------------------- */

static void health_check(void *arg)
{
    if (sl_has_succeeded()) {
        esp_ota_mark_app_valid_cancel_rollback();
        esp_timer_stop(health_timer);
        ESP_LOGI(TAG, "%s is working, keeping it", ota_version());
    } else if (esp_timer_get_time() > HEALTH_LIMIT_US) {
        ESP_LOGE(TAG, "%s never fetched departures, rolling back", ota_version());
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
}

void ota_init(void)
{
    mutex = xSemaphoreCreateMutex();
    esp_ota_img_states_t st;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(running, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "First boot of %s, checking that it works", ota_version());
        const esp_timer_create_args_t args = { .callback = health_check, .name = "ota_health" };
        esp_timer_create(&args, &health_timer);
        esp_timer_start_periodic(health_timer, 5 * 1000000);
    }
}

/* ---- checking ------------------------------------------------------- */

typedef struct {
    char *buf;
    int len;
} buf_t;

static esp_err_t buf_event(esp_http_client_event_t *evt)
{
    buf_t *b = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && b->len + evt->data_len < RELEASE_BUF) {
        memcpy(b->buf + b->len, evt->data, evt->data_len);
        b->len += evt->data_len;
    }
    return ESP_OK;
}

/* Fills tag and url from the latest release. */
static bool latest_release(char *tag, size_t tag_len, char *url, size_t url_len)
{
    buf_t b = { .buf = malloc(RELEASE_BUF) };
    if (!b.buf) {
        return false;
    }
    esp_http_client_config_t cfg = {
        .url = API_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = buf_event,
        .user_data = &b,
        .timeout_ms = 15000,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "User-Agent", "sl-departures");
    esp_http_client_set_header(client, "Accept", "application/vnd.github+json");
    esp_err_t err = esp_http_client_perform(client);
    int code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    bool ok = false;
    if (err == ESP_OK && code == 200) {
        b.buf[b.len] = '\0';
        cJSON *root = cJSON_Parse(b.buf);
        const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(root, "tag_name"));
        cJSON *asset;
        cJSON_ArrayForEach(asset, cJSON_GetObjectItem(root, "assets")) {
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(asset, "name"));
            const char *u = cJSON_GetStringValue(cJSON_GetObjectItem(asset, "browser_download_url"));
            if (t && name && u && !strcmp(name, ASSET_NAME)) {
                strlcpy(tag, t, tag_len);
                strlcpy(url, u, url_len);
                ok = true;
                break;
            }
        }
        cJSON_Delete(root);
    } else {
        ESP_LOGW(TAG, "Release check failed: %s, HTTP %d (free heap %u, largest block %u)",
                 esp_err_to_name(err), code, (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    }
    free(b.buf);
    return ok;
}

/* Called with departure polling paused. */
static bool install(const char *url)
{
    set_state(OTA_DOWNLOADING);

    esp_http_client_config_t http = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
        /* GitHub redirects to a long signed URL. */
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
    };
    esp_https_ota_config_t cfg = { .http_config = &http };
    esp_https_ota_handle_t ota = NULL;
    esp_err_t err = esp_https_ota_begin(&cfg, &ota);
    while (err == ESP_OK) {
        err = esp_https_ota_perform(ota);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        err = ESP_OK;
        int total = esp_https_ota_get_image_size(ota);
        int read = esp_https_ota_get_image_len_read(ota);
        if (total > 0) {
            xSemaphoreTake(mutex, portMAX_DELAY);
            status.progress = (int)(100LL * read / total);
            xSemaphoreGive(mutex);
        }
    }
    if (err == ESP_OK && esp_https_ota_is_complete_data_received(ota)) {
        err = esp_https_ota_finish(ota);
        ota = NULL;
    }
    if (ota) {
        esp_https_ota_abort(ota);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Update failed: %s", esp_err_to_name(err));
        set_state(OTA_INSTALL_FAILED);
        return false;
    }
    ESP_LOGI(TAG, "Update installed, restarting");
    set_state(OTA_DONE);
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return true;
}

/* Check for a newer release and install it. Manual checks also update
 * development builds; automatic ones only release builds. */
static void check(bool manual)
{
    if (!board_wifi_connected()) {
        set_state(OTA_FAILED);
        return;
    }
    set_state(OTA_CHECKING);
    /* Two TLS sessions at once don't fit in RAM. */
    sl_set_paused(true);
    char tag[24], url[256];
    if (!latest_release(tag, sizeof(tag), url, sizeof(url))) {
        set_state(OTA_FAILED);
        sl_set_paused(false);
        return;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    strlcpy(status.latest, tag, sizeof(status.latest));
    status.checked = time(NULL);
    xSemaphoreGive(mutex);

    long mine = version_number(ota_version()), theirs = version_number(tag);
    bool newer = theirs > mine && (mine >= 0 || manual);
    ESP_LOGI(TAG, "Running %s, latest release %s%s", ota_version(), tag, newer ? ": updating" : "");
    if (newer) {
        install(url);
    } else {
        set_state(OTA_UP_TO_DATE);
    }
    sl_set_paused(false);
}

static void ota_task(void *arg)
{
    /* Spread boards over the hour so they don't all ask GitHub at once. */
    int minute = esp_random() % 60;
    int last_day = -1;
    while (1) {
        bool manual = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(60 * 1000)) > 0;
        if (manual) {
            check(true);
            continue;
        }
        if (!board_time_valid()) {
            continue;
        }
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        if (tm.tm_hour == NIGHT_HOUR && tm.tm_min >= minute && tm.tm_yday != last_day) {
            last_day = tm.tm_yday;
            sl_set_paused(true);
            metro_refresh_if_stale();
            sl_set_paused(false);
#if CONFIG_OTA_AUTO_UPDATE
            check(false);
#endif
        }
    }
}

void ota_start(void)
{
    xTaskCreate(ota_task, "ota", 8192, NULL, 3, &task_handle);
}

void ota_check_now(void)
{
    if (task_handle) {
        xTaskNotifyGive(task_handle);
    }
}

void ota_get_status(ota_status_t *out)
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    *out = status;
    xSemaphoreGive(mutex);
}
