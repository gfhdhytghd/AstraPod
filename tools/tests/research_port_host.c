/* SPDX-License-Identifier: GPL-2.0-or-later
 * FU88: RAM-only port of the research shell for host tests. A small fake of
 * the sm1 block, VIC0, clock gates and IRAM1; files under $RS_TEST_DIR.
 * The PodLink glue runs each request synchronously.
 */
#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "research_shell.h"

#define FAKE_SCRATCH 0x09000000u
#define FAKE_FN      (FAKE_SCRATCH + 0x100)
#define FAKE_TEXT    0x08000000u

static struct { uint32_t base, size; uint8_t *mem; } regions[] = {
    { 0x38500000, 0x2000, NULL },   /* sm1 */
    { 0x22020000, 0x20000, NULL },  /* IRAM1: P, X, Y, parameter page */
    { 0x38E00000, 0x2000, NULL },   /* VIC0, VIC1 */
    { 0x3C500000, 0x1000, NULL },   /* clock and power */
    { 0x3CA00000, 0x1000, NULL },   /* I2S */
    { 0x38200000, 0x1000, NULL },   /* DMAC0 */
    { FAKE_SCRATCH, 0x10000, NULL },
    { 0x3C300000, 0x1000, NULL },   /* SPI0: must never be written */
};
static uint32_t fake_usec;
static int spi_writes, unmapped;
static uint32_t gates_pwr0 = 0xFFFFFFFF, gates_pwr1 = 0xFFFFFFFF;

static uint8_t *at(uint32_t addr, int width)
{
    for (unsigned i = 0; i < sizeof(regions) / sizeof(regions[0]); i++)
    {
        uint32_t a = addr;
        if (regions[i].base == FAKE_SCRATCH && (a & 0x40000000u))
            a &= ~0x40000000u;   /* uncached alias */
        if (a >= regions[i].base && a + width <= regions[i].base + regions[i].size)
        {
            if (!regions[i].mem)
                regions[i].mem = calloc(1, regions[i].size);
            return regions[i].mem + (a - regions[i].base);
        }
    }
    unmapped++;
    return NULL;
}

static uint32_t get(uint32_t addr, int width)
{
    uint8_t *p = at(addr, width);
    uint32_t v = 0;
    if (p)
        memcpy(&v, p, width);
    return v;
}

static void put(uint32_t addr, int width, uint32_t v)
{
    uint8_t *p = at(addr, width);
    if (p)
        memcpy(p, &v, width);
}

void fake_reset(void)
{
    for (unsigned i = 0; i < sizeof(regions) / sizeof(regions[0]); i++)
        if (regions[i].mem)
            memset(regions[i].mem, 0, regions[i].size);
    put(0x38500C80, 4, 0x004E7309);
    put(0x3C500048, 4, gates_pwr0);
    put(0x3C50004C, 4, gates_pwr1);
    spi_writes = unmapped = 0;
    rs_reboot_requested = false;
}

uint32_t rs_port_read(uint32_t addr, int width)
{
    fake_usec += 7;
    return get(addr, width);
}

void rs_port_write(uint32_t addr, int width, uint32_t value)
{
    if (addr >= 0x3C300000 && addr < 0x3C301000)
        spi_writes++;
    put(addr, width, value);
    /* Arming the engine completes at once: event bit 2 and VIC0 line 18 */
    if (addr == 0x38500984)
    {
        put(0x38500A98, 4, get(0x38500A98, 4) | 4);
        put(0x38E00008, 4, get(0x38E00008, 4) | 1u << 18);
    }
    if (addr == 0x38500C48 && value == 4)
    {
        put(0x38500A98, 4, get(0x38500A98, 4) & ~4u);
        put(0x38E00008, 4, get(0x38E00008, 4) & ~(1u << 18));
    }
}

uint32_t rs_port_scratch(uint32_t *size)
{
    *size = 0x10000;
    return FAKE_SCRATCH;
}

bool rs_port_in_firmware(uint32_t addr, uint32_t len)
{
    return addr >= FAKE_TEXT && addr + len <= FAKE_TEXT + 0x100000;
}

uint32_t rs_port_call(uint32_t addr, const uint32_t a[4])
{
    fake_usec += 50;
    if ((addr & ~1u) == FAKE_FN)
        return a[0] + a[1] + a[2] + a[3] + (addr & 1);
    return 0xDEADBEEF;
}

uint32_t rs_port_usec(void) { return fake_usec += 3; }
void rs_port_yield(void) { fake_usec += 500; }
void rs_port_cache_sync(void) {}
static uint32_t marks[8];
static int mark_count, boost_state;
void rs_port_mark(uint32_t value) { marks[mark_count++ & 7] = value; }
void rs_port_reboot(void) { abort(); }
void rs_port_boost(bool on) { boost_state = on; }

int rs_port_info(char *text, size_t cap)
{
    return snprintf(text, cap, "ver=host freq=0 boost=%d buf=%08x size=65536",
                    boost_state, FAKE_SCRATCH);
}

void rs_port_sm1_power(bool on)
{
    static bool saved;
    static uint32_t p0, p1;
    if (on)
    {
        if (!saved)
            p0 = get(0x3C500048, 4), p1 = get(0x3C50004C, 4), saved = true;
        put(0x3C500048, 4, get(0x3C500048, 4) & ~(1u << 2 | 1u << 4));
        put(0x3C50004C, 4, get(0x3C50004C, 4) & ~(1u << 3));
    }
    else if (saved)
    {
        put(0x3C500048, 4, p0);
        put(0x3C50004C, 4, p1);
        saved = false;
    }
}

static void path(char *out, size_t cap, const char *name)
{
    const char *dir = getenv("RS_TEST_DIR");
    assert(dir);
    snprintf(out, cap, "%s/%s", dir, name);
}

int rs_port_file_get(const char *name, uint32_t off, uint8_t *buf, uint32_t len)
{
    char p[512];
    path(p, sizeof p, name);
    int fd = open(p, O_RDONLY);
    if (fd < 0)
        return -1;
    int n = lseek(fd, off, SEEK_SET) == (off_t)off ? (int)read(fd, buf, len) : -1;
    close(fd);
    return n;
}

int rs_port_file_put(const char *name, uint32_t off, const uint8_t *buf, uint32_t len)
{
    char p[512];
    path(p, sizeof p, name);
    int fd = open(p, off ? O_WRONLY | O_CREAT : O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return -1;
    int n = lseek(fd, off, SEEK_SET) == (off_t)off ? (len ? (int)write(fd, buf, len) : 0) : -1;
    close(fd);
    return n;
}

int rs_port_file_rm(const char *name)
{
    char p[512];
    path(p, sizeof p, name);
    return unlink(p);
}

int rs_port_file_ls(char *text, size_t cap)
{
    DIR *dir = opendir(getenv("RS_TEST_DIR"));
    if (!dir)
        return -1;
    size_t n = 0;
    struct dirent *e;
    while ((e = readdir(dir)) && n + 48 < cap)
        if (e->d_name[0] != '.')
        {
            char p[512];
            struct stat st;
            path(p, sizeof p, e->d_name);
            stat(p, &st);
            n += snprintf(text + n, cap - n, "%s%s:%ld", n ? " " : "", e->d_name,
                          (long)st.st_size);
        }
    closedir(dir);
    return (int)n;
}

/* PodLink glue, synchronous */
static uint8_t host_reply[RS_FRAME_MAX];
static size_t host_reply_len;
static bool host_binary;
int research_pings;
void pfu_research_ping(void) { research_pings++; }
void pfu_research_request(const uint8_t *data, size_t len)
{
    host_reply_len = rs_exec(data, len, host_reply, &host_binary);
}
int pfu_research_reply(const uint8_t **data, size_t *len)
{
    if (!host_reply_len)
        return 0;
    *data = host_reply;
    *len = host_reply_len;
    return host_binary ? 2 : 1;
}
void pfu_research_sent(void) { host_reply_len = 0; }
