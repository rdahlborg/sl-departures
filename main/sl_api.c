/* Polls SL's Transport API and works out where approaching metro trains are.
 *
 * Every departure carries a journey id that is shared across stations, so
 * querying the stations upstream of home tells us which of them a train has
 * yet to reach. The furthest upstream station still listing a train is the
 * one it is heading for next.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "cJSON.h"
#include "board.h"
#include "sl_api.h"

#define API_URL    "https://transport.integration.sl.se/v1/sites/%d/departures?transport=METRO&direction=%d&forecast=%d"
#define RESP_MAX   (24 * 1024)
/* Used when there is no later station to measure the segment time from. */
#define DEFAULT_SEGMENT_S 90

static const char *TAG = "sl";

/* SL's direction_code per index; see the route ordering in metro_data.h. */
static const int direction_code[2] = { [SL_RIGHTBOUND] = 2, [SL_LEFTBOUND] = 1 };

static SemaphoreHandle_t state_mutex;
static sl_state_t state;
static sl_config_t config;
static TaskHandle_t poll_handle;
static volatile bool paused;
static volatile bool released;      /* connection and buffer freed while paused */
static volatile bool ever_succeeded;

typedef struct {
    char *buf;
    int len;
} resp_t;

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    resp_t *r = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && r->len + evt->data_len < RESP_MAX) {
        memcpy(r->buf + r->len, evt->data, evt->data_len);
        r->len += evt->data_len;
    }
    return ESP_OK;
}

/* SL returns local time without an offset, e.g. "2026-10-11T15:02:12". */
static time_t parse_time(const char *s)
{
    struct tm tm = { .tm_isdst = -1 };
    if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                     &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6) {
        return 0;
    }
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    return mktime(&tm);
}

static const char *json_str(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

/* Fetch one station's departures; returns the parsed "departures" array
 * owned by *root, or NULL on failure. */
static cJSON *fetch(esp_http_client_handle_t client, resp_t *resp, int site, int dir,
                    int forecast, cJSON **root)
{
    char url[160];
    snprintf(url, sizeof(url), API_URL, site, direction_code[dir], forecast);
    esp_http_client_set_url(client, url);
    resp->len = 0;

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "site %d: %s, HTTP %d", site, esp_err_to_name(err), status);
        return NULL;
    }
    resp->buf[resp->len] = '\0';
    *root = cJSON_Parse(resp->buf);
    cJSON *deps = cJSON_GetObjectItemCaseSensitive(*root, "departures");
    if (!cJSON_IsArray(deps)) {
        ESP_LOGW(TAG, "site %d: unexpected response", site);
        cJSON_Delete(*root);
        *root = NULL;
        return NULL;
    }
    return deps;
}

static bool is_at_stop(const char *st)
{
    return st && (!strcmp(st, "ATSTOP") || !strcmp(st, "BOARDING") ||
                  !strcmp(st, "BOARDINGCLOSED"));
}

static bool on_group(const metro_group_t *g, int line)
{
    for (int i = 0; i < g->n_routes; i++) {
        if (g->routes[i].line == line) {
            return true;
        }
    }
    return false;
}

static bool line_calls_at(const metro_group_t *g, int line, uint16_t site)
{
    for (int r = 0; r < g->n_routes; r++) {
        if (g->routes[r].line != line) {
            continue;
        }
        for (int i = 0; i < g->routes[r].n; i++) {
            if (g->routes[r].sites[i] == site) {
                return true;
            }
        }
    }
    return false;
}

static bool poll_direction(esp_http_client_handle_t client, resp_t *resp,
                           const sl_config_t *cfg, int dir, sl_dir_t *out)
{
    const metro_group_t *g = &metro_groups[cfg->group];
    cJSON *root = NULL;
    cJSON *deps = fetch(client, resp, cfg->home, dir, 60, &root);
    if (!deps) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->n_up = cfg->n_up[dir];
    const cJSON *d;
    cJSON_ArrayForEach(d, deps) {
        if (out->n == SL_MAX_TRAINS) {
            break;
        }
        const cJSON *journey = cJSON_GetObjectItemCaseSensitive(d, "journey");
        const cJSON *line = cJSON_GetObjectItemCaseSensitive(d, "line");
        const cJSON *jid = cJSON_GetObjectItemCaseSensitive(journey, "id");
        int line_no = atoi(json_str(line, "designation") ?: "0");
        /* Interchanges like T-Centralen list every line. */
        if (!cJSON_IsNumber(jid) || !on_group(g, line_no)) {
            continue;
        }
        sl_train_t *tr = &out->trains[out->n++];
        tr->journey = (int64_t)jid->valuedouble;
        tr->line = line_no;
        while (tr->reach < cfg->n_up[dir] && line_calls_at(g, line_no, cfg->up[dir][tr->reach])) {
            tr->reach++;
        }
        strlcpy(tr->dest, json_str(d, "destination") ?: "?", sizeof(tr->dest));
        const char *ex = json_str(d, "expected");
        tr->t[0] = parse_time(ex ? ex : json_str(d, "scheduled"));
        const char *st = json_str(d, "state");
        const char *jst = json_str(journey, "state");
        tr->cancelled = (st && !strcmp(st, "CANCELLED")) || (jst && !strcmp(jst, "CANCELLED"));
        if (is_at_stop(st)) {
            tr->at_stop |= 1;
        }
    }
    cJSON_Delete(root);

    for (int k = 1; k <= cfg->n_up[dir]; k++) {
        deps = fetch(client, resp, cfg->up[dir][k - 1], dir, 30, &root);
        if (!deps) {
            return false;
        }
        cJSON_ArrayForEach(d, deps) {
            const cJSON *jid = cJSON_GetObjectItemCaseSensitive(
                cJSON_GetObjectItemCaseSensitive(d, "journey"), "id");
            if (!cJSON_IsNumber(jid)) {
                continue;
            }
            for (int i = 0; i < out->n; i++) {
                sl_train_t *tr = &out->trains[i];
                if (tr->journey != (int64_t)jid->valuedouble) {
                    continue;
                }
                const char *ex = json_str(d, "expected");
                tr->t[k] = parse_time(ex ? ex : json_str(d, "scheduled"));
                if (is_at_stop(json_str(d, "state"))) {
                    tr->at_stop |= 1 << k;
                }
            }
        }
        cJSON_Delete(root);
    }
    return true;
}

static void poll_task(void *arg)
{
    resp_t resp = { .buf = malloc(RESP_MAX) };
    assert(resp.buf);
    esp_http_client_config_t cfg = {
        .url = "https://transport.integration.sl.se/",
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event,
        .user_data = &resp,
        .keep_alive_enable = true,
        .timeout_ms = 8000,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    static sl_state_t next;

    while (1) {
        if (paused) {
            /* A firmware update needs the RAM: close the kept-alive TLS
             * connection and free the response buffer. */
            if (!released) {
                esp_http_client_close(client);
                free(resp.buf);
                resp.buf = NULL;
                released = true;
            }
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
            continue;
        }
        if (!resp.buf) {
            resp.buf = malloc(RESP_MAX);
            released = false;
            if (!resp.buf) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        }
        if (!board_wifi_connected() || !board_time_valid()) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
            continue;
        }
        sl_config_t cfg;
        sl_get_config(&cfg);
        int64_t t0 = esp_log_timestamp();
        bool ok = poll_direction(client, &resp, &cfg, SL_RIGHTBOUND, &next.dir[SL_RIGHTBOUND]) &&
                  poll_direction(client, &resp, &cfg, SL_LEFTBOUND, &next.dir[SL_LEFTBOUND]);
        if (ok) {
            next.updated = time(NULL);
            ever_succeeded = true;
            next.generation = cfg.generation;
            xSemaphoreTake(state_mutex, portMAX_DELAY);
            /* Drop results for a station the user has since switched away from. */
            if (config.generation == cfg.generation) {
                state = next;
            }
            xSemaphoreGive(state_mutex);
            ESP_LOGI(TAG, "%s: updated in %lld ms, %d + %d trains",
                     metro_station_name(cfg.home), esp_log_timestamp() - t0,
                     next.dir[0].n, next.dir[1].n);
        } else {
            /* Drop the connection so the next poll starts clean. */
            esp_http_client_close(client);
        }
        /* sl_select() wakes us early. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ok ? CONFIG_SL_POLL_SECONDS * 1000 : 5000));
    }
}

/* Pick the first route in the group that calls at `site` and take up to
 * SL_UPSTREAM neighbours on each side of it. */
static bool build_config(int group, uint16_t site, sl_config_t *c)
{
    const metro_group_t *g = &metro_groups[group];
    for (int r = 0; r < g->n_routes; r++) {
        const metro_route_t *rt = &g->routes[r];
        for (int i = 0; i < rt->n; i++) {
            if (rt->sites[i] != site) {
                continue;
            }
            memset(c, 0, sizeof(*c));
            c->group = group;
            c->home = site;
            c->line = rt->line;
            for (int k = 1; k <= SL_UPSTREAM; k++) {
                if (i - k >= 0) c->up[SL_RIGHTBOUND][c->n_up[SL_RIGHTBOUND]++] = rt->sites[i - k];
                if (i + k < rt->n) c->up[SL_LEFTBOUND][c->n_up[SL_LEFTBOUND]++] = rt->sites[i + k];
            }
            c->end[SL_RIGHTBOUND] = rt->sites[rt->n - 1];
            c->end[SL_LEFTBOUND] = rt->sites[0];
            return true;
        }
    }
    return false;
}

void sl_select(int group, uint16_t site)
{
    sl_config_t c;
    if (group < 0 || group >= METRO_N_GROUPS || !build_config(group, site, &c)) {
        group = group >= 0 && group < METRO_N_GROUPS ? group : 0;
        build_config(group, metro_groups[group].stations[0], &c);
    }
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    c.generation = config.generation + 1;
    config = c;
    memset(&state, 0, sizeof(state));
    xSemaphoreGive(state_mutex);
    ESP_LOGI(TAG, "Showing %s on line %d", metro_station_name(c.home), c.line);
    if (poll_handle) {
        xTaskNotifyGive(poll_handle);
    }
}

void sl_get_config(sl_config_t *out)
{
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    *out = config;
    xSemaphoreGive(state_mutex);
}

void sl_init(void)
{
    state_mutex = xSemaphoreCreateMutex();
}

void sl_start(int group, uint16_t site)
{
    sl_select(group, site);
    xTaskCreate(poll_task, "sl_poll", 8192, NULL, 4, &poll_handle);
}

void sl_set_paused(bool pause)
{
    paused = pause;
    if (!poll_handle) {
        return;
    }
    xTaskNotifyGive(poll_handle);
    /* Wait until the connection is closed, finishing a poll in progress. */
    for (int i = 0; pause && !released && i < 100; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

bool sl_has_succeeded(void)
{
    return ever_succeeded;
}

bool sl_snapshot(sl_state_t *out)
{
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    *out = state;
    bool current = state.updated != 0 && state.generation == config.generation;
    xSemaphoreGive(state_mutex);
    return current;
}

float sl_train_position(const sl_train_t *tr, int n_up, time_t now)
{
    if (tr->cancelled) {
        return -1;
    }
    /* Furthest upstream station the train has yet to reach. */
    int m = -1;
    for (int k = n_up; k >= 0; k--) {
        if (tr->t[k]) {
            m = k;
            break;
        }
    }
    if (m < 0) {
        return -1;
    }
    if (tr->at_stop & (1 << m)) {
        return m;
    }
    double rem = difftime(tr->t[m], now);
    if (rem <= 0) {
        return m;   /* due now; the next poll will move it on */
    }
    if (m >= tr->reach) {
        /* Still on a branch that is not drawn, e.g. line 14 coming from
         * Stadion on a map that follows line 13 to Karlaplan. */
        return -1;
    }
    /* Time to cover the segment before station m is assumed to be like the
     * segment after it, which we know from the timetable. */
    double seg = m > 0 ? difftime(tr->t[m - 1], tr->t[m]) : DEFAULT_SEGMENT_S;
    if (seg < 40) seg = 40;
    if (seg > 240) seg = 240;
    double frac = rem / seg;
    if (frac > 1 && m < n_up) {
        /* Not listed at the next station out, yet more than a segment away:
         * the train starts its journey at station m and has not arrived. */
        return -1;
    }
    return m + (float)frac;
}
