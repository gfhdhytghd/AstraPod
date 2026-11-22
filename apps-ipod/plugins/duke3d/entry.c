/* Duke3D foreground entry. GNU GPL version 2 or later. */
#include "plugin.h"

#if UINTPTR_MAX == UINT32_MAX
#include <tlsf.h>
#include "duke-build.h"

extern int my_main(int argc, char **argv);
extern void uninitgroupfile(void);
extern void duke_abort_files(void);
extern void duke_sdl_quit(void);
extern int rb_atexit(void (*fn)(void));
extern void DSL_StopPlayback(void);

void duke_cleanup(void)
{
    DSL_StopPlayback();
    duke_abort_files();
    uninitgroupfile();
    duke_sdl_quit();
}

#endif

enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;
#if UINTPTR_MAX == UINT32_MAX
    if (!rb->dir_exists(DUKE_DIR))
    {
        rb->splash(3 * HZ, "Place duke3d.grp in " DUKE_DIR);
        return PLUGIN_OK;
    }

    const size_t bytes = 2 * 1024 * 1024;
    void *heap = rb->plugin_claim_heap(bytes);
    if (!heap)
    {
        rb->splash(2 * HZ, "Duke3D: insufficient engine memory");
        return PLUGIN_ERROR;
    }

    rb->memset(heap, 0, bytes);
    if (init_memory_pool(bytes, heap) == (size_t)-1)
        return PLUGIN_ERROR;
    if (rb->mkdir(DUKE_SAVES) < 0 && !rb->dir_exists(DUKE_SAVES))
    {
        rb->splash(2 * HZ, "Duke3D: cannot open save directory");
        return PLUGIN_ERROR;
    }

    rb_atexit(duke_cleanup);
    rb->backlight_set_timeout(0);
    rb->backlight_on();
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    rb->cpu_boost(true);
#endif
    char *argv[] = { "duke3d", NULL };
    my_main(1, argv);
    return PLUGIN_OK;
#else
    rb->splash(2 * HZ, "Duke3D requires a 32-bit simulator");
    return PLUGIN_OK;
#endif
}
