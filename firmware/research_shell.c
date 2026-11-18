/* SPDX-License-Identifier: GPL-2.0-or-later
 * FU88 research shell: parser, guard list and commands. Hardware access goes
 * through the port (research_port-s5l8702.c, or the host test's fake).
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "research_shell.h"

char rs_last_command[24];
bool rs_reboot_requested;

#define SM1        0x38500000u
#define SM1_P      0x22020000u   /* program rows: 4 lanes of 0x4000 bytes */
#define SM1_X      0x22030000u   /* X:a at +a*4 (24-bit values) */
#define SM1_Y      0x22038000u   /* Y:a at +a*2 (16-bit) */
#define SM1_PARAM  0x2203FF00u
#define SM1_OUT    0x2203FF28u
#define VIC0       0x38E00000u
#define ALIAS      0x40000000u   /* uncached view of DRAM and IRAM */

static const struct { uint32_t lo, hi; uint8_t cls; } guards[] = {
    { 0x3C300000, 0x3C301000, RS_HARD },  /* SPI0: NOR flash, bootloader */
    { 0x3CE00000, 0x3CE01000, RS_HARD },  /* SPI1 */
    { 0x3D200000, 0x3D201000, RS_HARD },  /* SPI2 */
    { 0x00000000, 0x20000000, RS_SOFT },  /* DRAM: the running firmware */
    { 0x40000000, 0x60000000, RS_SOFT },  /* its uncached view */
    { 0x22000000, 0x22020000, RS_SOFT },  /* IRAM below the sm1 memories */
    { 0x62000000, 0x62020000, RS_SOFT },
    { 0x38100000, 0x38101000, RS_SOFT },  /* MIU: SDRAM controller */
    { 0x38300000, 0x38301000, RS_SOFT },  /* LCD */
    { 0x38400000, 0x38500000, RS_SOFT },  /* USB OTG: this link */
    { 0x38700000, 0x38701000, RS_SOFT },  /* ATA */
    { 0x38A00000, 0x38A01000, RS_SOFT },  /* NAND / ATA glue */
    { 0x38E00000, 0x38E02000, RS_SOFT },  /* VIC0, VIC1 */
    { 0x3C500000, 0x3C501000, RS_SOFT },  /* clock and power gates */
    { 0x3C800000, 0x3C801000, RS_SOFT },  /* watchdog */
};

static const struct { char name[6]; uint32_t base; uint8_t words; } blocks[] = {
    { "vic0", 0x38E00000, 5 }, { "vic1", 0x38E01000, 5 },
    { "dmac0", 0x38200000, 16 }, { "dmac1", 0x39900000, 16 },
    { "i2s", 0x3CA00000, 16 }, { "clk", 0x3C500000, 32 },
};
/* Readable sm1 registers. +0x15C pops the mailbox and +0xC48 acks: omitted. */
static const uint16_t sm1_regs[] = {
    0x100, 0x114, 0x118, 0x160, 0x400, 0x804, 0x858, 0x860, 0x984, 0x988,
    0x98C, 0x99C, 0xA8C, 0xA90, 0xA98, 0xB10, 0xC10, 0xC80, 0x1000, 0x1004,
    0x100C,
};

static bool in(uint32_t a, uint32_t end, uint32_t lo, uint32_t size)
{
    return a >= lo && end <= lo + size;
}

enum rs_guard rs_guard(uint32_t a, uint32_t len)
{
    uint32_t size, s = rs_port_scratch(&size), end = a + (len ? len : 1);
    if (end < a)
        return RS_HARD;
    if (in(a, end, s, size) || in(a, end, s | ALIAS, size))
        return RS_ALLOW;
    enum rs_guard g = RS_ALLOW;
    for (unsigned i = 0; i < sizeof(guards) / sizeof(guards[0]); i++)
        if (a < guards[i].hi && end > guards[i].lo && guards[i].cls > g)
            g = guards[i].cls;
    return g;
}

struct ctx
{
    char *text;
    size_t tlen;
    uint8_t *bin;
    size_t blen;
    const uint8_t *data;
    size_t dlen;
    bool data_used, force;
    int argc;
    char *argv[20];
};

static void say(struct ctx *c, const char *fmt, ...)
{
    if (c->tlen >= RS_TEXT_MAX - 80)
        return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(c->text + c->tlen, RS_TEXT_MAX - 80 - c->tlen, fmt, ap);
    va_end(ap);
    if (n > 0)
        c->tlen = strlen(c->text);
}

static bool num(const char *s, uint32_t *v)
{
    uint32_t base = 10, r = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2, base = 16;
    if (!*s)
        return false;
    for (; *s; s++)
    {
        uint32_t d = *s >= '0' && *s <= '9' ? (uint32_t)(*s - '0') :
                     (*s | 32) >= 'a' && (*s | 32) <= 'f' ? (uint32_t)((*s | 32) - 'a' + 10) : 99;
        if (d >= base || r > (0xFFFFFFFFu - d) / base)
            return false;
        r = r * base + d;
    }
    *v = r;
    return true;
}

/* Parse argv[first..] as numbers; missing optional ones keep their value */
static const char *nums(struct ctx *c, int first, int need, uint32_t *v, int max)
{
    if (c->argc - first < need || c->argc - first > max)
        return "usage";
    for (int i = first; i < c->argc; i++)
        if (!num(c->argv[i], &v[i - first]))
            return "bad number";
    return NULL;
}

static const char *width(uint32_t w, uint32_t addr, uint32_t len)
{
    if (w != 1 && w != 2 && w != 4)
        return "width must be 1, 2 or 4";
    return (addr | len) & (w - 1) ? "unaligned" : NULL;
}

static const char *check(struct ctx *c, uint32_t addr, uint32_t len)
{
    enum rs_guard g = rs_guard(addr, len);
    if (g == RS_HARD)
        return "guard: refused (flash/bootloader path)";
    if (g == RS_SOFT && !c->force)
        return "guard: protected range, add --force";
    return NULL;
}

static const uint8_t *payload(struct ctx *c, const char **err)
{
    if (c->data_used || !c->dlen)
        *err = "payload missing";
    c->data_used = true;
    return c->data;
}

static uint32_t le(const uint8_t *p, uint32_t w)
{
    uint32_t v = 0;
    while (w--)
        v = v << 8 | p[w];
    return v;
}

/* Polls addr until (value & mask) == want. Spins for the first millisecond,
 * then yields between polls so the UI and audio keep running. */
static const char *poll(struct ctx *c, uint32_t addr, uint32_t mask, uint32_t want,
                        uint32_t ms, uint32_t w)
{
    if (ms > 10000)
        return "timeout over 10000 ms";
    uint32_t start = rs_port_usec(), v, us;
    while (true)
    {
        v = rs_port_read(addr, w);
        us = rs_port_usec() - start;
        if ((v & mask) == want)
        {
            say(c, "hit us=%lu v=%08lx", (unsigned long)us, (unsigned long)v);
            return NULL;
        }
        if (us >= ms * 1000)
            break;
        if (us > 1000)
            rs_port_yield();
    }
    say(c, "v=%08lx", (unsigned long)v);
    return "timeout";
}

static const char *cmd_mem(struct ctx *c, const char *op)
{
    uint32_t v[7] = { 0, 0, 0, 0, 0, 0, 0 };
    const char *err;
    if (!strcmp(op, "peek"))
    {
        v[2] = 1;
        if ((err = nums(c, 1, 2, v, 3)) || (err = width(v[0], v[1], 0)))
            return err;
        if (v[2] > 64)
            return "count over 64";
        for (uint32_t i = 0; i < v[2]; i++)
            say(c, "%s%0*lx", i ? " " : "", (int)v[0] * 2,
                (unsigned long)rs_port_read(v[1] + i * v[0], v[0]));
        return NULL;
    }
    if (!strcmp(op, "poke"))
    {
        if ((err = nums(c, 1, 3, v, 3)) || (err = width(v[0], v[1], 0)) ||
            (err = check(c, v[1], v[0])))
            return err;
        rs_port_write(v[1], v[0], v[2]);
        return NULL;
    }
    if (!strcmp(op, "dump"))
    {
        if ((err = nums(c, 1, 2, v, 3)))
            return err;
        uint32_t w = c->argc > 3 ? v[2] : (v[0] | v[1]) & 3 ? 1 : 4;
        if ((err = width(w, v[0], v[1])))
            return err;
        if (v[1] > RS_DATA_MAX - c->blen)
            return "length over limit";
        for (uint32_t i = 0; i < v[1]; i += w)
        {
            uint32_t x = rs_port_read(v[0] + i, w);
            for (uint32_t b = 0; b < w; b++)
                c->bin[c->blen++] = x >> (8 * b);
        }
        say(c, "len=%lu", (unsigned long)v[1]);
        return NULL;
    }
    if (!strcmp(op, "fill"))
    {
        v[3] = 4;
        if ((err = nums(c, 1, 3, v, 4)) || (err = width(v[3], v[0], v[1])) ||
            (err = check(c, v[0], v[1])))
            return err;
        if (v[1] > 0x1000000)
            return "length over 16 MiB";
        for (uint32_t i = 0; i < v[1]; i += v[3])
        {
            rs_port_write(v[0] + i, v[3], v[2]);
            if ((i & 0xFFFF) == 0xFFFC)
                rs_port_yield();
        }
        return NULL;
    }
    if (!strcmp(op, "memcpy"))
    {
        if ((err = nums(c, 1, 3, v, 3)) || (err = check(c, v[0], v[2])))
            return err;
        if (v[2] > 0x1000000)
            return "length over 16 MiB";
        uint32_t w = (v[0] | v[1] | v[2]) & 3 ? 1 : 4;
        for (uint32_t i = 0; i < v[2]; i += w)
        {
            rs_port_write(v[0] + i, w, rs_port_read(v[1] + i, w));
            if ((i & 0xFFFF) == 0xFFFC)
                rs_port_yield();
        }
        return NULL;
    }
    if (!strcmp(op, "load"))
    {
        if ((err = nums(c, 1, 1, v, 2)))
            return err;
        const uint8_t *p = payload(c, &err);
        uint32_t w = c->argc > 2 ? v[1] : (v[0] | c->dlen) & 3 ? 1 : 4;
        if (err || (err = width(w, v[0], c->dlen)) || (err = check(c, v[0], c->dlen)))
            return err;
        for (uint32_t i = 0; i < c->dlen; i += w)
            rs_port_write(v[0] + i, w, le(p + i, w));
        say(c, "len=%lu", (unsigned long)c->dlen);
        return NULL;
    }
    if (!strcmp(op, "wait"))
    {
        v[4] = 4;
        if ((err = nums(c, 1, 4, v, 5)) || (err = width(v[4], v[0], 0)))
            return err;
        return poll(c, v[0], v[1], v[2], v[3], v[4]);
    }
    /* call addr [a0..a3] / perf addr n [a0..a3] */
    bool perf = !strcmp(op, "perf");
    if ((err = nums(c, 1, perf ? 2 : 1, v, perf ? 6 : 5)))
        return err;
    uint32_t size, s = rs_port_scratch(&size), target = v[0] & ~1u;
    if (!c->force && !rs_port_in_firmware(target, 2) && !in(target, target + 2, s, size))
        return "guard: call target outside firmware and scratch, add --force";
    uint32_t n = perf ? v[1] : 1, *args = v + (perf ? 2 : 1), done = 0, r0 = 0;
    if (n < 1 || n > 1000000)
        return "count must be 1..1000000";
    rs_port_cache_sync();
    uint32_t start = rs_port_usec(), us = 0;
    while (done < n && us < 3000000)
    {
        r0 = rs_port_call(v[0], args);
        done++;
        us = rs_port_usec() - start;
    }
    say(c, "r0=%08lx n=%lu us=%lu", (unsigned long)r0, (unsigned long)done,
        (unsigned long)us);
    return NULL;
}

static void regs(struct ctx *c, uint32_t base, uint32_t words)
{
    for (uint32_t i = 0; i < words; i++)
        say(c, "%s%02lx=%08lx", i ? " " : "", (unsigned long)(i * 4),
            (unsigned long)rs_port_read(base + i * 4, 4));
}

static const char *cmd_sm1(struct ctx *c)
{
    const char *op = c->argc > 1 ? c->argv[1] : "", *err;
    uint32_t v[17] = { 0 };
    if (!strcmp(op, "on") || !strcmp(op, "off"))
    {
        bool on = op[1] == 'n';
        rs_port_sm1_power(on);
        if (on)
            rs_port_write(SM1 + 0x1000, 4, 3);
        say(c, "power=%s pwr0=%08lx pwr1=%08lx", op,
            (unsigned long)rs_port_read(0x3C500048, 4),
            (unsigned long)rs_port_read(0x3C50004C, 4));
    }
    if (!strcmp(op, "on") || !strcmp(op, "status"))
    {
        say(c, " sig=%08lx", (unsigned long)rs_port_read(SM1 + 0xC80, 4));
        if (op[0] == 's')
            say(c, " st=%08lx ev=%08lx pc=%08lx div=%08lx raw18=%lu en18=%lu"
                " pwr0=%08lx pwr1=%08lx",
                (unsigned long)rs_port_read(SM1 + 0x114, 4),
                (unsigned long)rs_port_read(SM1 + 0xA98, 4),
                (unsigned long)rs_port_read(SM1 + 0xC10, 4),
                (unsigned long)rs_port_read(SM1 + 0x1000, 4),
                (unsigned long)(rs_port_read(VIC0 + 8, 4) >> 18 & 1),
                (unsigned long)(rs_port_read(VIC0 + 0x10, 4) >> 18 & 1),
                (unsigned long)rs_port_read(0x3C500048, 4),
                (unsigned long)rs_port_read(0x3C50004C, 4));
        return NULL;
    }
    if (!strcmp(op, "off"))
        return NULL;
    if (!strcmp(op, "regs"))
    {
        for (unsigned i = 0; i < sizeof(sm1_regs) / sizeof(sm1_regs[0]); i++)
            say(c, "%s%03x=%08lx", i ? " " : "", sm1_regs[i],
                (unsigned long)rs_port_read(SM1 + sm1_regs[i], 4));
        return NULL;
    }
    if (!strcmp(op, "ack"))
    {
        rs_port_write(SM1 + 0xC48, 4, 4);
        return NULL;
    }
    if (!strcmp(op, "wait"))
    {
        if ((err = nums(c, 2, 1, v, 1)))
            return err;
        if ((err = poll(c, VIC0 + 8, 1u << 18, 1u << 18, v[0], 4)))
            return err;
        say(c, " ev=%08lx", (unsigned long)rs_port_read(SM1 + 0xA98, 4));
        return NULL;
    }
    if (!strcmp(op, "feed"))
    {
        /* sm1 feed <desc> <data> <len>: both in the scratch buffer. Writes
         * {0, len << 16 | 0x1F << 7, data | 1 << 31, 0} through the uncached
         * view and submits it, tagged 1, at +0x100. */
        uint32_t size, s = rs_port_scratch(&size);
        if ((err = nums(c, 2, 3, v, 3)))
            return err;
        uint32_t desc = v[0] & ~ALIAS, data = v[1] & ~ALIAS;
        if ((desc & 15) || !in(desc, desc + 16, s, size) || !v[2] || v[2] > 0xFFFF ||
            !in(data, data + v[2], s, size))
            return "descriptor and data must lie in scratch, len 1..65535";
        rs_port_cache_sync();
        rs_port_write(desc | ALIAS, 4, 0);
        rs_port_write((desc | ALIAS) + 4, 4, v[2] << 16 | 0x1F << 7);
        rs_port_write((desc | ALIAS) + 8, 4, data | 0x80000000u);
        rs_port_write((desc | ALIAS) + 12, 4, 0);
        rs_port_write(SM1 + 0x100, 4, desc | 1);
        return NULL;
    }
    if (!strcmp(op, "load"))
    {
        /* sm1 load p|x|y <index>: rows of 16 bytes, 32-bit X words or
         * 16-bit Y halfwords, stored with the widths the engine needs */
        const char *mem = c->argc > 2 ? c->argv[2] : "";
        if ((err = nums(c, 3, 1, v, 1)))
            return err;
        const uint8_t *p = payload(c, &err);
        if (err)
            return err;
        uint32_t unit = mem[0] == 'p' ? 16 : mem[0] == 'x' ? 4 : mem[0] == 'y' ? 2 : 0;
        uint32_t limit = unit == 16 ? 0x1000 : unit == 4 ? 0x1C00 : 0x4000;
        uint32_t count = unit ? c->dlen / unit : 0;
        if (!unit || mem[1] || c->dlen % unit)
            return "usage: sm1 load p|x|y <index> with whole units";
        if (v[0] > limit || count > limit - v[0])
            return "outside engine memory";
        for (uint32_t i = 0; i < count; i++)
        {
            uint32_t at = v[0] + i;
            if (unit == 16)
                for (uint32_t lane = 0; lane < 4; lane++)
                    rs_port_write(SM1_P + lane * 0x4000 + at * 4, 4, le(p + i * 16 + lane * 4, 4));
            else if (unit == 4)
                rs_port_write(SM1_X + at * 4, 4, le(p + i * 4, 4));
            else
                rs_port_write(SM1_Y + at * 2, 2, le(p + i * 2, 2));
        }
        say(c, "units=%lu", (unsigned long)count);
        return NULL;
    }
    if (!strcmp(op, "param"))
    {
        /* Halfwords at 0x2203FF00 + 2*offset. The page is shared with the
         * bootloader's reboot-reason data. */
        if ((err = nums(c, 2, 2, v, 17)))
            return err;
        uint32_t n = c->argc - 3;
        if (v[0] + n > 0x80)
            return "outside parameter block";
        for (uint32_t i = 0; i < n; i++)
            rs_port_write(SM1_PARAM + (v[0] + i) * 2, 2, v[i + 1]);
        return NULL;
    }
    uint32_t ring = rs_port_read(SM1_Y + 2, 2), units = rs_port_read(SM1_Y + 4, 2);
    if (!strcmp(op, "slots"))
    {
        say(c, "out=%08lx ring=%lu units=%lu", (unsigned long)rs_port_read(SM1_OUT, 4),
            (unsigned long)ring, (unsigned long)units);
        for (uint32_t i = 0; i < ring && i < 16; i++)
            say(c, " %lu:%lu/%lu", (unsigned long)i,
                (unsigned long)rs_port_read(SM1_Y + i * 4 + 0xA, 2),
                (unsigned long)rs_port_read(SM1_Y + i * 4 + 0xC, 2));
        return NULL;
    }
    if (!strcmp(op, "release") || !strcmp(op, "pcm"))
    {
        if ((err = nums(c, 2, 1, v, 1)))
            return err;
        if (v[0] >= ring || v[0] >= 64)
            return "slot outside ring";
        uint32_t entry = SM1_Y + v[0] * 4;
        if (op[0] == 'r')
        {
            rs_port_write(entry + 0xA, 2, 0);
            return NULL;
        }
        uint32_t base = SM1_Y + 0x100 + v[0] * units * 2;
        uint32_t len = rs_port_read(entry + 0xC, 2) * 2;
        if (len > RS_DATA_MAX - c->blen || base + len > SM1_PARAM)
            return "slot length outside Y memory";
        for (uint32_t i = 0; i < len; i += 2)
        {
            uint32_t x = rs_port_read(base + i, 2);
            c->bin[c->blen++] = x;
            c->bin[c->blen++] = x >> 8;
        }
        say(c, "len=%lu", (unsigned long)len);
        return NULL;
    }
    return "usage: sm1 on|off|status|regs|ack|wait|feed|load|param|slots|release|pcm";
}

static bool good_name(const char *s)
{
    size_t n = strlen(s);
    if (!n || n > RS_NAME_MAX || s[0] == '.')
        return false;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
              (*s >= '0' && *s <= '9') || *s == '.' || *s == '_' || *s == '-'))
            return false;
    return true;
}

static const char *cmd_file(struct ctx *c)
{
    const char *op = c->argc > 1 ? c->argv[1] : "", *name = c->argc > 2 ? c->argv[2] : "";
    const char *err;
    uint32_t v[2] = { 0, RS_DATA_MAX };
    int n;
    if (!strcmp(op, "ls"))
    {
        n = rs_port_file_ls(c->text + c->tlen, RS_TEXT_MAX - 80 - c->tlen);
        if (n < 0)
            return "no research directory";
        c->tlen += n;
        return NULL;
    }
    if (!good_name(name))
        return "name must be a plain file name under /.rockbox/research";
    if (!strcmp(op, "rm"))
        return rs_port_file_rm(name) < 0 ? "remove failed" : NULL;
    if (!strcmp(op, "get"))
    {
        if ((err = nums(c, 3, 0, v, 2)))
            return err;
        if (v[1] > RS_DATA_MAX - c->blen)
            return "length over limit";
        n = rs_port_file_get(name, v[0], c->bin + c->blen, v[1]);
        if (n < 0)
            return "read failed";
        c->blen += n;
        say(c, "len=%d", n);
        return NULL;
    }
    if (!strcmp(op, "put"))
    {
        if ((err = nums(c, 3, 0, v, 1)))
            return err;
        const uint8_t *p = c->data_used ? NULL : c->data;
        c->data_used = true;
        n = rs_port_file_put(name, v[0], p, p ? c->dlen : 0);
        if (n < 0)
            return "write failed";
        say(c, "len=%d", n);
        return NULL;
    }
    return "usage: file ls|get|put|rm";
}

static const char *run(struct ctx *c)
{
    const char *op = c->argv[0], *err;
    uint32_t v[1] = { 0 };
    if (!strcmp(op, "peek") || !strcmp(op, "poke") || !strcmp(op, "dump") ||
        !strcmp(op, "fill") || !strcmp(op, "memcpy") || !strcmp(op, "load") ||
        !strcmp(op, "wait") || !strcmp(op, "call") || !strcmp(op, "perf"))
        return cmd_mem(c, op);
    if (!strcmp(op, "sm1"))
        return cmd_sm1(c);
    if (!strcmp(op, "file"))
        return cmd_file(c);
    if (!strcmp(op, "regs") || !strcmp(op, "irq"))
    {
        const char *name = c->argc > 1 ? c->argv[1] : "";
        if (op[0] == 'i')
        {
            regs(c, blocks[0].base, 5);
            say(c, " |");
            regs(c, blocks[1].base, 5);
            return NULL;
        }
        if (!strcmp(name, "sm1"))
            return c->argv[1] = "regs", cmd_sm1(c);
        for (unsigned i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i++)
            if (!strcmp(name, blocks[i].name))
                return regs(c, blocks[i].base, blocks[i].words), NULL;
        return "usage: regs sm1|vic0|vic1|dmac0|dmac1|i2s|clk";
    }
    if (!strcmp(op, "ver") || !strcmp(op, "clock") || !strcmp(op, "buf"))
    {
        int n = rs_port_info(c->text + c->tlen, RS_TEXT_MAX - 80 - c->tlen);
        c->tlen += n > 0 ? (size_t)n : 0;
        return NULL;
    }
    if (!strcmp(op, "boost") || !strcmp(op, "mark"))
    {
        if ((err = nums(c, 1, 1, v, 1)))
            return err;
        if (op[0] == 'b')
            rs_port_boost(v[0] != 0);
        else
            rs_port_mark(v[0]);
        return NULL;
    }
    if (!strcmp(op, "watchdog"))
    {
        const char *arg = c->argc == 2 ? c->argv[1] : "";
        if (!strcmp(arg, "off"))
            return rs_port_file_put("watchdog.off", 0, NULL, 0) < 0 ? "write failed" : NULL;
        if (!strcmp(arg, "on"))
            return rs_port_file_rm("watchdog.off"), NULL;
        return "usage: watchdog on|off (applies after reboot)";
    }
    if (!strcmp(op, "reboot"))
    {
        rs_reboot_requested = true;
        return NULL;
    }
    if (!strcmp(op, "ping"))
        return say(c, "pong"), NULL;
    return "unknown command";
}

size_t rs_exec(const uint8_t *req, size_t len, uint8_t *out, bool *binary)
{
    static const char prefix[] = "RS\t1\t";
    char id[17], line[256], text[RS_TEXT_MAX];
    size_t i = sizeof(prefix) - 1, n = 0;
    if (len < i || memcmp(req, prefix, i))
        return 0;
    while (i < len && req[i] != '\t' && n < 16)
    {
        char ch = req[i++];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')))
            return 0;
        id[n++] = ch;
    }
    if (!n || i >= len || req[i++] != '\t')
        return 0;
    id[n] = 0;

    struct ctx c = { .text = text, .bin = out + RS_TEXT_MAX + 32 };
    text[0] = 0;
    const char *err = NULL;
    for (n = 0; i < len && req[i] != '\n'; i++)
    {
        if (n == sizeof(line) - 1 || req[i] < 32 || req[i] > 126)
        {
            err = "line too long or not printable";
            break;
        }
        line[n++] = req[i];
    }
    line[n] = 0;
    if (i < len)
    {
        c.data = req + i + 1;
        c.dlen = len - i - 1;
        if (c.dlen > RS_DATA_MAX)
            err = "payload over limit";
    }

    /* Commands separated by ';' run in order and stop at the first error */
    for (char *cmd = line, *next; !err && cmd; cmd = next)
    {
        if ((next = strchr(cmd, ';')))
            *next++ = 0;
        c.argc = 0;
        c.force = false;
        for (char *tok = cmd; *tok; )
        {
            if (*tok == ' ')
            {
                tok++;
                continue;
            }
            char *end = tok;
            while (*end && *end != ' ')
                end++;
            char *after = *end ? end + 1 : end;
            *end = 0;
            if (!strcmp(tok, "--force"))
                c.force = true;
            else if (c.argc < (int)(sizeof(c.argv) / sizeof(c.argv[0])))
                c.argv[c.argc++] = tok;
            else
                err = "too many arguments";
            tok = after;
        }
        if (err || !c.argc)
            continue;
        size_t k = 0;
        for (int a = 0; a < c.argc && k < sizeof(rs_last_command) - 1; a++)
        {
            if (a)
                rs_last_command[k++] = ' ';
            for (const char *s = c.argv[a]; *s && k < sizeof(rs_last_command) - 1; s++)
                rs_last_command[k++] = *s;
        }
        rs_last_command[k] = 0;
        if (cmd != line)
            say(&c, "; ");
        err = run(&c);
    }
    if (err)
        say(&c, "%s%s", c.tlen ? " " : "", err);

    int head = snprintf((char *)out, RS_TEXT_MAX + 32, "RS\t1\t%s\t%s\t%s", id,
                        err ? "ERR" : "OK", text);
    size_t total = (size_t)head;
    *binary = !err && c.blen;
    if (*binary)
    {
        out[total++] = '\n';
        memmove(out + total, c.bin, c.blen);
        total += c.blen;
    }
    return total;
}
