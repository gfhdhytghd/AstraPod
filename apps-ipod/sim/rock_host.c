/* Hosted plugin image checks. GNU GPL version 2 or later. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#if defined(__linux__)
#define ASTRAPOD_HOST_ELF
#include <link.h>
#undef ASTRAPOD_HOST_ELF
#elif defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include "plugins/sdk/plugin.h"

#if defined(__linux__)
struct ranges {
    const struct plugin_header *header;
    bool valid;
};

static bool in_range(uintptr_t start, size_t length, uintptr_t item, size_t size)
{
    return item >= start && size <= length && item - start <= length - size;
}

static int find_ranges(struct dl_phdr_info *info, size_t size, void *data)
{
    (void)size;
    struct ranges *ranges = data;
    uintptr_t header = (uintptr_t)ranges->header;
    bool have_header = false;
    for (int i = 0; i < info->dlpi_phnum; ++i)
    {
        const ElfW(Phdr) *segment = &info->dlpi_phdr[i];
        if (segment->p_type == PT_LOAD && (segment->p_flags & PF_R) &&
            in_range(info->dlpi_addr + segment->p_vaddr, segment->p_filesz,
                     header, sizeof(*ranges->header)))
            have_header = true;
    }
    if (!have_header)
        return 0;

    uintptr_t entry = (uintptr_t)ranges->header->entry_point;
    uintptr_t slot = (uintptr_t)ranges->header->api;
    if (slot % _Alignof(const struct plugin_api *))
        return 1;

    bool have_entry = false, have_slot = false;
    for (int i = 0; i < info->dlpi_phnum; ++i)
    {
        const ElfW(Phdr) *segment = &info->dlpi_phdr[i];
        uintptr_t start = info->dlpi_addr + segment->p_vaddr;
        if (segment->p_type != PT_LOAD)
            continue;
        if ((segment->p_flags & PF_X) &&
            in_range(start, segment->p_filesz, entry, 1))
            have_entry = true;
        if ((segment->p_flags & PF_W) &&
            in_range(start, segment->p_memsz, slot, sizeof(rb)))
            have_slot = true;
    }
    /* A relocatable read-only table can be in a writable PT_LOAD segment. */
    for (int i = 0; i < info->dlpi_phnum; ++i)
    {
        const ElfW(Phdr) *segment = &info->dlpi_phdr[i];
        if (segment->p_type == PT_GNU_RELRO &&
            in_range(info->dlpi_addr + segment->p_vaddr, segment->p_memsz,
                     slot, sizeof(rb)))
            have_slot = false;
    }
    ranges->valid = have_entry && have_slot;
    return 1;
}
#endif

bool rock_host_header_valid(const struct plugin_header *header)
{
    if (!header || (uintptr_t)header % _Alignof(struct plugin_header))
        return false;
#if defined(__linux__)
    struct ranges ranges = { .header = header };
    dl_iterate_phdr(find_ranges, &ranges);
    return ranges.valid;
#elif defined(_WIN32)
    MEMORY_BASIC_INFORMATION hdr, entry, api;
    return VirtualQuery(header, &hdr, sizeof(hdr)) &&
           hdr.State == MEM_COMMIT && hdr.Type == MEM_IMAGE &&
           (hdr.Protect & (PAGE_READONLY | PAGE_READWRITE)) &&
           (size_t)((const char *)header - (const char *)hdr.BaseAddress) <=
                hdr.RegionSize - sizeof(*header) &&
           VirtualQuery(header->entry_point, &entry, sizeof(entry)) &&
           entry.AllocationBase == hdr.AllocationBase &&
           (entry.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ)) &&
           VirtualQuery(header->api, &api, sizeof(api)) &&
           api.AllocationBase == hdr.AllocationBase &&
           (api.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)) &&
           (uintptr_t)header->api % _Alignof(const struct plugin_api *) == 0;
#else
    Dl_info hdr, entry, api;
    return dladdr(header, &hdr) && dladdr(header->entry_point, &entry) &&
           dladdr(header->api, &api) && hdr.dli_fbase == entry.dli_fbase &&
           hdr.dli_fbase == api.dli_fbase &&
           (uintptr_t)header->api % _Alignof(const struct plugin_api *) == 0;
#endif
}
