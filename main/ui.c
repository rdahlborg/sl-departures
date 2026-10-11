/* Departure board, 320x172 landscape.
 *
 *   header      station name, line badge, status dot, clock
 *   line map    four stations either side, trains placed from live data
 *   departures  next two trains in each direction
 *
 * The map is rebuilt whenever the selected station changes.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "lvgl.h"
#include "board.h"
#include "sl_api.h"
#include "ui.h"
#include "wifi_setup.h"
#include "metro_data.h"
#include "ota.h"

LV_FONT_DECLARE(sl_font_12);
LV_FONT_DECLARE(sl_font_16);
LV_FONT_DECLARE(sl_font_22);

#define COL_BG        0x0B1410
#define COL_TEXT      0xFFFFFF
#define COL_DIM       0x8FA39A
#define COL_TRACK     0x2C4237
#define COL_OK        0x2BC46B
#define COL_WARN      0xF5A623
#define COL_ERR       0xE5484D

#define MAP_CX        160        /* x of the home station */
#define MAP_SPACING   35         /* px between stations */
#define MAP_SHOWN     4          /* stations drawn on each side */
#define LABEL_MAX_W   (2 * MAP_SPACING - 4)
#define TRACK_R_Y     55         /* rightbound track */
#define TRACK_L_Y     71         /* leftbound track */
#define LIST_Y        100
#define LIST_ROWS     2
#define STALE_S       90

static lv_obj_t *title_label;
static lv_obj_t *line_badge, *line_badge_label;
static lv_obj_t *clock_label;
static lv_obj_t *status_dot;
static lv_obj_t *map_layer;
static lv_obj_t *markers[2][SL_MAX_TRAINS];
static lv_obj_t *marker_labels[2][SL_MAX_TRAINS];
/* Track drawn from the last station a train passed up to the train. */
static lv_obj_t *trails[2][SL_MAX_TRAINS];
/* Where each marker is drawn, easing towards the computed position, and
 * which train it shows, so a marker handed to another train jumps. */
static float marker_x[2][SL_MAX_TRAINS];
static int64_t marker_journey[2][SL_MAX_TRAINS];
static lv_obj_t *col_header[2];
static lv_obj_t *placeholder;
static lv_obj_t *picker, *picker_roller, *picker_title;
static lv_obj_t *setup_panel, *setup_qr, *setup_text, *setup_note;
static lv_obj_t *phone_panel, *phone_qr, *phone_text;
static lv_obj_t *boot_panel, *boot_status;
static lv_obj_t *about_panel, *about_text;
static lv_obj_t *update_panel, *update_title, *update_bar, *update_note;
static uint32_t boot_started;       /* lv_tick when the UI started */
static uint32_t phone_generation;   /* station shown when the QR code opened */

typedef struct {
    lv_obj_t *badge, *badge_label, *dest, *mins;
} row_t;
static row_t rows[2][LIST_ROWS];

static sl_config_t shown;       /* config the map was built for */

static void update_setup(void);
static void update_boot(void);
static void update_about(void);
static void update_progress(void);
static uint32_t line_color;

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, int radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    return o;
}

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *s)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, s);
    return l;
}

/* Fixed-size single-line label; text that does not fit ends in "…". */
static void one_line(lv_obj_t *l, int w)
{
    lv_obj_set_size(l, w, lv_font_get_line_height(lv_obj_get_style_text_font(l, 0)));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
}

/* Station label centred on x where possible, kept within [min_x, max_x]
 * and shortened to fit. Returns the label so callers can read its extent. */
static lv_obj_t *station_label(lv_obj_t *parent, const char *name, int x, int y,
                               int min_x, int max_x, bool home)
{
    lv_obj_t *l = text(parent, &sl_font_12, home ? COL_TEXT : COL_DIM, name);
    lv_obj_update_layout(l);
    int w = lv_obj_get_width(l);
    int max_w = home ? 2 * LABEL_MAX_W : LABEL_MAX_W;
    if (w > max_w) w = max_w;
    if (w > max_x - min_x) w = max_x - min_x;
    int lx = x - w / 2;
    if (lx < min_x) lx = min_x;
    if (lx + w > max_x) lx = max_x - w;
    one_line(l, w);
    lv_obj_set_pos(l, lx, y);
    return l;
}

static float map_xf(int dir, float pos)
{
    return dir == SL_RIGHTBOUND ? MAP_CX - pos * MAP_SPACING : MAP_CX + pos * MAP_SPACING;
}

static int map_x(int dir, float pos)
{
    /* Rightbound trains arrive from the left. */
    return dir == SL_RIGHTBOUND ? MAP_CX - (int)(pos * MAP_SPACING)
                                : MAP_CX + (int)(pos * MAP_SPACING);
}

static void build_map(const sl_config_t *c)
{
    lv_obj_clean(map_layer);
    line_color = metro_groups[c->group].color;

    box(map_layer, 0, TRACK_R_Y - 1, LCD_H_RES, 3, COL_TRACK, 0);
    box(map_layer, 0, TRACK_L_Y - 1, LCD_H_RES, 3, COL_TRACK, 0);

    /* Tracks stop at a terminus. */
    for (int dir = 0; dir < 2; dir++) {
        if (c->n_up[dir] == 0) {
            int x = dir == SL_RIGHTBOUND ? 0 : MAP_CX + 8;
            box(map_layer, x, TRACK_R_Y - 2, MAP_CX - 8, TRACK_L_Y - TRACK_R_Y + 5, COL_BG, 0);
        }
    }

    /* Labels alternate above and below the tracks. They are placed from
     * the home station outwards; each one must stay clear of the previous
     * label on the same row and of the screen edge. */
    const int row_y[2] = { TRACK_L_Y + 9, TRACK_R_Y - 25 };   /* k even, k odd */
    lv_obj_t *home_l = station_label(map_layer, metro_station_name(c->home), MAP_CX, row_y[0],
                                     2, LCD_H_RES - 2, true);
    lv_obj_update_layout(home_l);
    int home_x1 = lv_obj_get_x(home_l), home_x2 = home_x1 + lv_obj_get_width(home_l);

    for (int dir = 0; dir < 2; dir++) {
        bool left = dir == SL_RIGHTBOUND;
        /* Inner edge each row's labels must stay outside of. */
        int bound[2] = { left ? home_x1 - 4 : home_x2 + 4, left ? MAP_CX - 2 : MAP_CX + 2 };
        for (int k = 1; k <= c->n_up[dir] && k <= MAP_SHOWN; k++) {
            int x = map_x(dir, k);
            lv_obj_t *p = box(map_layer, x - 4, TRACK_R_Y - 6, 8, TRACK_L_Y - TRACK_R_Y + 12, COL_BG, 4);
            lv_obj_set_style_border_color(p, lv_color_hex(line_color), 0);
            lv_obj_set_style_border_width(p, 2, 0);
            int row = k % 2;
            const char *name = metro_station_name(c->up[dir][k - 1]);
            lv_obj_t *l = left ? station_label(map_layer, name, x, row_y[row], 2, bound[row], false)
                               : station_label(map_layer, name, x, row_y[row], bound[row], LCD_H_RES - 2, false);
            lv_obj_update_layout(l);
            bound[row] = left ? lv_obj_get_x(l) - 6 : lv_obj_get_x(l) + lv_obj_get_width(l) + 6;
        }
    }
    box(map_layer, MAP_CX - 6, TRACK_R_Y - 8, 12, TRACK_L_Y - TRACK_R_Y + 16, COL_TEXT, 6);

    /* Trails first, so they are drawn under the trains. */
    for (int dir = 0; dir < 2; dir++) {
        int y = dir == SL_RIGHTBOUND ? TRACK_R_Y : TRACK_L_Y;
        for (int i = 0; i < SL_MAX_TRAINS; i++) {
            trails[dir][i] = box(map_layer, 0, y - 1, 0, 3, line_color, 1);
            lv_obj_set_hidden(trails[dir][i], true);
        }
    }
    for (int dir = 0; dir < 2; dir++) {
        for (int i = 0; i < SL_MAX_TRAINS; i++) {
            marker_journey[dir][i] = 0;
            lv_obj_t *m = box(map_layer, 0, 0, 22, 14, line_color, 4);
            lv_obj_set_style_border_color(m, lv_color_hex(COL_TEXT), 0);
            lv_obj_set_style_border_width(m, 1, 0);
            lv_obj_t *l = text(m, &sl_font_12, COL_TEXT, "");
            lv_obj_center(l);
            lv_obj_set_hidden(m, true);
            markers[dir][i] = m;
            marker_labels[dir][i] = l;
        }
    }

    lv_label_set_text(title_label, metro_station_name(c->home));
    lv_obj_set_style_bg_color(line_badge, lv_color_hex(line_color), 0);
    lv_label_set_text_fmt(line_badge_label, "%d", c->line);
    lv_obj_align_to(line_badge, title_label, LV_ALIGN_OUT_RIGHT_MID, 6, 1);

    for (int dir = 0; dir < 2; dir++) {
        const char *end = metro_station_name(c->end[dir]);
        bool terminus = c->home == c->end[dir];
        if (dir == SL_LEFTBOUND) {
            lv_label_set_text_fmt(col_header[dir], terminus ? "" : "← %s", end);
        } else {
            lv_label_set_text_fmt(col_header[dir], terminus ? "" : "%s →", end);
        }
        for (int r = 0; r < LIST_ROWS; r++) {
            lv_obj_set_style_bg_color(rows[dir][r].badge, lv_color_hex(line_color), 0);
        }
    }
    shown = *c;
}

static void build_list(lv_obj_t *scr)
{
    box(scr, 8, LIST_Y - 4, LCD_H_RES - 16, 1, COL_TRACK, 0);
    box(scr, LCD_H_RES / 2, LIST_Y + 2, 1, LCD_V_RES - LIST_Y - 6, COL_TRACK, 0);

    /* Left column: trains heading left on the map, right column: right. */
    const int col_x[2] = { [SL_LEFTBOUND] = 6, [SL_RIGHTBOUND] = LCD_H_RES / 2 + 6 };
    for (int dir = 0; dir < 2; dir++) {
        col_header[dir] = text(scr, &sl_font_12, COL_DIM, "");
        lv_obj_set_pos(col_header[dir], col_x[dir], LIST_Y);
        one_line(col_header[dir], LCD_H_RES / 2 - 10);

        for (int r = 0; r < LIST_ROWS; r++) {
            int y = LIST_Y + 18 + r * 26;
            row_t *row = &rows[dir][r];
            row->badge = box(scr, col_x[dir], y + 2, 22, 16, COL_TRACK, 4);
            row->badge_label = text(row->badge, &sl_font_12, COL_TEXT, "");
            lv_obj_center(row->badge_label);
            row->dest = text(scr, &sl_font_12, COL_TEXT, "");
            lv_obj_set_pos(row->dest, col_x[dir] + 27, y + 3);
            one_line(row->dest, 62);
            /* Wide enough for "59 min". */
            row->mins = text(scr, &sl_font_16, COL_TEXT, "");
            one_line(row->mins, 60);
            lv_obj_set_style_text_align(row->mins, LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_set_pos(row->mins, col_x[dir] + 88, y);
        }
    }
    placeholder = text(scr, &sl_font_12, COL_DIM, "");
    lv_obj_set_width(placeholder, LCD_H_RES - 20);
    lv_obj_set_style_text_align(placeholder, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(placeholder, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(placeholder, LV_OPA_COVER, 0);
    lv_obj_align(placeholder, LV_ALIGN_TOP_MID, 0, LIST_Y + 22);
}

static void format_mins(char *buf, size_t len, const sl_train_t *tr, time_t now)
{
    if (tr->cancelled) {
        snprintf(buf, len, "Inst.");
        return;
    }
    int m = (int)((difftime(tr->t[0], now) + 30) / 60);
    if (m <= 0) {
        snprintf(buf, len, "Nu");
    } else if (m < 60) {
        snprintf(buf, len, "%d min", m);
    } else {
        struct tm tm;
        localtime_r(&tr->t[0], &tm);
        strftime(buf, len, "%H:%M", &tm);
    }
}

/* A train between stations has a direction arrow and a trail back to the
 * station it left; at a platform it has neither, and its border blinks
 * while it is boarding. Markers ease towards their position, so they glide
 * rather than jump when new data arrives. */
static void draw_train(int dir, int i, const sl_train_t *tr, float pos)
{
    int y = dir == SL_RIGHTBOUND ? TRACK_R_Y : TRACK_L_Y;
    float target = map_xf(dir, pos);
    float *x = &marker_x[dir][i];
    if (marker_journey[dir][i] != tr->journey || fabsf(target - *x) > MAP_SPACING) {
        *x = target;
        marker_journey[dir][i] = tr->journey;
    } else {
        *x += (target - *x) * 0.25f;
    }

    bool moving = pos - floorf(pos) > 0.02f;
    int w = moving ? 30 : 22;
    lv_obj_t *m = markers[dir][i];
    lv_obj_set_size(m, w, 14);
    lv_obj_set_pos(m, (int)(*x + 0.5f) - w / 2, y - 7);
    if (!moving) {
        lv_label_set_text_fmt(marker_labels[dir][i], "%d", tr->line);
    } else if (dir == SL_RIGHTBOUND) {
        lv_label_set_text_fmt(marker_labels[dir][i], "%d→", tr->line);
    } else {
        lv_label_set_text_fmt(marker_labels[dir][i], "←%d", tr->line);
    }
    lv_obj_center(marker_labels[dir][i]);

    /* Boarding: blink the border once a second. */
    bool boarding = !moving && (tr->at_stop & (1 << (int)pos));
    lv_obj_set_style_border_opa(m, boarding && (lv_tick_get() / 500) % 2 ? LV_OPA_TRANSP
                                                                        : LV_OPA_COVER, 0);

    if (moving) {
        /* From the station behind the train (or the screen edge) to it. */
        float from = map_xf(dir, ceilf(pos));
        if (from < 0) from = 0;
        if (from > LCD_H_RES) from = LCD_H_RES;
        int x0 = (int)fminf(from, *x), x1 = (int)fmaxf(from, *x);
        lv_obj_set_pos(trails[dir][i], x0, y - 1);
        lv_obj_set_width(trails[dir][i], x1 - x0);
        lv_obj_set_hidden(trails[dir][i], x1 - x0 < 2);
    }
}

static void update_cb(lv_timer_t *t)
{
    time_t now = time(NULL);
    char buf[16];

    update_setup();
    update_boot();
    update_about();
    update_progress();

    sl_config_t cfg;
    sl_get_config(&cfg);
    if (cfg.generation != shown.generation) {
        build_map(&cfg);
    }
    /* A station was chosen on the phone: show it. */
    if (ui_phone_visible() && cfg.generation != phone_generation) {
        ui_phone_hide();
    }

    if (board_time_valid()) {
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(buf, sizeof(buf), "%H:%M", &tm);
        lv_label_set_text(clock_label, buf);
    }

    static sl_state_t st;
    bool have = sl_snapshot(&st);
    uint32_t dot = !board_wifi_connected() ? COL_ERR
                 : !have || difftime(now, st.updated) > STALE_S ? COL_WARN : COL_OK;
    lv_obj_set_style_bg_color(status_dot, lv_color_hex(dot), 0);
    /* SL's terms ask for the data source to be credited. */
    lv_label_set_text(placeholder, board_wifi_configured()
        ? "Hämtar avgångar…\nData: Trafiklab.se"
        : "Wi-Fi saknas. Håll knappen och välj Wi-Fi…\neller öppna " CONFIG_INSTALLER_URL);
    lv_obj_set_hidden(placeholder, have);

    for (int dir = 0; dir < 2; dir++) {
        const sl_dir_t *d = &st.dir[dir];

        for (int i = 0; i < SL_MAX_TRAINS; i++) {
            const sl_train_t *tr = &d->trains[i];
            float pos = have && i < d->n ? sl_train_position(tr, d->n_up, now) : -1;
            /* Allow half a spacing past the last drawn station so trains
             * slide in from the edge. */
            bool show = pos >= 0 && pos <= MAP_SHOWN + 0.6f;
            lv_obj_set_hidden(markers[dir][i], !show);
            lv_obj_set_hidden(trails[dir][i], true);
            if (!show) {
                marker_journey[dir][i] = 0;
                continue;
            }
            draw_train(dir, i, tr, pos);
        }

        /* Departures list: skip trains that have already left. */
        int r = 0;
        for (int i = 0; have && i < d->n && r < LIST_ROWS; i++) {
            const sl_train_t *tr = &d->trains[i];
            if (difftime(tr->t[0], now) < -30) {
                continue;
            }
            row_t *row = &rows[dir][r++];
            lv_obj_set_hidden(row->badge, false);
            lv_label_set_text_fmt(row->badge_label, "%d", tr->line);
            lv_label_set_text(row->dest, tr->dest);
            format_mins(buf, sizeof(buf), tr, now);
            lv_label_set_text(row->mins, buf);
            lv_obj_set_style_text_color(row->mins,
                lv_color_hex(tr->cancelled ? COL_ERR : COL_TEXT), 0);
        }
        for (; r < LIST_ROWS; r++) {
            lv_obj_set_hidden(rows[dir][r].badge, true);
            lv_label_set_text(rows[dir][r].dest, "");
            lv_label_set_text(rows[dir][r].mins, "");
        }
    }
}

static void build_setup(lv_obj_t *scr)
{
    setup_panel = box(scr, 0, 0, LCD_H_RES, LCD_V_RES, COL_BG, 0);

    setup_qr = lv_qrcode_create(setup_panel);
    lv_qrcode_set_size(setup_qr, 132);
    lv_qrcode_set_dark_color(setup_qr, lv_color_black());
    lv_qrcode_set_light_color(setup_qr, lv_color_white());
    lv_qrcode_set_quiet_zone(setup_qr, true);
    lv_obj_set_pos(setup_qr, 12, 20);

    lv_obj_t *title = text(setup_panel, &sl_font_16, COL_TEXT, "Ställ in Wi-Fi");
    lv_obj_set_pos(title, 158, 10);

    setup_text = text(setup_panel, &sl_font_12, COL_TEXT, "");
    lv_obj_set_width(setup_text, LCD_H_RES - 166);
    lv_obj_set_pos(setup_text, 158, 34);

    setup_note = text(setup_panel, &sl_font_12, COL_DIM, "");
    lv_obj_set_width(setup_note, LCD_H_RES - 166);
    lv_obj_align(setup_note, LV_ALIGN_BOTTOM_LEFT, 158, -8);
    lv_obj_set_hidden(setup_panel, true);
}

static void show_qr(const char *data)
{
    static char shown_qr[128];
    lv_obj_set_hidden(setup_qr, data == NULL);
    if (data && strcmp(data, shown_qr) != 0) {
        strlcpy(shown_qr, data, sizeof(shown_qr));
        lv_qrcode_update(setup_qr, data, strlen(data));
    }
}

static void update_setup(void)
{
    setup_status_t ss;
    wifi_setup_get_status(&ss);
    lv_obj_set_hidden(setup_panel, ss.state == SETUP_OFF);
    if (ss.state == SETUP_OFF) {
        return;
    }
    char buf[160];
    uint32_t color = COL_TEXT;
    switch (ss.state) {
    case SETUP_STARTING:
        show_qr(NULL);
        lv_label_set_text(setup_text, "Startar…");
        break;
    case SETUP_WAITING:
        /* Standard Wi-Fi QR code: phone cameras offer to join directly. */
        snprintf(buf, sizeof(buf), "WIFI:T:WPA;S:%s;P:%s;;", ss.ap_ssid, ss.ap_password);
        show_qr(buf);
        snprintf(buf, sizeof(buf),
                 "1. Skanna koden med mobilens kamera.\n"
                 "2. Välj ditt nätverk på sidan som öppnas.");
        lv_label_set_text(setup_text, buf);
        break;
    case SETUP_PHONE:
        show_qr("http://192.168.4.1/");
        lv_label_set_text(setup_text, "Mobilen är ansluten.\n"
                                      "Öppnas ingen sida? Skanna koden igen eller gå till 192.168.4.1");
        break;
    case SETUP_CONNECTING:
        show_qr(NULL);
        snprintf(buf, sizeof(buf), "Ansluter till %s…", ss.target);
        lv_label_set_text(setup_text, buf);
        break;
    case SETUP_DONE:
        show_qr(NULL);
        snprintf(buf, sizeof(buf), "Klart!\nAnsluten till %s.", ss.target);
        lv_label_set_text(setup_text, buf);
        color = COL_OK;
        break;
    default:
        break;
    }
    lv_obj_set_style_text_color(setup_text, lv_color_hex(color), 0);

    if (ss.last_failed && (ss.state == SETUP_WAITING || ss.state == SETUP_PHONE)) {
        snprintf(buf, sizeof(buf), "Kunde inte ansluta till %s. Försök igen.", ss.target);
        lv_obj_set_style_text_color(setup_note, lv_color_hex(COL_ERR), 0);
    } else if (ss.state == SETUP_WAITING) {
        snprintf(buf, sizeof(buf), "%s\nLösenord: %s\nHåll knappen: avbryt",
                 ss.ap_ssid, ss.ap_password);
        lv_obj_set_style_text_color(setup_note, lv_color_hex(COL_DIM), 0);
    } else {
        snprintf(buf, sizeof(buf), "%s", ss.state == SETUP_DONE ? "" : "Håll knappen: avbryt");
        lv_obj_set_style_text_color(setup_note, lv_color_hex(COL_DIM), 0);
    }
    lv_label_set_text(setup_note, buf);
}

static void build_phone(lv_obj_t *scr)
{
    phone_panel = box(scr, 0, 0, LCD_H_RES, LCD_V_RES, COL_BG, 0);
    phone_qr = lv_qrcode_create(phone_panel);
    lv_qrcode_set_size(phone_qr, 132);
    lv_qrcode_set_dark_color(phone_qr, lv_color_black());
    lv_qrcode_set_light_color(phone_qr, lv_color_white());
    lv_qrcode_set_quiet_zone(phone_qr, true);
    lv_obj_set_pos(phone_qr, 12, 20);

    lv_obj_t *title = text(phone_panel, &sl_font_16, COL_TEXT, "Välj i mobilen");
    lv_obj_set_pos(title, 158, 10);
    phone_text = text(phone_panel, &sl_font_12, COL_TEXT, "");
    lv_obj_set_width(phone_text, LCD_H_RES - 166);
    lv_obj_set_pos(phone_text, 158, 34);
    lv_obj_t *hint = text(phone_panel, &sl_font_12, COL_DIM, "Tryck på knappen: stäng");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 158, -8);
    lv_obj_set_hidden(phone_panel, true);
}

void ui_phone_show(void)
{
    const char *ip = board_wifi_ip();
    char buf[160];
    if (ip[0]) {
        snprintf(buf, sizeof(buf), "http://%s/", ip);
        lv_qrcode_update(phone_qr, buf, strlen(buf));
        lv_obj_set_hidden(phone_qr, false);
        snprintf(buf, sizeof(buf), "Skanna koden för att välja linje och station.\n\n"
                                   "Mobilen måste vara på samma Wi-Fi.\n%s", ip);
    } else {
        lv_obj_set_hidden(phone_qr, true);
        snprintf(buf, sizeof(buf), "Tavlan är inte ansluten till Wi-Fi.");
    }
    lv_label_set_text(phone_text, buf);
    sl_config_t cfg;
    sl_get_config(&cfg);
    phone_generation = cfg.generation;
    lv_obj_set_hidden(phone_panel, false);
}

void ui_phone_hide(void)
{
    lv_obj_set_hidden(phone_panel, true);
}

bool ui_phone_visible(void)
{
    return !lv_obj_is_hidden(phone_panel);
}

/* ---- start screen -------------------------------------------------- */

#define BOOT_MIN_MS 2500

static void build_boot(lv_obj_t *scr)
{
    boot_panel = box(scr, 0, 0, LCD_H_RES, LCD_V_RES, COL_BG, 0);
    lv_obj_t *bars = box(boot_panel, 0, 0, 0, 0, COL_BG, 0);
    lv_obj_set_size(bars, 3 * 28 + 2 * 6, 6);
    lv_obj_align(bars, LV_ALIGN_TOP_MID, 0, 34);
    const uint32_t colors[] = { 0x179C4C, 0xD71D24, 0x0089CA };
    for (int i = 0; i < 3; i++) {
        box(bars, i * 34, 0, 28, 6, colors[i], 3);
    }
    lv_obj_t *title = text(boot_panel, &sl_font_22, COL_TEXT, "SL Departures");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_t *ver = text(boot_panel, &sl_font_16, COL_DIM, ota_version());
    lv_obj_align(ver, LV_ALIGN_TOP_MID, 0, 80);
    lv_obj_t *author = text(boot_panel, &sl_font_12, COL_DIM, "av " CONFIG_APP_AUTHOR);
    lv_obj_align(author, LV_ALIGN_TOP_MID, 0, 101);
    boot_status = text(boot_panel, &sl_font_12, COL_TEXT, "");
    lv_obj_set_width(boot_status, LCD_H_RES - 20);
    lv_obj_set_style_text_align(boot_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(boot_status, LV_ALIGN_TOP_MID, 0, 124);
    lv_obj_t *credit = text(boot_panel, &sl_font_12, COL_DIM, "Data: Trafiklab.se");
    lv_obj_align(credit, LV_ALIGN_BOTTOM_MID, 0, -8);
    boot_started = lv_tick_get();
}

static void update_boot(void)
{
    if (lv_obj_is_hidden(boot_panel)) {
        return;
    }
    sl_config_t cfg;
    sl_get_config(&cfg);
    if (lv_tick_elaps(boot_started) >= BOOT_MIN_MS && metro_ready() && cfg.generation) {
        lv_obj_set_hidden(boot_panel, true);
        return;
    }
    char buf[64] = "";
    if (!metro_ready()) {
        metro_status(buf, sizeof(buf));
    }
    lv_label_set_text(boot_status, buf);
}

/* ---- about and updates ---------------------------------------------- */

static void build_about(lv_obj_t *scr)
{
    about_panel = box(scr, 0, 0, LCD_H_RES, LCD_V_RES, COL_BG, 0);
    lv_obj_t *title = text(about_panel, &sl_font_16, COL_TEXT, "SL Departures");
    lv_obj_set_pos(title, 12, 10);
    about_text = text(about_panel, &sl_font_12, COL_TEXT, "");
    lv_obj_set_width(about_text, LCD_H_RES - 24);
    lv_obj_set_pos(about_text, 12, 36);
    lv_obj_t *hint = text(about_panel, &sl_font_12, COL_DIM,
                          "Håll: sök efter uppdatering · Tryck: stäng");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 12, -8);
    lv_obj_set_hidden(about_panel, true);
}

static void update_about(void)
{
    if (lv_obj_is_hidden(about_panel)) {
        return;
    }
    ota_status_t os;
    ota_get_status(&os);
    const char *state;
    char latest[96];
    switch (os.state) {
    case OTA_CHECKING:
        state = "Söker efter uppdatering…";
        break;
    case OTA_UP_TO_DATE:
        snprintf(latest, sizeof(latest), "Du har den senaste versionen (%s).", os.latest);
        state = latest;
        break;
    case OTA_FAILED:
        state = "Kunde inte söka efter uppdateringar. Försök igen senare.";
        break;
    case OTA_INSTALL_FAILED:
        snprintf(latest, sizeof(latest), "Uppdateringen till %s misslyckades. Försök igen senare.",
                 os.latest);
        state = latest;
        break;
    default:
        state = "Nya versioner installeras automatiskt på natten.";
    }
    const char *ip = board_wifi_ip();
    char buf[256];
    snprintf(buf, sizeof(buf), "Version %s av %s\n%s.local%s%s\n\n%s\n\nData: Trafiklab.se",
             ota_version(), CONFIG_APP_AUTHOR, board_hostname(), ip[0] ? " · " : "", ip, state);
    lv_label_set_text(about_text, buf);
}

void ui_about_show(void)
{
    lv_obj_set_hidden(about_panel, false);
    update_about();
}

void ui_about_hide(void)
{
    lv_obj_set_hidden(about_panel, true);
}

bool ui_about_visible(void)
{
    return !lv_obj_is_hidden(about_panel);
}

static void build_progress(lv_obj_t *scr)
{
    update_panel = box(scr, 0, 0, LCD_H_RES, LCD_V_RES, COL_BG, 0);
    update_title = text(update_panel, &sl_font_16, COL_TEXT, "");
    lv_obj_align(update_title, LV_ALIGN_TOP_MID, 0, 44);
    update_bar = lv_bar_create(update_panel);
    lv_obj_set_size(update_bar, LCD_H_RES - 60, 12);
    lv_obj_align(update_bar, LV_ALIGN_CENTER, 0, 4);
    lv_obj_set_style_bg_color(update_bar, lv_color_hex(COL_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_color(update_bar, lv_color_hex(COL_OK), LV_PART_INDICATOR);
    update_note = text(update_panel, &sl_font_12, COL_DIM, "");
    lv_obj_align(update_note, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_set_hidden(update_panel, true);
}

static void update_progress(void)
{
    ota_status_t os;
    ota_get_status(&os);
    bool show = os.state == OTA_DOWNLOADING || os.state == OTA_DONE;
    lv_obj_set_hidden(update_panel, !show);
    if (!show) {
        return;
    }
    char buf[48];
    if (os.state == OTA_DONE) {
        lv_label_set_text(update_title, "Startar om…");
        lv_bar_set_value(update_bar, 100, LV_ANIM_OFF);
    } else {
        snprintf(buf, sizeof(buf), "Uppdaterar till %s… %d %%", os.latest, os.progress);
        lv_label_set_text(update_title, buf);
        lv_bar_set_value(update_bar, os.progress, LV_ANIM_ON);
    }
    lv_label_set_text(update_note, "Dra inte ur sladden.");
}

static void build_picker(lv_obj_t *scr)
{
    picker = box(scr, 0, 0, LCD_H_RES, LCD_V_RES, COL_BG, 0);
    picker_title = text(picker, &sl_font_16, COL_TEXT, "");
    lv_obj_align(picker_title, LV_ALIGN_TOP_MID, 0, 6);

    picker_roller = lv_roller_create(picker);
    lv_obj_set_width(picker_roller, LCD_H_RES - 40);
    lv_roller_set_visible_row_count(picker_roller, 3);
    lv_obj_set_style_text_font(picker_roller, &sl_font_16, 0);
    lv_obj_set_style_text_color(picker_roller, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_bg_color(picker_roller, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_border_width(picker_roller, 0, 0);
    lv_obj_set_style_text_color(picker_roller, lv_color_hex(COL_TEXT), LV_PART_SELECTED);
    lv_obj_set_style_radius(picker_roller, 6, LV_PART_SELECTED);
    lv_obj_align(picker_roller, LV_ALIGN_CENTER, 0, 2);

    lv_obj_t *hint = text(picker, &sl_font_12, COL_DIM, "Tryck: nästa · Dubbel: föregående · Håll: välj");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_hidden(picker, true);
}

void ui_picker_show(const char *title, const char *options, int selected, uint32_t color)
{
    lv_label_set_text(picker_title, title);
    lv_roller_set_options(picker_roller, options, LV_ROLLER_MODE_NORMAL);
    lv_obj_set_style_bg_color(picker_roller, lv_color_hex(color), LV_PART_SELECTED);
    lv_roller_set_selected(picker_roller, selected, LV_ANIM_OFF);
    lv_obj_set_hidden(picker, false);
}

void ui_picker_select(int index)
{
    lv_roller_set_selected(picker_roller, index, LV_ANIM_ON);
}

void ui_picker_hide(void)
{
    lv_obj_set_hidden(picker, true);
}

void ui_init(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_scrollable(scr, false);

    title_label = text(scr, &sl_font_16, COL_TEXT, "");
    lv_obj_set_pos(title_label, 8, 4);
    lv_obj_set_style_max_width(title_label, 190, 0);
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(title_label, lv_font_get_line_height(&sl_font_16));
    line_badge = box(scr, 0, 0, 0, 0, COL_TRACK, 3);
    lv_obj_set_size(line_badge, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(line_badge, 5, 0);
    line_badge_label = text(line_badge, &sl_font_12, COL_TEXT, "");

    clock_label = text(scr, &sl_font_16, COL_TEXT, "--:--");
    one_line(clock_label, 50);
    lv_obj_set_style_text_align(clock_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(clock_label, LV_ALIGN_TOP_RIGHT, -8, 4);
    status_dot = box(scr, 0, 0, 7, 7, COL_WARN, 4);
    lv_obj_align_to(status_dot, clock_label, LV_ALIGN_OUT_LEFT_MID, -4, 0);

    map_layer = box(scr, 0, 0, LCD_H_RES, LIST_Y - 6, COL_BG, 0);
    lv_obj_set_style_bg_opa(map_layer, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollable(map_layer, false);

    build_list(scr);
    build_picker(scr);
    build_phone(scr);
    build_about(scr);
    build_boot(scr);
    build_setup(scr);
    build_progress(scr);

    update_cb(NULL);
    lv_timer_create(update_cb, 250, NULL);
}
