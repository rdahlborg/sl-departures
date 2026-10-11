#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "metro_data.h"

/* Max number of stations queried on each side of the home station. */
#define SL_UPSTREAM      5
#define SL_MAX_TRAINS    6

/* Index into the per-direction arrays. Rightbound trains travel towards
 * higher route indices (SL direction_code 2), leftbound the other way. */
enum { SL_RIGHTBOUND = 0, SL_LEFTBOUND = 1 };

/* What is being shown: the home station and its neighbours on one route. */
typedef struct {
    uint8_t  group;                     /* index into metro_groups */
    uint16_t home;
    uint8_t  line;                      /* route the map is drawn along */
    /* Upstream stations per direction of travel, nearest first.
     * Rightbound trains arrive from the left and vice versa. */
    uint8_t  n_up[2];
    uint16_t up[2][SL_UPSTREAM];
    /* Terminus each direction heads for, for the list headers. */
    uint16_t end[2];
    uint32_t generation;
} sl_config_t;

typedef struct {
    int64_t journey;
    uint8_t line;
    bool    cancelled;
    char    dest[28];
    /* Expected time at the home station (t[0]) and at upstream station k
     * (t[k], k = 1 is nearest). 0 when the train is not listed there,
     * i.e. it has already passed or never calls there. */
    time_t  t[SL_UPSTREAM + 1];
    /* Bit k set when the train is standing at station k right now. */
    uint8_t at_stop;
    /* Furthest upstream station on the map that this train's line also
     * calls at. Beyond it the train is on another branch. */
    uint8_t reach;
} sl_train_t;

typedef struct {
    sl_train_t trains[SL_MAX_TRAINS];
    int n;
    uint8_t n_up;
} sl_dir_t;

typedef struct {
    sl_dir_t dir[2];
    time_t   updated;   /* 0 until the first successful poll */
    uint32_t generation;
} sl_state_t;

/* Start the background polling task showing `site` on line group `group`.
 * Falls back to the group's first station if the site is not on it. */
void sl_start(int group, uint16_t site);

/* Switch station. Clears the current data and polls immediately. */
void sl_select(int group, uint16_t site);

void sl_get_config(sl_config_t *out);

/* Copy the latest state. Returns false if no data for the current
 * station has been fetched yet. */
bool sl_snapshot(sl_state_t *out);

/* Estimated position in stations away from home (0 = at home, 1 = at the
 * nearest upstream station, 1.5 = halfway between stations 1 and 2).
 * Returns a negative value if the train should not be drawn. */
float sl_train_position(const sl_train_t *tr, int n_up, time_t now);
