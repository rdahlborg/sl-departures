/* The board's web server, used from a phone.
 *
 *   While Wi-Fi setup is running (on the board's hotspot) it is a captive
 *   portal: every unknown URL redirects to the setup page, which asks for
 *   the home network and the line and station to show.
 *
 *   The rest of the time it serves a settings page on the home network for
 *   changing line and station. Wi-Fi can only be changed through setup, so
 *   someone else on the network can at most change the station.
 *
 * Forms are sent with fetch(): iOS warns that a password form on a plain
 * http page "is not secure", and in the captive portal sheet "Send Anyway"
 * then never sends it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "controls.h"
#include "metro_data.h"
#include "sl_api.h"
#include "wifi_setup.h"
#include "web.h"

static const char *TAG = "web";

static const char PAGE_HEAD[] =
    "<!doctype html><html lang=sv><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>SL Departures</title><style>"
    "body{font:17px/1.5 system-ui,sans-serif;margin:0;background:#0b1410;color:#e9f0ec}"
    "main{max-width:420px;margin:0 auto;padding:28px 18px}"
    "h1{font-size:1.4em;margin:0 0 4px}p{color:#93a59c;margin:0 0 20px}"
    "h2{font-size:1.05em;margin:28px 0 0}"
    "label{display:block;font-weight:600;margin:16px 0 6px}"
    "select,input{width:100%;box-sizing:border-box;font:inherit;padding:12px;border-radius:8px;"
    "border:1px solid #2c4237;background:#13201a;color:inherit}"
    "button{width:100%;margin-top:24px;padding:14px;font:inherit;font-weight:700;border:0;"
    "border-radius:8px;background:#179c4c;color:#fff}button:disabled{opacity:.6}"
    ".bar{display:flex;gap:6px;margin-bottom:18px}.bar i{width:28px;height:6px;border-radius:3px}"
    "#other{display:none}.err{color:#ff8a8f}.ok{color:#5fd38d}"
    "</style></head><body><main>"
    "<div class=bar><i style=background:#179c4c></i><i style=background:#d71d24></i>"
    "<i style=background:#0089ca></i></div>";

static const char PAGE_TAIL[] =
    "<p style='margin-top:40px;font-size:.85em'>SL Departures av " CONFIG_APP_AUTHOR
    " · <a style='color:#93a59c' href='https://github.com/" CONFIG_OTA_GITHUB_REPO "'>GitHub</a>"
    " · Data: Trafiklab.se</p></main></body></html>";

/* ---- helpers -------------------------------------------------------- */

static void send_escaped(httpd_req_t *req, const char *s)
{
    char buf[2] = { 0 };
    for (; *s; s++) {
        switch (*s) {
        case '&':  httpd_resp_sendstr_chunk(req, "&amp;"); break;
        case '<':  httpd_resp_sendstr_chunk(req, "&lt;"); break;
        case '>':  httpd_resp_sendstr_chunk(req, "&gt;"); break;
        case '"':  httpd_resp_sendstr_chunk(req, "&quot;"); break;
        case '\'': httpd_resp_sendstr_chunk(req, "&#39;"); break;
        default:
            buf[0] = *s;
            httpd_resp_sendstr_chunk(req, buf);
        }
    }
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
    out[0] = '\0';
    if (httpd_query_key_value(body, key, raw, sizeof(raw)) != ESP_OK) {
        return false;
    }
    url_decode(raw);
    if (strlen(raw) >= len) {
        return false;
    }
    strcpy(out, raw);
    return true;
}

static bool read_body(httpd_req_t *req, char *body, size_t size)
{
    int len = req->content_len < (int)size - 1 ? req->content_len : (int)size - 1;
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, body + got, len - got);
        if (r <= 0) {
            return false;
        }
        got += r;
    }
    body[got] = '\0';
    return true;
}

static bool station_on_group(int group, uint16_t site)
{
    const metro_group_t *g = &metro_groups[group];
    for (int i = 0; i < g->n_stations; i++) {
        if (g->stations[i] == site) {
            return true;
        }
    }
    return false;
}

/* Apply group/site fields from a form, if present and valid. */
static bool apply_station(const char *body)
{
    char g[4], s[8];
    if (!form_value(body, "group", g, sizeof(g)) || !form_value(body, "site", s, sizeof(s))) {
        return false;
    }
    if (!metro_ready()) {
        return false;
    }
    int group = atoi(g);
    uint16_t site = atoi(s);
    if (group < 0 || group >= METRO_N_GROUPS || !station_on_group(group, site)) {
        return false;
    }
    controls_select(group, site);
    ESP_LOGI(TAG, "Station set from phone: %s", metro_station_name(site));
    return true;
}

/* Line and station menus. The station list follows the chosen line. */
static void send_station_picker(httpd_req_t *req)
{
    if (!metro_ready()) {
        httpd_resp_sendstr_chunk(req, "<p>Stationslistan hämtas när tavlan är ansluten. "
                                      "Du kan välja station sedan.</p>");
        return;
    }
    sl_config_t cfg;
    sl_get_config(&cfg);
    char buf[64];

    httpd_resp_sendstr_chunk(req, "<label for=g>Linje</label><select id=g name=group></select>"
                                  "<label for=s>Station</label><select id=s name=site></select>"
                                  "<script>var L=[");
    for (int gi = 0; gi < METRO_N_GROUPS; gi++) {
        const metro_group_t *g = &metro_groups[gi];
        snprintf(buf, sizeof(buf), "%s{n:\"%s\",s:[", gi ? "," : "", g->name);
        httpd_resp_sendstr_chunk(req, buf);
        for (int i = 0; i < g->n_stations; i++) {
            /* Station names are plain text without quotes or backslashes. */
            snprintf(buf, sizeof(buf), "%s[%u,\"%s\"]", i ? "," : "", g->stations[i],
                     metro_station_name(g->stations[i]));
            httpd_resp_sendstr_chunk(req, buf);
        }
        httpd_resp_sendstr_chunk(req, "]}");
    }
    snprintf(buf, sizeof(buf), "],G=%d,S=%u;", cfg.group, cfg.home);
    httpd_resp_sendstr_chunk(req, buf);
    httpd_resp_sendstr_chunk(req,
        "var g=document.getElementById('g'),s=document.getElementById('s');"
        "L.forEach(function(l,i){g.add(new Option(l.n,i))});"
        "function fill(site){s.length=0;L[g.value].s.forEach(function(x){"
        "s.add(new Option(x[1],x[0],false,x[0]==site))})}"
        "g.value=G;fill(S);g.onchange=function(){fill(S)};"
        "</script>");
}

/* Submit a form with fetch() and hand the JSON answer to done(). */
static const char SUBMIT_JS[] =
    "function submit(f,busy,done){f.onsubmit=function(e){e.preventDefault();"
    "var b=f.querySelector('button'),t=b.textContent;b.disabled=true;b.textContent=busy;"
    "fetch(f.action,{method:'POST',body:new URLSearchParams(new FormData(f))})"
    ".then(function(r){return r.json()}).then(function(j){b.disabled=false;b.textContent=t;done(j)})"
    ".catch(function(){b.disabled=false;b.textContent=t;done(null)})}}";

/* ---- Wi-Fi setup (captive portal) ----------------------------------- */

static esp_err_t setup_page(httpd_req_t *req)
{
    setup_status_t st;
    wifi_setup_get_status(&st);
    const board_ap_t *aps;
    int n = wifi_setup_networks(&aps);
    ESP_LOGI(TAG, "Setup page requested");

    httpd_resp_sendstr_chunk(req, PAGE_HEAD);
    httpd_resp_sendstr_chunk(req, "<div id=form><h1>Ställ in tavlan</h1>"
                                  "<p>Välj ditt Wi-Fi och vilken station tavlan ska visa.</p>");
    if (st.last_failed) {
        httpd_resp_sendstr_chunk(req, "<p class=err>Kunde inte ansluta till ");
        send_escaped(req, st.target);
        httpd_resp_sendstr_chunk(req, ". Kontrollera lösenordet och försök igen.</p>");
    }
    httpd_resp_sendstr_chunk(req, "<form method=post action=/connect>"
                                  "<label for=ssid>Wi-Fi-nätverk</label><select id=ssid name=ssid "
                                  "onchange=\"document.getElementById('other').style.display="
                                  "this.value==''?'block':'none'\">");
    for (int i = 0; i < n; i++) {
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
                                  "<input id=pw name=password type=password autocomplete=off>");
    send_station_picker(req);
    httpd_resp_sendstr_chunk(req, "<button>Anslut</button></form></div>"
        "<div id=wait hidden><h1 id=t></h1><p id=m>Det tar upp till 20 sekunder. "
        "Mobilen kan tappa anslutningen under tiden, titta då på tavlans skärm.</p></div>");
    if (n == 0) {
        httpd_resp_sendstr_chunk(req, "<script>document.getElementById('other')"
                                      ".style.display='block'</script>");
    }
    httpd_resp_sendstr_chunk(req, "<script>");
    httpd_resp_sendstr_chunk(req, SUBMIT_JS);
    httpd_resp_sendstr_chunk(req,
        "var T=document.getElementById('t'),M=document.getElementById('m'),n=0;"
        "function show(h,m){document.getElementById('form').hidden=true;"
        "document.getElementById('wait').hidden=false;T.textContent=h;if(m)M.textContent=m}"
        "function lost(){show('Titta på tavlan','Skärmen visar om anslutningen lyckades.')}"
        "function poll(){fetch('/status').then(function(r){return r.json()}).then(function(s){"
        "if(s.state=='done'){show('Klart!','Tavlan är ansluten. Du kan stänga sidan.');return}"
        "if(s.state=='failed'){location='/';return}setTimeout(poll,1000)})"
        ".catch(function(){if(++n>30){lost();return}setTimeout(poll,1000)})}"
        "submit(document.querySelector('form'),'Skickar…',function(j){"
        "if(!j){lost();return}if(!j.ok){location='/';return}"
        "show('Ansluter till '+j.ssid+'…');setTimeout(poll,1500)})"
        "</script>");
    httpd_resp_sendstr_chunk(req, PAGE_TAIL);
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t connect_post(httpd_req_t *req)
{
    char body[512];
    if (!wifi_setup_active() || !read_body(req, body, sizeof(body))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);
    }
    char ssid[33], other[33], pass[65];
    form_value(body, "ssid", ssid, sizeof(ssid));
    form_value(body, "other", other, sizeof(other));
    form_value(body, "password", pass, sizeof(pass));
    if (ssid[0] == '\0') {
        strcpy(ssid, other);
    }
    httpd_resp_set_type(req, "application/json");
    if (ssid[0] == '\0' || (pass[0] && strlen(pass) < 8)) {
        return httpd_resp_sendstr(req, "{\"ok\":false}");
    }
    /* Show the chosen station as soon as the board is online. */
    apply_station(body);
    wifi_setup_submit(ssid, pass);

    httpd_resp_sendstr_chunk(req, "{\"ok\":true,\"ssid\":\"");
    /* JSON-escape the network name. */
    char c[7];
    for (const char *p = ssid; *p; p++) {
        if (*p == '"' || *p == '\\') {
            snprintf(c, sizeof(c), "\\%c", *p);
        } else if ((unsigned char)*p < 0x20) {
            snprintf(c, sizeof(c), "\\u%04x", *p);
        } else {
            c[0] = *p;
            c[1] = '\0';
        }
        httpd_resp_sendstr_chunk(req, c);
    }
    httpd_resp_sendstr_chunk(req, "\"}");
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

/* ---- settings on the home network ----------------------------------- */

static esp_err_t settings_page(httpd_req_t *req)
{
    httpd_resp_sendstr_chunk(req, PAGE_HEAD);
    httpd_resp_sendstr_chunk(req, "<h1>SL Departures</h1>"
                                  "<p>Välj vilken station tavlan ska visa.</p>"
                                  "<form method=post action=/station>");
    send_station_picker(req);
    httpd_resp_sendstr_chunk(req, "<button>Visa på tavlan</button></form>"
                                  "<p id=r style='margin-top:16px'></p>"
                                  "<h2>Byta Wi-Fi?</h2><p>Håll knappen på tavlan, gå till "
                                  "<b>Wi-Fi…</b> sist i listan och håll igen.</p><script>");
    httpd_resp_sendstr_chunk(req, SUBMIT_JS);
    httpd_resp_sendstr_chunk(req,
        "var R=document.getElementById('r');"
        "submit(document.querySelector('form'),'Sparar…',function(j){"
        "if(j&&j.ok){R.className='ok';R.textContent='Tavlan visar nu '+j.name+'.';S=+s.value}"
        "else{R.className='err';R.textContent='Det gick inte. Försök igen.'}})"
        "</script>");
    httpd_resp_sendstr_chunk(req, PAGE_TAIL);
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t station_post(httpd_req_t *req)
{
    char body[128];
    httpd_resp_set_type(req, "application/json");
    if (!read_body(req, body, sizeof(body)) || !apply_station(body)) {
        return httpd_resp_sendstr(req, "{\"ok\":false}");
    }
    sl_config_t cfg;
    sl_get_config(&cfg);
    char json[80];
    snprintf(json, sizeof(json), "{\"ok\":true,\"name\":\"%s\"}", metro_station_name(cfg.home));
    return httpd_resp_sendstr(req, json);
}

/* ---- routing -------------------------------------------------------- */

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return wifi_setup_active() ? setup_page(req) : settings_page(req);
}

/* During setup, phones probe well-known URLs to detect captive portals;
 * redirecting every unknown URL makes them open the setup page. */
static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t err)
{
    if (!wifi_setup_active()) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
    }
    char host[64] = "";
    httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
    ESP_LOGI(TAG, "Redirecting %s%s to the setup page", host, req->uri);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", WIFI_SETUP_PORTAL_URL "/");
    /* iOS needs a body to treat the response as a portal. */
    return httpd_resp_sendstr(req, "Redirect to the setup page");
}

void web_start(void)
{
    /* Captive portal probes produce a stream of harmless warnings. */
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_parse", ESP_LOG_ERROR);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets = 5;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 6144;
    httpd_handle_t httpd;
    if (httpd_start(&httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Could not start web server");
        return;
    }
    static const httpd_uri_t routes[] = {
        { .uri = "/",        .method = HTTP_GET,  .handler = root_get },
        { .uri = "/connect", .method = HTTP_POST, .handler = connect_post },
        { .uri = "/status",  .method = HTTP_GET,  .handler = status_get },
        { .uri = "/station", .method = HTTP_POST, .handler = station_post },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(httpd, &routes[i]);
    }
    httpd_register_err_handler(httpd, HTTPD_404_NOT_FOUND, not_found);
}
