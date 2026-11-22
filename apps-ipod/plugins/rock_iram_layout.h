/* Shared by the checked loader and the ipod6g plugin linker. GPL v2+. */
#ifndef ROCK_IRAM_LAYOUT_H
#define ROCK_IRAM_LAYOUT_H

#define ROCK_IRAM_ORIGIN 0xc000
#define ROCK_IRAM_LIMIT  0x20000

#ifndef __ASSEMBLER__
#include <stdbool.h>
#include <stdint.h>

static inline bool rock_iram_extent_valid(uintptr_t payload, uintptr_t file_end,
                                          uintptr_t source, uintptr_t data_end,
                                          uintptr_t end)
{
    return source >= payload && source <= file_end &&
           data_end > ROCK_IRAM_ORIGIN && data_end <= end &&
           end <= ROCK_IRAM_LIMIT && ((source | data_end | end) & 3u) == 0 &&
           file_end - source == data_end - ROCK_IRAM_ORIGIN;
}
#endif

#endif
