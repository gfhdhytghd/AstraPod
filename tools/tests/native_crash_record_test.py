#!/usr/bin/env python3
"""Exercise the retained 6G crash payload and boot-time export with fake storage."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

PROBE = r"""
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <fcntl.h>
#include "crumb_crash.h"

#define ROCKBOX_DIR "/.rockbox"
static volatile struct crumb_crash dram, iram1;
#define CRASH_A (&dram)
#define CRASH_B (&iram1)

enum failure { NONE, OPEN, PARTIAL_WRITE, SYNC, CLOSE, RENAME };
static enum failure fail;
static char disk[640], pending[640];
static int disk_writes, rename_calls;

static int fake_open(const char *name, int flags, int mode)
{
    assert(!strcmp(name, ROCKBOX_DIR "/crash-native.tmp"));
    assert(flags == (O_WRONLY | O_CREAT | O_TRUNC) && mode == 0666);
    return fail == OPEN ? -1 : 3;
}
static ssize_t fake_write(int fd, const void *data, size_t size)
{
    assert(fd == 3 && size < sizeof(pending));
    if (fail == PARTIAL_WRITE)
        size /= 2;
    memcpy(pending, data, size);
    pending[size] = '\0';
    return size;
}
static int fake_fsync(int fd)
{
    assert(fd == 3);
    return fail == SYNC ? -1 : 0;
}
static int fake_close(int fd)
{
    assert(fd == 3);
    return fail == CLOSE ? -1 : 0;
}
static int fake_remove(const char *name)
{
    assert(!strcmp(name, ROCKBOX_DIR "/crash-native.tmp"));
    *pending = '\0';
    return 0;
}
static int fake_rename(const char *from, const char *to)
{
    assert(!strcmp(from, ROCKBOX_DIR "/crash-native.tmp"));
    assert(!strcmp(to, ROCKBOX_DIR "/crash-native.txt"));
    rename_calls++;
    if (fail == RENAME)
        return -1;
    strcpy(disk, pending);
    *pending = '\0';
    disk_writes++;
    return 0;
}
#define open fake_open
#define write fake_write
#define fsync fake_fsync
#define close fake_close
#define remove fake_remove
#define rename fake_rename

/* production export implementation */
@REPORT@

int main(void)
{
    strcpy(disk, "previous report");
    crash_report();
    assert(!rename_calls && !strcmp(disk, "previous report"));

    struct crumb_crash_context panic = {
        .kind = CRUMB_CRASH_PANIC, .pc = 0x08001234, .sp = 0x08400000,
        .status = 0x1f,
    };
    crumb_crash_store(CRASH_A, 10, "firmware-1", "Stack overflow in menu", &panic);
    crumb_crash_store(CRASH_B, 10, "firmware-1", "Stack overflow in menu", &panic);
    assert(crumb_crash_latest(CRASH_A, CRASH_B) == CRASH_A);

    /* Corrupted or interrupted DRAM write must leave the IRAM1 copy useful. */
    CRASH_A->data.message[2] ^= 1;
    assert(crumb_crash_latest(CRASH_A, CRASH_B) == CRASH_B);
    CRASH_A->check = 0;

    for (fail = OPEN; fail <= RENAME; fail++)
    {
        crash_report();
        assert(!disk_writes && !strcmp(disk, "previous report"));
        assert(crumb_crash_valid(CRASH_B));
    }
    assert(rename_calls == 1); /* only the complete, synced output reached rename */

    fail = NONE;
    crash_report();
    assert(disk_writes == 1);
    assert(strstr(disk, "Firmware: firmware-1\nBoot: 10\n"));
    assert(strstr(disk, "Panic: Stack overflow in menu\npc: 08001234"));
    assert(strstr(disk, "sp: 08400000"));
    assert(!crumb_crash_latest(CRASH_A, CRASH_B));
    crash_report();
    assert(disk_writes == 1); /* no duplicate export on a later clean boot */

    struct crumb_crash_context abort = {
        .kind = CRUMB_CRASH_EXCEPTION, .exception = 2, .pc = 0x08101010,
        .sp = 0x08400000, .lr = 0x08003456, .status = 0x6000001f,
        .fault_status = 5, .fault_address = 0x10,
    };
    crumb_crash_store(CRASH_A, 11, "firmware-1", "old panic", &panic);
    crumb_crash_store(CRASH_B, 12, "firmware-2", "Data abort", &abort);
    assert(crumb_crash_latest(CRASH_A, CRASH_B) == CRASH_B);
    crash_report();
    assert(disk_writes == 2);
    assert(strstr(disk, "ARM exception: Data abort"));
    assert(strstr(disk, "lr: 08003456  status: 6000001f"));
    assert(strstr(disk, "exception: 2  fsr: 00000005  far: 00000010"));

    char long_text[500];
    memset(long_text, 'a', sizeof(long_text) - 1);
    long_text[sizeof(long_text) - 1] = '\0';
    crumb_crash_store(CRASH_A, UINT32_MAX, long_text, long_text, &panic);
    crumb_crash_store(CRASH_B, 0, "firmware-3", "wrapped boot", &panic);
    assert(CRASH_A->data.version[62] == 'a' && CRASH_A->data.version[63] == '\0');
    assert(CRASH_A->data.message[254] == 'a' && CRASH_A->data.message[255] == '\0');
    assert(crumb_crash_valid(CRASH_A));
    assert(crumb_crash_latest(CRASH_A, CRASH_B) == CRASH_B);
    puts("PASS retained crash checksum, fallback, formatting, and retried boot export");
}
"""


def main():
    source = (ROOT / "firmware/kernel/crumbs.c").read_text()
    start = source.index("static void crash_report(void)")
    end = source.index("void crumbs_report(void)", start)
    formatter = source[start:end]
    panic = (ROOT / "firmware/panic.c").read_text()
    assert panic.index("va_end( ap );") < panic.index("crumbs_crash_record(panic_buf")
    assert panic.index("crumbs_crash_record(panic_buf") < panic.index("lcd_set_viewport(NULL)")
    exception = (ROOT / "firmware/target/arm/system-arm-classic.c").read_text()
    assert exception.index("crumbs_crash_record(uiename[num]") < exception.index("lcd_clear_display()")
    main_c = (ROOT / "apps-ipod/main.c").read_text()
    assert main_c.index("disk_mount_all();") < main_c.index("crumbs_report();")

    with tempfile.TemporaryDirectory(prefix="podbox-crash-native-") as folder:
        scratch = Path(folder)
        (scratch / "config.h").write_text(
            "#define CONFIG_PLATFORM 1\n#define PLATFORM_NATIVE 1\n"
            "#define CONFIG_CPU 8702\n#define S5L8702 8702\n"
        )
        (scratch / "crumbs.h").write_bytes((ROOT / "firmware/export/crumbs.h").read_bytes())
        harness = scratch / "test.c"
        harness.write_text(PROBE.replace("@REPORT@", formatter))
        binary = scratch / "test"
        subprocess.run(
            [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
             "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
             "-I", str(scratch), "-I", str(ROOT / "firmware/export"),
             "-I", str(ROOT / "firmware/kernel/include"), str(harness), "-o", str(binary)],
            check=True,
        )
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
