/* Departure board, 320x172 landscape.
 *
 *   header      station name, line badge, status dot, clock
 *   line map    four stations either side, trains placed from live data
 *   departures  next two trains in each direction
 *
 * The map is rebuilt whenever the selected station changes.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "lvgl.h"
#include "board.h"
#include "sl_api.h"
#include "ui.h"
#include "wifi_setup.h"

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
static lv_obj_t *col_header[2];
static lv_obj_t *placeholder;
static lv_obj_t *picker, *picker_roller, *picker_title;
static lv_obj_t *setup_panel, *setup_qr, *setup_text, *setup_note;

typedef struct {
    lv_obj_t *badge, *badge_label, *dest, *mins;
} row_t;
static row_t rows[2][LIST_ROWS];

static sl_config_t shown;       /* config the map was built for */

static void update_setup(void);
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

    for (int dir = 0; dir < 2; dir++) {
        for (int i = 0; i < SL_MAX_TRAINS; i++) {
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
            one_line(row->dest, 72);
            row->mins = text(scr, &sl_font_16, COL_TEXT, "");
            one_line(row->mins, 50);
            lv_obj_set_style_text_align(row->mins, LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_set_pos(row->mins, col_x[dir] + 98, y);
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

static void update_cb(lv_timer_t *t)
{
    time_t now = time(NULL);
    char buf[16];

    update_setup();

    sl_config_t cfg;
    sl_get_config(&cfg);
    if (cfg.generation != shown.generation) {
        build_map(&cfg);
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
            float pos = have && i < d->n ? sl_train_position(&d->trains[i], d->n_up, now) : -1;
            /* Allow half a spacing past the last drawn station so trains
             * slide in from the edge. */
            bool show = pos >= 0 && pos <= MAP_SHOWN + 0.6f;
            lv_obj_set_hidden(markers[dir][i], !show);
            if (!show) {
                continue;
            }
            int y = dir == SL_RIGHTBOUND ? TRACK_R_Y : TRACK_L_Y;
            lv_obj_set_pos(markers[dir][i], map_x(dir, pos) - 11, y - 7);
            lv_label_set_text_fmt(marker_labels[dir][i], "%d", d->trains[i].line);
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
    build_setup(scr);

    update_cb(NULL);
    lv_timer_create(update_cb, 250, NULL);
}
