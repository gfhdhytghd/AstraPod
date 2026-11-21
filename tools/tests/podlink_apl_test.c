/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Real SDK state machine, Rust PLNK service and FAST IN serial driver.
 * Only the controller, clock and worker scheduler are substituted. */
#define PODLINK_SERVICE_TEST
#define main transport_fixture_main
#include "podlink_firmware_test.c"
#undef main
#undef podlink_open
#undef podlink_state
#undef podlink_max_message
#undef podlink_send
#undef podlink_close
#undef podlink_poll
#undef podlink_wait
#include "plugins/rock_podlink_port.h"
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#define LIMIT 2072
#define FIXTURE_SIZE 5000u
#define FIXTURE_CRC 0xc1607408u
#define CHANNEL "astrapod.echo"
#define OBJECT "from-ipod.bin"
enum { OPEN = 1, OPEN_OK, CLOSE, TEXT, BINARY, PUSH, PULL,
       ACCEPT, CREDIT, CHUNK, FINISH, DONE, CANCEL };

struct frame {
    unsigned int size;
    unsigned char data[LIMIT];
};

static struct frame frames[16];
static unsigned int frame_count, frame_read, c_calls;
static struct podlink_parser peer;
static unsigned char fixture[FIXTURE_SIZE];
static char source[4096], target[4096], root[4096];
static const unsigned char binary[] = {0, 1, 255, 128, 0, 127};
static const char text[] = "AstraPod echo";

void usb_podlink_disk(void) { assert(false); }
void button_queue_post(long id, intptr_t data) { (void)id; (void)data; }
int apl_lock(void) { return 0; }
void apl_unlock(int level) { (void)level; }
void apl_wake(void) {}
uint32_t apl_now(void) { return current_tick; }
unsigned int apl_hz(void) { return 100; }
unsigned int apl_start_worker(void) { return 1; }
int apl_file_size(int fd, uint32_t *size)
{
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode))
        return ROCK_PODLINK_IO;
    if (st.st_size > ROCK_PODLINK_FILE_MAX)
        return ROCK_PODLINK_LIMIT;
    *size = st.st_size;
    return ROCK_PODLINK_OK;
}

static uint32_t u32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void le32(unsigned char *p, uint32_t i)
{
    for (int j = 0; j < 4; j++, i >>= 8)
        p[j] = i & 255;
}

static void collect(void)
{
    controller();
    for (int i = 0; i < output_size; i++)
    {
        struct podlink_message m;
        int rc = podlink_decode(&peer, output[i], &m);
        assert(rc >= 0);
        if (rc != 1)
            continue;
        if (m.type == PODLINK_PING)
            input_frame(PODLINK_PONG, m.seq, m.data, m.length);
        if (m.type != PODLINK_BINARY || m.length < 4 || memcmp(m.data, "APL1", 4))
            continue;
        assert(m.length <= LIMIT && frame_count < 16);
        struct frame *frame = &frames[frame_count++];
        frame->size = m.length;
        memcpy(frame->data, m.data, m.length);
    }
    clear_output();
}

static void pump(bool disk)
{
    for (int i = 0; i < 8; i++)
    {
        podlink_rust_step();
        collect();
        if (disk)
            rock_podlink_work();
        current_tick++;
    }
}

static void connect_link(void)
{
    insert();
    pump(true);
    static const unsigned char offer[] = {1, 0, 0, 16, 0, 0};
    input_frame(PODLINK_OPEN, 1, offer, sizeof(offer));
    pump(true);
    assert(podlink_host_connected() && fast_in);
}

static void emit(unsigned int op, uint32_t id, uint32_t transfer,
                 const void *payload, unsigned int length)
{
    unsigned char frame[LIMIT] = "APL1";
    frame[4] = op;
    le32(frame + 8, id);
    le32(frame + 12, transfer);
    assert(length <= LIMIT - 16);
    if (length)
        memcpy(frame + 16, payload, length);
    input_frame(PODLINK_BINARY, 20, frame, 16 + length);
}

static struct frame take(unsigned int op)
{
    assert(frame_read < frame_count);
    struct frame f = frames[frame_read++];
    assert(f.size >= 16 && f.data[4] == op && !f.data[5] && !f.data[6] && !f.data[7]);
    if (frame_read == frame_count)
        frame_read = frame_count = 0;
    return f;
}

static void progress(const struct rock_podlink_event *event, void *context)
{
    assert(context == &c_calls);
    assert(event->type == ROCK_PODLINK_PROGRESS || event->type == ROCK_PODLINK_COMPLETE);
    c_calls++;
}

static struct rock_podlink_event event(int type, int result)
{
    struct rock_podlink_event e;
    unsigned int calls = c_calls;
    assert(rock_podlink_poll(&e, 0, progress, &c_calls) == type);
    assert(e.result == (unsigned int)result);
    assert(c_calls == calls + (type == ROCK_PODLINK_PROGRESS ||
                               type == ROCK_PODLINK_COMPLETE));
    return e;
}

static void empty(void)
{
    struct rock_podlink_event e;
    assert(rock_podlink_poll(&e, 0, NULL, NULL) == 0);
    assert(frame_count == 0);
}

static void write_file(const char *path, const void *data, size_t size)
{
    FILE *f = fopen(path, "wb");
    assert(f && fwrite(data, 1, size, f) == size && fclose(f) == 0);
}

static void assert_file(const char *path, const void *data, size_t count)
{
    unsigned char buf[FIXTURE_SIZE + 1];
    FILE *f = fopen(path, "rb");
    assert(f && fread(buf, 1, sizeof(buf), f) == count);
    assert(!memcmp(data, buf, count) && fclose(f) == 0);
}

static void no_temp(void)
{
    DIR *d = opendir(root);
    assert(d);
    struct dirent *entry;
    while ((entry = readdir(d)))
        assert(!strstr(entry->d_name, ".apl-"));
    closedir(d);
}

static uint32_t channel_open(void)
{
    char name[] = CHANNEL;
    assert(rock_podlink_open(name) == 0);
    assert(rock_podlink_open(name) == -ROCK_PODLINK_BUSY);
    memset(name, '?', strlen(name));
    pump(true);
    struct frame f = take(OPEN);
    assert(f.size == 16 + strlen(CHANNEL) &&
           !memcmp(f.data + 16, CHANNEL, strlen(CHANNEL)));
    uint32_t id = u32(f.data + 8);
    assert(id && !u32(f.data + 12));
    emit(OPEN_OK, id, 0, NULL, 0);
    pump(true);
    event(ROCK_PODLINK_OPENED, 0);
    return id;
}

static void verify_chunk(struct frame frame, unsigned int offset, unsigned int len)
{
    assert(frame.size == 24 + len && !memcmp(frame.data + 16, "MSF1", 4));
    assert(u32(frame.data + 20) == offset);
    assert(!memcmp(frame.data + 24, fixture + offset, len));
}

static void push(uint32_t id)
{
    char path[sizeof(source)], object[] = OBJECT;
    strcpy(path, source);
    assert(rock_podlink_push(path, object) == 0);
    memset(path, 0, sizeof(path));
    memset(object, 0, sizeof(object));
    assert(rock_podlink_pull(target, OBJECT) == -ROCK_PODLINK_BUSY);
    pump(true);
    struct frame f = take(PUSH);
    uint32_t t = u32(f.data + 12);
    assert(t && f.size == 20 + strlen(OBJECT) &&
           u32(f.data + 16) == FIXTURE_SIZE &&
           !memcmp(f.data + 20, OBJECT, strlen(OBJECT)));

    unsigned char size[4], count = 2;
    le32(size, FIXTURE_SIZE);
    emit(ACCEPT, id, t, size, sizeof(size));
    pump(true);
    empty();
    emit(CREDIT, id, t, &count, 1);
    pump(true);
    verify_chunk(take(CHUNK), 0, 2048);
    verify_chunk(take(CHUNK), 2048, 2048);
    assert(frame_count == 0);
    assert(c_calls == 0);
    assert(event(ROCK_PODLINK_PROGRESS, 0).bytes == 4096);
    count = 1;
    emit(CREDIT, id, t, &count, 1);
    pump(true);
    verify_chunk(take(CHUNK), 4096, FIXTURE_SIZE - 4096);
    f = take(FINISH);
    assert(f.size == 24 && u32(f.data + 16) == FIXTURE_SIZE &&
           u32(f.data + 20) == FIXTURE_CRC);
    assert(event(ROCK_PODLINK_PROGRESS, 0).bytes == FIXTURE_SIZE);
    empty(); /* FINISH cannot claim peer commit. */
    emit(CREDIT, id, t, &count, 1); /* Replenishment still in flight. */
    emit(DONE, id, t, NULL, 0);
    pump(true);
    assert(event(ROCK_PODLINK_COMPLETE, 0).crc32 == FIXTURE_CRC);
}

static uint32_t pull(uint32_t id, unsigned int length)
{
    assert(rock_podlink_pull(target, OBJECT) == 0);
    pump(true);
    struct frame f = take(PULL);
    uint32_t t = u32(f.data + 12);
    assert(t && f.size == 16 + strlen(OBJECT) &&
           !memcmp(f.data + 16, OBJECT, strlen(OBJECT)));
    unsigned char size[4];
    le32(size, length);
    emit(ACCEPT, id, t, size, sizeof(size));
    pump(true);
    assert(take(CREDIT).data[16] == 2);
    return t;
}

static void send_chunk(uint32_t id, uint32_t t, unsigned int offset, unsigned int len)
{
    unsigned char data[2056] = "MSF1";
    le32(data + 4, offset);
    memcpy(data + 8, fixture + offset, len);
    emit(CHUNK, id, t, data, 8 + len);
}

static void send_finish(uint32_t id, uint32_t t, unsigned int len, uint32_t crc)
{
    unsigned char end[8];
    le32(end, len);
    le32(end + 4, crc);
    emit(FINISH, id, t, end, sizeof(end));
}

static void test_files(uint32_t id)
{
    push(id);
    write_file(target, "keep", 4);
    uint32_t t = pull(id, FIXTURE_SIZE);
    send_chunk(id, t, 0, 2048);
    send_chunk(id, t, 2048, 2048);
    pump(true);
    unsigned int credit = take(CREDIT).data[16];
    if (frame_count)
        credit += take(CREDIT).data[16];
    assert(credit == 2);
    assert_file(target, "keep", 4);
    send_chunk(id, t, 4096, FIXTURE_SIZE - 4096);
    send_finish(id, t, FIXTURE_SIZE, FIXTURE_CRC);
    pump(true);
    assert(take(DONE).size == 16);
    assert(event(ROCK_PODLINK_COMPLETE, 0).crc32 == FIXTURE_CRC);
    assert_file(target, fixture, FIXTURE_SIZE);
    no_temp();

    t = pull(id, 3);
    send_chunk(id, t, 0, 3);
    send_finish(id, t, 3, 0);
    pump(true);
    assert(take(CANCEL).data[16] == ROCK_PODLINK_INVALID);
    event(ROCK_PODLINK_COMPLETE, ROCK_PODLINK_INVALID);
    assert_file(target, fixture, FIXTURE_SIZE);
    no_temp();

    t = pull(id, 0);
    send_finish(id, t, 0, 0);
    pump(true);
    take(DONE);
    event(ROCK_PODLINK_COMPLETE, 0);
    assert_file(target, "", 0);

    t = pull(id, 5000);
    send_chunk(id, t, 0, 100);
    pump(true);
    take(CREDIT);
    event(ROCK_PODLINK_PROGRESS, 0);
    assert(rock_podlink_cancel() == 0);
    pump(true);
    assert(take(CANCEL).data[16] == ROCK_PODLINK_CANCELLED);
    event(ROCK_PODLINK_COMPLETE, ROCK_PODLINK_CANCELLED);
    no_temp();
    assert_file(target, "", 0);

    t = pull(id, FIXTURE_SIZE);
    send_chunk(id, t, 0, 100);
    send_chunk(id, t, 100, 100);
    send_chunk(id, t, 200, 100);
    pump(false); /* Burst cannot borrow future credits from disk writes. */
    pump(true);
    assert(take(CANCEL).data[16] == ROCK_PODLINK_INVALID);
    event(ROCK_PODLINK_COMPLETE, ROCK_PODLINK_INVALID);
    no_temp();

    t = pull(id, 150);
    send_chunk(id, t, 0, 100);
    send_chunk(id, t, 100, 50);
    send_finish(id, t, 150, podlink_crc32(fixture, 150));
    static const unsigned char cancelled[] = {ROCK_PODLINK_CANCELLED, 0};
    emit(CANCEL, id, t, cancelled, sizeof(cancelled));
    pump(false); /* Cancellation wins over earlier unprocessed file data. */
    pump(true);
    event(ROCK_PODLINK_COMPLETE, ROCK_PODLINK_CANCELLED);
    empty();
    no_temp();
    assert_file(target, "", 0);

    assert(rock_podlink_pull(target, OBJECT) == 0);
    pump(true);
    struct frame request = take(PULL);
    unsigned char too_large[4];
    le32(too_large, ROCK_PODLINK_FILE_MAX + 1);
    emit(ACCEPT, id, u32(request.data + 12), too_large, sizeof(too_large));
    pump(true);
    assert(take(CANCEL).data[16] == ROCK_PODLINK_LIMIT);
    event(ROCK_PODLINK_COMPLETE, ROCK_PODLINK_LIMIT);
    no_temp();

    t = pull(id, FIXTURE_SIZE);
    send_chunk(id, t, 0, 300);
    pump(true);
    take(CREDIT);
    rock_podlink_storage(true); /* Same worker call as before USB ACK. */
    no_temp();
    pump(true);
    event(ROCK_PODLINK_CLOSED, ROCK_PODLINK_LINK_LOST);
    event(ROCK_PODLINK_COMPLETE, ROCK_PODLINK_LINK_LOST);
    assert(rock_podlink_open(CHANNEL) == -ROCK_PODLINK_LINK_LOST);
    rock_podlink_storage(false);
}

static void test_messages(uint32_t id)
{
    /* Frozen raw APL1 payload and PLNK v1 CRC from an independent codec.
     * Channel 1, TEXT 'ok', PLNK seq 0x11223344. */
    static const unsigned char golden[] = {
        0x50,0x4c,0x4e,0x4b,0x03,0,0,0,0x44,0x33,0x22,0x11,0x12,0,0,0,
        0x41,0x50,0x4c,0x31,0x04,0,0,0,0x01,0,0,0,0,0,0,0,0x6f,0x6b,
        0x03,0xa6,0xd7,0x60
    };
    assert(id == 1);
    assert(podlink_open()); /* Preserve independent foreground ownership. */
    input_bytes(golden, sizeof(golden));
    pump(true);
    struct rock_podlink_event e = event(ROCK_PODLINK_TEXT, 0);
    assert(e.length == 2 && !memcmp(e.data, "ok", 2));
    struct podlink_message m;
    assert(!podlink_poll(&m)); /* APL1 cannot leak to legacy consumers. */
    podlink_close();
    unsigned char corrupt[sizeof(golden)];
    memcpy(corrupt, golden, sizeof(corrupt));
    corrupt[sizeof(corrupt) - 1] ^= 1;
    input_bytes(corrupt, sizeof(corrupt));
    pump(true);
    empty();

    unsigned char bytes[] = {0, 128, 255};
    assert(rock_podlink_send(true, bytes, sizeof(bytes)) == 0);
    assert(rock_podlink_send(true, bytes, sizeof(bytes)) == -ROCK_PODLINK_BUSY);
    assert(rock_podlink_send(false, bytes, sizeof(bytes)) == -ROCK_PODLINK_INVALID);
    memset(bytes, 0, sizeof(bytes));
    pump(true);
    struct frame f = take(BINARY);
    assert(f.size == 19 && f.data[16] == 0 && f.data[17] == 128 && f.data[18] == 255);
    emit(BINARY, id, 0, f.data + 16, 3);
    pump(true);
    e = event(ROCK_PODLINK_BINARY, 0);
    assert(e.length == 3 && !memcmp(e.data, f.data + 16, 3));
}

static void test_link(uint32_t id)
{
    uint32_t t = pull(id, FIXTURE_SIZE);
    send_chunk(id, t, 0, 2048);
    pump(true);
    take(CREDIT);
    remove_usb();
    pump(true);
    event(ROCK_PODLINK_CLOSED, ROCK_PODLINK_LINK_LOST);
    event(ROCK_PODLINK_COMPLETE, ROCK_PODLINK_LINK_LOST);
    no_temp();
    assert(rock_podlink_open(CHANNEL) == -ROCK_PODLINK_LINK_LOST);

    connect_link();
    uint32_t next_id = channel_open();
    assert(next_id != id);
    emit(TEXT, id, 0, "stale", 5);
    pump(true);
    empty();
    emit(TEXT, next_id, 0, "one", 3);
    emit(TEXT, next_id, 0, "two", 3);
    pump(false);
    pump(true);
    assert(take(CLOSE).data[16] == ROCK_PODLINK_BUSY);
    event(ROCK_PODLINK_CLOSED, ROCK_PODLINK_BUSY);

    next_id = channel_open();
    assert(rock_podlink_pull(target, OBJECT) == 0);
    pump(true);
    assert(u32(take(PULL).data + 8) == next_id);
    current_tick += 10 * apl_hz();
    pump(true);
    assert(take(CANCEL).data[16] == ROCK_PODLINK_TIMEOUT);
    event(ROCK_PODLINK_COMPLETE, ROCK_PODLINK_TIMEOUT);
    rock_podlink_close();
    pump(true);
    take(CLOSE);
    no_temp();
}

static void test_time_and_reset(void)
{
    assert(rock_podlink_open("Uppercase") == -ROCK_PODLINK_INVALID);
    assert(rock_podlink_open(".bad") == -ROCK_PODLINK_INVALID);
    assert(rock_podlink_open(CHANNEL) == 0);
    pump(true);
    take(OPEN);
    struct rock_podlink_event e;
    assert(!rock_podlink_poll(&e, 100, NULL, NULL));
    current_tick += apl_hz() / 10;
    assert(rock_podlink_poll(&e, 100, NULL, NULL) == -ROCK_PODLINK_TIMEOUT);
    current_tick += 3 * apl_hz();
    pump(true);
    assert(take(CLOSE).data[16] == ROCK_PODLINK_TIMEOUT);
    event(ROCK_PODLINK_CLOSED, ROCK_PODLINK_TIMEOUT);

    uint32_t id = channel_open();
    assert(rock_podlink_pull(target, "../bad") == -ROCK_PODLINK_INVALID);
    uint32_t t = pull(id, FIXTURE_SIZE);
    send_chunk(id, t, 0, 100);
    pump(true);
    take(CREDIT);
    /* Another PLNK OPEN on the same configured cable invalidates the lane. */
    static const unsigned char offer[] = {1, 0, 0, 16, 0, 0};
    input_frame(PODLINK_OPEN, 42, offer, sizeof(offer));
    pump(true);
    event(ROCK_PODLINK_CLOSED, ROCK_PODLINK_LINK_LOST);
    event(ROCK_PODLINK_COMPLETE, ROCK_PODLINK_LINK_LOST);
    no_temp();
    assert(channel_open() != id);
    rock_podlink_close();
    pump(true);
    take(CLOSE);
}

/* The stdio fixture drives the same SDK steps as podlink_echo. Stdout is
 * exclusively raw APL1 hex. Diagnostics and final status use stderr. */
static int external_peer(void)
{
    assert(rock_podlink_open(CHANNEL) == 0);
    unsigned int state = 0;
    char line[LIMIT * 2 + 2];
    for (;;)
    {
        pump(true);
        while (frame_read < frame_count)
        {
            const struct frame *f = &frames[frame_read++];
            for (unsigned int i = 0; i < f->size; i++)
                printf("%02x", f->data[i]);
            putchar('\n');
        }
        frame_read = frame_count = 0;
        fflush(stdout);
        struct rock_podlink_event e;
        int rc = rock_podlink_poll(&e, 0, NULL, NULL);
        if (rc < 0 || (rc && (e.result || rc == ROCK_PODLINK_CLOSED)))
        {
            fprintf(stderr, "APL1 event %d result %d at step %u\n",
                    rc, rc < 0 ? -rc : (int)e.result, state);
            return 1;
        }
        if (rc > 0 && rc != ROCK_PODLINK_PROGRESS)
        {
            switch (state++)
            {
                case 0:
                    assert(rc == ROCK_PODLINK_OPENED);
                    assert(rock_podlink_send(false, text, sizeof(text) - 1) == 0);
                    break;
                case 1:
                    assert(rc == ROCK_PODLINK_TEXT && e.length == sizeof(text) - 1);
                    assert(!memcmp(text, e.data, e.length));
                    assert(rock_podlink_send(true, binary, sizeof(binary)) == 0);
                    break;
                case 2:
                    assert(rc == ROCK_PODLINK_BINARY && e.length == sizeof(binary));
                    assert(!memcmp(binary, e.data, e.length));
                    assert(rock_podlink_push(source, OBJECT) == 0);
                    break;
                case 3:
                    assert(rc == ROCK_PODLINK_COMPLETE && e.crc32 == FIXTURE_CRC);
                    assert(rock_podlink_pull(target, OBJECT) == 0);
                    break;
                case 4:
                    assert(rc == ROCK_PODLINK_COMPLETE && e.crc32 == FIXTURE_CRC);
                    assert_file(target, fixture, FIXTURE_SIZE);
                    no_temp();
                    fputs("PASS external APL1 echo, push and pull\n", stderr);
                    return 0;
            }
            continue;
        }
        if (!fgets(line, sizeof(line), stdin))
            return 1;
        size_t digits = strcspn(line, "\n");
        if (!digits || digits % 2 || digits > 2 * LIMIT ||
            digits != strspn(line, "0123456789abcdefABCDEF"))
            return 2;
        unsigned char p[LIMIT];
        for (size_t i = 0; i < digits / 2; i++)
        {
            char byte[] = {line[2 * i], line[2 * i + 1], 0};
            p[i] = strtoul(byte, NULL, 16);
        }
        input_frame(PODLINK_BINARY, 20, p, digits / 2);
    }
}

int main(int argc, char **argv)
{
    bool cli = argc == 3 && !strcmp(argv[1], "--peer");
    if (argc != 1 && !cli)
    {
        fprintf(stderr, "Usage: %s [--peer DEVICE_ROOT]\n", argv[0]);
        return 2;
    }
    char temp[] = "/tmp/podlink-apl.XXXXXX";
    char base[4096];
    if (cli)
        assert(realpath(argv[2], base));
    else
    {
        assert(mkdtemp(temp));
        strcpy(base, temp);
    }
    assert(snprintf(root, sizeof(root), "%s/.rockbox", base) < (int)sizeof(root));
    assert(snprintf(source, sizeof(source), "%s/podlink-echo.in", root) < (int)sizeof(source));
    assert(snprintf(target, sizeof(target), "%s/podlink-echo.out", root) < (int)sizeof(target));

    for (unsigned int i = 0; i < FIXTURE_SIZE; i++)
        fixture[i] = i % 251;
    if (cli)
        assert_file(source, fixture, FIXTURE_SIZE);
    else
    {
        assert(mkdir(root, 0700) == 0);
        write_file(source, fixture, FIXTURE_SIZE);
    }

    usb_serial_init();
    podlink_init();
    lease = true;
    rock_podlink_init();
    connect_link();
    if (cli)
        return external_peer();

    uint32_t id = channel_open();
    test_messages(id);
    test_files(id);
    test_link(channel_open());
    test_time_and_reset();
    assert(unlink(source) == 0 && unlink(target) == 0);
    assert(rmdir(root) == 0 && rmdir(base) == 0);
    puts("PASS APL1 state machine, golden framing, credits, cancel, CRC, USB and reopen");
}
