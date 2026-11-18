/* SPDX-License-Identifier: GPL-2.0-or-later
 * FU88: research shell parser, guard list and commands against the RAM-only
 * port. Build: cc -Ifirmware/export firmware/research_shell.c
 *              tools/tests/research_port_host.c tools/tests/research_shell_test.c
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "research_shell.h"
#include "research_port_host.c"

static uint8_t out[RS_FRAME_MAX];
static size_t out_len;
static bool out_bin;

static const char *exec_data(const char *line, const void *data, size_t n)
{
    static uint8_t req[RS_FRAME_MAX];
    size_t len = (size_t)snprintf((char *)req, sizeof req, "RS\t1\tt1\t%s", line);
    if (data)
    {
        req[len++] = '\n';
        memcpy(req + len, data, n);
        len += n;
    }
    out_len = rs_exec(req, len, out, &out_bin);
    assert(out_len < RS_FRAME_MAX);
    out[out_len] = 0;
    return (const char *)out;
}
static const char *exec(const char *line) { return exec_data(line, NULL, 0); }

#define EXPECT(line, reply) do { const char *r_ = exec(line); \
    if (strcmp(r_, reply)) { printf("FAIL %s\n got  %s\n want %s\n", line, r_, reply); return 1; } } while (0)
#define PREFIX(line, reply) do { const char *r_ = exec(line); \
    if (strncmp(r_, reply, strlen(reply))) { printf("FAIL %s\n got  %s\n want %s...\n", line, r_, reply); return 1; } } while (0)

int main(void)
{
    fake_reset();

    /* Framing: malformed requests get no reply at all */
    bool bin;
    assert(rs_exec((const uint8_t *)"RS\t2\tx\tping", 11, out, &bin) == 0);
    assert(rs_exec((const uint8_t *)"RS\t1\t\tping", 10, out, &bin) == 0);
    assert(rs_exec((const uint8_t *)"RS\t1\tbad!\tping", 14, out, &bin) == 0);
    assert(rs_exec((const uint8_t *)"RS\t1\tabcdefghijklmnopq\tping", 27, out, &bin) == 0);
    EXPECT("ping", "RS\t1\tt1\tOK\tpong");
    EXPECT("nonsense", "RS\t1\tt1\tERR\tunknown command");
    EXPECT("peek 4 0x38500zz0", "RS\t1\tt1\tERR\tbad number");
    EXPECT("peek 3 0x38500000", "RS\t1\tt1\tERR\twidth must be 1, 2 or 4");
    EXPECT("peek 4 0x38500002", "RS\t1\tt1\tERR\tunaligned");
    EXPECT("peek 4 0x100000000", "RS\t1\tt1\tERR\tbad number");
    EXPECT("peek 4", "RS\t1\tt1\tERR\tusage");
    puts("PASS framing and parser errors");

    /* Guard list */
    assert(rs_guard(0x3C300000, 4) == RS_HARD);           /* SPI0 / NOR */
    assert(rs_guard(0x3C2FFFFC, 8) == RS_HARD);           /* straddles into it */
    assert(rs_guard(0x3CE00010, 4) == RS_HARD);
    assert(rs_guard(0x3D200000, 1) == RS_HARD);
    assert(rs_guard(0x38300000, 4) == RS_SOFT);           /* LCD */
    assert(rs_guard(0x38700000, 4) == RS_SOFT);           /* ATA */
    assert(rs_guard(0x38A00000, 4) == RS_SOFT);           /* NAND/ATA glue */
    assert(rs_guard(0x3C500048, 4) == RS_SOFT);           /* clock gates */
    assert(rs_guard(0x08001000, 4) == RS_SOFT);           /* firmware DRAM */
    assert(rs_guard(0x48001000, 4) == RS_SOFT);
    assert(rs_guard(0x22001000, 4) == RS_SOFT);           /* IRAM0 */
    assert(rs_guard(0xFFFFFFF0, 0x20) == RS_HARD);        /* wraps */
    assert(rs_guard(FAKE_SCRATCH, 0x10000) == RS_ALLOW);
    assert(rs_guard(FAKE_SCRATCH | 0x40000000, 16) == RS_ALLOW);
    assert(rs_guard(FAKE_SCRATCH + 0xFFF0, 0x20) == RS_SOFT);  /* runs past */
    assert(rs_guard(0x22020000, 0x20000) == RS_ALLOW);    /* sm1 P/X/Y */
    assert(rs_guard(0x38500000, 0x2000) == RS_ALLOW);     /* sm1 registers */
    assert(rs_guard(0x3CA00000, 4) == RS_ALLOW);          /* I2S */
    EXPECT("poke 4 0x3C300000 1", "RS\t1\tt1\tERR\tguard: refused (flash/bootloader path)");
    EXPECT("poke 4 0x3C300000 1 --force", "RS\t1\tt1\tERR\tguard: refused (flash/bootloader path)");
    EXPECT("fill 0x3CE00000 16 0 --force", "RS\t1\tt1\tERR\tguard: refused (flash/bootloader path)");
    EXPECT("memcpy 0x3D200000 0x09000000 4 --force", "RS\t1\tt1\tERR\tguard: refused (flash/bootloader path)");
    assert(spi_writes == 0);
    EXPECT("poke 4 0x38300000 1", "RS\t1\tt1\tERR\tguard: protected range, add --force");
    EXPECT("load 0x08000000", "RS\t1\tt1\tERR\tpayload missing");
    EXPECT("call 0x22001000", "RS\t1\tt1\tERR\tguard: call target outside firmware and scratch, add --force");
    EXPECT("poke 4 0x3C500048 0xffffffff --force", "RS\t1\tt1\tOK\t");
    puts("PASS guard list: NOR/SPI hard, LCD/ATA/NAND/clock/firmware soft, scratch and sm1 open");

    /* Memory commands */
    EXPECT("poke 4 0x09000000 0x11223344; poke 2 0x09000004 0xbeef; poke 1 0x09000006 7",
           "RS\t1\tt1\tOK\t; ; ");
    EXPECT("peek 4 0x09000000 2", "RS\t1\tt1\tOK\t11223344 0007beef");
    EXPECT("peek 1 0x09000000 4", "RS\t1\tt1\tOK\t44 33 22 11");
    EXPECT("peek 4 0x38500C80", "RS\t1\tt1\tOK\t004e7309");
    EXPECT("peek 4 0x09000000 65", "RS\t1\tt1\tERR\tcount over 64");
    exec("dump 0x09000000 8");
    assert(out_bin && !memcmp(out, "RS\t1\tt1\tOK\tlen=8\n\x44\x33\x22\x11\xef\xbe\x07\x00", 25));
    assert(out_len == 25);
    EXPECT("dump 0x09000000 12289", "RS\t1\tt1\tERR\tlength over limit");
    uint8_t blob[300];
    for (unsigned i = 0; i < sizeof blob; i++)
        blob[i] = (uint8_t)(i * 7 + 1);
    PREFIX("x", "RS");
    assert(!strcmp(exec_data("load 0x09001000", blob, sizeof blob), "RS\t1\tt1\tOK\tlen=300"));
    exec("dump 0x09001000 300 1");
    assert(out_bin && out_len == strlen("RS\t1\tt1\tOK\tlen=300\n") + 300);
    assert(!memcmp(out + out_len - 300, blob, 300));
    EXPECT("memcpy 0x09002000 0x09001000 300", "RS\t1\tt1\tOK\t");
    exec("dump 0x09002000 300");
    assert(!memcmp(out + out_len - 300, blob, 300));
    EXPECT("fill 0x09003000 16 0xa5a5a5a5", "RS\t1\tt1\tOK\t");
    EXPECT("peek 4 0x0900300c", "RS\t1\tt1\tOK\ta5a5a5a5");
    /* Binary payload containing NUL, newline and ';' stays binary */
    const uint8_t odd[8] = { 0, '\n', ';', 0xff, 'R', 'S', '\t', 0 };
    exec_data("load 0x09004000 1", odd, sizeof odd);
    exec("dump 0x09004000 8");
    assert(!memcmp(out + out_len - 8, odd, 8));
    puts("PASS peek/poke/dump/load/memcpy/fill, binary-safe payloads, batches");

    /* wait / call / perf */
    PREFIX("wait 0x09000000 0xffffffff 0x11223344 10", "RS\t1\tt1\tOK\thit us=");
    PREFIX("wait 0x09000000 1 1 5", "RS\t1\tt1\tERR\tv=11223344 timeout");
    EXPECT("wait 0x09000000 1 0 10001", "RS\t1\tt1\tERR\ttimeout over 10000 ms");
    PREFIX("call 0x09000101 1 2 3 4", "RS\t1\tt1\tOK\tr0=0000000b n=1 us=");
    PREFIX("call 0x08000040", "RS\t1\tt1\tOK\tr0=deadbeef n=1");
    PREFIX("perf 0x09000100 10 1", "RS\t1\tt1\tOK\tr0=00000001 n=10 us=");
    EXPECT("perf 0x09000100 0", "RS\t1\tt1\tERR\tcount must be 1..1000000");
    puts("PASS wait timeout, call ARM/Thumb, perf");

    /* Named registers, irq, clock, boost, mark */
    PREFIX("regs vic0", "RS\t1\tt1\tOK\t00=00000000 04=00000000 08=");
    PREFIX("regs sm1", "RS\t1\tt1\tOK\t100=00000000 114=");
    assert(strstr(exec("regs sm1"), "c80=004e7309"));
    assert(!strstr((char *)out, "15c=") && !strstr((char *)out, "c48="));
    PREFIX("regs clk", "RS\t1\tt1\tOK\t00=");
    EXPECT("regs nope", "RS\t1\tt1\tERR\tusage: regs sm1|vic0|vic1|dmac0|dmac1|i2s|clk");
    PREFIX("irq", "RS\t1\tt1\tOK\t00=");
    PREFIX("clock", "RS\t1\tt1\tOK\tver=host");
    EXPECT("boost 1", "RS\t1\tt1\tOK\t");
    assert(boost_state == 1);
    EXPECT("mark 0x1234", "RS\t1\tt1\tOK\t");
    assert(marks[(mark_count - 1) & 7] == 0x1234);
    puts("PASS regs/irq/clock/boost/mark");

    /* sm1: gates snapshot+set and restore, load widths, param, E1 flow */
    fake_reset();
    PREFIX("sm1 on", "RS\t1\tt1\tOK\tpower=on pwr0=ffffffeb pwr1=fffffff7 sig=004e7309");
    assert(get(0x38501000, 4) == 3);
    EXPECT("sm1 off", "RS\t1\tt1\tOK\tpower=off pwr0=ffffffff pwr1=ffffffff");
    uint8_t rows[4 * 16];
    for (unsigned i = 0; i < sizeof rows; i++)
        rows[i] = (uint8_t)i;
    assert(!strcmp(exec_data("sm1 load p 0", rows, sizeof rows), "RS\t1\tt1\tOK\tunits=4"));
    assert(get(0x22020000, 4) == 0x03020100);          /* row 0 lane 0 */
    assert(get(0x22024000, 4) == 0x07060504);          /* row 0 lane 1 */
    assert(get(0x2202C00C, 4) == 0x3f3e3d3c);          /* row 3 lane 3 */
    const uint8_t y[2] = { 0x5a, 0xa5 };
    assert(!strcmp(exec_data("sm1 load y 0x600", y, 2), "RS\t1\tt1\tOK\tunits=1"));
    EXPECT("peek 2 0x22038C00", "RS\t1\tt1\tOK\ta55a");
    const uint8_t x[4] = { 1, 2, 3, 0 };
    assert(!strcmp(exec_data("sm1 load x 0x1bff", x, 4), "RS\t1\tt1\tOK\tunits=1"));
    assert(get(0x22036FFC, 4) == 0x030201);
    assert(!strcmp(exec_data("sm1 load x 0x1c00", x, 4), "RS\t1\tt1\tERR\toutside engine memory"));
    assert(!strcmp(exec_data("sm1 load y 0", x, 3), "RS\t1\tt1\tERR\tusage: sm1 load p|x|y <index> with whole units"));
    EXPECT("sm1 param 0 1 0 0 4", "RS\t1\tt1\tOK\t");
    assert(get(0x2203FF00, 2) == 1 && get(0x2203FF06, 2) == 4);
    EXPECT("sm1 param 0x7f 1 2", "RS\t1\tt1\tERR\toutside parameter block");
    /* arm, wait for IRQ18, ack, read the result */
    PREFIX("poke 4 0x38500984 0; sm1 wait 50", "RS\t1\tt1\tOK\t; hit us=");
    assert(strstr((char *)out, "ev=00000004"));
    PREFIX("sm1 status", "RS\t1\tt1\tOK\t sig=004e7309 st=00000000 ev=00000004");
    assert(strstr((char *)out, "raw18=1 en18=0"));
    EXPECT("sm1 ack; peek 4 0x38500a98", "RS\t1\tt1\tOK\t; 00000000");
    PREFIX("sm1 wait 5", "RS\t1\tt1\tERR\tv=00000000 timeout");
    /* descriptor submit */
    EXPECT("sm1 feed 0x09000010 0x09000100 417", "RS\t1\tt1\tOK\t");
    assert(get(0x09000014, 4) == (417u << 16 | 0x1F << 7));
    assert(get(0x09000018, 4) == 0x89000100);
    assert(get(0x38500100, 4) == 0x09000011);
    EXPECT("sm1 feed 0x08000010 0x09000100 4", "RS\t1\tt1\tERR\tdescriptor and data must lie in scratch, len 1..65535");
    EXPECT("sm1 feed 0x09000014 0x09000100 4", "RS\t1\tt1\tERR\tdescriptor and data must lie in scratch, len 1..65535");
    /* slots: one slot of 0x900 units, filled with 4 bytes of PCM */
    put(0x22038002, 2, 1);
    put(0x22038004, 2, 0x900);
    put(0x2203800A, 2, 1);
    put(0x2203800C, 2, 2);
    put(0x22038100, 4, 0x44332211);
    put(0x2203FF28, 4, 1);
    EXPECT("sm1 slots", "RS\t1\tt1\tOK\tout=00000001 ring=1 units=2304 0:1/2");
    exec("sm1 pcm 0");
    assert(out_bin && !memcmp(out + out_len - 4, "\x11\x22\x33\x44", 4));
    EXPECT("sm1 release 0; sm1 slots", "RS\t1\tt1\tOK\t; out=00000001 ring=1 units=2304 0:0/2");
    EXPECT("sm1 release 1", "RS\t1\tt1\tERR\tslot outside ring");
    puts("PASS sm1 gates snapshot/restore, P/X/Y widths, param, IRQ18 wait+ack, feed, slots");

    /* Files: research directory only */
    EXPECT("file put ../x", "RS\t1\tt1\tERR\tname must be a plain file name under /.rockbox/research");
    EXPECT("file put .hidden", "RS\t1\tt1\tERR\tname must be a plain file name under /.rockbox/research");
    EXPECT("file get a/b", "RS\t1\tt1\tERR\tname must be a plain file name under /.rockbox/research");
    assert(!strcmp(exec_data("file put r.bin", blob, 100), "RS\t1\tt1\tOK\tlen=100"));
    assert(!strcmp(exec_data("file put r.bin 100", blob + 100, 200), "RS\t1\tt1\tOK\tlen=200"));
    exec("file get r.bin");
    assert(out_bin && out_len == strlen("RS\t1\tt1\tOK\tlen=300\n") + 300 &&
           !memcmp(out + out_len - 300, blob, 300));
    exec("file get r.bin 290 100");
    assert(!memcmp(out + out_len - 10, blob + 290, 10));
    PREFIX("file ls", "RS\t1\tt1\tOK\t");
    assert(strstr((char *)out, "r.bin:300"));
    EXPECT("watchdog off", "RS\t1\tt1\tOK\t");
    assert(strstr(exec("file ls"), "watchdog.off:0"));
    EXPECT("watchdog on", "RS\t1\tt1\tOK\t");
    EXPECT("file rm r.bin", "RS\t1\tt1\tOK\t");
    EXPECT("file get r.bin", "RS\t1\tt1\tERR\tread failed");
    puts("PASS file get/put/rm/ls confined to research names, watchdog toggle");

    /* Errors stop a batch; last command is kept for crash records */
    EXPECT("poke 4 0x09000000 1; poke 4 0x38300000 1; poke 4 0x09000000 2",
           "RS\t1\tt1\tERR\t;  guard: protected range, add --force");
    EXPECT("peek 4 0x09000000", "RS\t1\tt1\tOK\t00000001");
    assert(!strcmp(rs_last_command, "peek 4 0x09000000"));
    EXPECT("reboot", "RS\t1\tt1\tOK\t");
    assert(rs_reboot_requested);
    char longline[300];
    memset(longline, 'a', sizeof longline - 1);
    longline[sizeof longline - 1] = 0;
    EXPECT(longline, "RS\t1\tt1\tERR\tline too long or not printable");
    assert(spi_writes == 0);
    puts("PASS batch errors, last command, reboot request, long lines; no SPI writes");
    return 0;
}
