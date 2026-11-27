/* AstraPod app reminders SDK. GPL version 2 or later. */
#ifndef ASTRAPOD_ROCK_TRAY_H
#define ASTRAPOD_ROCK_TRAY_H

#include <stdint.h>

#define ROCK_TRAY_ICON_SIZE 8
#define ROCK_TRAY_ICON12_SIZE 12
#define ROCK_TRAY_MAX_FRAMES 4
#define ROCK_TRAY_MAX_RULES 4
#define ROCK_TRAY_MIN_DELAY 30
#define ROCK_TRAY_HIDDEN 255

/* Zero bits are transparent; the status bar's theme colours set bits. */
struct rock_tray_bitmap {
    /* One byte per column, bit 0 at the top. Drawn centred in 12 x 12. */
    uint8_t columns[ROCK_TRAY_ICON_SIZE];
};

struct rock_tray_icon12 {
    /* One halfword per column, bit 0 at the top; bits 12-15 are ignored. */
    uint16_t columns[ROCK_TRAY_ICON12_SIZE];
};

struct rock_tray_rule {
    uint32_t delay_seconds; /* Offset from successful schedule installation. */
    uint8_t frame;          /* Zero-based frame or ROCK_TRAY_HIDDEN. */
};

#endif
