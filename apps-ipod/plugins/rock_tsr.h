/* Executable TSR leases for the AstraPod SDK. GPL version 2 or later. */
#ifndef ASTRAPOD_ROCK_TSR_H
#define ASTRAPOD_ROCK_TSR_H

#define ROCK_TSR_MAGIC 0x41535453UL
#define ROCK_TSR_ARENA_BYTES (64 * 1024)
#define ROCK_TSR_SUFFIX ".tsr.rock"

#ifndef __ASSEMBLER__
#include <stdbool.h>

enum rock_tsr_event {
    ROCK_TSR_TICK,
    ROCK_TSR_REPLACED,
    ROCK_TSR_USB,
    ROCK_TSR_POWER,
    ROCK_TSR_ABORT,
};

typedef void (*rock_tsr_callback)(enum rock_tsr_event event);
bool rock_tsr_register(rock_tsr_callback callback, int period);

#ifndef PLUGIN
#include "config.h"
#include "load_code.h"

#if CONFIG_BINFMT == BINFMT_ROCK
extern unsigned char rock_tsr_arena[ROCK_TSR_ARENA_BYTES];
#endif

struct plugin_header;
void rock_tsr_begin(void *handle, const struct plugin_header *module);
bool rock_tsr_finish(bool keep);
void rock_tsr_stop(enum rock_tsr_event reason);
#endif
#endif
#endif
