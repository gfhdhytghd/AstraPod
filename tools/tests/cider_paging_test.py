"""Run the native request/cache code against deterministic CL frames."""
from pathlib import Path
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
(out / 'config.h').write_text('')
(out / 'kernel.h').write_text(
    '#define HZ 100\n#define TIME_BEFORE(a,b) ((long)((a)-(b))<0)\n'
    'extern long current_tick;\n')
panel = (root / 'apps-ipod/screens/covers/panel_cache.c').read_text()
query_remote = re.search(r'(?ms)^static bool query_remote\(.*?^}', panel).group()
(out / 'paging.c').write_text(r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "screens/playback/cider_library.h"

long current_tick;
static char wire[16000], request[512];

static unsigned next(const char *route, int offset)
{
    unsigned session, id, limit;
    int actual;
    char actual_route[380];
    assert(cider_library_next(request, sizeof request, 7));
    assert(sscanf(request, "cq\t%u\t%u\t%d\t%u\t%379s",
                  &session, &id, &actual, &limit, actual_route) == 5);
    assert(session == 7 && actual == offset && limit == 50);
    assert(!strcmp(actual_route, route));
    return id;
}

static int frame(unsigned id, unsigned revision, int offset, int count)
{
    int n = snprintf(wire, sizeof wire, "CL\t7\t%u\t%u\t%d\t200\t0\t%d\n",
                     id, revision, offset, count);
    for (int i = offset; i < offset + count; i++)
        n += snprintf(wire + n, sizeof wire - n,
                      "s%d\tSong %d\tArtist\t-\t123000\t0\tsong\tplay/s%d\t181818\t2020\n",
                      i, i, i);
    return n;
}

static void receive(unsigned id, unsigned revision, int offset, int count)
{
    int n = frame(id, revision, offset, count);
    assert(cider_library_receive(wire, n, 7, revision));
}

#define HAVE_USB_PODLINK
#define USEC_TIMER (current_tick + 1)
#define PANEL_PAGE_ROWS 32
#define PP_REMOTE 0
#define MIN(a,b) ((a) < (b) ? (a) : (b))
struct panel_disk { int total; unsigned int count; };
struct panel_cache_row
{
    unsigned int duration, order;
    char title[124], route[320], remote_id[80];
};
struct panel_cached
{
    struct panel_disk disk;
    struct panel_cache_row rows[PANEL_PAGE_ROWS];
};
static struct
{
    unsigned int revision, key;
    int first;
    char route[320];
    bool legacy_labels;
} work;
static unsigned int remote_key, remote_at, remote_rows;
static int remote_first;
static bool remote_stale;
#define cider_panel_revision cider_library_revision
#define cider_track_page_open cider_library_tracks_open
#define cider_track_page_total cider_library_tracks_total
#define cider_track_page_row cider_library_track_row
static void copy_field(char *out, const char *in, size_t n)
{
    snprintf(out, n, "%s", in);
}
#define strlcpy copy_field
static bool cancelled(void) { return false; }
static unsigned int clean_title(char *title, unsigned int length,
                                unsigned int order, bool labels)
{
    (void)title; (void)length; (void)labels;
    return order;
}
static void panel_perf_record(int kind, unsigned int time, int rows, int count)
{
    (void)kind; (void)time; (void)rows; (void)count;
}
''' + query_remote + r'''
int main(void)
{
    struct cider_row row;
    const unsigned signature = 0xfeddcba9u;
    cider_library_open("albums");
    cider_library_invalidate(signature);
    assert(!cider_library_pending() && cider_library_total() == -1);
    unsigned id = next("albums", 0);
    current_tick += 600;
    assert(cider_library_pending());
    assert(!cider_library_next(request, sizeof request, 7));
    receive(id, signature, 0, 25);
    assert(cider_library_total() == 200 && !cider_library_pending());
    assert(cider_library_row(24, &row) && !strcmp(row.id, "s24"));
    assert(!cider_library_peek(180, &row));
    id = next("albums", 25);
    int n = frame(id, signature + 1, 25, 25);
    assert(!cider_library_receive(wire, n, 7, signature));
    unsigned retry = next("albums", 25);
    assert(id != retry);
    n = frame(retry, signature, 25, 25);
    assert(!cider_library_receive(wire, n - 1, 7, signature));
    assert(cider_library_receive(wire, n, 7, signature));
    assert(cider_library_row(49, &row));

    /* Keep both album pages while the panel consumes short track pages. */
    struct panel_cached panel_data = {0};
    bool persist;
    work.key = 1;
    work.revision = signature;
    strcpy(work.route, "album/one");
    assert(!query_remote(&panel_data, &persist));
    id = next("album/one?format=panel", 0);
    n = frame(id, signature, 0, 16);
    char *stale_bit = strstr(wire, "\t200\t0\t16\n");
    assert(stale_bit);
    stale_bit[5] = '1';
    assert(cider_library_receive(wire, n, 7, signature));
    assert(!query_remote(&panel_data, &persist) && remote_rows == 16);
    receive(next("album/one?format=panel", 16), signature, 16, 16);
    assert(query_remote(&panel_data, &persist) && !persist && remote_rows == 32);
    assert(cider_library_track_row(31, &row));
    for (unsigned int i = 0; i < 32; i++)
    {
        char key[8];
        snprintf(key, sizeof key, "s%u", i);
        assert(!strcmp(panel_data.rows[i].remote_id, key));
    }
    assert(cider_library_peek(0, &row) && cider_library_peek(49, &row));
    assert(!cider_library_row(50, &row));
    receive(next("albums", 50), signature, 50, 50);
    assert(cider_library_track_row(31, &row));
    assert(cider_library_peek(49, &row) && cider_library_peek(99, &row));
    cider_library_tracks_open("");

    /* Exact routes retain their totals/rows across a view switch and back. */
    cider_library_open("albums?format=panel");
    assert(cider_library_total() == -1 && !cider_library_peek(50, &row));
    receive(next("albums?format=panel", 0), signature, 0, 50);
    cider_library_open("albums");
    assert(cider_library_total() == 200 && cider_library_peek(99, &row));
    cider_library_open("albums?format=panel");
    assert(cider_library_total() == 200 && cider_library_peek(0, &row));

    cider_library_open("albums");
    assert(!cider_library_row(120, &row));
    receive(next("albums", 120), signature, 120, 50);
    /* Paging through a child keeps the last warm page of its parent. */
    assert(!cider_library_row(170, &row));
    receive(next("albums", 170), signature, 170, 30);
    cider_library_open("albums?format=panel");
    assert(cider_library_peek(0, &row));
    cider_library_open("albums");
    cider_library_invalidate(3);
    assert(cider_library_revision() == 3 && cider_library_total() == -1);
    assert(!cider_library_row(120, &row));
    receive(next("albums", 120), 3, 120, 50);
    assert(!cider_library_row(119, &row));
    receive(next("albums", 78), 3, 78, 20);
    assert(!cider_library_row(119, &row));
    receive(next("albums", 98), 3, 98, 22);
    assert(cider_library_row(119, &row));
    assert(cider_library_peek(120, &row));
    assert(!cider_library_row(10, &row));
    id = next("albums", 0);
    cider_library_invalidate(2);
    retry = next("albums", 0);
    n = frame(id, 3, 0, 50);
    assert(!cider_library_receive(wire, n, 7, 2));
    receive(retry, 2, 0, 50);

    /* Reopening a stale view retries its first page. */
    cider_library_open("albums?format=panel");
    id = next("albums?format=panel", 0);
    n = snprintf(wire, sizeof wire, "CL\t7\t%u\t2\t0\t0\t1\t0\n", id);
    assert(cider_library_receive(wire, n, 7, 2));
    assert(cider_library_total() == 0 && cider_library_stale());
    cider_library_open("albums");
    cider_library_open("albums?format=panel");
    assert(cider_library_total() == -1 && !cider_library_stale());
    next("albums?format=panel", 0);
    cider_library_reset();
    assert(!cider_library_revision() && cider_library_total() == -1);
    puts("Cider paging: warm routes, album/track slot split, passive lookups, "
         "backward short pages, stale request retry and unordered signatures PASS");
}
''')
subprocess.run([
    'cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-g',
    '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
    '-I' + str(out), '-I' + str(root / 'apps-ipod'), str(out / 'paging.c'),
    str(root / 'apps-ipod/screens/playback/cider_library.c'),
    '-o', str(out / 'paging'),
], check=True)
subprocess.run([str(out / 'paging')], check=True)
