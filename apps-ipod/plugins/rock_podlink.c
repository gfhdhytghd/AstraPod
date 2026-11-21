/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "config.h"
#include "rock_podlink.h"

#ifndef HAVE_USB_PODLINK

int rock_podlink_open(const char *name)
{
    (void)name;
    return -ROCK_PODLINK_UNSUPPORTED;
}
void rock_podlink_close(void) {}
int rock_podlink_send(bool binary, const void *data, unsigned int length)
{
    (void)binary; (void)data; (void)length;
    return -ROCK_PODLINK_UNSUPPORTED;
}
int rock_podlink_poll(struct rock_podlink_event *event, unsigned int timeout_ms,
                      rock_podlink_progress_fn progress, void *context)
{
    (void)event; (void)timeout_ms; (void)progress; (void)context;
    return -ROCK_PODLINK_UNSUPPORTED;
}
int rock_podlink_push(const char *path, const char *object)
{
    (void)path; (void)object;
    return -ROCK_PODLINK_UNSUPPORTED;
}
int rock_podlink_pull(const char *path, const char *object)
{
    return rock_podlink_push(path, object);
}
int rock_podlink_cancel(void) { return -ROCK_PODLINK_UNSUPPORTED; }

#else

#include "rock_podlink_port.h"
#include "podlink.h"
#include "file.h"
#include <stdio.h>
#include <string.h>

enum opcode {
    OPEN = 1, OPEN_OK, CLOSE, TEXT, BINARY, PUSH, PULL,
    ACCEPT, CREDIT, CHUNK, FINISH, DONE, CANCEL
};
enum phase { IDLE, PREPARE, REQUEST, WAIT_ACCEPT, STREAM, WAIT_DONE,
             SEND_DONE, COMMITTING, STOPPING };

#define HEADER 16u
#define CHUNK_HEADER 8u
#define WINDOW 2u
#define CAPACITY (HEADER + CHUNK_HEADER + ROCK_PODLINK_MESSAGE_MAX)
/* Two chunks, plus ACCEPT and FINISH. CLOSE and CANCEL bypass the queue. */
#define FILE_QUEUE (WINDOW + 2u)
#define TEMP_SUFFIX 32u

struct frame {
    unsigned int size;
    unsigned char data[CAPACITY];
};

struct control {
    uint32_t channel, transfer;
    unsigned char reason;
};

struct transfer {
    enum phase phase;
    bool push, report;
    unsigned char reason;
    uint32_t id, channel, total, bytes, crc, deadline;
    unsigned int credits;
    char path[MAX_PATH], object[65];
};

/* Shared mailboxes and state. Every access uses apl_lock(), except init.
 * The worker alone uses the disk state below and never holds this lock
 * across file calls. No path, message or callback points into a module. */
static bool initialized, exclusive;
static uint32_t serial, channel, open_deadline, poll_deadline;
static bool open_sent, opened, opened_event, closed_event, waiting_poll;
static unsigned char closed_reason;
static unsigned int wire_max;
static struct control close_tx, cancel_tx;
/* Short incoming controls have their own slots, even with a full inbox.
 * RX callbacks post these signals; the disk worker applies them. */
static bool accept_open, close_pending, cancel_pending;
static bool reply_close, reply_cancel;
static unsigned char close_reason, cancel_reason;
static struct frame open_tx, file_tx, message_tx, message_rx;
static struct frame file_rx[FILE_QUEUE];
static unsigned int file_head, file_count;
static bool prefer_message;
static struct transfer file;
static bool progress_event, complete_event;

/* Disk worker's private storage. Shared file.path/object remain immutable
 * until the worker finishes cleanup and the foreground consumes COMPLETE. */
static int fd = -1;
static char temp[MAX_PATH];
static bool temp_owned;
static struct frame input;
static unsigned char chunk[ROCK_PODLINK_MESSAGE_MAX];

static uint32_t get32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put32(unsigned char *p, uint32_t value)
{
    for (unsigned int i = 0; i < 4; i++, value >>= 8)
        p[i] = value & 255;
}

static void header(unsigned char *out, enum opcode op, uint32_t id, uint32_t xfer)
{
    memcpy(out, "APL1", 4);
    out[4] = op;
    memset(out + 5, 0, 3);
    put32(out + 8, id);
    put32(out + 12, xfer);
}

static unsigned char *stage(struct frame *frame, enum opcode op, uint32_t id,
                            uint32_t xfer, unsigned int len)
{
    header(frame->data, op, id, xfer);
    frame->size = HEADER + len;
    return frame->data + HEADER;
}

static uint32_t allocate_id(void)
{
    if (!++serial)
        ++serial;
    return serial;
}

static bool expired(uint32_t deadline)
{
    return (int32_t)(apl_now() - deadline) >= 0;
}

static void advance(void)
{
    file.deadline = apl_now() + 10 * apl_hz();
}

static bool name_valid(const char *name, unsigned int max, bool upper)
{
    if (!name || !*name)
        return false;
    for (unsigned int i = 0; name[i]; i++)
    {
        unsigned char c = name[i];
        bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                     (upper && c >= 'A' && c <= 'Z');
        if (i == max || (!alnum && !(i && (c == '.' || c == '_' || c == '-'))))
            return false;
    }
    return true;
}

static bool utf8_valid(const unsigned char *p, unsigned int len)
{
    while (len)
    {
        unsigned int first = *p++;
        len--;
        if (first < 0x80)
            continue;
        if (first < 0xc2 || first > 0xf4)
            return false;
        unsigned int count = first < 0xe0 ? 1 : first < 0xf0 ? 2 : 3;
        if (count > len || (first == 0xe0 && p[0] < 0xa0) ||
            (first == 0xed && p[0] >= 0xa0) || (first == 0xf0 && p[0] < 0x90) ||
            (first == 0xf4 && p[0] >= 0x90))
            return false;
        len -= count;
        while (count--)
            if ((*p++ & 0xc0) != 0x80)
                return false;
    }
    return true;
}

static void stop_file(unsigned char reason, bool notify)
{
    if (file.phase == IDLE || file.phase == STOPPING || file.phase == COMMITTING)
        return;
    if (notify && file.phase >= WAIT_ACCEPT)
        cancel_tx = (struct control){file.channel, file.id, reason};
    file_tx.size = 0;
    cancel_pending = false;
    file_count = 0;
    file.reason = reason;
    file.phase = STOPPING;
}

static void end_channel(unsigned char reason, bool notify, bool report)
{
    if (!channel)
        return;
    if (notify && open_sent)
        close_tx = (struct control){channel, 0, reason};
    open_tx.size = file_tx.size = message_tx.size = message_rx.size = 0;
    cancel_tx.channel = 0;
    accept_open = close_pending = cancel_pending = false;
    opened = open_sent = opened_event = false;
    channel = 0;
    wire_max = 0;
    stop_file(reason ? reason : ROCK_PODLINK_CANCELLED, false);
    closed_event = report;
    closed_reason = reason;
}

static void reset(void)
{
    int irq = apl_lock();
    end_channel(ROCK_PODLINK_LINK_LOST, false, true);
    close_tx.channel = cancel_tx.channel = 0;
    apl_unlock(irq);
    apl_wake();
}

static unsigned int take_control(struct control *control, enum opcode op,
                                 unsigned char *out, unsigned int size)
{
    if (!control->channel)
        return 0;
    if (size < HEADER + 2)
    {
        control->channel = 0;
        return 0;
    }
    header(out, op, control->channel, control->transfer);
    out[HEADER] = control->reason;
    out[HEADER + 1] = 0;
    control->channel = 0;
    return HEADER + 2;
}

static unsigned int next(unsigned char *out, unsigned int size)
{
    int irq = apl_lock();
    wire_max = size;
    unsigned int len = take_control(&close_tx, CLOSE, out, size);
    if (!len)
        len = take_control(&cancel_tx, CANCEL, out, size);
    if (!len && !close_pending && !cancel_pending)
    {
        struct frame *slot = &open_tx;
        if (!slot->size)
            slot = message_tx.size && (prefer_message || !file_tx.size)
                ? &message_tx : &file_tx;
        if (slot->size > size)
        {
            if (slot == &file_tx && file.phase != IDLE)
                stop_file(ROCK_PODLINK_LIMIT, true);
            else
                end_channel(ROCK_PODLINK_LIMIT, true, true);
        }
        else if (slot->size)
        {
            len = slot->size;
            memcpy(out, slot->data, len);
            slot->size = 0;
            switch (out[4])
            {
                case OPEN: open_sent = true; break;
                case PUSH:
                case PULL: file.phase = WAIT_ACCEPT; break;
                case CREDIT: file.credits += out[HEADER]; break;
                case FINISH: file.phase = WAIT_DONE; break;
                case DONE:
                    file.phase = IDLE;
                    complete_event = file.report;
                    break;
            }
            prefer_message = slot == &file_tx;
        }
    }
    apl_unlock(irq);
    if (len)
        apl_wake();
    return len;
}

static void receive(const struct podlink_message *message)
{
    const unsigned char *p = message->data;
    unsigned int len = message->length;
    if (len < HEADER)
        return;
    int irq = apl_lock();
    if (!channel || get32(p + 8) != channel || !open_sent)
    {
        apl_unlock(irq);
        return;
    }

    unsigned int op = p[4], payload = len - HEADER;
    uint32_t id = get32(p + 12);
    bool reserved = p[5] || p[6] || p[7];
    unsigned char reason = payload == 2 && !p[HEADER + 1] &&
                           p[HEADER] <= ROCK_PODLINK_LIMIT
                         ? p[HEADER] : ROCK_PODLINK_INVALID;

    if (!id)
    {
        if (!reserved && op == CLOSE)
        {
            close_pending = true;
            close_reason = reason;
            reply_close = false;
        }
        else if (!reserved && op == OPEN_OK && !opened && !payload)
            accept_open = true;
        else if (!reserved && (opened || accept_open) &&
                 (op == TEXT || op == BINARY) && payload <= ROCK_PODLINK_MESSAGE_MAX)
        {
            if (message_rx.size && !close_pending)
            {
                close_pending = true;
                close_reason = ROCK_PODLINK_BUSY;
                reply_close = true;
            }
            else if (!close_pending)
            {
                memcpy(message_rx.data, p, len);
                message_rx.size = len;
            }
        }
        else if (!close_pending)
        {
            close_pending = true;
            close_reason = ROCK_PODLINK_INVALID;
            reply_close = true;
        }
    }
    else if (opened && id == file.id && file.phase >= WAIT_ACCEPT &&
             file.phase <= WAIT_DONE && !cancel_pending)
    {
        if (!reserved && op == CANCEL)
        {
            cancel_pending = true;
            cancel_reason = reason ? reason : ROCK_PODLINK_CANCELLED;
            reply_cancel = false;
        }
        else if (reserved || len > CAPACITY || op < ACCEPT || op > DONE ||
                 (op == CHUNK && (file.push || !file.credits)))
        {
            cancel_pending = true;
            cancel_reason = ROCK_PODLINK_INVALID;
            reply_cancel = true;
        }
        else if (file_count == FILE_QUEUE)
        {
            cancel_pending = true;
            cancel_reason = ROCK_PODLINK_BUSY;
            reply_cancel = true;
        }
        else
        {
            if (op == CHUNK)
                file.credits--;
            struct frame *slot = &file_rx[(file_head + file_count++) % FILE_QUEUE];
            memcpy(slot->data, p, len);
            slot->size = len;
        }
    }
    apl_unlock(irq);
    apl_wake();
}

void rock_podlink_init(void)
{
    if (initialized || !apl_start_worker())
        return;
    const struct podlink_apl_consumer consumer = {receive, next, reset};
    podlink_set_apl_consumer(&consumer);
    initialized = true;
}

/* Foreground only. Snapshot the physical connection independently of APL1. */
static bool check_link(void)
{
    if (!exclusive && podlink_host_connected())
        return true;
    end_channel(ROCK_PODLINK_LINK_LOST, false, true);
    close_tx.channel = cancel_tx.channel = 0;
    return false;
}

int rock_podlink_open(const char *name)
{
    if (!name_valid(name, 32, false))
        return -ROCK_PODLINK_INVALID;
    int irq = apl_lock();
    int rc = -ROCK_PODLINK_LINK_LOST;
    if (initialized && check_link())
    {
        if (channel || close_tx.channel || file.phase != IDLE)
            rc = -ROCK_PODLINK_BUSY;
        else
        {
            channel = allocate_id();
            opened_event = closed_event = progress_event = complete_event = waiting_poll = false;
            open_deadline = apl_now() + 3 * apl_hz();
            unsigned int len = strlen(name);
            memcpy(stage(&open_tx, OPEN, channel, 0, len), name, len);
            rc = 0;
        }
    }
    apl_unlock(irq);
    if (rc == 0)
    {
        apl_wake();
        podlink_wake();
    }
    return rc;
}

void rock_podlink_close(void)
{
    if (!initialized)
        return;
    int irq = apl_lock();
    end_channel(ROCK_PODLINK_OK, check_link(), false);
    file.report = false;
    opened_event = closed_event = progress_event = complete_event = waiting_poll = false;
    apl_unlock(irq);
    apl_wake();
    podlink_wake();
}

int rock_podlink_send(bool binary, const void *data, unsigned int length)
{
    if (length > ROCK_PODLINK_MESSAGE_MAX)
        return -ROCK_PODLINK_LIMIT;
    if ((length && !data) || (!binary && !utf8_valid(data, length)))
        return -ROCK_PODLINK_INVALID;
    int irq = apl_lock();
    int rc = -ROCK_PODLINK_LINK_LOST;
    if (initialized && check_link() && opened)
    {
        if (HEADER + length > wire_max)
            rc = -ROCK_PODLINK_LIMIT;
        else if (message_tx.size)
            rc = -ROCK_PODLINK_BUSY;
        else
        {
            unsigned char *out = stage(&message_tx, binary ? BINARY : TEXT,
                                       channel, 0, length);
            if (length)
                memcpy(out, data, length);
            rc = 0;
        }
    }
    apl_unlock(irq);
    if (rc == 0)
        podlink_wake();
    return rc;
}

static int start_file(bool push, const char *path, const char *object)
{
    if (!path || path[0] != '/' || !name_valid(object, 64, true))
        return -ROCK_PODLINK_INVALID;
    unsigned int len = 0;
    while (len < MAX_PATH && path[len])
        len++;
    if (len >= MAX_PATH - (push ? 0 : TEMP_SUFFIX))
        return -ROCK_PODLINK_LIMIT;
    if (path[len - 1] == '/')
        return -ROCK_PODLINK_INVALID;
    int irq = apl_lock();
    int rc = -ROCK_PODLINK_LINK_LOST;
    if (initialized && check_link() && opened)
    {
        if (file.phase != IDLE || complete_event || file_tx.size || cancel_tx.channel)
            rc = -ROCK_PODLINK_BUSY;
        else
        {
            file.phase = PREPARE;
            file.push = push;
            file.report = true;
            file.reason = ROCK_PODLINK_OK;
            file.id = allocate_id();
            file.channel = channel;
            file.total = file.bytes = file.crc = file.credits = 0;
            memcpy(file.path, path, len + 1);
            strcpy(file.object, object);
            advance();
            rc = 0;
        }
    }
    apl_unlock(irq);
    if (rc == 0)
        apl_wake();
    return rc;
}

int rock_podlink_push(const char *path, const char *object)
{
    return start_file(true, path, object);
}

int rock_podlink_pull(const char *path, const char *object)
{
    return start_file(false, path, object);
}

int rock_podlink_cancel(void)
{
    int irq = apl_lock();
    bool active = file.phase != IDLE && file.phase < SEND_DONE;
    if (active)
        stop_file(ROCK_PODLINK_CANCELLED, true);
    apl_unlock(irq);
    if (!active)
        return -ROCK_PODLINK_INVALID;
    apl_wake();
    podlink_wake();
    return 0;
}

int rock_podlink_poll(struct rock_podlink_event *event, unsigned int timeout_ms,
                      rock_podlink_progress_fn progress, void *context)
{
    if (!event)
        return -ROCK_PODLINK_INVALID;
    if (!initialized)
        return -ROCK_PODLINK_LINK_LOST;

    int irq = apl_lock();
    check_link();
    int type = 0;
    unsigned int len = 0;
    unsigned char reason = ROCK_PODLINK_OK;
    if (closed_event)
    {
        closed_event = false;
        type = ROCK_PODLINK_CLOSED;
        reason = closed_reason;
    }
    else if (opened_event)
    {
        opened_event = false;
        type = ROCK_PODLINK_OPENED;
    }
    else if (complete_event || progress_event)
    {
        type = complete_event ? ROCK_PODLINK_COMPLETE : ROCK_PODLINK_PROGRESS;
        reason = file.reason;
        complete_event = progress_event = false;
    }
    else if (message_rx.size && opened && !close_pending)
    {
        len = message_rx.size - HEADER;
        type = message_rx.data[4] == TEXT ? ROCK_PODLINK_TEXT : ROCK_PODLINK_BINARY;
        memcpy(event->data, message_rx.data + HEADER, len);
        message_rx.size = 0;
    }

    if (type)
    {
        waiting_poll = false;
        event->type = type;
        event->result = reason;
        event->length = len;
        bool file_event = type == ROCK_PODLINK_COMPLETE || type == ROCK_PODLINK_PROGRESS;
        event->bytes = file_event ? file.bytes : 0;
        event->total = file_event ? file.total : 0;
        event->crc32 = file_event ? file.crc : 0;
    }
    else if (!timeout_ms)
        waiting_poll = false;
    else if (!waiting_poll)
    {
        /* Cap before conversion so a wrapped tick comparison stays valid. */
        if (timeout_ms > 60000)
            timeout_ms = 60000;
        poll_deadline = apl_now() + (timeout_ms * apl_hz() + 999) / 1000;
        waiting_poll = true;
    }
    else if (expired(poll_deadline))
    {
        waiting_poll = false;
        type = -ROCK_PODLINK_TIMEOUT;
    }
    apl_unlock(irq);

    if (type == ROCK_PODLINK_TEXT && !utf8_valid(event->data, len))
    {
        irq = apl_lock();
        end_channel(ROCK_PODLINK_INVALID, true, true);
        apl_unlock(irq);
        apl_wake();
        podlink_wake();
        return 0;
    }
    if (progress && (type == ROCK_PODLINK_COMPLETE || type == ROCK_PODLINK_PROGRESS))
        progress(event, context);
    return type;
}

/* Disk calls below are made by the worker with IRQs enabled. Release the
 * lock before each disk operation and recheck phase before publishing. */
static void cleanup(void)
{
    if (fd >= 0)
    {
        close(fd);
        fd = -1;
    }
    if (temp_owned)
    {
        remove(temp);
        temp_owned = false;
    }
}

void rock_podlink_storage(bool unavailable)
{
    int irq = apl_lock();
    exclusive = unavailable;
    if (unavailable)
    {
        end_channel(ROCK_PODLINK_LINK_LOST, false, true);
        close_tx.channel = cancel_tx.channel = 0;
    }
    apl_unlock(irq);
    if (unavailable)
        cleanup();
}

static int prepare_file(void)
{
    if (file.push)
    {
        fd = open(file.path, O_RDONLY);
        return fd < 0 ? ROCK_PODLINK_IO : ROCK_PODLINK_OK;
    }
    size_t len = strlen(file.path);
    memcpy(temp, file.path, len);
    snprintf(temp + len, sizeof(temp) - len, ".apl-%08lx-%08lx",
             (unsigned long)file.id, (unsigned long)apl_now());
    fd = open(temp, O_WRONLY | O_CREAT | O_EXCL, 0666);
    temp_owned = fd >= 0;
    return temp_owned ? ROCK_PODLINK_OK : ROCK_PODLINK_IO;
}

static bool prepare(int irq)
{
    apl_unlock(irq);
    uint32_t total = 0;
    int reason = prepare_file();
    if (reason == ROCK_PODLINK_OK && file.push)
        reason = apl_file_size(fd, &total);
    irq = apl_lock();
    if (file.phase == PREPARE && check_link())
    {
        if (reason != ROCK_PODLINK_OK)
            stop_file(reason, false);
        else
        {
            file.total = total;
            unsigned int prefix = file.push ? 4 : 0;
            unsigned int len = strlen(file.object);
            unsigned char *p = stage(&file_tx, file.push ? PUSH : PULL,
                                     file.channel, file.id, prefix + len);
            if (file.push)
                put32(p, total);
            memcpy(p + prefix, file.object, len);
            file.phase = REQUEST;
            advance();
        }
    }
    apl_unlock(irq);
    podlink_wake();
    return true;
}

static uint32_t update_crc(uint32_t previous, const unsigned char *data, unsigned int len)
{
    uint32_t raw;
    return ~podlink_bulk_crc(data, len, ~previous, &raw);
}

static bool write_chunk(int irq, const unsigned char *data, unsigned int len)
{
    uint32_t crc = file.crc;
    apl_unlock(irq);
    bool written = write(fd, data, len) == (ssize_t)len;
    if (written)
        crc = update_crc(crc, data, len);
    irq = apl_lock();
    if (file.phase == STREAM && check_link())
    {
        if (!written)
            stop_file(ROCK_PODLINK_IO, true);
        else
        {
            file.bytes += len;
            file.crc = crc;
            advance();
            progress_event = true;
            /* The peer may not spend the replenishment until next() takes it. */
            if (file.bytes < file.total)
            {
                if (!file_tx.size)
                    *stage(&file_tx, CREDIT, file.channel, file.id, 1) = 0;
                file_tx.data[HEADER]++;
            }
        }
    }
    apl_unlock(irq);
    podlink_wake();
    return true;
}

static void apply_controls(void)
{
    if (close_pending)
        end_channel(close_reason, reply_close, true);
    if (cancel_pending)
        stop_file(cancel_reason, reply_cancel);
    if (accept_open && channel)
    {
        accept_open = false;
        if (expired(open_deadline))
            end_channel(ROCK_PODLINK_TIMEOUT, true, true);
        else
            opened = opened_event = true;
    }
}

static bool commit(int irq)
{
    /* Closing flushes the staged file before we choose the commit point.
     * A cancel or reset during close still wins and leaves the target intact. */
    apl_unlock(irq);
    bool flushed = close(fd) == 0;
    fd = -1;
    irq = apl_lock();
    apply_controls();
    if (file.phase == STREAM && check_link())
    {
        if (!flushed)
            stop_file(ROCK_PODLINK_IO, true);
        else
        {
            file.phase = COMMITTING;
            apl_unlock(irq);
            bool renamed = rename(temp, file.path) == 0;
            irq = apl_lock();
            file.phase = STREAM;
            if (renamed)
                temp_owned = false;
            if (check_link() && channel == file.channel)
            {
                if (renamed)
                {
                    stage(&file_tx, DONE, file.channel, file.id, 0);
                    file.reason = ROCK_PODLINK_OK;
                    file.phase = SEND_DONE;
                    file_count = 0;
                    advance();
                }
                else
                    stop_file(ROCK_PODLINK_IO, true);
            }
            else
                stop_file(ROCK_PODLINK_LINK_LOST, false);
        }
    }
    apl_unlock(irq);
    podlink_wake();
    return true;
}

static bool handle_file(int irq)
{
    const struct frame *slot = &file_rx[file_head];
    memcpy(input.data, slot->data, slot->size);
    unsigned int len = slot->size - HEADER;
    const unsigned char *p = input.data + HEADER;
    file_head = (file_head + 1) % FILE_QUEUE;
    file_count--;

    unsigned char failure = ROCK_PODLINK_INVALID;
    switch (input.data[4])
    {
        case ACCEPT:
            if (file.phase != WAIT_ACCEPT || len != 4)
                break;
            if (get32(p) > ROCK_PODLINK_FILE_MAX)
            {
                failure = ROCK_PODLINK_LIMIT;
                break;
            }
            if (file.push && get32(p) != file.total)
                break;
            file.total = get32(p);
            file.phase = STREAM;
            if (!file.push)
                *stage(&file_tx, CREDIT, file.channel, file.id, 1) = WINDOW;
            goto valid;
        case CREDIT:
            if (!file.push || (file.phase != STREAM && file.phase != WAIT_DONE) || len != 1 ||
                !p[0] || p[0] > WINDOW || file.credits + p[0] > WINDOW)
                break;
            file.credits += p[0];
            goto valid;
        case CHUNK:
            if (file.push || file.phase != STREAM || len <= CHUNK_HEADER ||
                len > CHUNK_HEADER + ROCK_PODLINK_MESSAGE_MAX ||
                memcmp(p, "MSF1", 4) || get32(p + 4) != file.bytes ||
                len - CHUNK_HEADER > file.total - file.bytes)
                break;
            return write_chunk(irq, p + CHUNK_HEADER, len - CHUNK_HEADER);
        case FINISH:
            if (file.push || file.phase != STREAM || len != 8 ||
                file.bytes != file.total || get32(p) != file.total ||
                get32(p + 4) != file.crc)
                break;
            /* Drop unused replenishment. With all bytes written there is no
             * future chunk to credit. DONE gets the same reserved slot. */
            file_tx.size = 0;
            return commit(irq);
        case DONE:
            if (!file.push || file.phase != WAIT_DONE || len)
                break;
            stop_file(ROCK_PODLINK_OK, false);
            goto valid;
    }
    stop_file(failure, true);
valid:
    advance();
    apl_unlock(irq);
    podlink_wake();
    return true;
}

static bool send_chunk(int irq)
{
    unsigned int len = file.total - file.bytes;
    if (!len)
    {
        unsigned char *p = stage(&file_tx, FINISH, file.channel, file.id, 8);
        put32(p, file.total);
        put32(p + 4, file.crc);
        apl_unlock(irq);
        podlink_wake();
        return true;
    }
    if (!file.credits)
    {
        apl_unlock(irq);
        return false;
    }
    if (wire_max <= HEADER + CHUNK_HEADER)
    {
        stop_file(ROCK_PODLINK_LIMIT, true);
        apl_unlock(irq);
        podlink_wake();
        return true;
    }
    if (len > sizeof(chunk))
        len = sizeof(chunk);
    if (len > wire_max - HEADER - CHUNK_HEADER)
        len = wire_max - HEADER - CHUNK_HEADER;

    uint32_t crc = file.crc;
    apl_unlock(irq);
    bool read_ok = read(fd, chunk, len) == (ssize_t)len;
    if (read_ok)
        crc = update_crc(crc, chunk, len);
    irq = apl_lock();
    if (file.phase == STREAM && check_link())
    {
        if (!read_ok)
            stop_file(ROCK_PODLINK_IO, true);
        else
        {
            unsigned char *p = stage(&file_tx, CHUNK, file.channel,
                                     file.id, CHUNK_HEADER + len);
            memcpy(p, "MSF1", 4);
            put32(p + 4, file.bytes);
            memcpy(p + CHUNK_HEADER, chunk, len);
            file.bytes += len;
            file.crc = crc;
            file.credits--;
            advance();
            progress_event = true;
        }
    }
    apl_unlock(irq);
    podlink_wake();
    return true;
}

bool rock_podlink_has_deadline(void)
{
    int irq = apl_lock();
    bool pending = (channel && !opened) || file.phase != IDLE;
    apl_unlock(irq);
    return pending;
}

bool rock_podlink_work(void)
{
    int irq = apl_lock();
    check_link();
    bool control_pending = close_pending || cancel_pending;
    apply_controls();
    if (channel && !opened && expired(open_deadline))
        end_channel(ROCK_PODLINK_TIMEOUT, true, true);
    if (file.phase != IDLE && expired(file.deadline))
        stop_file(ROCK_PODLINK_TIMEOUT, true);

    if (file.phase == STOPPING)
    {
        apl_unlock(irq);
        cleanup();
        irq = apl_lock();
        file.phase = IDLE;
        complete_event = file.report;
        progress_event = false;
        apl_unlock(irq);
        podlink_wake();
        return true;
    }
    if (file.phase == PREPARE)
        return prepare(irq);
    if (file_count)
        return handle_file(irq);
    if (file.push && file.phase == STREAM && !file_tx.size)
        return send_chunk(irq);
    apl_unlock(irq);
    if (control_pending)
        podlink_wake();
    return false;
}
#endif
