/* Polls SL's Transport API and works out where metro trains are.
 *
 * Every departure carries a journey id that is shared across stations.
 * Querying the stations on each side of home tells us, for a train on its
 * way in, which of them it has yet to reach: the furthest one still listing
 * it is where it stops next. The same answers also list trains going the
 * other way, which have already left home; the nearest station still
 * listing one of those is where it stops next on its way out.
 *
 * Home and the nearest station on each side are fetched every
 * CONFIG_SL_POLL_SECONDS, the stations further out every SLOW_EVERY:th time:
 * arrivals near home show up quickly without loading SL's free API more.
 * Responses are read as a stream (json_stream.c), so even the largest
 * interchange station answers need no buffer.
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
#include "board.h"
#include "json_stream.h"
#include "sl_api.h"

#define API_HOME   "https://transport.integration.sl.se/v1/sites/%d/departures?transport=METRO&direction=%d&forecast=60"
#define API_NEAR   "https://transport.integration.sl.se/v1/sites/%d/departures?transport=METRO&forecast=30"
/* Used when there is no later station to measure the segment time from. */
#define DEFAULT_SEGMENT_S 90
/* Trains that have left home, collected before picking the nearest. */
#define MAX_CANDIDATES 12

static const char *TAG = "sl";

/* SL's direction_code per index; see the route ordering in metro_data.h. */
static const int direction_code[2] = { [SL_RIGHTBOUND] = 2, [SL_LEFTBOUND] = 1 };

static SemaphoreHandle_t state_mutex;
static sl_state_t state;
static sl_config_t config;
static TaskHandle_t poll_handle;
static volatile bool paused;
static volatile bool released;      /* connection closed while paused */
static volatile bool ever_succeeded;

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

static bool is_at_stop(const char *st)
{
    return !strcmp(st, "ATSTOP") || !strcmp(st, "BOARDING") || !strcmp(st, "BOARDINGCLOSED");
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

/* ---- streaming a departures answer ----------------------------------- */

/* One departure, collected field by field. */
typedef struct {
    int64_t journey;
    int dir_code;
    int line;
    char dest[28];
    char expected[24], scheduled[24];
    char state[20], journey_state[20];
} departure_t;

typedef struct {
    void (*on_departure)(void *ctx, const departure_t *d);
    void *ctx;
    departure_t d;
} stream_t;

#define DEP "departures/#"

static void dep_cb(js_parser_t *p, js_event_t ev, const char *v)
{
    stream_t *s = p->ctx;
    departure_t *d = &s->d;
    if (ev == JS_OBJECT_END) {
        if (js_path_is(p, DEP)) {
            s->on_departure(s->ctx, d);
            memset(d, 0, sizeof(*d));
        }
        return;
    }
    if (js_path_is(p, DEP "/journey/id")) d->journey = strtoll(v, NULL, 10);
    else if (js_path_is(p, DEP "/direction_code")) d->dir_code = atoi(v);
    else if (js_path_is(p, DEP "/line/designation")) d->line = atoi(v);
    else if (js_path_is(p, DEP "/destination")) strlcpy(d->dest, v, sizeof(d->dest));
    else if (js_path_is(p, DEP "/expected")) strlcpy(d->expected, v, sizeof(d->expected));
    else if (js_path_is(p, DEP "/scheduled")) strlcpy(d->scheduled, v, sizeof(d->scheduled));
    else if (js_path_is(p, DEP "/state")) strlcpy(d->state, v, sizeof(d->state));
    else if (js_path_is(p, DEP "/journey/state")) strlcpy(d->journey_state, v, sizeof(d->journey_state));
}

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        js_feed(evt->user_data, evt->data, evt->data_len);
    }
    return ESP_OK;
}

static bool fetch(esp_http_client_handle_t client, const char *url,
                  void (*on_departure)(void *, const departure_t *), void *ctx)
{
    static js_parser_t json;
    static stream_t stream;
    memset(&stream, 0, sizeof(stream));
    stream.on_departure = on_departure;
    stream.ctx = ctx;
    js_init(&json, dep_cb, &stream);
    esp_http_client_set_user_data(client, &json);
    esp_http_client_set_url(client, url);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    if (err != ESP_OK || status != 200 || json.error) {
        ESP_LOGW(TAG, "%s: %s, HTTP %d", url + 45, esp_err_to_name(err), status);
        return false;
    }
    return true;
}

static time_t dep_time(const departure_t *d)
{
    return parse_time(d->expected[0] ? d->expected : d->scheduled);
}

/* ---- home: departures and trains on their way in --------------------- */

typedef struct {
    const sl_config_t *cfg;
    const metro_group_t *g;
    int dir;
    sl_dir_t *out;
} home_ctx_t;

static void on_home(void *ctx, const departure_t *d)
{
    home_ctx_t *h = ctx;
    sl_dir_t *out = h->out;
    /* Interchanges like T-Centralen list every line. */
    if (out->n == SL_MAX_TRAINS || !d->journey || !on_group(h->g, d->line)) {
        return;
    }
    sl_train_t *tr = &out->trains[out->n++];
    tr->journey = d->journey;
    tr->line = d->line;
    while (tr->reach < h->cfg->n_up[h->dir] &&
           line_calls_at(h->g, d->line, h->cfg->up[h->dir][tr->reach])) {
        tr->reach++;
    }
    strlcpy(tr->dest, d->dest[0] ? d->dest : "?", sizeof(tr->dest));
    tr->t[0] = dep_time(d);
    tr->cancelled = !strcmp(d->state, "CANCELLED") || !strcmp(d->journey_state, "CANCELLED");
    if (is_at_stop(d->state)) {
        tr->at_stop |= 1;
    }
}

/* ---- neighbours: both directions at once ----------------------------- */

typedef struct {
    int64_t journey;
    uint8_t line;
    time_t t[SL_UPSTREAM + 1];
    uint8_t at_stop;
} candidate_t;

/* What a neighbouring station listed when it was last fetched. The nearest
 * station on each side is fetched on every poll, the others on every
 * SLOW_EVERY:th, so their answers are kept in between. */
#define CACHE_MAX   32
#define SLOW_EVERY  3
/* A kept answer whose time is this far past is probably for a train that
 * has since passed; it is ignored until the station is fetched again. */
#define CACHE_STALE_S 20

typedef struct {
    int64_t journey;
    time_t  t;
    uint8_t dir_code, line;
    bool    stop;
} listing_t;

typedef struct {
    listing_t e[CACHE_MAX];
    int n;
} station_cache_t;

static station_cache_t cache[2][SL_UPSTREAM + 1];   /* [side][k], k = 1 nearest */

static void on_near(void *ctx, const departure_t *d)
{
    station_cache_t *c = ctx;
    if (d->journey && c->n < CACHE_MAX) {
        c->e[c->n++] = (listing_t){
            d->journey, dep_time(d), d->dir_code, d->line, is_at_stop(d->state),
        };
    }
}

/* Use one listing from the station `k` away from home on `side`. */
static void apply_listing(const metro_group_t *g, int side, int k, const listing_t *l,
                          sl_dir_t *dirs, candidate_t (*cand)[MAX_CANDIDATES], int *n_cand)
{
    int in = side, out = !side;
    if (l->dir_code == direction_code[in]) {
        /* On its way in: which neighbours it has yet to reach. */
        sl_dir_t *dir = &dirs[in];
        for (int i = 0; i < dir->n; i++) {
            if (dir->trains[i].journey == l->journey) {
                dir->trains[i].t[k] = l->t;
                if (l->stop) dir->trains[i].at_stop |= 1 << k;
            }
        }
    } else if (l->dir_code == direction_code[out] && on_group(g, l->line)) {
        /* On its way out, past home: these stations are downstream. */
        candidate_t *c = NULL;
        for (int i = 0; i < n_cand[out]; i++) {
            if (cand[out][i].journey == l->journey) c = &cand[out][i];
        }
        if (!c && n_cand[out] < MAX_CANDIDATES) {
            c = &cand[out][n_cand[out]++];
            memset(c, 0, sizeof(*c));
            c->journey = l->journey;
            c->line = l->line;
        }
        if (c) {
            c->t[k] = l->t;
            if (l->stop) c->at_stop |= 1 << k;
        }
    }
}

/* Next stop of a train on its way out: the nearest station listing it. */
static int next_stop(const uint8_t n, const time_t *t)
{
    for (int k = 1; k <= n; k++) {
        if (t[k]) return k;
    }
    return 0;
}

/* Longest plausible time per station for a train on its way out. */
#define MAX_SEGMENT_S 240
/* Older than this, an expected time at the next stop is stale. */
#define STALE_S 90

/* A train counts as having left home only if its line calls at home and at
 * every station up to its next stop: line 18 trains starting at Alvik are
 * listed there too but never passed Brommaplan. */
static bool passed_home(const sl_config_t *cfg, const metro_group_t *g, int dir,
                        const candidate_t *c, int m, time_t now)
{
    if (!line_calls_at(g, c->line, cfg->home)) {
        return false;
    }
    for (int k = 1; k <= m; k++) {
        if (!line_calls_at(g, c->line, cfg->up[!dir][k - 1])) {
            return false;
        }
    }
    double rem = difftime(c->t[m], now);
    return rem > -STALE_S && rem < m * MAX_SEGMENT_S;
}

static void pick_departed(const sl_config_t *cfg, const metro_group_t *g, int dir_idx,
                          sl_dir_t *dir, candidate_t *cand, int n_cand, time_t now)
{
    dir->n_dep = 0;
    for (int pass = 1; pass <= dir->n_down && dir->n_dep < SL_MAX_DEPARTED; pass++) {
        for (int i = 0; i < n_cand && dir->n_dep < SL_MAX_DEPARTED; i++) {
            candidate_t *c = &cand[i];
            if (next_stop(dir->n_down, c->t) != pass ||
                !passed_home(cfg, g, dir_idx, c, pass, now)) {
                continue;
            }
            /* Still listed at home and not yet due there: it hasn't left. */
            bool at_home = false;
            for (int j = 0; j < dir->n; j++) {
                if (dir->trains[j].journey == c->journey) {
                    if (difftime(dir->trains[j].t[0], now) > -10) at_home = true;
                    else dir->trains[j].left = true;
                }
            }
            if (at_home) {
                continue;
            }
            sl_departed_t *d = &dir->departed[dir->n_dep++];
            d->journey = c->journey;
            d->line = c->line;
            memcpy(d->t, c->t, sizeof(d->t));
            d->at_stop = c->at_stop;
        }
    }
}

/* Fetch home and the nearest neighbours; with `full`, all neighbours. */
static bool poll_all(esp_http_client_handle_t client, const sl_config_t *cfg, sl_dir_t *dirs,
                     bool full)
{
    const metro_group_t *g = &metro_groups[cfg->group];
    char url[160];
    memset(dirs, 0, 2 * sizeof(*dirs));

    for (int dir = 0; dir < 2; dir++) {
        dirs[dir].n_up = cfg->n_up[dir];
        dirs[dir].n_down = cfg->n_up[!dir];
        home_ctx_t h = { cfg, g, dir, &dirs[dir] };
        snprintf(url, sizeof(url), API_HOME, cfg->home, direction_code[dir]);
        if (!fetch(client, url, on_home, &h)) {
            return false;
        }
    }
    for (int side = 0; side < 2; side++) {
        for (int k = 1; k <= cfg->n_up[side] && (k == 1 || full); k++) {
            station_cache_t *c = &cache[side][k];
            c->n = 0;
            snprintf(url, sizeof(url), API_NEAR, cfg->up[side][k - 1]);
            if (!fetch(client, url, on_near, c)) {
                return false;
            }
        }
    }

    static candidate_t cand[2][MAX_CANDIDATES];
    int n_cand[2] = { 0, 0 };
    time_t now = time(NULL);
    for (int side = 0; side < 2; side++) {
        for (int k = 1; k <= cfg->n_up[side]; k++) {
            for (int i = 0; i < cache[side][k].n; i++) {
                const listing_t *l = &cache[side][k].e[i];
                if (k > 1 && difftime(l->t, now) < -CACHE_STALE_S) {
                    continue;
                }
                apply_listing(g, side, k, l, dirs, cand, n_cand);
            }
        }
    }
    for (int dir = 0; dir < 2; dir++) {
        pick_departed(cfg, g, dir, &dirs[dir], cand[dir], n_cand[dir], now);
    }
    return true;
}

static void poll_task(void *arg)
{
    esp_http_client_config_t cfg = {
        .url = "https://transport.integration.sl.se/",
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event,
        .keep_alive_enable = true,
        .timeout_ms = 8000,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    static sl_state_t next;

    while (1) {
        if (paused) {
            /* A firmware update needs the RAM: close the kept-alive TLS
             * connection. */
            if (!released) {
                esp_http_client_close(client);
                released = true;
            }
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
            continue;
        }
        released = false;
        if (!board_wifi_connected() || !board_time_valid()) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
            continue;
        }
        sl_config_t cfg;
        sl_get_config(&cfg);
        /* Everything on a new station, then the outer neighbours every
         * SLOW_EVERY:th poll. */
        static uint32_t polled_generation;
        static int round;
        bool full = cfg.generation != polled_generation || round % SLOW_EVERY == 0;
        int64_t t0 = esp_log_timestamp();
        bool ok = poll_all(client, &cfg, next.dir, full);
        if (ok) {
            polled_generation = cfg.generation;
            round = full ? 1 : round + 1;
        }
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
            ESP_LOGI(TAG, "%s: %s update in %lld ms, %d + %d trains, %d + %d departed",
                     metro_station_name(cfg.home), full ? "full" : "near",
                     esp_log_timestamp() - t0,
                     next.dir[0].n, next.dir[1].n, next.dir[0].n_dep, next.dir[1].n_dep);
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

float sl_departed_position(const sl_departed_t *d, int n_down, time_t now)
{
    int m = next_stop(n_down, d->t);
    if (!m) {
        return -1;
    }
    if (d->at_stop & (1 << m)) {
        return m;
    }
    double rem = difftime(d->t[m], now);
    if (rem <= 0) {
        return m;   /* due now; the next poll will move it on */
    }
    double seg = m < n_down && d->t[m + 1] ? difftime(d->t[m + 1], d->t[m]) : DEFAULT_SEGMENT_S;
    if (seg < 40) seg = 40;
    if (seg > 240) seg = 240;
    double frac = rem / seg;
    if (frac > 1) frac = 1;     /* still at the station before (home, if m is 1) */
    return m - (float)frac;
}
