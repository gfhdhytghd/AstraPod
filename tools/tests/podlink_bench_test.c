/* Real demo/session/serial with a RAM controller and scripted peer. */
#define PODLINK_SERVICE_TEST
#define main firmware_harness_main
#include "podlink_firmware_test.c"
#undef main
#undef podlink_open
#undef podlink_state
#undef podlink_max_message
#undef podlink_send
#undef podlink_close
#undef podlink_poll
#undef podlink_wait
void yield(void);
#include "../../apps-ipod/screens/system/podlink_screen.c"

static int stage, turns;
static unsigned int peer_limit;
static bool observed;
const char *rbversion = "offline-bench-regression";

void usb_podlink_disk(void) { assert(!"unexpected disk request"); }
void button_queue_post(long id, intptr_t data)
{
    assert(id == 0x20000000 && !data);
}

unsigned long core_available(void)
{
    return 0;
}
int battery_level(void)
{
    return 50;
}
static void draw_diagnostics(const struct podlink_diagnostics *status, void *data)
{
    (void)data;
    assert(status->state == podlink_state());
}
void splash(int ticks, const char *s)
{
    (void)ticks;
    (void)s;
    assert(0);
}
void yield(void)
{
    controller();
}
int default_event_handler(int event)
{
    (void)event;
    return 0;
}
int button_get(bool block)
{
    (void)block;
    controller();
    /* The resident worker pumps TX even when the screen has a pending reply. */
    podlink_rust_step();
    controller();
    ++current_tick;
    if (stage == 0)
    {
        lease = true;
        insert();
        stage = 1;
    }
    else if (stage == 1)
    {
        unsigned char offer[] = {1, 0, peer_limit & 255,
                                 (peer_limit >> 8) & 255, 0, 0};
        input_frame(PODLINK_OPEN, 1, offer, sizeof(offer));
        stage = 2;
    }
    else if (stage == 2)
    {
        assert(podlink_state() == PODLINK_CONNECTED);
        assert(podlink_max_message() == peer_limit);
        clear_output();
        char command[32];
        int length = snprintf(command, sizeof(command), "BENCH RX %u",
                              2 * peer_limit);
        input_frame(PODLINK_TEXT, 2, command, length);
        stage = 3;
    }
    else if (stage == 3)
    {
        assert(++turns < 100);
        if (goal != 2 * peer_limit || operation != ECHO ||
            reply_type || podlink_rust_debug(1) ||
            tx_count || tx_prepared || sent_size)
            return 0;
        assert(offset == 2 * peer_limit);
        struct podlink_parser check = {0};
        struct podlink_message m;
        unsigned int binaries = 0, bytes = 0;
        for (int i = 0; i < output_size; ++i)
            if (podlink_decode(&check, output[i], &m) == 1 &&
                m.type == PODLINK_BINARY)
            {
                assert(m.length == peer_limit);
                for (unsigned int j = 0; j < m.length; ++j)
                    assert(m.data[j] == (bytes + j) % 251);
                bytes += m.length;
                ++binaries;
            }
        assert(binaries == 2 && bytes == 2 * peer_limit);
        observed = true;
        stage = 4;
        return BUTTON_MENU;
    }
    return 0;
}

int agents_shortcut_get_button(int timeout)
{
    (void)timeout;
    return button_get(false);
}
void root_menu_agents_shortcut(void)
{
    assert(0);
}

void root_menu_wps_shortcut(void)
{
    root_menu_agents_shortcut();
}

int main(void)
{
    usb_serial_init();
    podlink_init();
    for (test_high_speed = 0; test_high_speed <= 1; ++test_high_speed)
        for (peer_limit = 4096; peer_limit <= 16384; peer_limit *= 4)
        {
            current_tick = stage = turns = 0;
            observed = false;
            podlink_screen_run(draw_diagnostics, NULL, NULL);
            assert(observed && podlink_state() == PODLINK_CLOSED && lease);
            remove_usb();
            lease = false;
            podlink_rust_step();
            printf("PASS: BENCH RX %u, negotiated %u, %s, %d turns\n",
                   2 * peer_limit, peer_limit,
                   test_high_speed ? "high speed" : "full speed", turns);
        }
    return 0;
}

void root_menu_cover_shortcut(void) { assert(!"unexpected covers shortcut"); }
