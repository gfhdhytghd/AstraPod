/* Exercise the staged XWorld parsers. GPL-2.0-or-later. */
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include "bank.h"
#include "resource.h"
#include "reference_unpack.h"

static jmp_buf malformed;
static bool expect_error;

void error(const char *format, ...)
{
    if (expect_error)
        longjmp(malformed, 1);
    va_list args;
    va_start(args, format);
    fputs("Unexpected XWorld error: ", stderr);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
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

#define MIN(a, b) ((a) < (b) ? (a) : (b))

static uint32_t next_random(uint32_t *rng)
{
    *rng = *rng * UINT32_C(1664525) + UINT32_C(1013904223);
    return *rng >> 8;
}

static uint8_t stream_bits[1 << 16];
static size_t stream_count;

static void emit(unsigned value, unsigned width)
{
    while (width--)
        stream_bits[stream_count++] = value >> width & 1;
}

/* Build a token stream in the upstream format: literal runs and copies
 * from already produced output. */
static void encode_random(size_t unpacked, uint32_t *rng)
{
    stream_count = 0;
    size_t produced = 0;
    while (produced < unpacked) {
        size_t left = unpacked - produced;
        unsigned kind = produced ? next_random(rng) % 6 : 0;
        unsigned count, width;
        if (kind <= 1) {
            if (kind == 1 && left >= 9) {
                count = 9 + next_random(rng) % MIN(left - 8, 256);
                emit(1, 1); emit(3, 2); emit(count - 9, 8);
            } else {
                count = 1 + next_random(rng) % MIN(left, 8);
                emit(0, 2); emit(count - 1, 3);
            }
            for (unsigned n = 0; n < count; n++)
                emit(next_random(rng) & 0xff, 8);
            produced += count;
            continue;
        }
        if (kind == 2 && left >= 2) {
            count = 2; width = 8;
            emit(1, 2);
        } else if (kind == 3 && left >= 3) {
            count = 3; width = 9;
            emit(1, 1); emit(0, 2);
        } else if (kind == 4 && left >= 4) {
            count = 4; width = 10;
            emit(1, 1); emit(1, 2);
        } else {
            count = 1 + next_random(rng) % MIN(left, 256); width = 12;
            emit(1, 1); emit(2, 2); emit(count - 1, 8);
        }
        unsigned max_distance = MIN(produced, (1u << width) - 1);
        emit(1 + next_random(rng) % max_distance, width);
        produced += count;
    }
}

/* Lay the bit stream out as upstream reads it: the first bits below the
 * sentinel of the last stream word, then whole words toward the start. */
static size_t pack_stream(uint8_t *bytes, size_t unpacked, uint32_t crc)
{
    size_t first = stream_count % 32;
    size_t words = stream_count / 32;
    uint32_t chk = UINT32_C(1) << first;
    for (size_t i = 0; i < first; i++)
        chk |= (uint32_t)stream_bits[i] << i;
    size_t packed = 12 + 4 * words;
    uint32_t fold = chk;
    for (size_t w = 0; w < words; w++) {
        uint32_t word = 0;
        for (size_t i = 0; i < 32; i++)
            word |= (uint32_t)stream_bits[first + 32 * w + i] << i;
        put_word(bytes + packed - 16 - 4 * w, word);
        fold ^= word;
    }
    put_word(bytes + packed - 12, chk);
    put_word(bytes + packed - 8, fold ^ crc);
    put_word(bytes + packed - 4, unpacked);
    return packed;
}

/* Streams that upstream decodes completely must decode the same way. */
static void differential_cases(void)
{
    enum { MAX_OUT = 8192 };
    static uint8_t ref[MAX_OUT], cur[MAX_OUT], input[MAX_OUT];
    uint32_t rng = 1234567;
    unsigned compared = 0, crc_ok = 0, rejected = 0;
    for (unsigned i = 0; i < 20000; i++) {
        size_t unpacked = 16 + next_random(&rng) % (MAX_OUT - 16);
        encode_random(unpacked, &rng);
        if (stream_count / 8 + 16 > MAX_OUT)
            continue;
        memset(input, 0, sizeof(input));
        /* Alternate streams carry a wrong CRC word. */
        size_t packed = pack_stream(input, unpacked, (i & 1) ? 0 : 0x10);
        if (packed >= unpacked)
            continue;

        uint32_t residue;
        memcpy(ref, input, packed);
        int verdict = reference_unpack(ref, packed, unpacked, &residue);
        memcpy(cur, input, packed);
        bool ok = bank_unpack(cur, packed, unpacked);
        if (verdict == REF_FAULT) {
            /* Output would overrun unread input; both must refuse it. */
            assert(!ok);
            rejected++;
            continue;
        }
        assert(verdict == ((i & 1) ? REF_OK : REF_BAD_CRC));
        assert(ok == (verdict == REF_OK));
        assert(memcmp(cur, ref, unpacked) == 0);
        compared++;
        crc_ok += ok;
    }
    assert(compared > 1000 && crc_ok > 500);
    printf("bank: %u encoded streams match upstream byte for byte "
           "(%u with a valid CRC, %u refused by both)\n",
           compared, crc_ok, rejected);
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
    bytes[1] = 6; /* VIDEO2 polygons */
    memlist_case(dir, bytes, 21, true);
    bytes[1] = 7;
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
    setvbuf(stdout, NULL, _IONBF, 0);
    assert(argc == 2);
    bank_cases();
    differential_cases();
    memlist_cases(argv[1]);
    operand_cases();
}
