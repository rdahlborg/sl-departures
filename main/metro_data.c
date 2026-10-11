/* Metro lines and stations, fetched from SL. See metro_data.h.
 *
 * For each line, a metro-only trip between its end stations in SL's journey
 * planner lists every station in order. The planner names stations by stop
 * area, so each name is then looked up in its stop finder to get the site
 * ID the departures API uses. That is about 100 requests, done once and
 * saved in NVS; the list is refreshed monthly.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "board.h"
#include "json_stream.h"
#include "metro_data.h"

#define JP_BASE         "https://journeyplanner.integration.sl.se/v2/"
#define MAX_ROUTE       60      /* stations per line */
#define MAX_STATIONS    160
#define MAX_NAME        40
#define NVS_PART        "stations"   /* own partition, see partitions.csv */
#define NVS_NS          "metro"
#define NVS_KEY         "data"
#define DATA_MAGIC      0x314D4C53  /* "SLM1" */
#define REFRESH_AGE_S   (30 * 24 * 3600)
#define RETRY_MS        30000
#define FINDER_BUF      (12 * 1024)
#define MIN_LINE_LEN    5       /* shorter means the planner took a shortcut */

static const char *TAG = "metro";

/* The only built-in data: which lines exist and where they end. A trip
 * from `from` to `to` follows SL direction_code 2. */
static const struct {
    uint8_t group, line;
    uint16_t from, to;
} LINES[] = {
    { 0, 19, 9100, 9160 },   /* Hässelby strand – Hagsätra */
    { 0, 17, 9108, 9140 },   /* Åkeshov – Skarpnäck */
    { 0, 18, 9112, 9180 },   /* Alvik – Farsta strand */
    { 1, 13, 9220, 9280 },   /* Ropsten – Norsborg */
    { 1, 14, 9200, 9260 },   /* Mörby centrum – Fruängen */
    { 2, 10, 9320, 9340 },   /* Hjulsta – Kungsträdgården */
    { 2, 11, 9300, 9340 },   /* Akalla – Kungsträdgården */
};
#define N_LINES (sizeof(LINES) / sizeof(LINES[0]))

static const struct {
    const char *name;
    uint32_t color;
} GROUPS[METRO_N_GROUPS] = {
    { "Gröna linjen", 0x179C4C },
    { "Röda linjen",  0xD71D24 },
    { "Blå linjen",   0x0089CA },
};

/* Working copy while fetching or loading; freed afterwards. */
typedef struct {
    uint32_t fetched;
    uint8_t n[N_LINES];
    uint16_t sites[N_LINES][MAX_ROUTE];
    uint16_t n_st;
    struct {
        uint16_t site;
        char name[MAX_NAME];
    } st[MAX_STATIONS];
} dataset_t;

/* The tables everything else reads, built once from a dataset. */
metro_group_t metro_groups[METRO_N_GROUPS];
static metro_route_t routes[N_LINES];
static uint16_t route_sites[N_LINES][MAX_ROUTE];
static metro_station_t stations[MAX_STATIONS];
static uint16_t n_stations;
static uint16_t group_stations[METRO_N_GROUPS][MAX_STATIONS];
static char *name_pool;
static uint32_t data_fetched;
static volatile bool ready;
static char status[64] = "Läser stationslistan…";
static void (*ready_cb)(void);

/* ---- names ---------------------------------------------------------- */

/* "Universitetet (Tunnelbanan)" -> "Universitetet" */
static void clean_name(char *s)
{
    char *paren = strstr(s, " (");
    if (paren && s[strlen(s) - 1] == ')') {
        *paren = '\0';
    }
}

static int find_station(const dataset_t *d, const char *name)
{
    for (int i = 0; i < d->n_st; i++) {
        if (!strcmp(d->st[i].name, name)) {
            return i;
        }
    }
    return -1;
}

/* ---- HTTP ----------------------------------------------------------- */

typedef struct {
    js_parser_t *json;          /* streaming mode */
    char *buf;                  /* buffered mode */
    int len, cap;
} http_ctx_t;

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    http_ctx_t *c = evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA) {
        return ESP_OK;
    }
    if (c->json) {
        js_feed(c->json, evt->data, evt->data_len);
    } else if (c->len + evt->data_len < c->cap) {
        memcpy(c->buf + c->len, evt->data, evt->data_len);
        c->len += evt->data_len;
    }
    return ESP_OK;
}

static bool http_get(esp_http_client_handle_t client, http_ctx_t *ctx, const char *url)
{
    esp_http_client_set_url(client, url);
    ctx->len = 0;
    esp_err_t err = esp_http_client_perform(client);
    int code = esp_http_client_get_status_code(client);
    if (err != ESP_OK || code != 200) {
        ESP_LOGW(TAG, "%s: %s, HTTP %d", url, esp_err_to_name(err), code);
        return false;
    }
    return true;
}

static void url_encode(char *out, size_t len, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *s && o + 4 < len; s++) {
        unsigned char c = *s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.') {
            out[o++] = c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    out[o] = '\0';
}

/* ---- trips: station order per line ---------------------------------- */

typedef struct {
    dataset_t *d;
    int line;                   /* index into LINES */
    /* Current stopSequence element and its parent stop area. */
    char e_id[24], e_name[MAX_NAME], p_name[MAX_NAME];
    char last[MAX_NAME];
    char names[MAX_ROUTE][MAX_NAME];
    int n;
} trip_ctx_t;

#define SEQ "journeys/0/legs/#/stopSequence/#"

static void trip_cb(js_parser_t *p, js_event_t ev, const char *v)
{
    trip_ctx_t *t = p->ctx;
    if (ev == JS_OBJECT_END) {
        if (!js_path_is(p, SEQ)) {
            return;
        }
        /* Stops are usually platforms whose parent is the stop area, but
         * the first stop can be the stop area itself. */
        char name[MAX_NAME];
        strlcpy(name, strncmp(t->e_id, "9021", 4) == 0 ? t->e_name : t->p_name, sizeof(name));
        clean_name(name);
        /* Legs share a stop where they meet. */
        if (name[0] && strcmp(name, t->last) != 0 && t->n < MAX_ROUTE) {
            strlcpy(t->names[t->n++], name, MAX_NAME);
            strlcpy(t->last, name, sizeof(t->last));
        }
        t->e_id[0] = t->e_name[0] = t->p_name[0] = '\0';
        return;
    }
    if (js_path_is(p, SEQ "/id")) {
        strlcpy(t->e_id, v, sizeof(t->e_id));
    } else if (js_path_is(p, SEQ "/disassembledName")) {
        strlcpy(t->e_name, v, sizeof(t->e_name));
    } else if (js_path_is(p, SEQ "/parent/disassembledName")) {
        strlcpy(t->p_name, v, sizeof(t->p_name));
    } else if (js_path_is(p, SEQ "/name") && !t->e_name[0]) {
        /* "Hjulsta, Stockholm" when there is no short name */
        strlcpy(t->e_name, v, sizeof(t->e_name));
        char *comma = strchr(t->e_name, ',');
        if (comma) *comma = '\0';
    } else if (js_path_is(p, SEQ "/parent/name") && !t->p_name[0]) {
        strlcpy(t->p_name, v, sizeof(t->p_name));
        char *comma = strchr(t->p_name, ',');
        if (comma) *comma = '\0';
    }
}

/* Trips are planned for midday on the next weekday, when every line runs
 * end to end. In the evening the planner can return a trip that lists
 * only the first and last stop. */
static void next_weekday_noon(char *date, size_t len)
{
    time_t t = time(NULL) + 24 * 3600;
    struct tm tm;
    localtime_r(&t, &tm);
    while (tm.tm_wday == 0 || tm.tm_wday == 6) {
        t += 24 * 3600;
        localtime_r(&t, &tm);
    }
    strftime(date, len, "%Y%m%d", &tm);
}

static bool fetch_line(esp_http_client_handle_t client, trip_ctx_t *t)
{
    char url[512], date[12];
    next_weekday_noon(date, sizeof(date));
    snprintf(url, sizeof(url), JP_BASE "trips?type_origin=any&name_origin=909100100000%04u"
             "&type_destination=any&name_destination=909100100000%04u&calc_number_of_trips=1"
             "&itd_date=%s&itd_time=1200"
             "&gen_c=false&incl_mot_2=true&incl_mot_0=false&incl_mot_4=false&incl_mot_5=false"
             "&incl_mot_9=false&incl_mot_10=false&incl_mot_14=false&incl_mot_19=false",
             LINES[t->line].from, LINES[t->line].to, date);
    static js_parser_t json;
    js_init(&json, trip_cb, t);
    http_ctx_t ctx = { .json = &json };
    esp_http_client_set_user_data(client, &ctx);
    t->n = 0;
    t->last[0] = '\0';
    if (!http_get(client, &ctx, url) || json.error) {
        return false;
    }
    ESP_LOGI(TAG, "Line %d: %d stations", LINES[t->line].line, t->n);
    return t->n >= MIN_LINE_LEN;
}

/* ---- stop finder: site ID per station ------------------------------- */

static int lookup_site(esp_http_client_handle_t client, const char *name)
{
    char enc[3 * MAX_NAME], url[256];
    url_encode(enc, sizeof(enc), name);
    snprintf(url, sizeof(url), JP_BASE "stop-finder?type_sf=any&any_obj_filter_sf=2&name_sf=%s", enc);
    http_ctx_t ctx = { .buf = malloc(FINDER_BUF), .cap = FINDER_BUF };
    if (!ctx.buf) {
        return -1;
    }
    esp_http_client_set_user_data(client, &ctx);
    int site = -1;
    if (http_get(client, &ctx, url)) {
        ctx.buf[ctx.len] = '\0';
        cJSON *root = cJSON_Parse(ctx.buf);
        cJSON *loc;
        /* The first metro stop whose name is this station. */
        cJSON_ArrayForEach(loc, cJSON_GetObjectItem(root, "locations")) {
            const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(loc, "id"));
            const char *full = cJSON_GetStringValue(cJSON_GetObjectItem(loc, "name"));
            bool metro = false;
            cJSON *pc;
            cJSON_ArrayForEach(pc, cJSON_GetObjectItem(loc, "productClasses")) {
                metro |= pc->valueint == 2;
            }
            if (!id || !full || !metro) {
                continue;
            }
            /* "Stockholm, Universitetet (Tunnelbanan)" -> "Universitetet" */
            char n[MAX_NAME];
            const char *comma = strstr(full, ", ");
            strlcpy(n, comma ? comma + 2 : full, sizeof(n));
            clean_name(n);
            if (!strcmp(n, name)) {
                /* Global IDs end in the site: 9091001000009109 and
                 * 9091001001009116 are sites 9109 and 9116. */
                site = (int)(strtoull(id, NULL, 10) % 1000000ULL);
                break;
            }
        }
        cJSON_Delete(root);
    }
    free(ctx.buf);
    return site;
}

/* ---- fetching ------------------------------------------------------- */

static bool fetch_all(dataset_t *d)
{
    memset(d, 0, sizeof(*d));
    esp_http_client_config_t cfg = {
        .url = JP_BASE,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event,
        .keep_alive_enable = true,
        .timeout_ms = 15000,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    trip_ctx_t *t = calloc(1, sizeof(*t));
    bool ok = client && t;

    for (int li = 0; ok && li < (int)N_LINES; li++) {
        snprintf(status, sizeof(status), "Hämtar linje %d från SL…", LINES[li].line);
        t->d = d;
        t->line = li;
        if (!fetch_line(client, t)) {
            ok = false;
            break;
        }
        for (int i = 0; i < t->n; i++) {
            int s = find_station(d, t->names[i]);
            if (s < 0) {
                if (d->n_st >= MAX_STATIONS) {
                    ok = false;
                    break;
                }
                snprintf(status, sizeof(status), "Hämtar stationer… %d", d->n_st + 1);
                int site = lookup_site(client, t->names[i]);
                if (site <= 0 || site > 0xFFFF) {
                    ESP_LOGW(TAG, "No site for %s", t->names[i]);
                    ok = false;
                    break;
                }
                s = d->n_st++;
                d->st[s].site = site;
                strlcpy(d->st[s].name, t->names[i], MAX_NAME);
            }
            d->sites[li][d->n[li]++] = d->st[s].site;
        }
        /* The planner should have gone from end to end. */
        if (ok && (d->sites[li][0] != LINES[li].from ||
                   d->sites[li][d->n[li] - 1] != LINES[li].to)) {
            ESP_LOGW(TAG, "Line %d does not run %u-%u", LINES[li].line, LINES[li].from, LINES[li].to);
            ok = false;
        }
    }
    free(t);
    if (client) {
        esp_http_client_cleanup(client);
    }
    d->fetched = time(NULL);
    return ok;
}

/* ---- flash ---------------------------------------------------------- */

/* magic | fetched | per line: n, sites | n_st | per station: site, len, name */
static bool save(const dataset_t *d)
{
    size_t cap = 16 + N_LINES * (1 + MAX_ROUTE * 2) + MAX_STATIONS * (3 + MAX_NAME);
    uint8_t *b = malloc(cap), *p = b;
    if (!b) {
        return false;
    }
    uint32_t magic = DATA_MAGIC;
    memcpy(p, &magic, 4); p += 4;
    memcpy(p, &d->fetched, 4); p += 4;
    for (int li = 0; li < (int)N_LINES; li++) {
        *p++ = d->n[li];
        memcpy(p, d->sites[li], d->n[li] * 2); p += d->n[li] * 2;
    }
    memcpy(p, &d->n_st, 2); p += 2;
    for (int i = 0; i < d->n_st; i++) {
        size_t l = strlen(d->st[i].name);
        memcpy(p, &d->st[i].site, 2); p += 2;
        *p++ = l;
        memcpy(p, d->st[i].name, l); p += l;
    }
    size_t size = p - b;
    nvs_handle_t h;
    bool ok = nvs_open_from_partition(NVS_PART, NVS_NS, NVS_READWRITE, &h) == ESP_OK;
    if (ok) {
        ok = nvs_set_blob(h, NVS_KEY, b, size) == ESP_OK && nvs_commit(h) == ESP_OK;
        nvs_close(h);
    }
    free(b);
    ESP_LOGI(TAG, "Saved %d stations (%d bytes): %s", d->n_st, (int)size, ok ? "ok" : "failed");
    return ok;
}

static bool load(dataset_t *d)
{
    memset(d, 0, sizeof(*d));
    nvs_handle_t h;
    if (nvs_open_from_partition(NVS_PART, NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = 0;
    uint8_t *b = NULL;
    bool ok = nvs_get_blob(h, NVS_KEY, NULL, &len) == ESP_OK && (b = malloc(len)) &&
              nvs_get_blob(h, NVS_KEY, b, &len) == ESP_OK;
    nvs_close(h);
    const uint8_t *p = b, *end = b + len;
    uint32_t magic = 0;
#define TAKE(dst, n) do { if (p + (n) > end) { ok = false; goto out; } memcpy((dst), p, (n)); p += (n); } while (0)
    if (!ok) goto out;
    TAKE(&magic, 4);
    if (magic != DATA_MAGIC) { ok = false; goto out; }
    TAKE(&d->fetched, 4);
    for (int li = 0; li < (int)N_LINES; li++) {
        TAKE(&d->n[li], 1);
        if (d->n[li] > MAX_ROUTE) { ok = false; goto out; }
        TAKE(d->sites[li], d->n[li] * 2);
    }
    TAKE(&d->n_st, 2);
    if (d->n_st > MAX_STATIONS) { ok = false; goto out; }
    for (int i = 0; i < d->n_st; i++) {
        uint8_t l;
        TAKE(&d->st[i].site, 2);
        TAKE(&l, 1);
        if (l >= MAX_NAME) { ok = false; goto out; }
        TAKE(d->st[i].name, l);
        d->st[i].name[l] = '\0';
    }
#undef TAKE
out:
    free(b);
    return ok;
}

/* ---- runtime tables -------------------------------------------------- */

static void build(const dataset_t *d)
{
    size_t pool = 0;
    for (int i = 0; i < d->n_st; i++) {
        pool += strlen(d->st[i].name) + 1;
    }
    name_pool = malloc(pool);
    assert(name_pool);
    char *np = name_pool;
    n_stations = d->n_st;
    for (int i = 0; i < d->n_st; i++) {
        stations[i].site = d->st[i].site;
        stations[i].name = np;
        strcpy(np, d->st[i].name);
        np += strlen(np) + 1;
    }
    for (int li = 0; li < (int)N_LINES; li++) {
        memcpy(route_sites[li], d->sites[li], d->n[li] * 2);
        routes[li] = (metro_route_t){ LINES[li].line, d->n[li], route_sites[li] };
    }
    /* Groups: their lines are consecutive in LINES. The picker lists the
     * first line's stations in order, then the other lines' extra ones. */
    for (int g = 0; g < METRO_N_GROUPS; g++) {
        int first = -1, count = 0, n = 0;
        for (int li = 0; li < (int)N_LINES; li++) {
            if (LINES[li].group != g) continue;
            if (first < 0) first = li;
            count++;
            for (int i = 0; i < d->n[li]; i++) {
                bool seen = false;
                for (int k = 0; k < n && !seen; k++) seen = group_stations[g][k] == d->sites[li][i];
                if (!seen && n < MAX_STATIONS) group_stations[g][n++] = d->sites[li][i];
            }
        }
        metro_groups[g] = (metro_group_t){
            GROUPS[g].name, GROUPS[g].color, count, &routes[first], n, group_stations[g],
        };
    }
    data_fetched = d->fetched;
}

/* ---- loader task ---------------------------------------------------- */

static void init_partition(void)
{
    esp_err_t err = nvs_flash_init_partition(NVS_PART);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* Blank or leftover data from an older layout. */
        nvs_flash_erase_partition(NVS_PART);
        err = nvs_flash_init_partition(NVS_PART);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Station storage unavailable: %s", esp_err_to_name(err));
    }
}

static void loader_task(void *arg)
{
    init_partition();
    dataset_t *d = malloc(sizeof(*d));
    assert(d);
    if (load(d)) {
        ESP_LOGI(TAG, "Loaded %d stations from flash", d->n_st);
    } else {
        while (1) {
            snprintf(status, sizeof(status), "Väntar på Wi-Fi…");
            while (!board_wifi_connected() || !board_time_valid()) {
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            ESP_LOGI(TAG, "Fetching the station list from SL");
            if (fetch_all(d)) {
                save(d);
                break;
            }
            snprintf(status, sizeof(status), "Kunde inte hämta stationer, försöker igen…");
            vTaskDelay(pdMS_TO_TICKS(RETRY_MS));
        }
    }
    build(d);
    free(d);
    ready = true;
    if (ready_cb) {
        ready_cb();
    }
    vTaskDelete(NULL);
}

void metro_start(void (*on_ready)(void))
{
    ready_cb = on_ready;
    xTaskCreate(loader_task, "metro", 8192, NULL, 4, NULL);
}

bool metro_ready(void)
{
    return ready;
}

void metro_status(char *buf, size_t len)
{
    strlcpy(buf, status, len);
}

void metro_refresh_if_stale(void)
{
    if (!ready || difftime(time(NULL), data_fetched) < REFRESH_AGE_S) {
        return;
    }
    dataset_t *d = malloc(sizeof(*d));
    if (!d) {
        return;
    }
    /* Used from the next restart, so nothing reading the current tables
     * has them change underneath it. */
    if (fetch_all(d) && save(d)) {
        data_fetched = d->fetched;
    }
    free(d);
}

const char *metro_station_name(uint16_t site)
{
    for (int i = 0; i < n_stations; i++) {
        if (stations[i].site == site) {
            return stations[i].name;
        }
    }
    return "?";
}
