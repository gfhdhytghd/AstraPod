/* SPDX-License-Identifier: GPL-2.0-or-later
 * FU88 research shell on the iPod 6G: buffers, worker thread, PodLink glue
 * and the hardware port of research_shell.c.
 *
 * The PodLink worker hands a request over and returns at once; the shell
 * thread runs it, so a slow command never stalls the link, the UI or audio.
 * All buffers come from core_alloc at boot (none are static).
 */
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "thread.h"
#include "semaphore.h"
#include "core_alloc.h"
#include "file.h"
#include "dir.h"
#include "s5l87xx.h"
#include "clocking-s5l8702.h"
#include "crumbs.h"
#include "rbversion.h"
#include "research_shell.h"

#define DIR_PATH    ROCKBOX_DIR "/research"
#define STACK_SIZE  8192
#define SCRATCH     (64 * 1024)

enum { IDLE, QUEUED, REPLY };
static struct
{
    uint8_t *req, *reply, *scratch;
    long *stack;
    size_t req_len, reply_len;
    int state;
    bool binary, boosted, gates_saved, ready;
    uint32_t pwr0, pwr1;
    struct semaphore wake;
    /* A request that arrives while one runs gets this short answer */
    char busy[48];
    size_t busy_len;
} rs;

static void shell_thread(void)
{
    while (true)
    {
        semaphore_wait(&rs.wake, TIMEOUT_BLOCK);
        if (rs.state != QUEUED)
            continue;
        rs.reply_len = rs_exec(rs.req, rs.req_len, rs.reply, &rs.binary);
        rs.state = rs.reply_len ? REPLY : IDLE;
    }
}

void research_shell_init(void)
{
    size_t total = STACK_SIZE + 2 * RS_FRAME_MAX + SCRATCH;
    int handle = core_alloc(total);
    if (handle <= 0)
        return;
    uint8_t *base = core_get_data_pinned(handle);   /* never moves */
    rs.stack = (long *)base;
    rs.req = base + STACK_SIZE;
    rs.reply = rs.req + RS_FRAME_MAX;
    rs.scratch = rs.reply + RS_FRAME_MAX;
    memset(rs.scratch, 0, SCRATCH);
    semaphore_init(&rs.wake, 1, 0);
    rs.ready = create_thread(shell_thread, rs.stack, STACK_SIZE, 0, "research"
                             IF_PRIO(, PRIORITY_BACKGROUND) IF_COP(, CPU)) != 0;
    rlog(CR_RESEARCH_MARK, 0x88000000u | rs.ready);
}

/*** PodLink worker side (called from Rust under the service mutex) ***/

void pfu_research_request(const uint8_t *data, size_t len)
{
    if (!rs.ready || len > RS_FRAME_MAX)
        return;
    if (rs.state != IDLE)
    {
        /* Echo the id: "RS\t1\t<id>\tERR\tbusy" */
        size_t n = 5;
        while (n < len && n < 5 + 16 && data[n] != '\t')
            n++;
        memcpy(rs.busy, data, n);
        memcpy(rs.busy + n, "\tERR\tbusy", 9);
        rs.busy_len = n + 9;
        return;
    }
    memcpy(rs.req, data, len);
    rs.req_len = len;
    rs.state = QUEUED;
    semaphore_release(&rs.wake);
}

/* The pending reply: 0 none, 1 TEXT, 2 BINARY */
int pfu_research_reply(const uint8_t **data, size_t *len)
{
    if (rs.busy_len)
    {
        *data = (const uint8_t *)rs.busy;
        *len = rs.busy_len;
        return 1;
    }
    if (rs.state != REPLY)
        return 0;
    *data = rs.reply;
    *len = rs.reply_len;
    return rs.binary ? 2 : 1;
}

void pfu_research_sent(void)
{
    if (rs.busy_len)
    {
        rs.busy_len = 0;
        return;
    }
    rs.state = IDLE;
    if (rs_reboot_requested)
    {
        sleep(HZ / 5);   /* let the reply leave the CDC FIFO */
        system_reboot();
    }
}

/*** Port ***/

uint32_t rs_port_read(uint32_t addr, int width)
{
    return width == 1 ? *(volatile uint8_t *)addr :
           width == 2 ? *(volatile uint16_t *)addr : *(volatile uint32_t *)addr;
}

void rs_port_write(uint32_t addr, int width, uint32_t value)
{
    if (width == 1)
        *(volatile uint8_t *)addr = value;
    else if (width == 2)
        *(volatile uint16_t *)addr = value;
    else
        *(volatile uint32_t *)addr = value;
}

uint32_t rs_port_scratch(uint32_t *size)
{
    *size = rs.scratch ? SCRATCH : 0;
    return (uint32_t)rs.scratch;
}

bool rs_port_in_firmware(uint32_t addr, uint32_t len)
{
    extern char _textstart[], _textend[], _icodestart[], _icodeend[];
    return (addr >= (uint32_t)_textstart && addr + len <= (uint32_t)_textend) ||
           (addr >= (uint32_t)_icodestart && addr + len <= (uint32_t)_icodeend);
}

uint32_t rs_port_call(uint32_t addr, const uint32_t a[4])
{
    /* Bit 0 selects Thumb: an interworking call does the rest */
    uint32_t (*fn)(uint32_t, uint32_t, uint32_t, uint32_t) = (void *)addr;
    return fn(a[0], a[1], a[2], a[3]);
}

uint32_t rs_port_usec(void) { return USEC_TIMER; }
void rs_port_yield(void) { yield(); }
void rs_port_cache_sync(void) { commit_discard_idcache(); }
void rs_port_mark(uint32_t value) { rlog(CR_RESEARCH_MARK, value); }
void rs_port_reboot(void) { system_reboot(); }

int rs_port_info(char *text, size_t cap)
{
    return snprintf(text, cap, "ver=%s freq=%ld boost=%d up=%ld hz=%d buf=%08lx size=%d",
                    RBVERSION, (long)FREQ, get_cpu_boost_counter(), current_tick, HZ,
                    (unsigned long)rs.scratch, SCRATCH);
}

void rs_port_boost(bool on)
{
    if (on != rs.boosted)
        cpu_boost(on);
    rs.boosted = on;
}

/* Gates per Chikuma (PWRCON0[2], PWRCON1[3]) and Rockbox's SM1/IRAM1 gate
 * (PWRCON0[4]). "off" restores exactly what "on" found. */
void rs_port_sm1_power(bool on)
{
    if (on)
    {
        if (!rs.gates_saved)
            rs.pwr0 = PWRCON(0), rs.pwr1 = PWRCON(1), rs.gates_saved = true;
        clockgate_enable(2, true);
        clockgate_enable(CLOCKGATE_SM1, true);
        clockgate_enable(32 + 3, true);
    }
    else if (rs.gates_saved)
    {
        PWRCON(0) = rs.pwr0;
        PWRCON(1) = rs.pwr1;
        rs.gates_saved = false;
    }
}

static void path(char *out, const char *name)
{
    snprintf(out, MAX_PATH, DIR_PATH "/%s", name);
}

int rs_port_file_get(const char *name, uint32_t off, uint8_t *buf, uint32_t len)
{
    char p[MAX_PATH];
    path(p, name);
    int fd = open(p, O_RDONLY);
    if (fd < 0)
        return -1;
    int n = lseek(fd, off, SEEK_SET) == (off_t)off ? read(fd, buf, len) : -1;
    close(fd);
    return n;
}

int rs_port_file_put(const char *name, uint32_t off, const uint8_t *buf, uint32_t len)
{
    char p[MAX_PATH];
    path(p, name);
    mkdir(DIR_PATH);
    int fd = open(p, off ? O_WRONLY | O_CREAT : O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return -1;
    int n = lseek(fd, off, SEEK_SET) == (off_t)off ? (len ? write(fd, buf, len) : 0) : -1;
    if (close(fd) < 0)
        n = -1;
    return n;
}

int rs_port_file_rm(const char *name)
{
    char p[MAX_PATH];
    path(p, name);
    return remove(p);
}

int rs_port_file_ls(char *text, size_t cap)
{
    DIR *dir = opendir(DIR_PATH);
    if (!dir)
        return -1;
    size_t n = 0;
    struct dirent *e;
    while ((e = readdir(dir)) && n + 48 < cap)
    {
        struct dirinfo info = dir_get_info(dir, e);
        if (!(info.attribute & ATTR_DIRECTORY))
            n += snprintf(text + n, cap - n, "%s%s:%ld", n ? " " : "", e->d_name,
                          (long)info.size);
    }
    closedir(dir);
    return n;
}
