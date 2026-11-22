#!/usr/bin/env python3
"""Exercise the TSR runtime with a controllable worker and close acknowledgement."""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
out = Path(sys.argv[1]).resolve()
sdk = out / "plugins/sdk"
sdk.mkdir(parents=True, exist_ok=True)
(out / "config.h").write_text("#define CONFIG_BINFMT 1\n#define BINFMT_ROCK 2\n")
(out / "load_code.h").write_text("void lc_close(void *handle);\n")
(sdk / "plugin.h").write_text("""
#include "plugins/rock_tsr.h"
#define DEFAULT_STACK_SIZE 1024
#define OBJ_WAIT_TIMEDOUT (-1)
#define IF_PRIO(...)
#define IF_COP(...)
struct plugin_header { int dummy; };
struct semaphore { int count; };
unsigned thread_self(void);
unsigned create_thread(void (*start)(void), void *stack, unsigned bytes,
                       unsigned flags, const char *name);
void thread_wait(unsigned id);
void thread_exit(void) __attribute__((noreturn));
void semaphore_init(struct semaphore *sem, int max, int initial);
int semaphore_wait(struct semaphore *sem, int period);
void semaphore_release(struct semaphore *sem);
""")
exe = out / "rock_tsr_test"
subprocess.run([
    "cc", "-std=gnu11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
    "-fsanitize=undefined", "-fno-sanitize-recover=all", "-pthread",
    "-I", str(out), "-I", str(ROOT / "apps-ipod"),
    str(ROOT / "apps-ipod/plugins/rock_tsr.c"),
    str(ROOT / "tools/tests/fixtures/rock-tsr/host.c"), "-ldl", "-o", str(exe),
], check=True)
subprocess.run([str(exe)], timeout=10, check=True)
