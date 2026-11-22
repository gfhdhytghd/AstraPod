/* Limits for the PDBox patch reader.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef ASTRAPOD_PDBOX_PATCH_H
#define ASTRAPOD_PDBOX_PATCH_H

#include <stdbool.h>
#include <stddef.h>

#define PDBOX_PATCH_BYTES (32 * 1024)
#define PDBOX_WIDGETS 64
#define PDBOX_NAME_BYTES 64

enum pdbox_widget_kind {
    PDBOX_TEXT, PDBOX_NUMBER, PDBOX_BANG, PDBOX_HSL, PDBOX_VSL,
    PDBOX_HRADIO, PDBOX_VRADIO
};

struct pdbox_widget_spec {
    enum pdbox_widget_kind kind;
    int x, y, w, h, low, high;
    char name[PDBOX_NAME_BYTES];
};

struct pdbox_patch {
    unsigned int count;
    struct pdbox_widget_spec widgets[PDBOX_WIDGETS];
};

bool pdbox_validate_patch(const char *data, size_t size,
                          struct pdbox_patch *patch);

#endif
