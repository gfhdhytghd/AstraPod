/* Foreground owner for the stock PDBox GUI and PDa DSP.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "plugin.h"
#include "usb.h"
#include "pdbox.h"
#include "pdbox-patch.h"
#include "PDa/src/s_stuff.h"

#define PDBOX_HEAP_BYTES (1024 * 1024)
#define PDBOX_DSP_STACK_BYTES (64 * 1024)
#define PDBOX_GUI_STACK_BYTES (64 * 1024)
#define PDBOX_PCM_FRAMES (ROCK_STREAM_BLOCK_BYTES / (2 * sizeof(int16_t)))
#define PDBOX_MESSAGES 16
#define PDBOX_DSP_MESSAGES_PER_TICK 4
#define PDBOX_TIME_UNITS (32.0 * 441000.0)

_Static_assert(PDBOX_PCM_FRAMES % DEFDACBLKSIZE == 0,
               "A stream block must contain whole PDa ticks");

char *filename;
uint64_t runningtime;
int sys_verbose;
int sys_noloadbang;
t_symbol *sys_libdir;
t_namelist *sys_openlist;
t_binbuf *inbinbuf;

extern t_time sys_time;
extern t_time sys_time_per_dsp_tick;
extern void sched_tick(t_time next);
extern void outlet_setstacklim(void);
extern void glob_evalfile(t_pd *, t_symbol *name, t_symbol *directory);
extern void glob_dsp(void *, t_symbol *, int, t_atom *);
extern t_sample *sys_soundout;

struct pdbox_mailbox {
    unsigned int head, tail;
    struct datagram slots[PDBOX_MESSAGES];
};

struct pdbox_mailbox gui_to_core, core_to_gui;

static long dsp_stack[PDBOX_DSP_STACK_BYTES / sizeof(long)];
static long gui_stack[PDBOX_GUI_STACK_BYTES / sizeof(long)];
static char path[MAX_PATH], directory[MAX_PATH], patch_name[MAX_PATH];
static char patch_bytes[PDBOX_PATCH_BYTES];
static size_t patch_size;
static bool patch_consumed;
static struct pdbox_patch patch;
static struct pd_widget widgets[PDBOX_WIDGETS], display[PDBOX_WIDGETS];
static volatile bool display_dirty;

static unsigned int rate;
static int16_t pcm[PDBOX_PCM_FRAMES * 2];
static volatile unsigned int pcm_frames;
static volatile bool stopping;

/* The two workers and the entry thread run on one cooperative core.
 * No transfer between a slot and its owner can yield. */
bool pdbox_send_datagram(struct pdbox_mailbox *box, int port, char *data,
                         size_t size)
{
    if (!size || size > MAX_DATAGRAM_SIZE ||
        port != (box == &gui_to_core ? PD_CORE_PORT : PD_GUI_PORT))
        return false;

    unsigned int next = (box->tail + 1) % PDBOX_MESSAGES;
    if (next == box->head)
        return false;

    struct datagram *slot = &box->slots[box->tail];
    memcpy(slot->data, data, size);
    slot->data[size] = '\0';
    slot->size = size;
    box->tail = next;
    return true;
}

bool pdbox_receive_datagram(struct pdbox_mailbox *box, int port,
                            struct datagram *out)
{
    if (box->head == box->tail ||
        port != (box == &gui_to_core ? PD_CORE_PORT : PD_GUI_PORT))
        return false;
    *out = box->slots[box->head];
    box->head = (box->head + 1) % PDBOX_MESSAGES;
    return true;
}

/* PDa may only evaluate the single already-validated input image. */
int pdbox_read_patch(t_binbuf *buffer, const char *name, const char *dir)
{
    if (patch_consumed || strcmp(name, patch_name) || strcmp(dir, directory))
        return 1;
    patch_consumed = true;
    binbuf_text(buffer, patch_bytes, patch_size);
    return 0;
}

void rockbox_open_audio(int sample_rate)
{
    (void)sample_rate;
    pcm_frames = 0;
}

void rockbox_close_audio(void)
{
    pcm_frames = 0;
}

static int16_t pdbox_sample(t_sample value)
{
    int sample = SCALE16(value);
    return MAX(-32768, MIN(32767, sample));
}

int rockbox_send_dacs(void)
{
    if (pcm_frames == PDBOX_PCM_FRAMES)
        return SENDDACS_NO;

    int16_t *output = pcm + 2 * pcm_frames;
    for (unsigned int i = 0; i < DEFDACBLKSIZE; i++)
    {
        *output++ = pdbox_sample(sys_soundout[i]);
        *output++ = pdbox_sample(sys_soundout[DEFDACBLKSIZE + i]);
    }
    memset(sys_soundout, 0, 2 * DEFDACBLKSIZE * sizeof(t_sample));
    pcm_frames += DEFDACBLKSIZE;
    return SENDDACS_YES;
}

void pdbox_request_stop(void)
{
    stopping = true;
}

static bool pdbox_stopping(void)
{
    return stopping || rb->rock_worker_stopping();
}

static void pdbox_dsp_worker(void *unused)
{
    (void)unused;
    int in[MAXAUDIOINDEV] = {0};
    int out[MAXAUDIOOUTDEV] = {0};
    int channels[MAXAUDIOOUTDEV] = {2};
    t_atom on;

    pd_init();
    sys_set_audio_api(API_ROCKBOX);
    sys_open_audio(0, in, 0, in, 1, out, 1, channels, rate, DEFAULTADVANCE, 1);
    sys_time = 0;
    sys_time_per_dsp_tick = PDBOX_TIME_UNITS * sys_schedblocksize / rate;
    inbinbuf = binbuf_new();
    sys_libdir = gensym(directory);
    outlet_setstacklim();
    glob_evalfile(NULL, gensym(patch_name), sys_libdir);
    SETFLOAT(&on, 1);
    glob_dsp(NULL, NULL, 1, &on);

    while (!pdbox_stopping())
    {
        struct datagram message;
        for (unsigned int i = 0; i < PDBOX_DSP_MESSAGES_PER_TICK &&
             RECEIVE_TO_CORE(&message); i++)
            rockbox_receive_callback(&message);

        /* Each batch produces at most one PCM block. */
        while (sys_send_dacs() != SENDDACS_NO)
            sched_tick(sys_time + sys_time_per_dsp_tick);
        runningtime = sys_time * (1000.0 / PDBOX_TIME_UNITS);
        rb->sleep(1);
    }
    sys_close_audio();
}

static void pdbox_gui_worker(void *unused)
{
    (void)unused;
    while (!pdbox_stopping())
    {
        struct datagram message;
        bool dirty = pd_gui_apply_timeouts(widgets, patch.count);
        for (unsigned int i = 0; i < PDBOX_MESSAGES &&
             RECEIVE_FROM_CORE(&message); i++)
        {
            pd_gui_parse_message(&message, widgets, patch.count);
            dirty = true;
        }
        if (dirty)
            display_dirty = true;
        rb->sleep(1);
    }
}

static void pdbox_draw(void)
{
    memcpy(display, widgets, patch.count * sizeof(*widgets));
    display_dirty = false;
    pd_gui_draw(display, patch.count);
}

static void pdbox_gui_setup(void)
{
    static const enum pd_widget_id renderer[] = {
        [PDBOX_TEXT] = PD_TEXT, [PDBOX_NUMBER] = PD_NUMBER,
        [PDBOX_BANG] = PD_BANG, [PDBOX_HSL] = PD_HSLIDER,
        [PDBOX_VSL] = PD_VSLIDER, [PDBOX_HRADIO] = PD_HRADIO,
        [PDBOX_VRADIO] = PD_VRADIO,
    };
    for (unsigned int i = 0; i < patch.count; i++)
    {
        const struct pdbox_widget_spec *spec = &patch.widgets[i];
        widgets[i] = (struct pd_widget){
            .id = renderer[spec->kind],
            .x = spec->x * LCD_WIDTH / 160,
            .y = spec->y * LCD_WIDTH / 160,
            .w = spec->w * LCD_WIDTH / 160,
            .h = spec->h * LCD_WIDTH / 160,
            .min = spec->low, .max = spec->high,
            .value = MAX(spec->low, MIN(0, spec->high)),
        };
        strcpy(widgets[i].name, spec->name);
    }
    pd_gui_init();
    pdbox_draw();
}

static bool pdbox_load(const char *selected)
{
    size_t length = strlen(selected);
    const char *slash = strrchr(selected, '/');
    if (!slash || length >= sizeof(path) || length < 4 ||
        strcmp(selected + length - 3, ".pd"))
        return false;

    strcpy(path, selected);
    strcpy(patch_name, slash + 1);
    size_t parent = MAX(1, slash - selected);
    memcpy(directory, selected, parent);
    directory[parent] = '\0';
    filename = path;

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    off_t size = rb->ffilesize(fd);
    bool ok = size > 0 && size <= PDBOX_PATCH_BYTES &&
              read(fd, patch_bytes, size) == size;
    if (close(fd) < 0 || !ok)
        return false;
    patch_size = size;
    return pdbox_validate_patch(patch_bytes, patch_size, &patch);
}

static enum plugin_status pdbox_status(enum plugin_status result)
{
    return rb->plugin_usb_taken() ? PLUGIN_USB_CONNECTED : result;
}

static enum plugin_status pdbox_run(void)
{
    bool priming = true;
    while (!stopping)
    {
        int pending = rb->rock_stream_pending();
        if (pending < 0)
            return pdbox_status(PLUGIN_OK);
        if (pcm_frames == PDBOX_PCM_FRAMES &&
            pending < ROCK_STREAM_QUEUE_SIZE)
        {
            if (!rb->rock_stream_submit(pcm, sizeof(pcm)))
                return pdbox_status(PLUGIN_OK);
            pcm_frames = 0;
            if (priming && pending + 1 == ROCK_STREAM_QUEUE_SIZE)
            {
                if (!rb->rock_stream_pause(false))
                    return pdbox_status(PLUGIN_OK);
                priming = false;
            }
        }

        rb->reset_poweroff_timer();
        long button = rb->button_get_w_tmo(1);
        if (rb->default_event_handler(button) == SYS_USB_CONNECTED)
            return PLUGIN_USB_CONNECTED;
        if (IS_SYSEVENT(button) || button == BUTTON_REDRAW)
            continue;
        if (button == PDPOD_QUIT || button == (BUTTON_MENU | BUTTON_REPEAT))
            return PLUGIN_OK;

#ifdef BUTTON_RC_VOL_UP
        if (button == BUTTON_RC_VOL_UP ||
            button == (BUTTON_RC_VOL_UP | BUTTON_REPEAT))
            rb->adjust_volume(1);
        if (button == BUTTON_RC_VOL_DOWN ||
            button == (BUTTON_RC_VOL_DOWN | BUTTON_REPEAT))
            rb->adjust_volume(-1);
#endif
        long keys = rb->button_status() &
                    (BUTTON_SELECT | BUTTON_MENU | BUTTON_PLAY |
                     BUTTON_LEFT | BUTTON_RIGHT);
        keys |= button & (BUTTON_SCROLL_BACK | BUTTON_SCROLL_FWD);
        bool change = pd_gui_parse_buttons(patch.count, keys);
        if (display_dirty || change)
            pdbox_draw();
    }
    return pdbox_status(PLUGIN_OK);
}

enum plugin_status plugin_start(const void *parameter)
{
    char choice[MAX_PATH];
    const char *selected = parameter;
    if (!selected)
    {
        if (rb->pick_file(choice, sizeof(choice), "/", "Choose PDBox patch",
                          ".pd", 0, false) <= 0)
            return pdbox_status(PLUGIN_OK);
        selected = choice;
    }
    if (!pdbox_load(selected))
    {
        if (!rb->plugin_usb_taken())
            rb->splash(2 * HZ, "Unsupported or incomplete PDBox patch");
        return pdbox_status(PLUGIN_ERROR);
    }

    size_t bytes;
    void *pool = rb->plugin_claim_scratch(PDBOX_HEAP_BYTES, PDBOX_HEAP_BYTES,
                                          &bytes);
    if (!pool || init_memory_pool(bytes, pool) == (size_t)-1)
        return pdbox_status(PLUGIN_ERROR);

    rate = rb->rock_stream_open();
    if (!rate || !rb->rock_stream_pause(true))
    {
        rb->rock_stream_close();
        destroy_memory_pool(pool);
        if (!rb->plugin_usb_taken())
            rb->splash(HZ, "Stop other audio before opening PDBox");
        return pdbox_status(PLUGIN_OK);
    }

    if (rb->global_settings->backlight_timeout > 0)
        rb->backlight_set_timeout(0);
#if CONFIG_CHARGING
    if (rb->global_settings->backlight_timeout_plugged > 0)
        rb->backlight_set_timeout_plugged(0);
#endif
    pdbox_gui_setup();

#if defined(CPU_BOOST_LOGGING)
    cpu_boost_(true, __FILE__, __LINE__);
#elif defined(HAVE_ADJUSTABLE_CPU_FREQ)
    cpu_boost(true);
#endif
    unsigned int dsp = rb->rock_worker_start(pdbox_dsp_worker, NULL,
                                              dsp_stack, sizeof(dsp_stack));
    unsigned int gui = dsp ? rb->rock_worker_start(pdbox_gui_worker, NULL,
                              gui_stack, sizeof(gui_stack)) : 0;
    enum plugin_status result = gui ? pdbox_run() : PLUGIN_ERROR;
    pdbox_request_stop();
    rb->rock_stream_close();
    /* USB takeover joins the leases before acknowledging storage. */
    if (!rb->plugin_usb_taken())
    {
        if (gui)
            rb->rock_worker_join(gui);
        if (dsp)
            rb->rock_worker_join(dsp);
    }
#if defined(CPU_BOOST_LOGGING)
    cpu_boost_(false, __FILE__, __LINE__);
#elif defined(HAVE_ADJUSTABLE_CPU_FREQ)
    cpu_boost(false);
#endif
    destroy_memory_pool(pool);
    return pdbox_status(result);
}
