/* Fruit Company Now Playing controls. GPL version 2+. */
#ifndef PODBOX_STOCK_WPS_H
#define PODBOX_STOCK_WPS_H

#include <stdbool.h>
#include "config.h"

struct wps_state;

enum stock_wps_mode {
    STOCK_WPS_PROGRESS, STOCK_WPS_SCRUB, STOCK_WPS_RATING,
    STOCK_WPS_SHUFFLE, STOCK_WPS_VOLUME,
};

struct stock_wps_controls {
    enum stock_wps_mode mode;
    unsigned long position, length;
    int stars, shuffle;
    bool remote;
};

struct stock_wps {
    struct stock_wps_controls controls;
    char path[MAX_PATH];
    long deadline;
    unsigned track_serial;
    bool edited;
};

void stock_wps_update(struct stock_wps *stock, const struct wps_state *state,
                      unsigned track_serial);
bool stock_wps_action(struct stock_wps *stock, const struct wps_state *state,
                      long action);

#endif
