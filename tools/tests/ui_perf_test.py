#!/usr/bin/env python3
"""Check bounded UI frame capture with a controllable clock and log sink."""
from pathlib import Path
import subprocess
import tempfile


root = Path(__file__).resolve().parents[2]
ui = root / "apps-ipod/ui"
stubs = {
    "config.h": "",
    "system.h": """#include <stdint.h>
extern uint32_t test_clock;
#define USEC_TIMER test_clock
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
""",
    "core_alloc.h": """#include <stddef.h>
int core_alloc_free_space(size_t size);
int core_free(int handle);
void *core_get_data_pinned(int handle);
void core_put_data_pinned(void *data);
""",
    "file.h": """#include <stdbool.h>
bool file_exists(const char *path);
""",
    "logfile.h": """#include <stddef.h>
#include <sys/types.h>
int logfile_open(const char *path);
ssize_t logfile_write(int fd, const void *data, size_t size);
int logfile_close(int fd);
""",
    "rbpaths.h": '#define ROCKBOX_DIR "/.rockbox"',
    "string-extra.h": """#include <stddef.h>
size_t strlcpy(char *dst, const char *src, size_t size);
""",
    "thread.h": "unsigned thread_self(void);",
}

test = r"""
#include "perf.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

uint32_t test_clock;
static bool marker, reject_rows;
static unsigned thread = 1, opened, writes;
static char journal[65536];
static size_t journal_size;
static void *allocations[2];
static bool pinned[2];

bool file_exists(const char *path)
{
    assert(!strcmp(path, "/.rockbox/ui-perf.enable"));
    return marker;
}

unsigned thread_self(void) { return thread; }

int core_alloc_free_space(size_t size)
{
    for (int i = 0; i < 2; ++i)
    {
        if (!allocations[i])
        {
            if (i == 1 && reject_rows)
                return -1;
            allocations[i] = malloc(size);
            assert(allocations[i]);
            return i + 1;
        }
    }
    abort();
}

void *core_get_data_pinned(int handle)
{
    int i = handle - 1;
    assert(allocations[i] && !pinned[i]);
    pinned[i] = true;
    return allocations[i];
}

void core_put_data_pinned(void *data)
{
    for (int i = 0; i < 2; ++i)
        if (allocations[i] == data && pinned[i])
        {
            pinned[i] = false;
            return;
        }
    abort();
}

int core_free(int handle)
{
    int i = handle - 1;
    assert(allocations[i] && !pinned[i]);
    free(allocations[i]);
    allocations[i] = NULL;
    return 0;
}

size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t length = strlen(src);
    if (size)
    {
        size_t n = length < size ? length : size - 1;
        memcpy(dst, src, n);
        dst[n] = 0;
    }
    return length;
}

int logfile_open(const char *path)
{
    assert(!strcmp(path, "/.rockbox/ui-perf.txt"));
    ++opened;
    journal_size = writes = 0;
    return 5;
}

ssize_t logfile_write(int fd, const void *data, size_t size)
{
    assert(fd == 5 && size < sizeof(journal) - journal_size);
    ++writes;
    memcpy(journal + journal_size, data, size);
    journal_size += size;
    journal[journal_size] = 0;
    return size;
}

int logfile_close(int fd) { assert(fd == 5); return 0; }

static void submit(uint32_t delta, uint32_t render, uint32_t transfer)
{
    test_clock += delta;
    ui_perf_note_render(render);
    ui_perf_note_submit(transfer);
}

int main(void)
{
    ui_perf_init();
    assert(!ui_perf_begin("disabled"));
    marker = true;
    ui_perf_init();

    unsigned capture = ui_perf_begin("push");
    assert(capture && !ui_perf_begin("nested"));
    thread = 2;
    submit(1000, 100000, 200000);
    ui_perf_end(capture);
    thread = 1;
    test_clock = UINT32_MAX - 5000;
    ui_perf_note_render(3000);
    submit(0, 2000, 2000);
    submit(16700, 18000, 1000);
    submit(33334, 3000, 1000);
    assert(!opened);
    ui_perf_end(capture);
    assert(opened == 1 && writes == 2);
    assert(strstr(journal, "route=push seen=3 retained=3 dropped=0 rows=3 "));
    assert(strstr(journal, "work_p99_f=1.1400 interval_p99_f=2.0000"));
    assert(strstr(journal, "work_misses=1 interval_misses=2 skipped_slots=1"));
    assert(strstr(journal, "render_worst_f=1.0800 render_misses=1"));
    assert(strstr(journal, "\n1,0.3000,0.1200,0.4200,0.0000,0,0,0\n"));
    assert(strstr(journal, "\n2,1.0800,0.0600,1.1400,1.0020,1,1,0\n"));
    assert(!allocations[0] && !allocations[1]);

    capture = ui_perf_begin("cover-flow");
    submit(0, 30000, 5000);
    for (unsigned i = 1; i < 520; ++i)
        submit(16667, 1000, 1000);
    ui_perf_end(capture);
    assert(opened == 2 && writes == 2);
    assert(strstr(journal, "route=cover-flow seen=520 retained=512 dropped=8 rows=512 "));
    assert(strstr(journal, "work_p99_f=0.1200 interval_p99_f=1.0000"));
    assert(strstr(journal, "work_worst_f=2.1000 interval_worst_f=1.0000"));
    assert(strstr(journal, "render_worst_f=1.8000 render_misses=1"));
    assert(strstr(journal, "\n9,0.0600,0.0600,0.1200,1.0000,0,0,0\n"));
    assert(strstr(journal, "\n520,0.0600,0.0600,0.1200,1.0000,0,0,0\n"));

    reject_rows = true;
    capture = ui_perf_begin("pop");
    submit(0, 100, 100);
    ui_perf_end(capture);
    assert(opened == 3 && writes == 1);
    assert(strstr(journal, "route=pop seen=1 retained=1 dropped=0 rows=0 "));
    assert(!allocations[0] && !allocations[1]);

    unsigned passive = ui_perf_begin_passive("status-icons");
    submit(0, 100, 100);
    capture = ui_perf_begin("push");
    assert(passive && capture && capture != passive && opened == 3);
    assert(!ui_perf_begin_passive("status-icons"));
    ui_perf_end(passive);
    assert(opened == 3);
    submit(0, 1000, 1000);
    ui_perf_end(capture);
    assert(opened == 4 && strstr(journal, "route=push seen=1 "));
    puts("PASS frame clock wrap, p99, misses, ring cap, and exit-only logging");
}
"""

with tempfile.TemporaryDirectory(prefix="podbox-perf-test-") as temp:
    work = Path(temp)
    for name, code in stubs.items():
        (work / name).write_text(code)
    (work / "test.c").write_text(test)
    binary = work / "test"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    "-I", str(work), "-I", str(ui), str(work / "test.c"),
                    str(ui / "perf.c"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
