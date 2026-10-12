#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>

/* Metro lines and stations. Only the lines and their end stations are built
 * in; the stations along each line, their names and SL site IDs are fetched
 * from SL's journey planner on first boot and kept in flash. */

#define METRO_N_GROUPS 3

typedef struct {
    uint16_t site;
    const char *name;
} metro_station_t;

/* One line's stations, ordered so that trains with SL direction_code 2
 * travel towards higher indices (left to right on the map). */
typedef struct {
    uint8_t line;
    uint8_t n;
    const uint16_t *sites;
} metro_route_t;

typedef struct {
    const char *name;           /* "Gröna linjen" */
    uint32_t color;
    uint8_t n_routes;
    const metro_route_t *routes;
    uint8_t n_stations;         /* every station on the group, picker order */
    const uint16_t *stations;
} metro_group_t;

/* Valid once metro_ready() is true. */
extern metro_group_t metro_groups[METRO_N_GROUPS];

/* "Gröna linjen" or "Green line", in the current language. */
const char *metro_group_name(int group);

/* Load the station list from flash, or fetch it from SL in the background
 * once Wi-Fi is up. `on_ready` runs once, from the loader task, when the
 * list is available. */
void metro_start(void (*on_ready)(void));
bool metro_ready(void);

/* Progress of the first download, for the start screen. */
void metro_status(char *buf, size_t len);

/* Refresh the list from SL if it is older than a month. Blocks; run it
 * from a maintenance task while departure polling is paused. */
void metro_refresh_if_stale(void);

const char *metro_station_name(uint16_t site);
