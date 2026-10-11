/* Wi-Fi setup from a phone, see wifi_setup.h.
 *
 * The slow parts (scanning, starting the hotspot, trying credentials) run on
 * a small task so the button handler and web server never block on them.
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
#include "esp_http_server.h"
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

#define PORTAL_URL      "http://192.168.4.1"
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
static httpd_handle_t httpd;
static dns_server_handle_t dns;
static bool had_wifi;           /* already online when setup started */
static TickType_t started;

static void set_state(setup_state_t s)
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    status.state = s;
    xSemaphoreGive(mutex);
}

/* ---- web page ------------------------------------------------------- */

static const char PAGE_HEAD[] =
    "<!doctype html><html lang=sv><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>SL Departures Wi-Fi</title><style>"
    "body{font:17px/1.5 system-ui,sans-serif;margin:0;background:#0b1410;color:#e9f0ec}"
    "main{max-width:420px;margin:0 auto;padding:28px 18px}"
    "h1{font-size:1.4em;margin:0 0 4px}p{color:#93a59c;margin:0 0 20px}"
    "label{display:block;font-weight:600;margin:16px 0 6px}"
    "select,input{width:100%;box-sizing:border-box;font:inherit;padding:12px;border-radius:8px;"
    "border:1px solid #2c4237;background:#13201a;color:inherit}"
    "button{width:100%;margin-top:24px;padding:14px;font:inherit;font-weight:700;border:0;"
    "border-radius:8px;background:#179c4c;color:#fff}"
    ".bar{display:flex;gap:6px;margin-bottom:18px}.bar i{width:28px;height:6px;border-radius:3px}"
    "#other{display:none}.err{color:#ff8a8f}"
    "</style></head><body><main>"
    "<div class=bar><i style=background:#179c4c></i><i style=background:#d71d24></i>"
    "<i style=background:#0089ca></i></div>";

static void send_escaped(httpd_req_t *req, const char *s)
{
    char buf[8];
    for (; *s; s++) {
        switch (*s) {
        case '&':  httpd_resp_sendstr_chunk(req, "&amp;"); break;
        case '<':  httpd_resp_sendstr_chunk(req, "&lt;"); break;
        case '>':  httpd_resp_sendstr_chunk(req, "&gt;"); break;
        case '"':  httpd_resp_sendstr_chunk(req, "&quot;"); break;
        case '\'': httpd_resp_sendstr_chunk(req, "&#39;"); break;
        default:
            buf[0] = *s;
            buf[1] = '\0';
            httpd_resp_sendstr_chunk(req, buf);
        }
    }
}

static esp_err_t root_get(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Setup page requested");
    setup_status_t st;
    wifi_setup_get_status(&st);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr_chunk(req, PAGE_HEAD);
    httpd_resp_sendstr_chunk(req, "<h1>Anslut tavlan till Wi-Fi</h1>"
                                  "<p>Välj ditt nätverk och skriv lösenordet.</p>");
    if (st.last_failed) {
        httpd_resp_sendstr_chunk(req, "<p class=err>Kunde inte ansluta till ");
        send_escaped(req, st.target);
        httpd_resp_sendstr_chunk(req, ". Kontrollera lösenordet och försök igen.</p>");
    }
    httpd_resp_sendstr_chunk(req, "<form method=post action=/connect>"
                                  "<label for=ssid>Nätverk</label><select id=ssid name=ssid "
                                  "onchange=\"document.getElementById('other').style.display="
                                  "this.value==''?'block':'none'\">");
    for (int i = 0; i < n_aps; i++) {
        httpd_resp_sendstr_chunk(req, "<option value=\"");
        send_escaped(req, aps[i].ssid);
        httpd_resp_sendstr_chunk(req, "\">");
        send_escaped(req, aps[i].ssid);
        httpd_resp_sendstr_chunk(req, "</option>");
    }
    httpd_resp_sendstr_chunk(req, "<option value=''>Annat nätverk…</option></select>"
                                  "<input id=other name=other placeholder='Nätverkets namn' "
                                  "autocapitalize=off autocorrect=off>"
                                  "<label for=pw>Lösenord</label>"
                                  "<input id=pw name=password type=password autocomplete=off>"
                                  "<button>Anslut</button></form>");
    if (n_aps == 0) {
        httpd_resp_sendstr_chunk(req, "<script>document.getElementById('other')"
                                      ".style.display='block'</script>");
    }
    /* Send with fetch() rather than a form post: iOS warns that a password
     * form on a plain http page "is not secure", and in the captive portal
     * sheet "Send Anyway" then never sends it. The response replaces the
     * page as if it had been navigated to. */
    httpd_resp_sendstr_chunk(req, "<script>"
        "document.querySelector('form').onsubmit=function(e){e.preventDefault();"
        "var b=this.querySelector('button');b.disabled=true;b.textContent='Skickar…';"
        "fetch('/connect',{method:'POST',body:new URLSearchParams(new FormData(this))})"
        ".then(function(r){return r.text()}).then(function(h){"
        "document.open();document.write(h);document.close()})"
        ".catch(function(){document.querySelector('h1').textContent='Titta på tavlan';"
        "document.querySelector('p').textContent='Skärmen visar om anslutningen lyckades.'})}"
        "</script>");
    httpd_resp_sendstr_chunk(req, "</main></body></html>");
    return httpd_resp_sendstr_chunk(req, NULL);
}

/* In-place decoding of application/x-www-form-urlencoded values. */
static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') {
            *o++ = ' ';
        } else if (*s == '%' && s[1] && s[2]) {
            char hex[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            s += 2;
        } else {
            *o++ = *s;
        }
    }
    *o = '\0';
}

static bool form_value(const char *body, const char *key, char *out, size_t len)
{
    /* Encoded values can be up to 3x longer than decoded. */
    char raw[200];
    if (httpd_query_key_value(body, key, raw, sizeof(raw)) != ESP_OK) {
        out[0] = '\0';
        return false;
    }
    url_decode(raw);
    if (strlen(raw) >= len) {
        return false;
    }
    strcpy(out, raw);
    return true;
}

static esp_err_t connect_post(httpd_req_t *req)
{
    char body[512];
    int len = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, body + got, len - got);
        if (r <= 0) {
            return ESP_FAIL;
        }
        got += r;
    }
    body[got] = '\0';

    char ssid[33], other[33], pass[65];
    form_value(body, "ssid", ssid, sizeof(ssid));
    form_value(body, "other", other, sizeof(other));
    form_value(body, "password", pass, sizeof(pass));
    if (ssid[0] == '\0') {
        strcpy(ssid, other);
    }
    if (ssid[0] == '\0' || (pass[0] && strlen(pass) < 8)) {
        httpd_resp_set_status(req, "303 See Other");
        httpd_resp_set_hdr(req, "Location", "/");
        return httpd_resp_sendstr(req, "");
    }

    ESP_LOGI(TAG, "Got network \"%s\" from the phone", ssid);
    xSemaphoreTake(mutex, portMAX_DELAY);
    strlcpy(pending_ssid, ssid, sizeof(pending_ssid));
    strlcpy(pending_pass, pass, sizeof(pending_pass));
    xSemaphoreGive(mutex);
    xTaskNotify(task_handle, CMD_CONNECT, eSetBits);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, PAGE_HEAD);
    httpd_resp_sendstr_chunk(req, "<h1 id=t>Ansluter till ");
    send_escaped(req, ssid);
    httpd_resp_sendstr_chunk(req, "…</h1><p id=m>Det tar upp till 20 sekunder. "
        "Mobilen kan tappa anslutningen under tiden, titta då på tavlans skärm.</p>"
        "<script>"
        "var t=document.getElementById('t'),m=document.getElementById('m'),n=0;"
        "function poll(){fetch('/status').then(r=>r.json()).then(s=>{"
        "if(s.state=='done'){t.textContent='Klart!';"
        "m.textContent='Tavlan är ansluten. Du kan stänga sidan.';return}"
        "if(s.state=='failed'){location='/';return}"
        "setTimeout(poll,1000)}).catch(()=>{if(++n>30){t.textContent='Titta på tavlan';"
        "m.textContent='Skärmen visar om anslutningen lyckades.';return}"
        "setTimeout(poll,1000)})}setTimeout(poll,1500)"
        "</script></main></body></html>");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t status_get(httpd_req_t *req)
{
    setup_status_t st;
    wifi_setup_get_status(&st);
    const char *s = st.state == SETUP_CONNECTING ? "connecting"
                  : st.state == SETUP_DONE ? "done"
                  : st.last_failed ? "failed" : "idle";
    char json[48];
    snprintf(json, sizeof(json), "{\"state\":\"%s\"}", s);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

/* Phones probe well-known URLs to detect captive portals; answering every
 * unknown URL with a redirect makes them open the setup page by themselves. */
static esp_err_t redirect_404(httpd_req_t *req, httpd_err_code_t err)
{
    char host[64] = "";
    httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
    ESP_LOGI(TAG, "Redirecting %s%s to the setup page", host, req->uri);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", PORTAL_URL "/");
    /* iOS needs a body to treat the response as a portal. */
    return httpd_resp_sendstr(req, "Redirect to the setup page");
}

static void start_portal(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets = 5;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 6144;
    if (httpd_start(&httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Could not start web server");
        return;
    }
    static const httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_get };
    static const httpd_uri_t conn = { .uri = "/connect", .method = HTTP_POST, .handler = connect_post };
    static const httpd_uri_t stat = { .uri = "/status", .method = HTTP_GET, .handler = status_get };
    httpd_register_uri_handler(httpd, &root);
    httpd_register_uri_handler(httpd, &conn);
    httpd_register_uri_handler(httpd, &stat);
    httpd_register_err_handler(httpd, HTTPD_404_NOT_FOUND, redirect_404);

    /* DHCP option 114 tells newer phones the portal address directly. */
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey(BOARD_AP_NETIF_KEY);
    static char uri[] = PORTAL_URL;
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
    if (httpd) {
        httpd_stop(httpd);
        httpd = NULL;
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
    /* Captive portal probes produce a stream of harmless warnings. */
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_parse", ESP_LOG_ERROR);
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
