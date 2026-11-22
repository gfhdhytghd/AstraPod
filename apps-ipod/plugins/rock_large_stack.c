/* Exclusive foreground launch on a larger stack. GNU GPL version 2 or later. */
#include "core_alloc.h"
#include "kernel.h"
#include "plugins/rock_large_stack.h"
#include "plugins/sdk/plugin.h"

#define ROCK_STACK_BYTES (80 * 1024)

struct rock_stack_launch {
    int (*launch)(void *);
    void *arg;
    int result;
};

static struct rock_stack_launch *pending;

static void foreground_main(void)
{
    struct rock_stack_launch *call = pending;
    call->result = call->launch(call->arg);
    thread_exit();
}

int rock_large_stack_launch(int (*launch)(void *), void *arg)
{
    if (pending)
        return PLUGIN_ERROR;

    int handle = core_alloc(ROCK_STACK_BYTES);
    if (handle <= 0)
        return PLUGIN_ERROR;
    core_pin(handle);
    struct rock_stack_launch call = { launch, arg, PLUGIN_ERROR };
    pending = &call;
    unsigned int id = create_thread(foreground_main, core_get_data(handle),
        ROCK_STACK_BYTES, 0, "plugin foreground"
        IF_PRIO(, PRIORITY_USER_INTERFACE) IF_COP(, CPU));
    if (id)
        thread_wait(id);
    pending = NULL;
    core_unpin(handle);
    core_free(handle);
    return call.result;
}
