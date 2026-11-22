 /***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Port of xrick, a Rick Dangerous clone, to Rockbox.
 * See http://www.bigorno.net/xrick/
 *
 * Copyright (C) 2008-2014 Pierluigi Vicinanza
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#include "xrick/system/system.h"

#include "xrick/debug.h"

#include "plugin.h"

/*
 * Local variables
 */
enum
{
    ALIGNMENT = sizeof(void*)  /* this is more of an educated guess; might want to adjust for your specific architecture */
};
static U8 * stackBuffer;
static U8 * stackTop;
static size_t stackSize;
static size_t stackMaxSize;
static bool isMemoryInitialised = false;
IFDEBUG_MEMORY( static size_t maxUsedMemory = 0; );

/*
 * Initialise memory stack
 */
bool sysmem_init(void)
{
    if (isMemoryInitialised)
    {
        return true;
    }

#ifdef ASTRAPOD_XRICK
    stackBuffer = rb->plugin_claim_scratch(256 * 1024, 256 * 1024, &stackMaxSize);
    if (!stackBuffer)
        return false;
#else
    if (rb->audio_status())
    {
        /* Playback must be stopped the entire time the sound buffer is used.*/
        rb->audio_stop();
    }

    stackBuffer = rb->plugin_get_audio_buffer(&stackMaxSize);
#endif
    stackTop = stackBuffer;
    stackSize = 0;
    isMemoryInitialised = true;
    return true;
}

/*
 * Cleanup memory stack
 */
void sysmem_shutdown(void)
{
    if (!isMemoryInitialised)
    {
        return;
    }

    if (stackTop != stackBuffer || stackSize != 0)
    {
        sys_error("(memory) improper deallocation detected");
    }

    IFDEBUG_MEMORY(
        sys_printf("xrick/memory: max memory usage was %u bytes\n", maxUsedMemory);
    );

    isMemoryInitialised = false;
}

/*
 * Allocate a memory-aligned block on top of the memory stack
 */
void *sysmem_push(size_t size)
{
    size_t overhead = sizeof(size_t) + ALIGNMENT - 1;
    size_t available = stackMaxSize - stackSize;
    if (available < overhead || size > available - overhead)
        return NULL;

    uintptr_t alignedPtr = ((uintptr_t)stackTop + overhead) &
                           ~(uintptr_t)(ALIGNMENT - 1);
    size_t neededSize = alignedPtr - (uintptr_t)stackTop + size;
    ((size_t *)alignedPtr)[-1] = neededSize;
    stackTop += neededSize;
    stackSize += neededSize;
    /* Partially parsed structures must be safe to unwind. */
    rb->memset((void *)alignedPtr, 0, size);

    IFDEBUG_MEMORY(
        sys_printf("xrick/memory: allocated %u bytes\n", neededSize);
        if (stackSize > maxUsedMemory) maxUsedMemory = stackSize;
    );

    return (void *)alignedPtr;
}

/*
 * Release block from the top of the memory stack
 */
void sysmem_pop(void * alignedPtr)
{
    size_t allocatedSize;

    if (!alignedPtr)
    {
        return;
    }

    if (stackSize == 0)
    {
        sys_error("(memory) tried to release a block when memory empty");
        return;
    }

    if ((uintptr_t)alignedPtr < (uintptr_t)stackBuffer + sizeof(size_t) ||
        (uintptr_t)alignedPtr > (uintptr_t)stackTop ||
        (uintptr_t)alignedPtr % ALIGNMENT)
    {
        sys_error("(memory) invalid block address");
        return;
    }
    allocatedSize = ((size_t *)(alignedPtr))[-1];
    if (allocatedSize > stackSize ||
        (uintptr_t)alignedPtr !=
        (((uintptr_t)(stackTop - allocatedSize) + sizeof(size_t) + ALIGNMENT - 1) &
         ~(uintptr_t)(ALIGNMENT - 1)))
    {
        sys_error("(memory) tried to release a wrong block");
        return;
    }

    stackTop -= allocatedSize;
    stackSize -= allocatedSize;

    IFDEBUG_MEMORY(
        sys_printf("xrick/memory: released %u bytes\n", allocatedSize);
    );
}

/* eof */
