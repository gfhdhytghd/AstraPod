/* Executable TSR leases for the AstraPod SDK. GPL version 2 or later. */
#include <stddef.h>
#include "plugins/sdk/plugin.h"

#if CONFIG_BINFMT == BINFMT_ROCK
/* Keep this address in the firmware ELF for the TSR image link. */
unsigned char rock_tsr_arena[ROCK_TSR_ARENA_BYTES] CACHEALIGN_ATTR;
#elif defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

/* The foreground loader owns this slot. The worker borrows its callback
 * until joined. A staged image cannot run a background callback. */
static struct {
    void *handle;
    const struct plugin_header *module;
    unsigned int owner;
    unsigned int worker;
    rock_tsr_callback callback;
    int period;
    enum rock_tsr_event reason;
    struct semaphore stop;
} tsr;

static long stack[2 * DEFAULT_STACK_SIZE / sizeof(long)];

static bool callback_in_image(rock_tsr_callback callback)
{
#if CONFIG_BINFMT == BINFMT_ROCK
    uintptr_t address = (uintptr_t)callback;
    return !(address % 4) &&
           address >= (uintptr_t)(tsr.module + 1) &&
           address <= (uintptr_t)tsr.module->file_end - 4;
#elif defined(_WIN32)
    MEMORY_BASIC_INFORMATION module, entry;
    return VirtualQuery(tsr.module, &module, sizeof(module)) &&
           VirtualQuery(callback, &entry, sizeof(entry)) &&
           module.AllocationBase == entry.AllocationBase &&
           (entry.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ));
#else
    Dl_info module, entry;
    return dladdr(tsr.module, &module) && dladdr(callback, &entry) &&
           module.dli_fbase == entry.dli_fbase;
#endif
}

bool rock_tsr_register(rock_tsr_callback callback, int period)
{
    if (!tsr.owner || tsr.owner != thread_self() || tsr.callback ||
        !callback || period <= 0 || !callback_in_image(callback))
        return false;

    tsr.callback = callback;
    tsr.period = period;
    return true;
}

static void tsr_main(void)
{
    while (semaphore_wait(&tsr.stop, tsr.period) == OBJ_WAIT_TIMEDOUT)
        tsr.callback(ROCK_TSR_TICK);

    tsr.callback(tsr.reason);
    thread_exit();
}

static void release_image(void)
{
    lc_close(tsr.handle);
    tsr.handle = NULL;
    tsr.module = NULL;
    tsr.callback = NULL;
    tsr.worker = 0;
}

void rock_tsr_begin(void *handle, const struct plugin_header *module)
{
    tsr.handle = handle;
    tsr.module = module;
    tsr.owner = thread_self();
    tsr.reason = ROCK_TSR_ABORT;
}

bool rock_tsr_finish(bool keep)
{
    bool start = keep && tsr.owner && tsr.callback;
    tsr.owner = 0;
    if (start)
    {
        semaphore_init(&tsr.stop, 1, 0);
        tsr.worker = create_thread(tsr_main, stack, sizeof(stack), 0,
            "plugin TSR" IF_PRIO(, PRIORITY_BACKGROUND) IF_COP(, CPU));
        if (tsr.worker)
            return true;
    }

    if (tsr.callback)
        tsr.callback(tsr.reason);
    release_image();
    return !start;
}

void rock_tsr_stop(enum rock_tsr_event reason)
{
    if (!tsr.handle)
        return;

    if (tsr.owner)
    {
        /* The foreground entry is still on the stack. Cancel promotion.
         * The loader closes the image when the entry returns. */
        tsr.reason = reason;
        tsr.owner = 0;
        return;
    }
    if (!tsr.worker)
        return;

    tsr.reason = reason;
    semaphore_release(&tsr.stop);
    thread_wait(tsr.worker);
    release_image();
}
