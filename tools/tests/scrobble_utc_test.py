"""Exercise the real scrobble header/migration on a temporary player root."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

PORTS = r'''
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "system/scrobble_log.h"

static int test_open(const char *path, int flags, ...)
{
    return open(path + 1, flags, 0666);
}

static int test_rename(const char *from, const char *to)
{
    return rename(from + 1, to + 1);
}

static int logfile_open_header(const char *path, const char *header)
{
    int fd = test_open(path, O_RDWR | O_CREAT | O_APPEND);
    assert(fd >= 0);
    if (lseek(fd, 0, SEEK_END) == 0)
        assert(write(fd, header, strlen(header)) == (ssize_t)strlen(header));
    return fd;
}

#define TARGET_NAME "ipod6g"
#define open test_open
#define rename test_rename
'''

TEST = r'''
static void write_file(const char *path, const char *bytes)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    assert(fd >= 0);
    assert(write(fd, bytes, strlen(bytes)) == (ssize_t)strlen(bytes));
    close(fd);
}

static void check_file(const char *path, const char *expected)
{
    char buf[256];
    int fd = open(path, O_RDONLY);
    assert(fd >= 0);
    ssize_t bytes = read(fd, buf, sizeof(buf));
    assert(bytes == (ssize_t)strlen(expected) && !memcmp(buf, expected, bytes));
    close(fd);
}

int main(void)
{
    const char *older = "#AUDIOSCROBBLER/1.1\n#TZ/UNKNOWN\nolder\n";
    const char *current = "#AUDIOSCROBBLER/1.1\n#TZ/UNKNOWN\ncurrent\n";
    write_file("/.scrobbler.1.log", older);
    write_file(SCROBBLER_LOG_PATH, current);
    write_file(SCROBBLER_LEGACY_STEM "_0001.log", "existing");
    int fd = scrobble_log_open();
    assert(fd >= 0);
    close(fd);
    check_file(SCROBBLER_LEGACY_STEM "_0001.log", "existing");
    check_file(SCROBBLER_LEGACY_STEM "_0002.log", older);
    check_file(SCROBBLER_LEGACY_STEM "_0003.log", current);
    check_file(SCROBBLER_LOG_PATH, utc_header);
    fd = scrobble_log_open();
    assert(fd >= 0);
    close(fd);
    assert(open(SCROBBLER_LEGACY_STEM "_0004.log", O_RDONLY) < 0);
    check_file(SCROBBLER_LOG_PATH, utc_header);
    puts("Scrobble UTC header, existing archives, migration and retry PASS");
}
'''

with tempfile.TemporaryDirectory(prefix="astrapod-scrobble-") as path:
    out = Path(path)
    source = (ROOT / "apps-ipod/system/scrobble_log.c").read_text()
    source = "\n".join(line for line in source.splitlines()
                       if line not in ('#include "config.h"', '#include "file.h"',
                                       '#include "logfile.h"'))
    (out / "test.c").write_text(PORTS + source + TEST)
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
                    "-I", str(ROOT / "apps-ipod"), "test.c", "-o", "test"],
                   cwd=out, check=True)
    subprocess.run([str(out / "test")], cwd=out, check=True)
