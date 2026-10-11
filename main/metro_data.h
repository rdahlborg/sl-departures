#pragma once
#include <stdint.h>
#include <stddef.h>

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

extern const metro_group_t metro_groups[METRO_N_GROUPS];

const char *metro_station_name(uint16_t site);
