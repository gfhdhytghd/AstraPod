/* FU47: every indirect call the podlink worker makes, driven through the
 * field triggers (Agents entry, Cider return with an empty item id + CN
 * pushes + queue change, remote WPS control) with partial resident tables,
 * resident swaps and unplug. Real service/session/serial; ASan/UBSan turn a
 * NULL or stale callback into a failure. */
#define PODLINK_SERVICE_TEST
#define DEFAULT_STACK_SIZE 4096
#ifndef MAX
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#endif
#define main transport_harness_main
#include "podlink_firmware_test.c"
#undef main
#undef podlink_open
#undef podlink_state
#undef podlink_max_message
#undef podlink_send
#undef podlink_close
#undef podlink_poll
#undef podlink_wait

void usb_podlink_disk(void) {}
void button_queue_post(long id, intptr_t data) { (void)id; (void)data; }
#include "podlink_rust_service.h"
#include "herdremote_model.h"

static struct hr_model cached;
static int background_calls, receive_calls, next_calls, reset_calls;
static void background(const struct podlink_message *m)
{
    background_calls++;
    if (m->type == PODLINK_TEXT)
        hr_model_apply(&cached, (const char *)m->data, m->length);
}
static bool receive(const struct podlink_message *m)
{
    receive_calls++;
    return m->type == PODLINK_TEXT && m->length >= 3 && m->data[0] == 'C';
}
static unsigned int next(char *record, unsigned int size)
{
    next_calls++;
    return next_calls % 3 ? 0 : (unsigned int)snprintf(record, size, "cc\t1\t%d\tplay\t0", next_calls);
}
static void reset(void) { reset_calls++; }

static const struct podlink_resident full = {receive, next, reset, NULL};
static const struct podlink_resident only_receive = {receive, NULL, NULL, NULL};
static const struct podlink_resident only_next = {NULL, next, NULL, NULL};
static const struct podlink_resident empty = {NULL, NULL, NULL, NULL};

static void work(void)
{
    static struct podlink_parser peer;
    static int cursor;
    if (output_size < cursor) { cursor = 0; memset(&peer, 0, sizeof peer); }
    for (int i = 0; i < 20; i++)
    {
        service_step(); controller(); current_tick++;
        unsigned snap[]={podlink_state(),podlink_host_connected(),podlink_generation(),podlink_session_state(),podlink_session_revision()};trace(7,snap,sizeof snap);
        struct podlink_message m;
        while (cursor < output_size)
            if (podlink_decode(&peer, output[cursor++], &m) == 1 &&
                m.type == PODLINK_PING)
                input_frame(PODLINK_PONG, m.seq, m.data, m.length);
    }
}
static void text(int seq, const char *s) { input_frame(PODLINK_TEXT, seq, s, strlen(s)); }

/* Host plugin after a long outage: item with an empty id, 4 CN, queue change. */
static void cider_return(int seq)
{
    text(seq, "CA\t9\t1\t");
    text(seq + 1, "CN\t9\t1\t\t\t\t0\t0\t0");
    text(seq + 2, "CN\t9\t2\t\tTitle\tArtist\t0\t1000\t1");
    text(seq + 3, "CN\t9\t3\t\t\t\t0\t0\t0");
    text(seq + 4, "CN\t9\t4\tid\tTitle\tArtist\t500\t1000\t1");
    text(seq + 5, "CQ\t9\tqueueChanged");
}

int main(void)
{
    usb_serial_init(); podlink_init(); lease = true; insert(); work();
    handshake(PODLINK_MAX_MESSAGE, 1); work();
    text(1, "HR\t1\thost"); work();
    assert(podlink_host_connected());
    const struct podlink_resident *tables[] = {&full, &only_receive, &only_next, &empty, NULL};
    for (int round = 0; round < 200; round++)
    {
        podlink_set_resident(tables[round % 5]);
        /* Agents entry: attach, list deltas, exit (background keeps them). */
        if (round % 2 == 0)
        {
            assert(podlink_open_agents(background));
            text(10, "W\t1\nw1\t-\t1\t0\t0\t0\tCached"); work();
            struct podlink_message m;
            while (podlink_poll(&m)) {}
            podlink_close();
        }
        cider_return(20); work();
        /* Remote WPS: foreground podlink_rust_debug(10) 2 with Cider traffic and overflow. */
        if (round % 3 == 0)
        {
            assert(podlink_open());
            for (int i = 0; i < 24; i++) text(40 + i, i % 2 ? "CN\t9\t5\t\t\t\t0\t0\t0" : "delta");
            work();
            podlink_close();
        }
        text(70, "W\t1\nw2\t-\t1\t0\t0\t0\tBackground"); work();
        if (round % 7 == 0) { input_frame(PODLINK_CLOSE, 80, NULL, 0); work();
                              handshake(PODLINK_MAX_MESSAGE, 1); work(); }
        if (round % 11 == 0) { remove_usb(); work(); insert(); work();
                               handshake(PODLINK_MAX_MESSAGE, 1); work(); }
    }
    podlink_set_resident(NULL); work();
    assert(receive_calls > 0 && next_calls > 0 && reset_calls > 0 && background_calls > 0);
    printf("FU47 dispatch: 200 rounds, receive=%d next=%d reset=%d background=%d; "
           "partial/NULL resident tables, Agents/Cider/WPS triggers, CLOSE, unplug PASS\n",
           receive_calls, next_calls, reset_calls, background_calls);
    unsigned calls[]={receive_calls,next_calls,reset_calls,background_calls};trace(16,calls,sizeof calls);
    return 0;
}
