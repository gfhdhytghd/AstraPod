/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "pdbox-patch.h"

static struct pdbox_patch patch;
static char fixture[PDBOX_PATCH_BYTES + 1];
static const char prefix[] = "#N canvas 0 0 160 120 10;";

static bool accepts(const char *text)
{
    return pdbox_validate_patch(text, strlen(text), &patch);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *file = fopen(argv[1], "rb");
    assert(file);
    size_t bytes = fread(fixture, 1, sizeof(fixture), file);
    assert(fclose(file) == 0);
    assert(pdbox_validate_patch(fixture, bytes, &patch));
    assert(patch.count == 2);
    assert(patch.widgets[0].kind == PDBOX_TEXT);
    assert(patch.widgets[1].kind == PDBOX_NUMBER);
    assert(strcmp(patch.widgets[1].name, "pod_pitch") == 0);

    /* EOF is only legal at a complete message boundary. */
    assert(!pdbox_validate_patch(fixture, bytes - 2, &patch));
    assert(!pdbox_validate_patch(fixture, sizeof(fixture), &patch));
    fixture[20] = '\0';
    assert(!pdbox_validate_patch(fixture, bytes, &patch));

#define REJECT(suffix) \
    do { snprintf(fixture, sizeof(fixture), "%s%s", prefix, suffix); \
         assert(!accepts(fixture)); } while (0)
    REJECT("#X obj 1 1 osc~ 1e999999;");
    REJECT("#X obj 1 1 osc~ 8192;");
    REJECT("#X obj 1 1 dac~ 65535;");
    REJECT("#X obj 1 1 soundfiler;");
    REJECT("#X obj 1 1 until;");
    REJECT("#X obj 1 1 pda_abstraction;");
    REJECT("#X obj 1 1 netreceive 3333 0;");
    REJECT("#X msg 1 1 \\; pd quit;");
    REJECT("#X obj 1 1 osc~ 440;#X connect 0 2 0 0;");
    REJECT("#X obj 1 1 osc~ 440;#X connect 0 0 1 1;");
    REJECT("#X obj 1 1 f;#X connect 0 0 0 0;");
    REJECT("#X obj 1 1 s pod_number;#X floatatom 1 1 5 0 0 0 - pod_number -;"
           "#X connect 1 0 0 0;");
    REJECT("#X obj 1 1 hsl 10 10 1 1 0 0 empty pod_s empty "
           "0 0 0 8 -1 -1 -1 0;");
    REJECT("#X text 1 1 this-comment-is-longer-than-the-sixty-three-byte-"
           "limit-for-one-display-label;"
           "#X obj 1 1 dac~;");
#undef REJECT

    /* An acyclic chain of duplicated connections still exceeds work bounds. */
    size_t used = (size_t)snprintf(fixture, sizeof(fixture), "%s", prefix);
    for (unsigned int i = 0; i < 12; i++)
        used += snprintf(fixture + used, sizeof(fixture) - used,
                         "#X obj 0 0 f;");
    for (unsigned int i = 0; i < 11; i++)
        for (int copy = 0; copy < 2; copy++)
            used += snprintf(fixture + used, sizeof(fixture) - used,
                             "#X connect %u 0 %u 0;", i, i + 1);
    assert(!accepts(fixture));
    puts("PDBox valid snapshot, malformed records and bounded graph PASS");
    return 0;
}
