/* SPDX-License-Identifier: GPL-2.0-or-later
 * Threaded PodLink liveness fixture (FU49 round 2). No device access.
 *
 * Real code under test: the Rust PodLink library, the firmware platform shim
 * (podlink_rust_platform.c, including its worker thread) and the CDC ring
 * (usb_serial.c). Emulated: Rockbox mutex/semaphore/sleep/IRQ-mask semantics
 * on pthreads, the USB controller + USB thread (completions and class
 * notifications are queued and handled on a separate thread, as on the
 * device), a herdremote-like host, an Agents-like UI thread, a Cider-like
 * resident consumer with its own mutex, a disk thread contending for that
 * mutex, and an EP0 "interrupt" polling podlink_host_connected().
 *
 * Checks: host bytes never wait undrained beyond a bound; every CH liveness
 * challenge is answered in time; UI API calls never block beyond a bound;
 * Agents bursts arrive; CN pushes and art-failure bursts flow; USB reconnect
 * and host CLOSE recover; the worker keeps stepping; the USB event queue
 * never exceeds the Rockbox QUEUE_LENGTH.
 */
#include "config.h"
#include "kernel.h"
#include "thread.h"
#include "panic.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sched.h>
#include <unistd.h>
#include "../../firmware/usbstack/usb_serial.c"
#include "../../firmware/usbstack/podlink_rust_platform.c"

/* ---------- clock, kernel and IRQ-mask emulation ---------- */
static struct timespec start_time;
static long ms_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (t.tv_sec - start_time.tv_sec) * 1000 +
           (t.tv_nsec - start_time.tv_nsec) / 1000000;
}
long live_tick(void) { return ms_now() / 10; }
uint32_t live_usec(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000);
}
static void ms_sleep(long ms)
{
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000};
    while (nanosleep(&t, &t) && errno == EINTR) {}
}
void yield(void) { sched_yield(); }
unsigned sleep(unsigned ticks) { ms_sleep(10L * ticks); return 0; }
static pthread_mutex_t irq_lock;
int disable_irq_save(void) { pthread_mutex_lock(&irq_lock); return 0; }
void restore_irq(int level) { (void)level; pthread_mutex_unlock(&irq_lock); }
void mutex_init(struct mutex *m)
{
    pthread_mutex_init(&m->lock, NULL);
    pthread_cond_init(&m->open, NULL);
    m->held = false;
    m->recursion = 0;
}
void mutex_lock(struct mutex *m)
{
    pthread_mutex_lock(&m->lock);
    if (m->held && pthread_equal(m->owner, pthread_self()))
        m->recursion++; /* Rockbox: the owner may recurse */
    else
    {
        while (m->held) pthread_cond_wait(&m->open, &m->lock);
        m->held = true;
        m->owner = pthread_self();
    }
    pthread_mutex_unlock(&m->lock);
}
void mutex_unlock(struct mutex *m)
{
    pthread_mutex_lock(&m->lock);
    if (!m->held || !pthread_equal(m->owner, pthread_self()))
    {
        fprintf(stderr, "FAIL: mutex_unlock by non-owner\n");
        abort();
    }
    if (m->recursion) m->recursion--;
    else { m->held = false; pthread_cond_signal(&m->open); }
    pthread_mutex_unlock(&m->lock);
}
void semaphore_init(struct semaphore *s, int maximum, int count)
{
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->cond, NULL);
    s->maximum = maximum;
    s->count = count;
}
void semaphore_release(struct semaphore *s)
{
    pthread_mutex_lock(&s->lock);
    if (s->count < s->maximum) s->count++;
    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->lock);
}
int semaphore_wait(struct semaphore *s, int ticks)
{
    pthread_mutex_lock(&s->lock);
    if (ticks < 0)
    {
        while (!s->count)
            pthread_cond_wait(&s->cond, &s->lock);
    }
    else
    {
        struct timespec t;
        clock_gettime(CLOCK_REALTIME, &t);
        t.tv_nsec += (long)ticks * 10000000L;
        t.tv_sec += t.tv_nsec / 1000000000L;
        t.tv_nsec %= 1000000000L;
        while (!s->count && pthread_cond_timedwait(&s->cond, &s->lock, &t) != ETIMEDOUT) {}
    }
    int ok = s->count > 0;
    if (ok) s->count--;
    pthread_mutex_unlock(&s->lock);
    return ok ? 0 : -1;
}
static void *thread_trampoline(void *fn)
{
    ((void (*)(void))fn)();
    return NULL;
}
unsigned int create_thread(void (*function)(void), void *stack, size_t size,
                           unsigned flags, const char *name, int priority)
{
    (void)stack; (void)size; (void)flags; (void)name; (void)priority;
    pthread_t t;
    if (pthread_create(&t, NULL, thread_trampoline, (void *)function)) return 0;
    pthread_detach(t);
    return 1;
}
void panicf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("FAIL: panicf: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    abort();
}
static volatile int redraws;
void button_queue_post(long id, intptr_t data) { (void)id; (void)data; __sync_fetch_and_add(&redraws, 1); }
bool remote_log_enabled(void) { return false; }
unsigned remote_log_epoch(void) { return 0; }
unsigned remote_log_read(char *out, unsigned size) { (void)out; (void)size; return 0; }

/* ---------- USB controller + USB thread ---------- */
#define QUEUE_LENGTH 16 /* Rockbox kernel queue length on this target */
enum { EV_OUT, EV_IN, EV_NOTIFY, EV_DISCONNECT, EV_CONNECT };
static pthread_mutex_t hw = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t hw_cond = PTHREAD_COND_INITIALIZER;
static int events[64], ev_len[64], ev_first, ev_used, ev_max;
static unsigned char *out_buf; static int out_size; static bool out_armed;
static unsigned char in_copy[FAST_IN_BYTES]; static int in_size;
static unsigned char host_tx[1 << 20]; static int host_tx_len; /* host -> device */
static unsigned char host_rx[1 << 20]; static int host_rx_len; /* device -> host */
static long last_drain_ms, max_stall_ms, stall_at_ms;
static volatile bool stop;
static void post(int id, int len) /* called with hw held */
{
    events[(ev_first + ev_used) % 64] = id;
    ev_len[(ev_first + ev_used) % 64] = len;
    if (++ev_used > ev_max) ev_max = ev_used;
    pthread_cond_broadcast(&hw_cond);
}
int usb_drv_port_speed(void) { return 1; }
int usb_drv_recv_nonblocking(int ep, void *data, int size)
{
    (void)ep;
    pthread_mutex_lock(&hw);
    out_buf = data; out_size = size; out_armed = true;
    pthread_cond_broadcast(&hw_cond);
    pthread_mutex_unlock(&hw);
    return 0;
}
int usb_drv_send_nonblocking(int ep, const void *data, int size)
{
    (void)ep;
    pthread_mutex_lock(&hw);
    memcpy(in_copy, data, size); in_size = size;
    pthread_cond_broadcast(&hw_cond);
    pthread_mutex_unlock(&hw);
    return 0;
}
void usb_signal_class_notify(int driver, uint32_t data)
{
    (void)driver; (void)data;
    pthread_mutex_lock(&hw); post(EV_NOTIFY, 0); pthread_mutex_unlock(&hw);
}
void usb_core_control_response(int response, void *data, int size) { (void)response; (void)data; (void)size; }
static volatile bool podlink_requested = true;
bool usb_podlink_open(void) { return true; }
void usb_podlink_close(void) {}
bool usb_podlink_active(void) { return podlink_requested; }
void usb_podlink_disk(void) { fputs("FAIL: unexpected disk handover\n", stderr); abort(); }

/* The "hardware": moves bytes and raises completions (IRQ -> queue). */
static void *controller(void *unused)
{
    (void)unused;
    pthread_mutex_lock(&hw);
    while (!stop)
    {
        bool moved = false;
        if (out_armed && host_tx_len)
        {
            int n = MIN(out_size, host_tx_len);
            memcpy(out_buf, host_tx, n);
            memmove(host_tx, host_tx + n, host_tx_len - n);
            host_tx_len -= n;
            out_armed = false;
            last_drain_ms = ms_now();
            post(EV_OUT, n);
            moved = true;
        }
        if (in_size)
        {
            memcpy(host_rx + host_rx_len, in_copy, in_size);
            host_rx_len += in_size;
            post(EV_IN, in_size);
            in_size = 0;
            moved = true;
        }
        long now = ms_now();
        if (host_tx_len && now - last_drain_ms > max_stall_ms)
        {
            max_stall_ms = now - last_drain_ms;
            stall_at_ms = now;
        }
        if (!moved)
        {
            struct timespec t;
            clock_gettime(CLOCK_REALTIME, &t);
            t.tv_nsec += 1000000; if (t.tv_nsec >= 1000000000L) { t.tv_sec++; t.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&hw_cond, &hw, &t);
        }
    }
    pthread_mutex_unlock(&hw);
    return NULL;
}
/* The USB thread: class driver callbacks run here, as in usb_core. */
static void *usb_thread(void *unused)
{
    (void)unused;
    while (!stop)
    {
        pthread_mutex_lock(&hw);
        while (!ev_used && !stop) pthread_cond_wait(&hw_cond, &hw);
        if (stop) { pthread_mutex_unlock(&hw); break; }
        int id = events[ev_first], len = ev_len[ev_first];
        ev_first = (ev_first + 1) % 64; ev_used--;
        pthread_mutex_unlock(&hw);
        static bool faulted; /* negative control: a starved USB thread must fail */
        if (!faulted && getenv("LIVE_FAULT_USB_STALL") && ms_now() > 10000)
        {
            faulted = true;
            ms_sleep(atoi(getenv("LIVE_FAULT_USB_STALL")));
        }
        if (id == EV_OUT) usb_serial_transfer_complete(0, USB_DIR_OUT, 0, len);
        else if (id == EV_IN) usb_serial_transfer_complete(0, USB_DIR_IN, 0, len);
        else if (id == EV_NOTIFY) serial_notify(0);
        else if (id == EV_DISCONNECT)
        {
            usb_serial_disconnect();
            pthread_mutex_lock(&hw);
            out_armed = false; in_size = 0; host_tx_len = 0; host_rx_len = 0;
            last_drain_ms = ms_now();
            pthread_mutex_unlock(&hw);
        }
        else if (id == EV_CONNECT)
        {
            usb_serial_init_connection();
            if (getenv("LIVE_FAST_IN"))
            {
                mutex_lock(&serial_mtx);
                fast_in = true;
                mutex_unlock(&serial_mtx);
            }
        }
    }
    return NULL;
}

static struct host_log { long ms; unsigned type, length, code; } host_log[64];
static int host_log_n;
/* ---------- herdremote-like host ---------- */
static struct podlink_parser host_parser;
/* Payloads are not NUL-terminated: parse digits within the message only. */
static int field_number(const struct podlink_message *m, unsigned at)
{
    int n = 0;
    for (unsigned i = at; i < m->length && m->data[i] >= '0' && m->data[i] <= '9'; i++)
        n = n * 10 + (m->data[i] - '0');
    return n;
}
static uint32_t host_seq = 1;
static unsigned char seen_seq[65536]; static long sent_seq_ms[65536]; static unsigned char sent_seq_type[65536];
static int host_epoch; /* bumps on each host reconnect */
static volatile bool host_open;
static long ch_sent_at[4096]; static int ch_sent, ch_answered, ch_late;
static long ch_max_ms, ch_dev_rx[4096], ch_dev_tx[4096], ch_host_rx[4096];
static int agents_bursts, cn_pushes, art_requests, art_failures_sent, opens, hellos;
static volatile bool want_agents_burst;
static void host_send(unsigned type, const void *data, unsigned len)
{
    static __thread unsigned char frame[PODLINK_FRAME_MAX];
    uint32_t seq = __sync_fetch_and_add(&host_seq, 1);
    unsigned n = podlink_encode(frame, type, seq, data, len);
    if (seq < 65536) { sent_seq_ms[seq] = ms_now(); sent_seq_type[seq] = type == PODLINK_TEXT ? ((const unsigned char *)data)[0] : '0' + type; }

    if (!n) { fprintf(stderr, "FAIL: host encode\n"); abort(); }
    pthread_mutex_lock(&hw);
    /* A host write blocks (write_timeout) while the CDC FIFO is full. */
    while (host_tx_len + (int)n > 65536 && !stop)
    {
        pthread_mutex_unlock(&hw); ms_sleep(1); pthread_mutex_lock(&hw);
    }
    if (!host_tx_len) last_drain_ms = ms_now();
    memcpy(host_tx + host_tx_len, frame, n);
    host_tx_len += n;
    pthread_cond_broadcast(&hw_cond);
    pthread_mutex_unlock(&hw);
}
static void host_text(const char *fmt, ...)
{
    char text[PODLINK_MAX_MESSAGE];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    host_send(PODLINK_TEXT, text, n);
}
static void host_handle(const struct podlink_message *m)
{
    if (m->type != PODLINK_TEXT && m->type != PODLINK_BINARY && host_log_n < 64)
        host_log[host_log_n++] = (struct host_log){ms_now(), m->type, m->length,
            m->length >= 2 ? m->data[0] | m->data[1] << 8 : 0};
    if (m->type == PODLINK_TEXT && m->length >= 2 && m->data[0] == 'h' && host_log_n < 64)
        host_log[host_log_n++] = (struct host_log){ms_now(), 'h', m->length, 0};
    if (m->type == PODLINK_HELLO)
    {
        hellos++;
        unsigned char open[6] = {1, 0, 0, 0x40, 0, 0};
        host_send(PODLINK_OPEN, open, 6);
    }
    else if (m->type == PODLINK_OPEN) { host_open = true; opens++; host_text("HR\t1\tfixture"); }
    else if (m->type == PODLINK_PING) host_send(PODLINK_PONG, m->data, m->length);
    else if (m->type == PODLINK_CLOSE) host_open = false;
    else if (m->type == PODLINK_TEXT)
    {
        const char *t = (const char *)m->data;
        if (m->length > 3 && !memcmp(t, "ch\t", 3))
        {
            int nonce = field_number(m, 3);
            if (nonce >= 0 && nonce < 4096) ch_host_rx[nonce] = ms_now();
            if (nonce >= 0 && nonce < ch_sent && ch_sent_at[nonce])
            {
                long age = ms_now() - ch_sent_at[nonce];
                if (age > ch_max_ms) ch_max_ms = age;
                ch_sent_at[nonce] = 0;
                ch_answered++;
            }
        }
        else if (m->length > 3 && !memcmp(t, "cr\t", 3))
        {
            art_requests++;
            unsigned char fail[16] = "CAR1";
            fail[4] = 1; /* failure record */
            host_send(PODLINK_BINARY, fail, sizeof fail);
            art_failures_sent++;
        }
        else if (m->length >= 4 && !memcmp(t, "hr\t1", 4)) want_agents_burst = true;
    }
}
static void *host_thread(void *unused)
{
    (void)unused;
    long next_ch = 0, next_cn = 0, next_cb = 0, next_tick = 0;
    int seen_epoch = 0;
    while (!stop)
    {
        unsigned char chunk[4096];
        int n;
        pthread_mutex_lock(&hw);
        if (seen_epoch != host_epoch)
        {
            seen_epoch = host_epoch;
            memset(&host_parser, 0, sizeof host_parser);
            host_open = false;
        }
        n = MIN(host_rx_len, (int)sizeof chunk);
        memcpy(chunk, host_rx, n);
        memmove(host_rx, host_rx + n, host_rx_len - n);
        host_rx_len -= n;
        pthread_mutex_unlock(&hw);
        unsigned off = 0;
        do
        {
            struct podlink_message m;
            unsigned used = 0;
            int rc = podlink_decode_bytes(&host_parser, chunk + off, n - off, &used, &m);
            off += used;
            if (rc == 1) host_handle(&m);
            else if (rc == 0 && off >= (unsigned)n) break;
        } while (1);
        long now = ms_now();
        if (host_open)
        {
            if (now >= next_ch && ch_sent < 4096)
            {
                ch_sent_at[ch_sent] = now;
                host_text("CH\t1\t%04d", ch_sent++);
                next_ch = now + 1000;
            }
            if (now >= next_cn)
            {
                char filler[1500];
                memset(filler, 'n', sizeof filler - 1); filler[sizeof filler - 1] = 0;
                host_text("CN\t1\t%d\t%s", cn_pushes++, filler);
                next_cn = now + 500;
            }
            if (now >= next_cb) { host_text("CB\t1\t551\t262\t966\t3"); next_cb = now + 5000; }
            if (now >= next_tick) { host_text("T\t%ld\t1", now / 1000); next_tick = now + 1000; }
            if (want_agents_burst)
            {
                want_agents_burst = false;
                agents_bursts++;
                char row[PODLINK_MAX_MESSAGE];
                for (int i = 0; i < 120; i++)
                {
                    int len = 100 + (i * 397) % 1900;
                    memset(row, 'a' + i % 26, len); row[len] = 0;
                    host_text("W\t%d\t%s", i, row);
                }
                for (int i = 0; i < 3; i++)
                {
                    memset(row, 'z', 16000); row[16000] = 0;
                    host_text("D\t%d\t%s", i, row);
                }
            }
        }
        ms_sleep(2);
    }
    return NULL;
}

/* ---------- Cider-like resident consumer + disk thread ---------- */
static struct mutex cider;
static long reset_ms[16];
static int ch_pending[64], ch_count, cider_resets, art_burst, art_failures_seen;
static long next_ld, next_art;
static bool cider_receive(const struct podlink_message *m)
{
    if (m->seq < 65536) seen_seq[m->seq] = 1;
    bool binary = m->type == PODLINK_BINARY && m->length >= 4 && !memcmp(m->data, "CAR1", 4);
    bool text = m->type == PODLINK_TEXT && m->length >= 3 && m->data[0] == 'C' && m->data[2] == '\t';
    if (!binary && !text) return false;
    mutex_lock(&cider);
    if (binary) art_failures_seen++;
    else if (m->data[1] == 'H' && ch_count < 64)
    {
        int nonce = field_number(m, 5);
        ch_pending[ch_count++] = nonce;
        if (nonce >= 0 && nonce < 4096) ch_dev_rx[nonce] = ms_now();
    }
    mutex_unlock(&cider);
    return true;
}
static unsigned cider_next(char *record, unsigned size)
{
    unsigned n = 0;
    mutex_lock(&cider);
    long now = ms_now();
    if (ch_count)
    {
        n = snprintf(record, size, "ch\t%04d", ch_pending[0]);
        if (ch_pending[0] >= 0 && ch_pending[0] < 4096) ch_dev_tx[ch_pending[0]] = now;
        memmove(ch_pending, ch_pending + 1, --ch_count * sizeof ch_pending[0]);
    }
    else if (art_burst)
    {
        n = snprintf(record, size, "cr\t1\t%d\tXS", art_burst--);
    }
    else if (now >= next_art) { art_burst = 5; next_art = now + 3000; }
    else if (now >= next_ld) { n = snprintf(record, size, "ld\t1\t1\t783"); next_ld = now + 5000; }
    mutex_unlock(&cider);
    return n;
}
static void cider_reset(void)
{
    mutex_lock(&cider);
    ch_count = 0; art_burst = 0; cider_resets++;
    if (cider_resets <= 16) reset_ms[cider_resets - 1] = ms_now();
    mutex_unlock(&cider);
    button_queue_post(BUTTON_REDRAW, 0);
}
static void *disk_thread(void *unused)
{
    (void)unused;
    while (!stop) /* cider-cache holds the consumer mutex for disk work */
    {
        mutex_lock(&cider); ms_sleep(15); mutex_unlock(&cider);
        ms_sleep(40);
    }
    return NULL;
}
static volatile long irq_polls;
static void *ep0_irq(void *unused) /* EP0 callback: IRQ-masked, nonblocking */
{
    (void)unused;
    while (!stop)
    {
        int level = disable_irq_save();
        (void)podlink_host_connected();
        restore_irq(level);
        irq_polls++;
        ms_sleep(5);
    }
    return NULL;
}

/* ---------- Agents-like UI thread ---------- */
static long ui_max_ms; static const char *ui_max_call = "";
static int agents_messages, agents_phases, agents_closes;
static void background(const struct podlink_message *m) { (void)m; }
#define TIMED(name, expr) ({ long t0_ = ms_now(); __auto_type r_ = (expr); long d_ = ms_now() - t0_; \
    if (d_ > ui_max_ms) { ui_max_ms = d_; ui_max_call = name; } r_; })
static void *ui_thread(void *unused)
{
    (void)unused;
    long phase_end = 0;
    bool open = false;
    while (!stop)
    {
        long now = ms_now();
        if (!open && now >= phase_end)
        {
            if (TIMED("open_agents", podlink_open_agents(background)))
            {
                open = true; agents_phases++;
                phase_end = now + 4000;
            }
        }
        if (open)
        {
            static uint32_t generation;
            static bool asked;
            enum podlink_connection state = TIMED("state", podlink_state());
            uint32_t g = TIMED("generation", podlink_generation());
            if (g != generation) { generation = g; asked = false; }
            if (state == PODLINK_CONNECTED && !asked &&
                TIMED("send", podlink_send(PODLINK_TEXT, "hr\t1", 4)) == PODLINK_SENT)
                asked = true;
            for (int i = 0; i < 16; i++)
            {
                struct podlink_message m;
                if (!TIMED("poll", podlink_poll(&m))) break;
                if (m.type == PODLINK_TEXT) agents_messages++;
            }
            char stamp[PODLINK_TIME_RECORD]; unsigned long tick;
            (void)TIMED("take_time", podlink_take_time(stamp, &tick));
            (void)TIMED("max_message", podlink_max_message());
            if (now >= phase_end)
            {
                long t0 = ms_now(); podlink_close(); long d = ms_now() - t0;
                if (d > ui_max_ms) { ui_max_ms = d; ui_max_call = "close"; }
                open = false; agents_closes++; asked = false;
                phase_end = now + 1000;
            }
        }
        ms_sleep(20);
    }
    if (open) podlink_close();
    return NULL;
}

/* ---------- scenario ---------- */
static void usb_event(int id) { pthread_mutex_lock(&hw); post(id, 0); pthread_mutex_unlock(&hw); }
static int failures;
static void check(bool ok, const char *what)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}
int main(int argc, char **argv)
{
    int seconds = argc > 1 ? atoi(argv[1]) : 20;
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&irq_lock, &a);
    mutex_init(&cider);
    usb_serial_init();
    podlink_init(); /* real shim: creates the real worker thread */
    static const struct podlink_resident consumer = {cider_receive, cider_next, cider_reset, NULL};
    podlink_set_resident(&consumer);
    pthread_t t[6];
    pthread_create(&t[0], NULL, controller, NULL);
    pthread_create(&t[1], NULL, usb_thread, NULL);
    pthread_create(&t[2], NULL, host_thread, NULL);
    pthread_create(&t[3], NULL, disk_thread, NULL);
    pthread_create(&t[4], NULL, ep0_irq, NULL);
    pthread_create(&t[5], NULL, ui_thread, NULL);
    usb_event(EV_CONNECT);
    long end = seconds * 1000L, reconnect = end * 2 / 5, host_close = end * 7 / 10;
    bool reconnected = false, closed = false;
    int opens_before_reconnect = 0, opens_before_close = 0, ch_before_reconnect = 0;
    long reconnect_at = 0;
    while (ms_now() < end)
    {
        long now = ms_now();
        if (!reconnected && now >= reconnect)
        {
            reconnected = true; reconnect_at = now;
            opens_before_reconnect = opens; ch_before_reconnect = ch_answered;
            usb_event(EV_DISCONNECT);
            ms_sleep(200);
            pthread_mutex_lock(&hw);
            host_epoch++;
            /* drop pending challenges from the dead stream */
            for (int i = 0; i < ch_sent; i++) ch_sent_at[i] = 0;
            pthread_mutex_unlock(&hw);
            usb_event(EV_CONNECT);
        }
        if (!closed && now >= host_close)
        {
            closed = true; opens_before_close = opens;
            unsigned char code[2] = {0xe8, 0x03};
            host_open = false;
            host_send(PODLINK_CLOSE, code, 2);
            ms_sleep(100);
            /* CLOSE resets the consumer: earlier challenges are void */
            pthread_mutex_lock(&hw);
            for (int i = 0; i < ch_sent; i++) ch_sent_at[i] = 0;
            pthread_mutex_unlock(&hw);
            unsigned char open[6] = {1, 0, 0, 0x40, 0, 0};
            host_send(PODLINK_OPEN, open, 6);
        }
        ms_sleep(10);
    }
    long now = ms_now();
    int outstanding_late = 0;
    for (int i = 0; i < ch_sent; i++)
        if (ch_sent_at[i] && now - ch_sent_at[i] > 3000)
        {
            outstanding_late++;
            printf("late CH %04d sent %ld dev_rx %ld dev_tx %ld host_rx %ld ms\n", i,
                   ch_sent_at[i], ch_dev_rx[i], ch_dev_tx[i], ch_host_rx[i]);
        }
    stop = true;
    pthread_cond_broadcast(&hw_cond);
    for (int i = 0; i < 6; i++) pthread_join(t[i], NULL);
    printf("{\"seconds\":%d,\"max_rx_stall_ms\":%ld,\"stall_at_ms\":%ld,\"ch_sent\":%d,"
           "\"ch_answered\":%d,\"ch_max_ms\":%ld,\"ch_late\":%d,\"ui_max_ms\":%ld,"
           "\"ui_max_call\":\"%s\",\"agents_phases\":%d,\"agents_closes\":%d,"
           "\"agents_messages\":%d,\"agents_bursts\":%d,\"cn_pushes\":%d,\"art_requests\":%d,"
           "\"art_failures_seen\":%d,\"opens\":%d,\"hellos\":%d,\"cider_resets\":%d,"
           "\"usb_queue_max\":%d,\"irq_polls\":%ld,\"redraws\":%d}\n",
           seconds, max_stall_ms, stall_at_ms, ch_sent, ch_answered, ch_max_ms,
           ch_late + outstanding_late, ui_max_ms, ui_max_call, agents_phases, agents_closes,
           agents_messages, agents_bursts, cn_pushes, art_requests, art_failures_seen, opens,
           hellos, cider_resets, ev_max, irq_polls, redraws);
    check(max_stall_ms < 500, "host bytes always drained within 500 ms (mixed traffic)");
    check(ch_answered >= ch_sent - 3 && outstanding_late == 0 && ch_max_ms < 3000,
          "every CH liveness challenge answered within 3 s");
    check(ui_max_ms < 100, "UI-thread PodLink calls never block 100 ms");
    check(agents_bursts >= 2 && agents_messages >= 2 * 100 && agents_closes >= 2,
          "Agents bursts delivered, then idle; open/close cycles");
    check(cn_pushes >= seconds && art_requests >= 5 && art_failures_seen >= 5,
          "Cider CN pushes and art-failure bursts flow");
    check(reconnected && opens > opens_before_reconnect && ch_answered > ch_before_reconnect &&
          reconnect_at > 0, "USB reconnect: session reopens and traffic resumes");
    check(closed && opens > opens_before_close, "host CLOSE then OPEN: session recovers");
    check(ev_max <= QUEUE_LENGTH, "USB event queue stays within QUEUE_LENGTH");
    check(irq_polls > seconds * 100, "IRQ-masked host_connected never blocks");
    if (failures)
        for (unsigned i = 1; i < host_seq && i < 65536; i++)
            if (!seen_seq[i] && sent_seq_type[i] != '0' + PODLINK_PONG && sent_seq_type[i] != '0' + PODLINK_OPEN)
                printf("device never saw seq %u type %c sent %ld\n", i, sent_seq_type[i], sent_seq_ms[i]);
    if (failures)
        for (int i = 0; i < cider_resets && i < 16; i++) printf("consumer reset %ld\n", reset_ms[i]);
    if (failures)
        for (int i = 0; i < host_log_n; i++)
            printf("host got %6ld type %u len %u code %u\n", host_log[i].ms, host_log[i].type,
                   host_log[i].length, host_log[i].code);
    if (failures) { printf("FAIL: %d liveness checks\n", failures); return 1; }
    puts("PASS: podlink liveness");
    return 0;
}
