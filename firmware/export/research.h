/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RESEARCH_H
#define RESEARCH_H

#include "config.h"
#ifdef HAVE_RESEARCH_DIAGNOSTICS
#include <stdint.h>
#include "gcc_extensions.h"

enum research_fault_kind
{
    RESEARCH_PANIC = 1,
    RESEARCH_UNDEFINED,
    RESEARCH_PREFETCH_ABORT,
    RESEARCH_DATA_ABORT,
    RESEARCH_HANG,
    RESEARCH_FREEZE,
};

struct research_fault
{
    uint32_t pc, sp, lr, cpsr, far, fsr;
    const uint32_t *regs; /* r0–r12, when captured by the exception veneer */
};

#ifdef HAVE_RESEARCH_SHELL
/* Before audio_init: takes the shell's buffers from core_alloc */
void research_shell_init(void);
#endif
void NORETURN_ATTR research_capture(enum research_fault_kind kind,
    const struct research_fault *fault, const char *message);
#endif
#endif
