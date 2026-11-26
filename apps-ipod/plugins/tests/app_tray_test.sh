#!/bin/sh
# Build the tray scheduler and disk store with host adapters. GPL v2+.
set -eu
root=$(cd "$(dirname "$0")/../../.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
: > "$out/config.h"
printf '#define ROCKBOX_DIR "."\n' > "$out/rbpaths.h"
printf '#include <fcntl.h>\n#include <stdio.h>\n#include <unistd.h>\n' > "$out/file.h"
cat > "$out/kernel.h" <<'HEADER'
#include <pthread.h>
struct mutex { pthread_mutex_t lock; };
#define mutex_init(m) ((m)->lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER)
#define mutex_lock(m) pthread_mutex_lock(&(m)->lock)
#define mutex_unlock(m) pthread_mutex_unlock(&(m)->lock)
HEADER
cat > "$out/timefuncs.h" <<'HEADER'
#include <stdbool.h>
#include <time.h>
static inline bool valid_time(const struct tm *tm)
{
    return tm && tm->tm_year >= 100 && tm->tm_year <= 199 &&
           tm->tm_mon >= 0 && tm->tm_mon <= 11 && tm->tm_mday >= 1 &&
           tm->tm_mday <= 31 && tm->tm_hour >= 0 && tm->tm_hour <= 23 &&
           tm->tm_min >= 0 && tm->tm_min <= 59 &&
           tm->tm_sec >= 0 && tm->tm_sec <= 59;
}
HEADER
"${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror -fsanitize=undefined -pthread \
    -I"$out" -I"$root/apps-ipod" -idirafter "$root/firmware/include" \
    "$root/firmware/common/crc32.c" "$root/apps-ipod/plugins/app_tray.c" \
    "$root/apps-ipod/plugins/tests/app_tray_test.c" -o "$out/check"
(cd "$out" && ./check)
