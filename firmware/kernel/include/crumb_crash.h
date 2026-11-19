/* Reset-retained panic records for the S5L8702. GPL version 2+. */
#ifndef CRUMB_CRASH_H
#define CRUMB_CRASH_H

#include <stddef.h>
#include "crumbs.h"

#define CRUMB_CRASH_MAGIC 0x43525332u /* CRS2 */

struct crumb_crash_details
{
    uint32_t ticks, regs[13], calls[8];
    uint32_t reg_count, call_count;
    char thread[16], command[24];
};

struct crumb_crash
{
    uint32_t check;
    struct
    {
        uint32_t magic, boot;
        struct crumb_crash_context context;
        char version[64], message[256];
        struct crumb_crash_details details;
    } data;
};

static inline uint32_t crumb_crash_checksum(const volatile struct crumb_crash *crash)
{
    const volatile unsigned char *bytes = (const volatile unsigned char *)&crash->data;
    uint32_t check = 0x9e3779b9u;
    for (unsigned i = 0; i < sizeof(crash->data); i++)
        check = (check ^ bytes[i]) * 0x01000193u;
    return check ? check : 1;
}

static inline void crumb_crash_string(volatile char *dest, unsigned size, const char *src)
{
    unsigned i = 0;
    for (; i + 1 < size && *src; i++)
        dest[i] = *src++;
    for (; i < size; i++)
        dest[i] = 0;
}

static inline void crumb_crash_store(volatile struct crumb_crash *crash,
                                     uint32_t boot, const char *version,
                                     const char *message,
                                     const struct crumb_crash_context *context,
                                     const struct crumb_crash_details *details)
{
    crash->check = 0;
    crash->data.magic = CRUMB_CRASH_MAGIC;
    crash->data.boot = boot;
    crash->data.context = *context;
    crumb_crash_string(crash->data.version, sizeof(crash->data.version), version);
    crumb_crash_string(crash->data.message, sizeof(crash->data.message), message);
    crash->data.details = *details;
    /* A reset during the copy leaves a record with no valid check word. */
    crash->check = crumb_crash_checksum(crash);
}

static inline bool crumb_crash_valid(const volatile struct crumb_crash *crash)
{
    return crash->data.magic == CRUMB_CRASH_MAGIC &&
           crash->data.context.kind >= CRUMB_CRASH_PANIC &&
           crash->data.context.kind <= CRUMB_CRASH_FREEZE &&
           crash->data.version[sizeof(crash->data.version) - 1] == 0 &&
           crash->data.message[sizeof(crash->data.message) - 1] == 0 &&
           crash->data.details.thread[sizeof(crash->data.details.thread) - 1] == 0 &&
           crash->data.details.command[sizeof(crash->data.details.command) - 1] == 0 &&
           crash->data.details.reg_count <= sizeof(crash->data.details.regs) / sizeof(uint32_t) &&
           crash->data.details.call_count <= sizeof(crash->data.details.calls) / sizeof(uint32_t) &&
           crash->check == crumb_crash_checksum(crash);
}

#endif
