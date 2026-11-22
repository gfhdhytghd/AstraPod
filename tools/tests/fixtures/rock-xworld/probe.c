/* Exercise the staged XWorld parsers. GPL-2.0-or-later. */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include "bank.h"
#include "resource.h"

static jmp_buf malformed;
static bool expect_error;

void error(const char *format, ...)
{
    if (expect_error)
        longjmp(malformed, 1);
    fprintf(stderr, "Unexpected XWorld error: %s\n", format);
    abort();
}

void warning(const char *format, ...)
{
    (void)format;
}

void string_lower(char *p)
{
    (void)p;
}

void string_upper(char *p)
{
    (void)p;
}

static off_t file_size(int fd)
{
    struct stat st;
    return fstat(fd, &st) == 0 ? st.st_size : -1;
}

static const struct plugin_api api = {
    .snprintf = snprintf,
    .strlen = strlen,
    .memset = memset,
    .open = open,
    .close = close,
    .read = read,
    .write = write,
    .lseek = lseek,
    .ffilesize = file_size,
};
const struct plugin_api *rb = &api;

static void put_word(uint8_t *p, uint32_t word)
{
    for (unsigned i = 0; i < 4; i++)
        p[i] = word >> (24 - 8 * i);
}

static uint32_t bit_word(const char *bits, size_t n)
{
    uint32_t word = 0;
    for (size_t i = 0; i < n; i++)
        word |= (uint32_t)(bits[i] == '1') << i;
    return word;
}

static void pack(uint8_t *bytes, const char *bits)
{
    /* The first four stream bits sit below the sentinel in the last word. */
    uint32_t last = bit_word(bits, 4) | 16;
    uint32_t first = bit_word(bits + 4, 32);
    put_word(bytes, first);
    put_word(bytes + 4, last);
    put_word(bytes + 8, first ^ last);
    put_word(bytes + 12, 32);
}

static void bank_cases(void)
{
    const char good[] = "00000" "01011010" "110" "00011110" "000000000001";
    uint8_t bytes[32];
    pack(bytes, good);
    assert(bank_unpack(bytes, 16, sizeof(bytes)));
    for (size_t i = 0; i < sizeof(bytes); i++)
        assert(bytes[i] == 0x5a);

    for (size_t i = 0; i < 16; i++) {
        pack(bytes, good);
        assert(!bank_unpack(bytes, i, sizeof(bytes)));
    }
    pack(bytes, good);
    bytes[15]++;
    assert(!bank_unpack(bytes, 16, sizeof(bytes)));
    pack(bytes, good);
    bytes[8] ^= 1;
    assert(!bank_unpack(bytes, 16, sizeof(bytes)));

    for (size_t i = 0; i < 3; i++) {
        char bad[sizeof(good)];
        memcpy(bad, good, sizeof(bad));
        if (i == 0)
            bad[35] = '0'; /* distance zero */
        else if (i == 1)
            bad[34] = '1'; /* distance larger than produced output */
        else
            bad[16] = '1'; /* run larger than declared output */
        pack(bytes, bad);
        assert(!bank_unpack(bytes, 16, sizeof(bytes)));
    }

    /* Random streams have a valid footer, so they reach the bit reader. */
    uint32_t rng = 71;
    for (size_t i = 0; i < 20000; i++) {
        for (size_t j = 0; j < 12; j++) {
            rng = rng * UINT32_C(1664525) + UINT32_C(1013904223);
            bytes[j] = rng >> 24;
        }
        put_word(bytes + 12, sizeof(bytes));
        bank_unpack(bytes, 16, sizeof(bytes));
    }
    puts("bank: valid literal/back-reference, truncation, footer, CRC, "
         "distance, output bounds and 20000 random streams");
}

static void memlist_case(const char *dir, const uint8_t *bytes, size_t length,
                         bool valid)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/memlist.bin", dir);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0 && write(fd, bytes, length) == (ssize_t)length);
    assert(close(fd) == 0);

    struct Resource res = { ._dataDir = dir };
    expect_error = !valid;
    if (setjmp(malformed) == 0) {
        res_readEntries(&res);
        assert(valid && res._numMemList == 1);
    }
    expect_error = false;
}

static void memlist_cases(const char *dir)
{
    uint8_t bytes[151 * 20 + 1] = {0};
    bytes[20] = 0xff;
    memlist_case(dir, bytes, 21, true);
    memlist_case(dir, bytes, 40, true);
    memlist_case(dir, bytes, 39, false);
    memlist_case(dir, bytes, 20, false);
    memlist_case(dir, bytes, 19, false);
    bytes[1] = RT_POLY_GAMEPLAY;
    memlist_case(dir, bytes, 21, true);
    bytes[1] = RT_POLY_GAMEPLAY + 1;
    memlist_case(dir, bytes, 21, false);
    bytes[1] = RT_POLY_ANIM;
    bytes[7] = 1; /* The DOS list may reserve an empty bitmap slot. */
    memlist_case(dir, bytes, 21, true);
    bytes[19] = 1;
    bytes[15] = 1;
    memlist_case(dir, bytes, 21, false);
    bytes[1] = 0;
    bytes[15] = 3;
    bytes[19] = 4; /* compressed record cannot fit a footer */
    memlist_case(dir, bytes, 21, false);
    bytes[15] = 8;
    memlist_case(dir, bytes, 21, false);
    memset(bytes, 0, sizeof(bytes));
    bytes[151 * 20] = 0xff;
    memlist_case(dir, bytes, sizeof(bytes), false);
    puts("memlist: both sentinel layouts, short/missing sentinel, short record, "
         "invalid type/sizes and record capacity");
}

static void operand_cases(void)
{
    uint8_t bytes[] = {0x8f, 0x17};
    struct Ptr good = { .pc = bytes, .end = bytes + sizeof(bytes) };
    assert(scriptPtr_fetchWord(&good) == 0x8f17);

    expect_error = true;
    if (setjmp(malformed) == 0) {
        struct Ptr short_word = { .pc = bytes + 1, .end = bytes + 2 };
        scriptPtr_fetchWord(&short_word);
        abort();
    }
    expect_error = false;
    puts("VM operand: valid big-endian word and truncated word");
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    bank_cases();
    memlist_cases(argv[1]);
    operand_cases();
}
