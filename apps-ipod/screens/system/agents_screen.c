/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * System > Agents: the herdr agent dashboard and remote, over PodLink.
 *
 * The resident PodLink session supplies the dashboard and short replies.
 * T3 uses a choice-only remote. Legacy pane sessions also expose their
 * existing computer key bridge. List rows use the native theme, including
 * optional %Lv right-hand cells and the %LD preview.
 ****************************************************************************/

#include "crumbs.h"
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "kernel.h"
#include "backlight.h"
#include "audio.h"
#include "metadata.h"
#include "core_alloc.h"
#include "button.h"
#include "font.h"
#include "rbunicode.h"
#include "diacritic.h"
#include "podlink.h"
#include "usb.h"
#include "input/action.h"
#include "input/agents_shortcut.h"
#ifdef IPOD_ACCESSORY_PROTOCOL
#include "iap/agents_volume.h"
#endif
#include "draw/color.h"
#include "draw/viewport.h"
#include "draw/screen_access.h"
#include "draw/text_wrap.h"
#include "draw/round_rect.h"
#include "widgets/list.h"
#include "ui/frame.h"
#include "ui/native_theme_runtime.h"
#include "ui/scene.h"
#include "ui/sheet.h"
#include "ui/statusbar.h"
#include "widgets/splash.h"
#include "widgets/dialog.h"
#include "settings/settings.h"
#include "system/shutdown.h"
#include "system/activity.h"
#include "speech/talk.h"
#include "herdremote_model.h"
#include "detail_layout.h"
#include "agents_screen.h"
#include "agents_background.h"
#include "screens/playback/cider.h"
#include "screens/playback/cider_wps.h"
#include "screens/playback/quick_screen.h"

enum view { VIEW_SPACES, VIEW_AGENTS, VIEW_DETAIL, VIEW_QUESTION, VIEW_USAGE,
            VIEW_ALERT, VIEW_INPUT, VIEW_ACTIONS, VIEW_CONFIRM };

enum row_kind {
    ROW_STATUS,     /* connection trouble; not selectable */
    ROW_HINT,       /* what to do about it; not selectable */
    ROW_ALERT,      /* a question is waiting */
    ROW_SPACE,
    ROW_T3_HEADER,
    ROW_AGENT,
    ROW_TEXT,       /* a wrapped line of the question */
    ROW_OPTION,
    ROW_SUBMIT,     /* a multi-select question's Submit */
    ROW_EMPTY,
    ROW_ACTION,
    ROW_FILTER,
    ROW_USAGE,
    ROW_DISMISS,
    ROW_ANSWER_PENDING,
    ROW_VOICE,
    ROW_OPEN_COMPUTER,
    ROW_RETRY,
};

/* id is the workspace or pane a row stands for, copied when the rows are
 * built: the computer reorders the lists, so an index alone does not say
 * which item the selection was on once the model has changed */
struct row {
    unsigned char kind;
    unsigned char index;
    char id[16];
};

#define MAX_ROWS   64
#define TEXT_ROWS  24
#define KEYBAR     20
#define OUTBOX     10
#define SILENCE    (6 * HZ)

static struct hr_model model;
static struct row rows[MAX_ROWS];
static int nrows;
static struct dialog_text_line text_rows[TEXT_ROWS];
/* The question's lines, the terminal's wrapping undone, which text_rows
 * point into */
static char question_text[HR_MAX_LINES * 124 + HR_MAX_LINES];

static enum view view;
static struct ui_scene page_scene;
static const char spaces_page, usage_page, detail_page, tab_page[HR_MAX_TABS];
static char space_id[16];       /* the workspace the agents view shows */
static char space_tab[16];      /* its tab; "" is the first */
static char detail_pane[16];    /* the pane the detail view shows */
static struct hr_agent detail_agent;    /* that agent, as last listed */
static bool detail_ready;       /* its fp has succeeded, so keys may go */
static long actions_until;
static bool modal_open;
static int agents_body_font = FONT_UI;
static int action_choice;
static struct hr_action confirm_action;
static int action_row;
static bool confirm_selected;
static int alert_choice;
static long alert_at;
static bool alert_direction_ready;
static bool question_resolving;
static long question_next_until;
static bool select_down, select_held, select_remote;
static long select_at;
static enum view select_view;
static long focus_at;
static long detail_wheel_at;
static bool filter_loading, received_projects;
static bool detail_follow, detail_new;
static volatile bool agents_foreground;
static bool agents_visited, shortcut_entry;
static char action_req[8];
static long action_until;
static long detail_at;          /* when the last D came: its age counts on */
static int detail_shown_age;    /* the minutes the header shows, -1 none */
/* The detail's task line scrolls when it is wider than the view: it rests
 * at either end, then moves TASK_STEP pixels every ANIM_TICKS */
#define TASK_REST   (3 * HZ / 2)
#define TASK_STEP   2
static char task_text[HR_DETAIL_TEXT];  /* the task line as drawn */
static int task_w;              /* its width in pixels */
static int task_x;              /* its scroll, pixels from the left */
static int task_y;              /* its line's top in the view */
static long task_next;          /* when it moves next */
static bool task_at_end;        /* it rests at its end, to go back */
static char question_id[12], question_saved_detail[16];
static char question_agent[44], question_tab[44], question_line[124];
static int question_choice;
static bool question_custom, question_closing;
static char question_pane[16];  /* the pane the question view answers */
static enum view question_from; /* the view to go back to, on which row */
static struct row question_from_row;
static struct hr_popped popped; /* the questions that have popped up */

static char outbox[OUTBOX][80];
static int nout;
static unsigned int next_req = 1;
static char pending_req[8];     /* the answer awaiting its result */
static bool pending_answer;
static long pending_until;
/* Focus follows the wheel: the selected workspace (fw) or pane (fp) is
 * focused on the computer as soon as the wheel moves. One fw, fp or ft is
 * out at a time, and says nothing unless it fails; the wheel moving
 * meanwhile leaves only its latest row wanted, sent when the result comes.
 * A row the last request already focused is not sent again. */
#define FOCUS_REPLY (8 * HZ)
static char focus_want[16];     /* the row to focus next; "" for none */
static char focus_done[16];     /* the row the last fw or fp was for */
static char focus_req[8];       /* the fw, fp or ft awaiting its result */
static long focus_until;
static bool focus_for_detail;   /* that request is the detail view's fp */
static long last_message;
static bool was_connected, link_seen;
_Static_assert(sizeof(struct hr_model) <= 32 * 1024, "Agents model cache budget");
static bool cache_valid;
static uint32_t cache_generation;
static struct row cache_row;
static int cache_top;


/* The usage view asks again this often while it is open */
#define USAGE_EVERY (20 * HZ)
static long usage_next;         /* when to ask next */
static long usage_at;           /* when the shown figures came */
static unsigned int usage_serial;
#ifdef IPOD_ACCESSORY_PROTOCOL
static bool volume_paging;
#endif
static bool native_play_pending;
static unsigned long native_play_at;
_Static_assert(sizeof(struct detail_layout) <= 6400, "Detail layout cache budget");
static struct detail_layout detail_layout;
static int detail_body_y, detail_body_avail;
static int usage_scroll, detail_scroll, detail_scroll_max, detail_page_lines;
static struct row usage_from;   /* the spaces row to go back to */

/* Hold to talk: the band at the foot of the list, while it is wanted */
#define TALK_REPLY  (8 * HZ)    /* how long vd and vu wait for their R */
#define TALK_BAND   30          /* the band's height in pixels */
static bool talk_remote, talk_discarded, talk_select, talk_toggle;
static bool talk_held;          /* vd sent, with vu/vc still to come */
static long talk_since;         /* when the recording began, for its count */
static char talk_req[8];        /* the vd/vu/vc awaiting its result */
static char t3_talk_start[8];   /* vd may return after the button is up */
static bool talk_t3;
static bool talk_req_up;        /* that request is the vu */
static long talk_until;         /* when the request or the note expires */
static char talk_note[64];      /* the last result */
static bool talk_failed;        /* the note is an error */
static bool talk_shown;         /* the band is on screen */
/* One k is in flight at a time; the taps that come meanwhile wait as runs
 * with a count, and the next run goes when its R comes */
#define KEY_LOST    (2 * HZ)    /* a k unanswered this long is forgotten */
static char key_req[8];         /* the k in flight */
static long key_sent;           /* when it went */
static struct hr_keys key_runs; /* the taps waiting */
/* Wheel steps this close together are fast and jump words */
#define WHEEL_FAST  (HZ / 20)
static struct hr_wheel wheel;

static void talk_result(const char *text, bool failed);
static bool take_talk_result(void);
bool agents_fruit_nav(void)
{
    return ui_wheel_navigation();
}

static bool is_t3(const char *id);

static bool t3_mode(void)
{
    return model.mode == HR_MODE_T3;
}

static bool is_t3(const char *id)
{
    return !strncmp(id, "t3", 2);
}

static bool usage_legacy_keys(void)
{
    /* A legacy heartbeat can precede its W. Desktop keys are enabled only
     * when a legacy workspace actually identifies the current source. */
    return !agents_fruit_nav() && model.mode == HR_MODE_HERDR;
}


static bool take_action_result(void)
{
    if (!action_req[0] || strcmp(model.result_req, action_req))
        return false;
    model.result_new = false;
    action_req[0] = '\0';
    talk_discarded = false;
    talk_result(model.result_text, !model.result_ok);
    return true;
}

static void background_link(const struct podlink_message *m)
{
    if (m->type == PODLINK_CLOSE)
    {
        agents_background_disconnected();
        cache_valid = false;
        return;
    }
    if (!cache_valid || m->type != PODLINK_TEXT || !m->length ||
        m->data[0] == 'D')
        return;
    enum hr_mode mode = model.mode;
    int type = hr_model_apply(&model, (const char *)m->data, m->length);
    if (type == 'M' && mode != model.mode)
        agents_visited = false;
    if (type)
    {
        last_message = current_tick;
        agents_background_update(&model, type, false);
    }
    if (type == 'U') usage_at = current_tick;
    if (type == 'V' && talk_discarded && !talk_failed)
        model.voice = 0;
    if (type == 'R')
    {
        if (take_talk_result())
            return;
        else if (focus_req[0] && !strcmp(model.result_req, focus_req))
        {
            focus_req[0] = '\0';
            if (focus_for_detail) detail_ready = model.result_ok;
            focus_for_detail = false;
            if (!model.result_ok) talk_result(model.result_text, true);
        }
        else if (key_req[0] && !strcmp(model.result_req, key_req))
            key_req[0] = '\0';
        else if (take_action_result())
            return;
        else if (pending_req[0] && !strcmp(model.result_req, pending_req))
        {
            pending_req[0] = '\0';
            pending_answer = false;
            if (!model.result_ok) talk_result(model.result_text, true);
        }
        model.result_new = false;
    }
}


/* The resident registry owns incoming frames when no foreground client
 * is attached. Its reset and receive callbacks run serially on that worker. */
bool agents_background_foreground(void)
{
    return agents_foreground;
}

void agents_background_reset_model(void)
{
    if (agents_foreground)
        return;
    cache_valid = false;
    was_connected = false;
}

bool agents_background_receive(const struct podlink_message *m)
{
    if (agents_foreground || m->type != PODLINK_TEXT || !m->length ||
        !strchr("HMWSTQXE", m->data[0]))
        return false;
    if (!cache_valid)
    {
        hr_model_reset(&model);
        cache_valid = true;
        agents_visited = false;
        was_connected = true;
    }
    background_link(m);
    return true;
}

/* A free outbox slot, or NULL while the link is wedged (the user can press
 * again) */
static char *outbox_slot(void)
{
    return nout < OUTBOX - (talk_held ? 1 : 0) ? outbox[nout++] : NULL;
}

static void take_req(char *req, size_t size)
{
    snprintf(req, size, "%u", next_req);
    next_req = next_req % 9999 + 1;
}

/* Queue a command that the daemon answers with R: "<name>\t<req>\t<args>" */
static bool queue_request(const char *name, const char *args)
{
    char *slot = outbox_slot();
    if (!slot)
        return false;
    take_req(pending_req, sizeof pending_req);
    /* An answer waits up to five seconds for the pane to change */
    pending_until = current_tick + 8 * HZ;
    snprintf(slot, sizeof outbox[0], "%s\t%s\t%s", name, pending_req, args);
    return true;
}

/* Queue fw, fp or ft, whose success is silent */
static bool queue_focus(const char *name, const char *args, bool for_detail)
{
    char *slot = outbox_slot();
    if (!slot)
        return false;
    take_req(focus_req, sizeof focus_req);
    focus_until = current_tick + FOCUS_REPLY;
    focus_for_detail = for_detail;
    snprintf(slot, sizeof outbox[0], "%s\t%s\t%s", name, focus_req, args);
    return true;
}

static void ask_usage(void)
{
    char *slot = outbox_slot();
    if (slot)
        strcpy(slot, "us");
    usage_next = current_tick + USAGE_EVERY;
}

/* Ask for the tabs and agents of one workspace tab; "" for the first tab,
 * and an empty ws for none */
static void subscribe(const char *ws, const char *tab)
{
    char *slot = outbox_slot();
    if (slot)
        snprintf(slot, sizeof outbox[0], "sub\t%s%s%s", ws[0] ? ws : "-",
                 tab[0] ? "\t" : "", tab);
}

/* Ask for one pane's last lines; "" for none */
static void subscribe_detail(const char *pane)
{
    char *slot = outbox_slot();
    if (slot)
        snprintf(slot, sizeof outbox[0], "det\t%s", pane[0] ? pane : "-");
}

static void queue_handshake(void)
{
    if (nout > OUTBOX - 4 - (talk_held ? 1 : 0))
        return;
    strcpy(outbox_slot(), "hr\t1");
    strcpy(outbox_slot(), "tc\t1");
    strcpy(outbox_slot(), "tc\t2");
    strcpy(outbox_slot(), "qx\t1");
    agents_background_handshake_sent();
}

static void flush_outbox(void)
{
    while (nout && podlink_state() == PODLINK_CONNECTED)
    {
        if (podlink_send(PODLINK_TEXT, outbox[0], strlen(outbox[0]))
                != PODLINK_SENT)
            return;
        memmove(outbox[0], outbox[1], (nout - 1) * sizeof outbox[0]);
        nout--;
    }
}

#define USAGE_VIEW_SIGNAL_TICKS MAX(1, HZ / 4)
static unsigned long usage_view_signal_at;
static int usage_view_signal_value;
static char usage_view_signal_pane[16];

/* Direct, expiring lease: never enqueue old view heartbeats behind commands. */
static void signal_usage_view(int active, bool force)
{
    /* Detail is armed only after its matching pane-focus reply succeeded. */
    if (active == 2 && (!detail_ready || focus_req[0] || is_t3(detail_pane)))
        active = 0;
    const char *pane = active == 2 ? detail_pane : "";
    if (!force && active == usage_view_signal_value &&
        !strcmp(pane, usage_view_signal_pane) &&
        TIME_BEFORE(current_tick, usage_view_signal_at))
        return;
    char text[32];
    if (active == 2) snprintf(text, sizeof text, "vw\tdetail\t%s", pane);
    else strcpy(text, active ? "vw\t1" : "vw\t0");
    /* Old daemons reject detail records. Revoke any earlier Usage lease
     * first so a mixed-version pair cannot retain it while detail opens. */
    if (active == 2 &&
        (usage_view_signal_value != 2 || strcmp(pane, usage_view_signal_pane)))
    {
        if (podlink_state() != PODLINK_CONNECTED ||
            podlink_send(PODLINK_TEXT, "vw\t0", 4) != PODLINK_SENT)
            return;
    }
    if (podlink_state() == PODLINK_CONNECTED &&
        podlink_send(PODLINK_TEXT, text, strlen(text)) == PODLINK_SENT)
    {
        usage_view_signal_value = active;
        strcpy(usage_view_signal_pane, pane);
        usage_view_signal_at = (unsigned long)current_tick + USAGE_VIEW_SIGNAL_TICKS;
    }
}

static const struct hr_question *current_question(void)
{
    if (is_t3(question_pane) && question_id[0])
    {
        for (int i = 0; i < model.nquestions; i++)
            if (!strcmp(model.questions[i].id, question_id) &&
                !strcmp(model.questions[i].pane, question_pane))
                return &model.questions[i];
        return NULL;
    }
    const struct hr_question *q = hr_model_question_for(&model, question_pane);
    return q && (!question_id[0] || !strcmp(q->id, question_id)) ? q : NULL;
}

static const struct hr_workspace *find_space(const char *id)
{
    for (int i = 0; i < model.nws; i++)
        if (!strcmp(model.ws[i].id, id))
            return &model.ws[i];
    return NULL;
}

static const struct hr_agent *find_agent(const char *pane)
{
    if (!model.agents_valid)
        return NULL;
    for (int i = 0; i < model.nagents; i++)
        if (!strcmp(model.agents[i].pane, pane))
            return &model.agents[i];
    return NULL;
}

static const char *status_word(char status)
{
    switch (status)
    {
        case 'w': return "working";
        case 'f': return "failed";
        case 'b': return "asking";
        case 'd': return "done";
        case 'i': return "idle";
        default:  return "";
    }
}

/* The most pressing state among a workspace's agents, as an icon letter,
 * and how many agents are in it */
static char space_state(const struct hr_workspace *w, char *buf, size_t size)
{
    if ((w->tab1 == 'f' || w->other == 'f') &&
        w->tab1 != 'b' && w->other != 'b')
    {
        snprintf(buf, size, "err");
        return 'f';
    }
    static const char order[] = "bwdi";
    const unsigned char count[] = { w->waiting, w->working, w->done,
                                    w->idle };
    for (int i = 0; i < 4; i++)
        if (count[i])
        {
            if (is_t3(w->id))
            {
                if (i < 2) snprintf(buf, size, "%u %s", count[i], i ? "run" : "ask");
                else snprintf(buf, size, "%s", i == 2 ? "done" : "");
            }
            else
                snprintf(buf, size, "%u %s", count[i], status_word(order[i]));
            return order[i];
        }
    buf[0] = '\0';
    return 'i';
}

static char agent_state(const struct hr_agent *a)
{
    return a->question ? 'b' : a->status;
}

/* The status dot a theme's %Ls shows. Asking and working in tab 1 are
 * the two that move; the rest are herdr's own dots and colours. */
enum dot {
    DOT_NONE,
    DOT_ASKING,         /* red, blinking */
    DOT_WORKING,        /* green, breathing */
    DOT_FAILED,
    DOT_FAILED_ELSEWHERE,
    DOT_ASKING_ELSEWHERE,   /* in another tab: red, a … badge */
    DOT_WORKING_ELSEWHERE,  /* in another tab: green, a … badge */
    DOT_DONE,           /* herdr's teal dot */
    DOT_IDLE,           /* herdr's green ring */
    DOT_UNKNOWN,        /* herdr's small grey dot */
};

/* What is left after asking and working, in herdr's order: done, idle,
 * unknown */
static enum dot rest_dot(char a, char b)
{
    if (a == 'd' || b == 'd')
        return DOT_DONE;
    if (a == 'i' || b == 'i')
        return DOT_IDLE;
    return DOT_UNKNOWN;
}

static enum dot row_dot(int item)
{
    if (item < 0 || item >= nrows)
        return DOT_NONE;
    const struct row *r = &rows[item];
    if (r->kind == ROW_SPACE)
    {
        const struct hr_workspace *w = &model.ws[r->index];
        if (w->tab1 == 'b')
            return DOT_ASKING;
        if (w->other == 'b')
            return DOT_ASKING_ELSEWHERE;
        if (w->tab1 == 'f')
            return DOT_FAILED;
        if (w->other == 'f')
            return DOT_FAILED_ELSEWHERE;
        if (w->tab1 == 'w')
            return DOT_WORKING;
        if (w->other == 'w')
            return DOT_WORKING_ELSEWHERE;
        return rest_dot(w->tab1, w->other);
    }
    if (r->kind == ROW_AGENT)
    {
        char state = agent_state(&model.agents[r->index]);
        return state == 'b' ? DOT_ASKING : state == 'w' ? DOT_WORKING :
               state == 'f' ? DOT_FAILED :
               rest_dot(state, state);
    }
    return r->kind == ROW_ALERT ? DOT_ASKING : DOT_NONE;
}

/* catppuccin-latte, the palette herdr is set to on the computer */
#define DOT_CORAL   LCD_RGBPACK(224, 79, 51)
#define DOT_RED     LCD_RGBPACK(210, 15, 57)
#define DOT_GREEN   LCD_RGBPACK(64, 160, 43)
#define DOT_TEAL    LCD_RGBPACK(23, 146, 153)
#define DOT_GREY    LCD_RGBPACK(156, 160, 176)
#define DOT_HALO    LCD_RGBPACK(255, 255, 255)

/* One frame in ANIM_STEPS of the breathing: (1 - cos) / 2 from 40% up */
#define ANIM_STEPS 24
#define ANIM_TICKS (HZ / 10)
static const unsigned char breath[ANIM_STEPS] = {
    102, 105, 112, 124, 140, 159, 178, 198, 217, 233, 245, 252,
    255, 252, 245, 233, 217, 198, 178, 159, 140, 124, 112, 105,
};

/* How strongly to draw a dot now, 0 to 255 */
static int dot_strength(enum dot dot)
{
    long frame = current_tick / ANIM_TICKS;
    if (dot == DOT_WORKING || dot == DOT_WORKING_ELSEWHERE)
        return breath[frame % ANIM_STEPS];
    if (dot == DOT_ASKING || dot == DOT_ASKING_ELSEWHERE)
        return frame % 10 < 6 ? 255 : 0;     /* 0.6 s on, 0.4 s off */
    return 255;
}

/* Each dot is two coverage masks in the format alpha_bitmap_part() reads:
 * 4 bits a pixel, two to a byte, 0 opaque and 15 clear. The halo is a light
 * edge round the shape, so a dot stands out on any cover; the fill is the
 * shape in the state's colour. Both are worked out once per cell size from
 * 8x8 samples a pixel. Lengths are sixteenths of a pixel at a 24-pixel cell
 * and scale with it. */
#define DOT_MAX 32
#define DOT_BYTES ((DOT_MAX * DOT_MAX + 1) / 2)
enum { SHAPE_DISC, SHAPE_RING, SHAPE_SMALL, SHAPE_BADGE, SHAPES };
static unsigned char dot_masks[SHAPES][2][DOT_BYTES];
static unsigned char dot_faded[DOT_BYTES];
static int dot_masks_size;

/* Whether the point (x, y) from the cell's centre is inside one layer */
static bool dot_inside(int shape, bool halo, int x, int y, int size)
{
#define L(sixteenths) ((sixteenths) * size / 24)
#define IN(cx, cy, r) (((x) - (cx)) * ((x) - (cx)) + \
                       ((y) - (cy)) * ((y) - (cy)) <= (r) * (r))
    switch (shape)
    {
        case SHAPE_DISC:
            return halo ? IN(0, 0, L(108)) && !IN(0, 0, L(88))
                        : IN(0, 0, L(88));
        case SHAPE_RING:
            return halo ? IN(0, 0, L(108)) && !IN(0, 0, L(88))
                        : IN(0, 0, L(88)) && !IN(0, 0, L(56));
        case SHAPE_SMALL:
            return halo ? IN(0, 0, L(64)) && !IN(0, 0, L(44))
                        : IN(0, 0, L(44));
        default:
        {
            /* A disc up and left, and below right of it a light badge
             * holding three dots in the disc's colour: "busy, elsewhere" */
            int d = L(-28), by = L(104), bx = L(80);
            int run = L(56), br = L(44);
            int ex = x < bx - run ? bx - run : x > bx + run ? bx + run : x;
            bool badge = (x - ex) * (x - ex) + (y - by) * (y - by) <=
                         br * br;
            if (halo)
                return badge || (IN(d, d, L(108)) && !IN(d, d, L(88)));
            if (badge)
                return IN(bx - L(48), by, L(16)) || IN(bx, by, L(16)) ||
                       IN(bx + L(48), by, L(16));
            return IN(d, d, L(88));
        }
    }
#undef IN
#undef L
}

static void dot_build(int size)
{
    for (int shape = 0; shape < SHAPES; shape++)
        for (int halo = 0; halo < 2; halo++)
        {
            unsigned char *m = dot_masks[shape][halo];
            memset(m, 0xff, DOT_BYTES);
            for (int y = 0; y < size; y++)
                for (int x = 0; x < size; x++)
                {
                    int n = 0;
                    for (int sy = 0; sy < 8; sy++)
                        for (int sx = 0; sx < 8; sx++)
                            n += dot_inside(shape, halo,
                                            x * 16 + sx * 2 + 1 - size * 8,
                                            y * 16 + sy * 2 + 1 - size * 8,
                                            size);
                    int i = y * size + x;
                    unsigned a = 15 - (n * 15 + 32) / 64;
                    m[i / 2] = i & 1 ? (m[i / 2] & 0x0f) | (a << 4)
                                     : (m[i / 2] & 0xf0) | a;
                }
        }
    dot_masks_size = size;
}

static int dot_shape(enum dot dot)
{
    switch (dot)
    {
        case DOT_IDLE:              return SHAPE_RING;
        case DOT_UNKNOWN:           return SHAPE_SMALL;
        case DOT_FAILED_ELSEWHERE:
        case DOT_ASKING_ELSEWHERE:
        case DOT_WORKING_ELSEWHERE: return SHAPE_BADGE;
        default:                    return SHAPE_DISC;
    }
}

static unsigned dot_colour(enum dot dot)
{
    switch (dot)
    {
        case DOT_FAILED:
        case DOT_FAILED_ELSEWHERE: return DOT_CORAL;
        case DOT_ASKING:
        case DOT_ASKING_ELSEWHERE:  return DOT_RED;
        case DOT_DONE:              return DOT_TEAL;
        case DOT_UNKNOWN:           return DOT_GREY;
        default:                    return DOT_GREEN;
    }
}

/* The list restores the cell before both full and status-only draws. Blend
 * the halo and fill into those pixels, including the selection highlight. */
static bool draw_dot(int item, void *data, struct screen *display,
                     struct viewport *vp)
{
    (void)data;
    enum dot dot = row_dot(item);
    if (dot == DOT_NONE)
        return false;
    int size = MIN(MIN(vp->width, vp->height), DOT_MAX);
    int strength = dot_strength(dot);
    if (!strength)
        return true;
    if (size != dot_masks_size)
        dot_build(size);
    int shape = dot_shape(dot);
    const unsigned char *fill = dot_masks[shape][0];
    if (strength < 255)
    {
        /* A breath: the fill alone fades, towards the backdrop */
        for (int i = 0; i < (size * size + 1) / 2; i++)
        {
            int lo = 15 - (15 - (fill[i] & 15)) * strength / 255;
            int hi = 15 - (15 - (fill[i] >> 4)) * strength / 255;
            dot_faded[i] = lo | hi << 4;
        }
        fill = dot_faded;
    }
    int ox = (vp->width - size) / 2, oy = (vp->height - size) / 2;
    display->set_drawmode(DRMODE_FG);
    display->set_foreground(DOT_HALO);
    display->alpha_bitmap_part(dot_masks[shape][1], 0, 0, size,
                               ox, oy, size, size);
    display->set_foreground(dot_colour(dot));
    display->alpha_bitmap_part(fill, 0, 0, size, ox, oy, size, size);
    if ((dot == DOT_FAILED || dot == DOT_FAILED_ELSEWHERE) && size >= 12)
    {
        int x = ox + size / 2, y = oy + size / 2;
        display->set_foreground(DOT_HALO);
        display->vline(x, y - 3, y);
        display->drawpixel(x, y + 2);
    }
    return true;
}

/* Whether a dot in view moves, so the screen has frames to draw */
static bool animating(struct gui_synclist *list)
{
    int top = list->start_item[SCREEN_MAIN];
    int lines = gui_synclist_get_nb_lines(list, SCREEN_MAIN);
    for (int i = top; i < top + lines && i < nrows; i++)
    {
        enum dot dot = row_dot(i);
        if (dot == DOT_ASKING || dot == DOT_WORKING ||
            dot == DOT_ASKING_ELSEWHERE || dot == DOT_WORKING_ELSEWHERE)
            return true;
    }
    return false;
}

static const char *agent_name(const struct hr_agent *a)
{
    return a->name[0] ? a->name : a->title[0] ? a->title : a->kind;
}

/* An option's label without its tick box, and the detail after its " — ",
 * which is NULL when there is none */
static const char *option_parts(const struct hr_option *o, bool *ticked,
                                char *label, size_t size)
{
    const char *text = o->label;
    *ticked = !strncmp(text, "[x] ", 4);
    if (*ticked || !strncmp(text, "[ ] ", 4))
        text += 4;
    snprintf(label, size, "%s", text);
    char *dash = strstr(label, " — ");
    if (!dash)
        return NULL;
    *dash = '\0';
    return text + (dash - label) + strlen(" — ");
}

/* Scrim's cells for a row's text, with and without a right-hand side, and
 * for that side */
#define TEXT_CELL   250
#define SHORT_CELL  160
#define RIGHT_CELL  86
/* The title bar's cell while the link is held, 140 px, less what its bold
 * font adds to a width measured in the list's font: a title cut to this
 * does not scroll. The numeric battery display leaves a narrower cell. */
#define TITLE_CELL  124

static int text_px(const char *text)
{
    int w, h;
    font_getstringsize((const unsigned char *)text, &w, &h, FONT_UI);
    return w;
}

/* A detail goes on the right of its row when it fits there, and otherwise
 * stays in the row's text, which scrolls when selected */
static bool fits_right(const char *text)
{
    return text_px(text) <= RIGHT_CELL;
}

/* Cut text to width on a character boundary and end it with an ellipsis,
 * when it is wider. The selected row is left whole: it scrolls. */
static const char *fit(char *buf, size_t size, const char *text, int width)
{
    if (text != buf)
        snprintf(buf, size, "%s", text);
    if (text_px(buf) <= width)
        return buf;
    size_t len = strlen(buf);
    while (len > 0)
    {
        do
            len--;
        while (len > 0 && (buf[len] & 0xc0) == 0x80);
        if (len + 4 > size)
            continue;
        strcpy(buf + len, "…");
        if (text_px(buf) <= width)
            break;
    }
    return buf;
}

/* What is wrong with the link or the data, or NULL when nothing is */
static bool link_stale(void)
{
    return model.hello && TIME_AFTER(current_tick, last_message + SILENCE);
}

static bool t3_unavailable(void)
{
    return model.t3_health_known && !model.t3_ok;
}

static const char *trouble(char *buf, size_t size)
{
    switch (podlink_state())
    {
        case PODLINK_WAIT_USB:
            return link_seen ? "Computer disconnected" : "Connect USB to computer";
        case PODLINK_OPENING:
            return link_seen ? "Computer disconnected" : "Computer not connected";
        case PODLINK_CONNECTED: break;
        default: return "Computer disconnected";
    }
    if (!model.hello || model.mode == HR_MODE_UNKNOWN)
        return "Waiting for computer…";
    if (t3_unavailable())
        return "T3 Code not running";
    if (link_stale())
    {
        snprintf(buf, size, "No update for %ld s",
                 (current_tick - last_message) / HZ);
        return buf;
    }
    if (t3_mode() && !model.workspaces_seen)
        return "Connecting to T3…";
    if (model.tick_seen && !model.herdr_ok && model.mode == HR_MODE_HERDR)
        return "Computer service not running";
    return NULL;
}

static const char *trouble_hint(void)
{
    switch (podlink_state())
    {
        case PODLINK_WAIT_USB: return "Plug into your computer to use T3 remote";
        case PODLINK_OPENING: return "Open the AstraPod app on your computer";
        default: break;
    }
    if (!model.hello)
        return "Check the AstraPod app on your computer";
    if (t3_unavailable())
        return "Open T3 Code on your computer";
    if (model.tick_seen && !model.herdr_ok && !t3_mode())
        return "Check the AstraPod app on your computer";
    return NULL;
}

static bool mutating_ready(void)
{
    const char *error = podlink_state() != PODLINK_CONNECTED || !model.hello ?
        "Computer not connected" : link_stale() ? "Computer not responding" :
        t3_unavailable() ? "T3 Code not running" : NULL;
    if (error)
    {
        talk_result(error, true);
        return false;
    }
    return true;
}

static void add_row(int kind, int index)
{
    if (nrows < MAX_ROWS)
    {
        const char *id = kind == ROW_SPACE ? model.ws[index].id :
                         kind == ROW_AGENT ? model.agents[index].pane : "";
        rows[nrows].kind = kind;
        rows[nrows].index = index;
        snprintf(rows[nrows].id, sizeof rows[nrows].id, "%s", id);
        nrows++;
    }
}

static int text_width(struct gui_synclist *list)
{
    /* The skinned row's text is inset from the list viewport on both sides */
    return list->parent[SCREEN_MAIN]->width - 24;
}

/* A terminal wraps a long question across lines. A line that is long and
 * does not end a sentence runs on into the next; any other ends a
 * paragraph. */
static void join_question(const struct hr_question *q)
{
    size_t n = 0;
    question_text[0] = '\0';
    for (int i = 0; i < q->nlines; i++)
    {
        const char *prev = i ? q->lines[i - 1] : "";
        size_t len = strlen(prev);
        bool runs_on = len >= 60 && !strchr(".?!:", prev[len - 1]) &&
                       (len < 3 || (strcmp(prev + len - 3, "。") &&
                                    strcmp(prev + len - 3, "？") &&
                                    strcmp(prev + len - 3, "！") &&
                                    strcmp(prev + len - 3, "：")));
        n += snprintf(question_text + n, sizeof question_text - n, "%s%s",
                      !i ? "" : runs_on ? " " : "\n", q->lines[i]);
        if (n >= sizeof question_text)
            break;
    }
}

static void list_chrome(struct gui_synclist *list)
{
    list->empty_title = list->empty_hint = list->footer = NULL;
    gui_synclist_set_notes(list, view == VIEW_AGENTS && is_t3(space_id) &&
                          model.agents_valid && model.nagents > 0);
    if (view == VIEW_SPACES)
    {
        if (!model.nws && !model.nquestions)
        {
            static char status[48];
            if (!link_stale())
                list->empty_title = trouble(status, sizeof status);
            if (list->empty_title)
                list->empty_hint = trouble_hint();
            else if (model.hello && !link_stale())
            {
                list->empty_title = t3_mode() ? "No T3 projects" : "No projects";
                if (t3_mode())
                    list->empty_hint = "Open a project in T3 Code";
            }
        }
        if (t3_unavailable())
            list->footer = agents_fruit_nav() ? "SELECT Retry · MENU Exit" :
                           "SELECT Retry · LEFT Usage · MENU Exit";
        else
            list->footer = agents_fruit_nav() ? "◀ Usage · Hold SEL / PLAY Voice" :
                           "◀ Usage · Hold PLAY Voice";
    }
    else if (view == VIEW_AGENTS && is_t3(space_id) && model.agents_valid &&
             !model.nagents && !model.nquestions)
    {
        bool all = !strcmp(space_tab, "all");
        list->empty_title = all ? "No threads" : "No active threads";
        if (!all)
            list->empty_hint = "Press RIGHT for All";
    }
    else if (view == VIEW_QUESTION)
        list->footer = "SEL Choose · Wheel Scroll · MENU Later";
}

static bool preset_action(const struct hr_action *action)
{
    return !strncmp(action->id, "reply", 5) &&
           action->id[5] >= '1' && action->id[5] <= '7' && !action->id[6];
}

static bool confirm_action_required(const struct hr_action *action)
{
    return action->confirm || (is_t3(detail_pane) && !strcmp(action->id, "interrupt"));
}

static void build_rows(struct gui_synclist *list)
{
    char buf[48];
    list_chrome(list);
    nrows = 0;
    if (view == VIEW_USAGE || view == VIEW_DETAIL ||
        view == VIEW_ALERT || view == VIEW_INPUT)
        return;
    if (view == VIEW_ACTIONS || view == VIEW_CONFIRM)
    {
        bool t3 = is_t3(detail_pane);
        if (hr_model_question_for(&model, detail_pane))
            add_row(ROW_ANSWER_PENDING, 0);
        if (agents_fruit_nav())
            add_row(ROW_VOICE, 0);
        if (model.actions_valid)
        {
            if (!t3)
            {
                for (int i = 0; i < model.nactions; i++)
                    add_row(ROW_ACTION, i);
            }
            else
            {
                for (int i = 0; i < model.nactions; i++)
                    if (!strcmp(model.actions[i].id, "interrupt") &&
                        (agent_state(&detail_agent) == 'w' ||
                         agent_state(&detail_agent) == 'b'))
                        add_row(ROW_ACTION, i);
                add_row(ROW_OPEN_COMPUTER, 0);
                for (int i = 0; i < model.nactions; i++)
                    if (preset_action(&model.actions[i]))
                        add_row(ROW_ACTION, i);
            }
        }
        if (!nrows)
            add_row(ROW_EMPTY, 0);
        return;
    }
    if (view != VIEW_QUESTION)
    {
        if (!list->empty_title && trouble(buf, sizeof buf))
            add_row(ROW_STATUS, 0);
        if (!list->empty_title && trouble_hint())
            add_row(ROW_HINT, 0);
        if (model.nquestions)
            add_row(ROW_ALERT, 0);
    }
    if (view == VIEW_SPACES)
    {
        bool legacy = false;
        for (int i = 0; i < model.nws; i++)
            if (!is_t3(model.ws[i].id))
            {
                add_row(ROW_SPACE, i);
                legacy = true;
            }
        if (legacy)
            for (int i = 0; i < model.nws; i++)
                if (is_t3(model.ws[i].id))
                {
                    add_row(ROW_T3_HEADER, 0);
                    break;
                }
        static const char priority[] = "bfwdi-";
        for (const char *state = priority; *state; state++)
            for (int i = 0; i < model.nws; i++)
            {
                const struct hr_workspace *w = &model.ws[i];
                if (!is_t3(w->id))
                    continue;
                char best = '-';
                for (const char *p = priority; *p; p++)
                    if (w->tab1 == *p || w->other == *p)
                    {
                        best = *p;
                        break;
                    }
                if (best == *state)
                    add_row(ROW_SPACE, i);
            }
        if (t3_unavailable() && agents_fruit_nav())
            add_row(ROW_RETRY, 0);
        if (agents_fruit_nav())
            add_row(ROW_USAGE, 0);
    }
    else if (view == VIEW_AGENTS)
    {
        if (agents_fruit_nav() && is_t3(space_id))
            add_row(ROW_FILTER, 0);
        if (model.agents_valid && !strcmp(model.agents_ws, space_id))
            for (int i = 0; i < model.nagents; i++)
                add_row(ROW_AGENT, i);
        if (!list->empty_title && (!model.agents_valid ||
            strcmp(model.agents_ws, space_id) || model.nagents == 0))
            add_row(ROW_EMPTY, 0);
    }
    else
    {
        const struct hr_question *q = current_question();
        if (!q)
            return;
        join_question(q);
        int n = dialog_wrap_text(question_text, text_width(list), FONT_UI,
                                 text_rows, TEXT_ROWS);
        for (int i = 0; i < n; i++)
            add_row(ROW_TEXT, i);
        for (int i = 0; i < q->noptions; i++)
            if (!is_t3(q->id) || !q->options[i].custom)
                add_row(ROW_OPTION, i);
        if (q->multi)
            add_row(ROW_SUBMIT, 0);
        if (!strncmp(q->id, "t3q", 3))
            add_row(ROW_DISMISS, 0);
    }
}

static const char *row_text(int item, char *buf, size_t size)
{
    if (item < 0 || item >= nrows)
        return "";
    const struct row *r = &rows[item];
    const struct hr_question *q;
    switch (r->kind)
    {
        case ROW_STATUS:
        {
            const char *t = trouble(buf, size);
            return t ? t : "";
        }
        case ROW_HINT:
        {
            const char *t = trouble_hint();
            return t ? t : "";
        }
        case ROW_ALERT:
            q = &model.questions[0];
            if (model.nquestions == 1)
                return q->nlines ? q->lines[q->nlines - 1] : q->agent;
            snprintf(buf, size, "%d questions waiting", model.nquestions);
            return buf;
        case ROW_SPACE:
            return model.ws[r->index].label;
        case ROW_FILTER:
            return !strcmp(space_tab, "all") ? "Active   [All]" : "[Active]   All";
        case ROW_USAGE:
            return "Usage";
        case ROW_DISMISS:
            return "Dismiss";
        case ROW_ANSWER_PENDING:
            return "Answer pending request…";
        case ROW_VOICE:
            return "Voice reply";
        case ROW_OPEN_COMPUTER:
            return "Open on computer";
        case ROW_RETRY:
            return "Retry";
        case ROW_T3_HEADER:
            return "T3";
        case ROW_AGENT:
            return agent_name(&model.agents[r->index]);
        case ROW_TEXT:
        {
            int len = text_rows[r->index].len;
            if (len >= (int)size)
                len = size - 1;
            memcpy(buf, text_rows[r->index].str, len);
            buf[len] = '\0';
            return buf;
        }
        case ROW_OPTION:
        {
            bool ticked;
            q = current_question();
            if (!q)
                return "";
            const char *detail = option_parts(&q->options[r->index], &ticked,
                                              buf, size);
            if (detail && !fits_right(detail))
            {
                size_t n = strlen(buf);
                snprintf(buf + n, size - n, " — %s", detail);
            }
            return buf;
        }
        case ROW_SUBMIT:
            return "Submit";
        case ROW_ACTION:
            return model.actions[r->index].label;
        default:
            if (view == VIEW_SPACES)
                return t3_mode() ? "No T3 projects" : "No projects";
            if (view == VIEW_ACTIONS)
                return "No actions";
            if (is_t3(space_id))
                return !strcmp(space_tab, "all") ? "No threads" : "No active threads";
            return model.hello ? "No agents in this tab"
                               : "Waiting for data…";
    }
}

static const char *row_value(int item, void *data, char *buf, size_t size);
static bool selectable(int item);

/* A row's text, cut to its cell unless it is the selected row */
static const char *row_name(int item, void *data, char *buf, size_t size)
{
    struct gui_synclist *list = data;
    const char *text = row_text(item, buf, size);
    if (item < 0 || item >= nrows || rows[item].kind == ROW_TEXT ||
        (item == list->selected_item && selectable(item)))
        return text;
    char value[64];
    const char *v = row_value(item, data, value, sizeof value);
    return fit(buf, size, text, v[0] && v[1] && v[2] ? SHORT_CELL
                                                     : TEXT_CELL);
}

/* %Lv: see the file header */
static const char *row_value(int item, void *data, char *buf, size_t size)
{
    (void)data;
    if (item < 0 || item >= nrows || size < 3)
        return "";
    const struct row *r = &rows[item];
    const struct hr_question *q = current_question();
    char style = 'r', icon = '-';
    char right[48] = "";
    switch (r->kind)
    {
        case ROW_STATUS:
            style = 'q';
            icon = 'l';
            break;
        case ROW_ALERT:
        {
            icon = 'a';
            const struct hr_workspace *w =
                find_space(model.questions[0].workspace);
            if (model.nquestions == 1 && w)
                snprintf(right, sizeof right, "%s", w->label);
            break;
        }
        case ROW_SPACE:
            icon = space_state(&model.ws[r->index], right, sizeof right);
            break;
        case ROW_AGENT:
            icon = agent_state(&model.agents[r->index]);
            const char *word = status_word(icon);
            if (is_t3(r->id))
                word = icon == 'b' ? "ask" : icon == 'w' ? "run" :
                       icon == 'f' ? "err" : word;
            snprintf(right, sizeof right, "%s", word);
            break;
        case ROW_FILTER:
            snprintf(right, sizeof right, "SEL Switch");
            break;
        case ROW_USAGE:
            snprintf(right, sizeof right, "7d stats ›");
            break;
        case ROW_DISMISS:
            snprintf(right, sizeof right, "skip on T3");
            break;
        case ROW_TEXT:
        case ROW_T3_HEADER:
            style = 'h';
            break;
        case ROW_ACTION:
            break;
        case ROW_OPTION:
        {
            bool ticked;
            char label[96];
            if (!q)
                break;
            const char *detail = option_parts(&q->options[r->index], &ticked,
                                              label, sizeof label);
            if (q->multi)
                icon = ticked ? 'x' : 'c';
            else if (r->index < 8)
                icon = '1' + r->index;
            if (detail && fits_right(detail))
                snprintf(right, sizeof right, "%s", detail);
            break;
        }
        case ROW_SUBMIT:
        {
            icon = 's';
            int n = 0;
            for (int i = 0; q && i < q->noptions; i++)
                n += !strncmp(q->options[i].label, "[x] ", 4);
            if (n)
                snprintf(right, sizeof right, "%d chosen", n);
            break;
        }
        default:
            style = 'n';
            break;
    }
    snprintf(buf, size, "%c%c%s", style, icon,
             fit(right, sizeof right, right, RIGHT_CELL));
    return buf;
}

/* %LD, in the agents view only, where every row that can be chosen has
 * one: an agent's kind and title over what it did last, or who asks the
 * waiting question */
static const char *row_description(int item, void *data, char *buf,
                                   size_t size)
{
    (void)data;
    if (view != VIEW_AGENTS || item < 0 || item >= nrows)
        return NULL;
    if (rows[item].kind == ROW_ALERT)
    {
        const struct hr_question *q = &model.questions[0];
        if (model.nquestions > 1)
            return "Select to answer the first";
        snprintf(buf, size, "%s asks", q->agent);
        return buf;
    }
    if (rows[item].kind != ROW_AGENT)
        return NULL;
    const struct hr_agent *a = &model.agents[rows[item].index];
    if (is_t3(a->pane))
    {
        snprintf(buf, size, "%s%s%s", a->title, a->last[0] ? "\n" : "", a->last);
        return buf;
    }
    bool titled = a->title[0] && agent_name(a) != a->title;
    snprintf(buf, size, "%s%s%s%s%s", a->kind, titled ? " · " : "",
             titled ? a->title : "", a->last[0] ? "\n" : "", a->last);
    return buf;
}

/* Say the row's text and its right-hand side, not the letters for the
 * theme */
static int speak_row(int item, void *data)
{
    char buf[128];
    talk_text(row_text(item, buf, sizeof buf), false);
    const char *value = row_value(item, data, buf, sizeof buf);
    if (value[0] && value[1] && value[2])
        talk_text(value + 2, true);
    return 0;
}

static bool selectable(int item)
{
    if (item < 0 || item >= nrows)
        return false;
    int kind = rows[item].kind;
    return kind != ROW_STATUS && kind != ROW_HINT && kind != ROW_TEXT &&
           kind != ROW_EMPTY && kind != ROW_T3_HEADER;
}

static const char *view_title(void)
{
    static char title[64];
    if (pending_req[0])
        return "Answering…";
    if (view == VIEW_AGENTS)
    {
        if (is_t3(space_id))
        {
            const struct hr_workspace *w = find_space(space_id);
            snprintf(title, sizeof title, "%s%s", w ? w->label : "Threads",
                     agents_fruit_nav() ? "" :
                     !strcmp(space_tab, "all") ? " · ‹ All" : " · Active ›");
            return title;
        }
        /* The tab, with an arrow for each neighbour; the workspace alone
         * when it has one tab, or from a computer that lists no tabs */
        const struct hr_workspace *w = find_space(space_id);
        int i = hr_model_tab_index(&model, space_id, space_tab);
        if (i >= 0 && model.ntabs > 1)
        {
            const struct hr_tab *t = &model.tabs[i];
            snprintf(title, sizeof title, "%s%d/%d %s%s", i ? "‹ " : "",
                     i + 1, model.ntabs, t->label[0] ? t->label : t->id,
                     i < model.ntabs - 1 ? " ›" : "");
        }
        else
            snprintf(title, sizeof title, "%s", w ? w->label : "Agents");
        return title;
    }
    if (view == VIEW_DETAIL || view == VIEW_ACTIONS || view == VIEW_CONFIRM)
    {
        /* The agent's name, or its kind when it has none: never the task,
         * which the page's TASK line carries. Cut rather than scrolled. */
        const struct hr_agent *a = &detail_agent;
        return fit(title, sizeof title, a->name[0] ? a->name : a->kind,
                   TITLE_CELL);
    }
    if (view == VIEW_QUESTION)
    {
        const struct hr_question *q = current_question();
        const struct hr_workspace *w = q ? find_space(q->workspace) : NULL;
        snprintf(title, sizeof title, "%s%s", q && is_t3(q->id) ? "T3 · " : "",
                 w ? w->label : q ? q->agent : "Question");
        return title;
    }
    if (view == VIEW_USAGE)
        return "Usage";
    return t3_mode() ? "T3 Projects" : "Agents";
}

/* Semantic colours used by Usage and activity indicators. */
#define USE_WARN    LCD_RGBPACK(255, 183, 77)
#define USE_PACE    LCD_RGBPACK(255, 214, 0)
#define USE_FULL    LCD_RGBPACK(255, 110, 110)
#define USE_WEEK    (7 * 86400)
#define USE_LINE    21      /* pixels from one text line to the next */
#define USE_BLOCK   40      /* pixels a service takes */

struct panel_ink {
    unsigned text, muted, fill, warn;
};

static struct panel_ink list_panel_ink(void)
{
    unsigned bg = ui_theme_rgb565(UI_COLOR_BACKGROUND);
    return (struct panel_ink){
        .text = ui_theme_rgb565(UI_COLOR_FOREGROUND),
        .muted = ui_theme_rgb565(UI_COLOR_MUTED),
        .fill = ui_theme_rgb565(UI_COLOR_ACCENT),
        .warn = color_fit_contrast(USE_WARN, bg, 450),
    };
}

/* Refresh the empty list's backdrop and chrome before drawing the panel.
 * Submit the base and panel together from the caller. */
static struct viewport panel_viewport(struct gui_synclist *list)
{
    bool held = ui_flush_inhibited();
    ui_inhibit_flush(true);
    gui_synclist_draw(list);
    ui_inhibit_flush(held);
    struct viewport vp = *list->parent[SCREEN_MAIN];
    vp.drawmode = DRMODE_FG;
    return vp;
}

/* Seconds since the shown figures were sent */
static int usage_age(void)
{
    return model.usage.age + (current_tick - usage_at) / HZ;
}

/* A reset time sent seconds ago, counted down: "3d4h", "2h13m", "13m" */
static const char *fmt_reset(char *buf, size_t size, int secs)
{
    if (secs < 0)
        return "—";
    secs -= (current_tick - usage_at) / HZ;
    if (secs < 60)
        return "<1m";
    if (secs >= 86400)
        snprintf(buf, size, "%dd%dh", secs / 86400, secs % 86400 / 3600);
    else if (secs >= 3600)
        snprintf(buf, size, "%dh%dm", secs / 3600, secs % 3600 / 60);
    else
        snprintf(buf, size, "%dm", secs / 60);
    return buf;
}

/* "950", "12.3k", "123M", "4.5B" */
static const char *fmt_tokens(char *buf, size_t size, long long n)
{
    static const char units[] = "kMBT";
    if (n < 0)
        return "—";
    if (n < 1000)
    {
        snprintf(buf, size, "%d", (int)n);
        return buf;
    }
    int i = 0;
    long long u = 1000;
    for (; i < 3 && n / u >= 1000; i++)
        u *= 1000;
    int tenths = n / (u / 10);
    if (tenths < 1000)
        snprintf(buf, size, "%d.%d%c", tenths / 10, tenths % 10, units[i]);
    else
        snprintf(buf, size, "%d%c", tenths / 10, units[i]);
    return buf;
}

/* "$41.20", or whole dollars from $1000 */
static const char *fmt_usd(char *buf, size_t size, long long milli)
{
    if (milli < 0)
        return "—";
    long long cents = (milli + 5) / 10;
    if (cents < 100000)
        snprintf(buf, size, "$%d.%02d", (int)(cents / 100),
                 (int)(cents % 100));
    else
        snprintf(buf, size, "$%ld", (long)MIN(cents / 100, 999999999LL));
    return buf;
}

static int text_w(struct screen *d, const char *text)
{
    int w, h;
    d->getstringsize((const unsigned char *)text, &w, &h);
    return w;
}

static void text_at(struct screen *d, int x, int y, unsigned colour,
                    const char *text)
{
    d->set_foreground(colour);
    d->putsxy(x, y, (const unsigned char *)text);
}

/* The track is transparent. Fit its outline and fill against the most
 * demanding part of the artwork under this bar, before either is painted. */
static unsigned limit_background(int x, int y, int width, unsigned text)
{
    const struct viewport *vp = lcd_current_viewport;
    const fb_data *row = FBADDR(vp->x + x, vp->y + y);
    bool light_text = color_contrast(text, LCD_BLACK) >= color_contrast(text, LCD_WHITE);
    unsigned worst = row[0];
    int worst_light = color_contrast(worst, LCD_BLACK);
    for (int i = 1; i < width; ++i)
    {
        unsigned pixel = row[i];
        int light = color_contrast(pixel, LCD_BLACK);
        if (light_text ? light > worst_light : light < worst_light)
        {
            worst = pixel;
            worst_light = light;
        }
    }
    return worst;
}

static unsigned limit_colour(unsigned colour, unsigned bg, unsigned text)
{
    /* Keep the bar on the same light/dark side as the text. Darkening a
     * warning against a bright part of the cover can hide it elsewhere. */
    int ground = color_contrast(bg, LCD_BLACK);
    bool light_text = color_contrast(text, LCD_BLACK) > ground;
    for (int mix = 0; mix < 256; mix += 32)
    {
        unsigned fitted = color_blend(colour, text, mix);
        int ink = color_contrast(fitted, LCD_BLACK);
        if (light_text ? ink * 100 >= ground * 350 : ground * 100 >= ink * 350)
            return fitted;
    }
    return text;
}

/* One limit in a cell w wide: its label, a bar, and the used percent and
 * reset time at the cell's right. A bar with no room is left out. */
static void draw_limit(struct screen *d, const struct panel_ink *ink, int x, int y, int w,
                       const char *label, int pct, int reset, int period,
                       bool dim)
{
    char r[16], text[24];
    if (pct < 0)
        snprintf(text, sizeof text, "—");
    else
        snprintf(text, sizeof text, "%d%% %s", pct,
                 fmt_reset(r, sizeof r, reset));
    unsigned colour = dim ? ink->muted : ink->text;
    int lw = text_w(d, label), tw = text_w(d, text);
    text_at(d, x, y, ink->muted, label);
    text_at(d, x + w - tw, y, colour, text);
    int bx = x + lw + 5, bw = w - lw - tw - 11;
    if (bw < 12)
        return;
    int by = y + 6, bh = 8;
    int height = lcd_current_viewport->height;
    if (by >= height || by + bh <= 0)
        return;
    int sample_y = MAX(0, MIN(by + bh / 2, height - 1));
    unsigned bg = limit_background(bx, sample_y, bw, ink->text);
    d->set_foreground(limit_colour(ink->muted, bg, ink->text));
    d->drawrect(bx, by, bw, bh);
    if (pct > 0)
    {
        /* Stale only greys a bar that is neither full nor ahead of its
         * period; the shown reset time is counted down like fmt_reset */
        int left = reset < 0 ? -1
                   : MAX(0, reset - (current_tick - usage_at) / HZ);
        unsigned fill = pct >= 95 ? USE_FULL :
                        hr_usage_over_pace(pct, left, period) ? USE_PACE :
                        dim ? ink->muted : ink->fill;
        d->set_foreground(limit_colour(fill, bg, ink->text));
        d->fillrect(bx + 1, by + 1, MAX(1, (bw - 2) * pct / 100), bh - 2);
    }
}

/* A service: its name, tokens and cost, then its limits. A service
 * with nothing to show, or whose figures could not be read, is greyed and
 * gives its note in place of the limits. */
static void draw_service(struct screen *d, const struct panel_ink *ink, int y, int w,
                         const struct hr_service *s, bool stale)
{
    static const char *const names[HR_USAGE_SVCS] =
        { "Codex", "Claude", "Grok", "Gemini" };
    char t[16], c[16], right[40];
    bool grey = s->state == 'n' || s->state == 'e';
    bool dim = stale || grey;
    int i = s - model.usage.svc;
    text_at(d, 0, y, dim ? ink->muted : ink->text,
            names[i]);
    if (s->state == 'e' || (s->state == 'n' && s->tokens <= 0))
        snprintf(right, sizeof right, "—");
    else
        snprintf(right, sizeof right, "%s  %s",
                 fmt_tokens(t, sizeof t, s->tokens),
                 fmt_usd(c, sizeof c, s->usd_milli));
    text_at(d, w - text_w(d, right), y, dim ? ink->muted : ink->text, right);
    y += USE_LINE - 2;
    if (grey)
    {
        char note[48];
        text_at(d, 0, y, ink->muted, fit(note, sizeof note,
                s->note[0] ? s->note : s->state == 'e' ? "Cannot read"
                                                       : "Not used", w));
        return;
    }
    /* A service with no 5h limit has one bar, the week's, the row wide */
    if (s->h5_pct < 0 && s->h5_reset < 0)
    {
        draw_limit(d, ink, 0, y, w, "Week", s->wk_pct, s->wk_reset,
                   USE_WEEK, dim);
        return;
    }
    int half = (w - 12) / 2;
    draw_limit(d, ink, 0, y, half, "5h", s->h5_pct, s->h5_reset, 5 * 3600,
               dim);
    draw_limit(d, ink, w - half, y, half, "wk", s->wk_pct, s->wk_reset, USE_WEEK,
               dim);
}

/* Draw the usage figures and bar outlines directly over the list backdrop. */
static void draw_usage(struct gui_synclist *list)
{
    struct screen *d = &screens[SCREEN_MAIN];
    struct viewport canvas = panel_viewport(list);
    const struct viewport *vp = &canvas;
    struct panel_ink ink = list_panel_ink();
    const struct hr_usage *u = &model.usage;
    char buf[48], t[16], c[16];
    struct viewport *previous = d->set_viewport(&canvas);
    const char *t_trouble = trouble(buf, sizeof buf);
    if (!u->have)
    {
        const char *msg = t_trouble ? t_trouble : "Waiting for usage…";
        text_at(d, (vp->width - text_w(d, msg)) / 2,
                (vp->height - USE_LINE) / 2, ink.muted, msg);
    }
    else
    {
        int age = usage_age();
        bool stale = t_trouble || !u->ok || age > 3 * USAGE_EVERY / HZ;
        for (int i = 0; i < HR_USAGE_SVCS; i++)
            draw_service(d, &ink, i * USE_BLOCK - usage_scroll, vp->width,
                         &u->svc[i], stale);
        int y = HR_USAGE_SVCS * USE_BLOCK - usage_scroll;
        d->set_foreground(ink.muted);
        d->hline(0, vp->width - 1, y);
        y += 3;
        snprintf(buf, sizeof buf, "%s  %s", fmt_tokens(t, sizeof t, u->tokens),
                 fmt_usd(c, sizeof c, u->usd_milli));
        text_at(d, 0, y, stale ? ink.muted : ink.text, "Total");
        text_at(d, text_w(d, "Total") + 10, y, stale ? ink.muted : ink.text,
                buf);
        if (stale)
            text_at(d, vp->width - text_w(d, "stale"), y, ink.warn, "stale");
    }
    d->set_viewport(previous);
    ui_flush_dirty();
}

/* The detail view's colours beyond the usage view's: the working line, and
 * the dot by the state */
#define DET_GAP     4       /* pixels between the view's parts */
#define DET_DOT     9       /* the dot's diameter */

/* Seconds in the state now: the age the last D gave, counted on since it
 * came; -1 unknown, -2 fallback */
static int detail_age(void)
{
    int age = model.detail.age;
    if (age < 0)
        return age;
    long on = (current_tick - detail_at) / HZ;
    return on > 2147483647 - age ? 2147483647 : age + (int)on;
}

/* "<1m", "29m", "1h05m", "2d3h" */
static const char *fmt_age(char *buf, size_t size, int secs)
{
    if (secs < 60)
        return "<1m";
    if (secs < 3600)
        snprintf(buf, size, "%dm", secs / 60);
    else if (secs < 86400)
        snprintf(buf, size, "%dh%02dm", secs / 3600, secs % 3600 / 60);
    else
        snprintf(buf, size, "%dd%dh", secs / 86400, secs % 86400 / 3600);
    return buf;
}

/* A disc DET_DOT across, its top left at x, y */
static void draw_disc(struct screen *d, int x, int y, unsigned colour)
{
    static const unsigned char inset[DET_DOT] = { 3, 1, 1, 0, 0, 0, 1, 1, 3 };
    d->set_foreground(colour);
    for (int i = 0; i < DET_DOT; i++)
        d->hline(x + inset[i], x + DET_DOT - 1 - inset[i], y + i);
}

/* Draw the task line at its scroll, in a strip of its own so the ticker can
 * draw it alone */
static void draw_task(struct gui_synclist *list, struct viewport *vp,
                      unsigned colour, bool repaint)
{
    struct screen *d = &screens[SCREEN_MAIN];
    struct viewport strip = *vp;
    strip.y += task_y;
    strip.height = MIN(USE_LINE, vp->height - task_y);
    strip.drawmode = DRMODE_FG;
    if (strip.height <= 0 || (repaint && !gui_synclist_paint_background(list, &strip)))
        return;
    struct viewport *previous = d->set_viewport(&strip);
    text_at(d, -task_x, 0, colour, task_text);
    d->set_viewport(previous);
    if (repaint)
    {
        ui_mark_dirty_rect(SCREEN_MAIN, strip.x, strip.y, strip.width, strip.height);
        ui_flush_dirty();
    }
}

/* A frame of the task line's scroll, while the detail view is open */
static void tick_task(struct gui_synclist *list)
{
    struct viewport *vp = list->parent[SCREEN_MAIN];
    int over = task_w - vp->width;
    if (!task_text[0] || over <= 0 || TIME_BEFORE(current_tick, task_next))
        return;
    if (task_at_end)
    {
        task_x = 0;
        task_at_end = false;
        task_next = current_tick + TASK_REST;
    }
    else
    {
        task_x += TASK_STEP;
        if (task_x >= over)
        {
            task_x = over;
            task_at_end = true;
            task_next = current_tick + TASK_REST;
        }
    }
    draw_task(list, vp, ui_theme_rgb565(UI_COLOR_FOREGROUND), true);
}

/* The input block: what is typed in the agent's input box, in a frame, up
 * to INPUT_LINES lines of it in a window that follows the cursor's line */
#define INPUT_LINES 5
#define INPUT_PAD   3       /* between the frame and the text */
static int input_top;       /* the window's first line */

/* The line of the wrapped input the cursor is on, and its byte position:
 * the first line whose text or the spaces after it hold the cursor; after
 * a line break, the next line. Returns the number of lines. */
static int input_lines(const char *text, int width, struct font *pf,
                       const char *cursor_at, int *cursor_line)
{
    const char *end;
    int n = 0;
    *cursor_line = -1;
    for (const char *p = text; *p; n++)
    {
        const char *next = text_wrap_next_punct(p, width, pf, &end);
        if (*cursor_line < 0 && cursor_at >= p &&
            (cursor_at <= end || cursor_at < next || !*next))
            *cursor_line = n;
        p = next;
    }
    if (n == 0 || text[strlen(text) - 1] == '\n')
    {
        /* The cursor sits on the empty line after a final line break */
        if (*cursor_line < 0)
            *cursor_line = n;
        n++;
    }
    return n;
}

static int draw_input(struct screen *d, struct viewport *vp, int y,
                      const char *text, const char *cursor_at,
                      unsigned text_colour, unsigned border_colour)
{
    struct font *pf = font_get(vp->font);
    int width = vp->width - 2 * (INPUT_PAD + 1), x0 = INPUT_PAD + 1;
    int h = pf->height, cursor_line;
    const char *end;
    char line[200];
    font_lock(vp->font, true);
    int nlines = input_lines(text, width, pf, cursor_at, &cursor_line);
    input_top = hr_input_window(cursor_line, nlines, INPUT_LINES, input_top);
    int shown = MIN(nlines - input_top, INPUT_LINES);
    d->set_foreground(border_colour);
    d->drawrect(0, y, vp->width, shown * USE_LINE + 2 * INPUT_PAD);
    int ly = y + INPUT_PAD;
    const char *p = text;
    for (int i = 0; i < input_top + shown; i++)
    {
        const char *next = *p ? text_wrap_next_punct(p, width, pf, &end)
                              : p + (end = p, 0);
        if (i >= input_top)
        {
            snprintf(line, sizeof line, "%.*s",
                     (int)MIN(end - p, (long)sizeof line - 1), p);
            text_at(d, x0, ly, text_colour, line);
            if (i == cursor_line)
            {
                /* The bar after the character before the cursor */
                const char *at = cursor_at < end ? cursor_at : end;
                snprintf(line, sizeof line, "%.*s",
                         (int)MIN(at - p, (long)sizeof line - 1), p);
                int cx = x0 + (at > p ? text_w(d, line) : 0);
                d->set_foreground(text_colour);
                d->vline(MIN(cx, vp->width - 2), ly + 1, ly + h - 2);
            }
            ly += USE_LINE;
        }
        p = next;
    }
    font_lock(vp->font, false);
    return y + shown * USE_LINE + 2 * INPUT_PAD + DET_GAP;
}

static int detail_measure(const char *text, int bytes, void *ctx)
{
    int font = *(const int *)ctx;
    return font_getstringnsize(text, bytes, NULL, NULL, font);
}

/* Draw the detail view over the empty list. The agent's name and kind head
 * it, with a dot, its state and the time in it on the right; below a rule
 * come the task, the working line (or the state when done, idle or
 * asking), the message, wrapped to as many lines as it needs, and the last
 * steps, the oldest dropped first when the message needs their lines. A
 * fallback detail is the pane's last lines in grey, with no time. */
static void draw_t3_body(struct screen *d, struct viewport *vp, int y,
                         unsigned colour, unsigned muted)
{
    const struct hr_detail *det = &model.detail;
    struct detail_layout *body = &detail_layout;
    const int pitch = MAX(17, font_get(vp->font)->height + 1);
    const int width = MAX(1, vp->width - 8);
    bool changed = strcmp(body->wire, det->say) ||
                   memcmp(body->steps, det->step, sizeof body->steps);
    detail_new = changed && body->used && !detail_follow && detail_scroll < detail_scroll_max;
    if (body->width != width || (changed && !detail_new))
    {
        hr_copy(body->wire, det->say, sizeof body->wire);
        memcpy(body->steps, det->step, sizeof body->steps);
        body->width = width;
        body->used = body->count = 0;
        struct font *pf = font_get(vp->font);
        font_lock(vp->font, true);
        const char *p = body->wire;
        while (*p && body->count < DETAIL_LAYOUT_LINES)
        {
            const char *end;
            const char *next = text_wrap_next_punct(p, width, pf, &end);
            int len = end - p;
            if (body->used + len + 1 >= DETAIL_LAYOUT_BYTES || next <= p)
                break;
            int i = body->count++;
            body->start[i] = body->used;
            memcpy(body->text + body->used, p, len);
            body->used += len;
            body->end[i] = body->used;
            body->text[body->used++] = '\0';
            p = next;
        }
        font_lock(vp->font, false);
    }

    int count = body->count;
    for (int i = 0; i < HR_DETAIL_STEPS; i++)
        count += body->steps[i][0] != '\0';
    detail_page_lines = MAX(1, (vp->height - KEYBAR - y) / pitch);
    detail_scroll_max = MAX(0, count - detail_page_lines);
    detail_scroll = detail_follow ? detail_scroll_max :
                                   MIN(detail_scroll, detail_scroll_max);
    if (!detail_scroll_max && !detail_new)
        detail_follow = true;

    int row = -detail_scroll;
    for (int i = 0; i < body->count && row < detail_page_lines; i++, row++)
        if (row >= 0)
            text_at(d, 0, y + row * pitch, colour, body->text + body->start[i]);
    for (int i = 0; i < HR_DETAIL_STEPS && row < detail_page_lines; i++)
    {
        if (!body->steps[i][0])
            continue;
        if (row >= 0)
        {
            char line[HR_DETAIL_TEXT + 8];
            snprintf(line, sizeof line, "› %s", body->steps[i]);
            text_at(d, 0, y + row * pitch, muted, fit(line, sizeof line, line, width));
        }
        row++;
    }
    if (detail_scroll_max > 0)
    {
        int track = MAX(1, vp->height - KEYBAR - y - 2);
        int thumb = MAX(4, track * detail_page_lines / count);
        d->set_foreground(ui_theme_rgb565(UI_COLOR_SEPARATOR));
        d->fillrect(vp->width - 3, y, 3, track);
        d->set_foreground(ui_theme_rgb565(UI_COLOR_ACCENT));
        d->fillrect(vp->width - 3, y + (track - thumb) * detail_scroll / detail_scroll_max,
                    3, thumb);
    }
    if (detail_new)
    {
        const char *note = "↓ New";
        int w = text_w(d, note) + 8;
        int py = vp->height - KEYBAR - pitch;
        d->set_foreground(ui_theme_rgb565(UI_COLOR_MODAL_BG));
        d->fillrect(vp->width - w - 6, py, w, pitch);
        text_at(d, vp->width - w - 2, py, ui_theme_rgb565(UI_COLOR_MODAL_FG), note);
    }
}

static void detail_keybar(struct screen *d, const struct viewport *vp)
{
    int y = vp->height - KEYBAR;
    const char *key = agents_fruit_nav() ?
        "Wheel Scroll · SEL Actions · Hold SEL Voice" :
        "SEL Actions · ◀/▶ Page · Hold PLAY Voice";
    d->set_foreground(ui_theme_rgb565(UI_COLOR_LIST_TINT));
    d->fillrect(0, y, vp->width, KEYBAR);
    d->set_foreground(ui_theme_rgb565(UI_COLOR_SEPARATOR));
    d->hline(0, vp->width - 1, y);
    char cut[88];
    text_at(d, 4, y + 2, color_fit_contrast(ui_theme_rgb565(UI_COLOR_FOREGROUND),
                  ui_theme_rgb565(UI_COLOR_LIST_TINT), 450), fit(cut, sizeof cut, key, vp->width - 8));
}

static void draw_detail(struct gui_synclist *list)
{
    struct screen *d = &screens[SCREEN_MAIN];
    struct viewport canvas = panel_viewport(list);
    struct viewport *vp = &canvas;
    struct panel_ink ink = list_panel_ink();
    const struct hr_detail *det = &model.detail;
    char buf[128], age[16], line[200], trouble_buf[48];
    struct viewport *previous = d->set_viewport(vp);
    const struct hr_agent *a = find_agent(detail_pane);
    if (a)
        detail_agent = *a;
    a = &detail_agent;
    bool t3 = is_t3(detail_pane);
    bool have = model.detail_valid && !strcmp(model.detail_pane, detail_pane);
    bool fallback = have && det->age == -2;
    int secs = have ? detail_age() : -1;
    const char *t_trouble = trouble(trouble_buf, sizeof trouble_buf);

    /* The header: name · kind, and the dot, state and time on the right */
    char state = agent_state(a);
    snprintf(buf, sizeof buf, "%s%s", status_word(state),
             secs >= 0 ? " " : "");
    if (secs >= 0)
    {
        size_t n = strlen(buf);
        snprintf(buf + n, sizeof buf - n, "%s", fmt_age(age, sizeof age, secs));
    }
    detail_shown_age = secs >= 0 ? secs / 60 : -1;
    int sw = text_w(d, buf);
    int h = font_get(vp->font)->height;
    text_at(d, vp->width - sw, 0, state == 'b' ? ink.warn : ink.text, buf);
    draw_disc(d, vp->width - sw - DET_DOT - 5, (h - DET_DOT) / 2,
              state == 'b' ? DOT_RED : state == 'w' ? DOT_GREEN :
              state == 'f' ? DOT_CORAL : state == 'd' ? DOT_TEAL : DOT_GREY);
    snprintf(buf, sizeof buf, "%s%s%s", a->name[0] ? a->name : "",
             a->name[0] ? " · " : "", a->kind);
    text_at(d, 0, 0, ink.muted,
            fit(buf, sizeof buf, t3 ? a->title : buf, vp->width - sw - DET_DOT - 13));
    int y = USE_LINE + 2;
    d->set_foreground(ink.muted);
    d->hline(0, vp->width - 1, y);
    y += DET_GAP;
    if (t3)
    {
        task_text[0] = '\0';
        detail_keybar(d, vp);
        if (t_trouble)
        {
            const char *message = !model.hello ? "Computer disconnected" : t_trouble;
            text_at(d, 0, y, ink.warn, fit(line, sizeof line, message, vp->width));
            y += USE_LINE;
        }
        if (!t_trouble && (hr_model_question_for(&model, detail_pane) ||
                           (have && det->input[0])))
        {
            d->set_foreground(ui_theme_rgb565(UI_COLOR_LIST_TINT));
            d->fillrect(0, y, vp->width, 24);
            text_at(d, 4, y + 2, ink.warn, "! Approval needed · Open Actions");
            y += 24;
        }
        if (have && (state == 'w' || state == 'f') && det->now[0])
        {
            snprintf(line, sizeof line, "%s %s", state == 'f' ? "!" : "•", det->now);
            text_at(d, 0, y, state == 'f' ? ink.warn : ink.text,
                    fit(line, sizeof line, line, vp->width));
            y += USE_LINE;
        }
        if (!have || (!det->say[0] && !det->step[0][0] && !det->step[1][0] && !det->step[2][0] &&
                      (!detail_layout.used || detail_follow)))
        {
            const char *msg = have ? "Nothing in the thread" : "Waiting for the thread…";
            text_at(d, (vp->width - text_w(d, msg)) / 2,
                    y + MAX(0, (vp->height - KEYBAR - y - USE_LINE) / 2), ink.muted, msg);
        }
        else
            draw_t3_body(d, vp, y, t_trouble ? ink.muted : ink.text, ink.muted);
        d->set_viewport(previous);
        ui_flush_dirty();
        return;
    }

    bool empty = have &&
                 !det->task[0] && !det->now[0] && !det->say[0] &&
                 !det->step[0][0] && !det->step[1][0] && !det->step[2][0];
    if (!have || empty)
    {
        task_text[0] = '\0';
        const char *msg = t_trouble ? t_trouble :
                          have ? "Nothing in the pane" : "Waiting for the pane…";
        text_at(d, (vp->width - text_w(d, msg)) / 2,
                y + (vp->height - y - USE_LINE) / 2, ink.muted, msg);
        d->set_viewport(previous);
        ui_flush_dirty();
        return;
    }

    /* The task, scrolled when wide: a new one starts at its left */
    if (strcmp(task_text, det->task))
    {
        snprintf(task_text, sizeof task_text, "%s", det->task);
        task_w = text_w(d, task_text);
        task_x = 0;
        task_at_end = false;
        task_next = current_tick + TASK_REST;
    }
    if (task_text[0])
    {
        task_y = y;
        draw_task(list, vp, ink.text, false);
        y += USE_LINE;
    }

    /* What is typed, first: it is what the wheel's keys are editing */
    if (det->input[0])
        y = draw_input(d, vp, y, det->input,
                       det->input + hr_utf8_offset(det->input, det->cursor),
                       t_trouble ? ink.muted : ink.text, ink.muted);

    /* The working line, or the state */
    if (det->now[0])
    {
        unsigned colour = ink.text;
        const char *now = det->now;
        if (!strcmp(now, "done") || !strcmp(now, "idle"))
        {
            snprintf(line, sizeof line, "%s%s%s", now,
                     secs >= 0 ? " · " : "",
                     secs >= 0 ? fmt_age(age, sizeof age, secs) : "");
            now = line;
        }
        else if (now[0] == '?')
            colour = ink.warn;
        text_at(d, 0, y, t_trouble ? ink.muted : colour,
                fit(line, sizeof line, now, vp->width));
        y += USE_LINE;
    }
    y += DET_GAP;

    /* Retain closed lines during streaming. A reader above the bottom
     * keeps the accepted message, steps and body geometry until returning. */
    bool frozen = detail_scroll < detail_scroll_max;
    char identity[9] = "";
    if (!strcmp(a->kind, "codex") && strlen(model.detail_revision) == 16)
        snprintf(identity, sizeof identity, "%.8s", model.detail_revision);
    detail_layout_update(&detail_layout, detail_pane, identity, det->say,
                         det, vp->width, frozen, detail_measure, &vp->font);
    if (!frozen || !detail_body_avail)
    {
        detail_body_y = y;
        detail_body_avail = MAX(0, (vp->height - y) / USE_LINE);
    }
    y = detail_body_y;
    int avail = detail_body_avail, need = detail_layout.count;
    detail_page_lines = MAX(1, avail);
    /* Tool rows consume spare space; starting/finishing a tool must never
     * push a message line off screen or change the scroll anchor. */
    int maximum = MAX(0, need - avail);
    detail_scroll = hr_detail_scroll(detail_scroll, detail_scroll_max, maximum);
    detail_scroll_max = maximum;
    unsigned colour = t_trouble || fallback ? ink.muted : ink.text;
    for (int i = detail_scroll; i < need && i < detail_scroll + avail; i++)
    {
        int start = detail_layout.start[i], end = detail_layout.end[i];
        snprintf(line, sizeof line, "%.*s", MIN(end - start, (int)sizeof line - 1),
                 detail_layout.text + start);
        text_at(d, 0, y + (i - detail_scroll) * USE_LINE, colour, line);
    }
    int row = need - detail_scroll;
    for (int i = 0; i < HR_DETAIL_STEPS && row < avail; i++)
    {
        if (!detail_layout.steps[i][0]) continue;
        snprintf(line, sizeof line, "› %s", detail_layout.steps[i]);
        text_at(d, 0, y + row++ * USE_LINE, ink.muted,
                fit(line, sizeof line, line, vp->width));
    }
    d->set_viewport(previous);
    ui_flush_dirty();
}

/* Sheets and alerts share the native modal palette and compositor. */
#define Q_MARGIN 12
#define ACTION_ROW_HEIGHT 24

static void draw_mic(struct screen *d, int x, int y, unsigned colour);
static int quick_choice(const struct hr_question *q, bool deny);

static void modal_label(struct screen *d, int x, int y, int width,
                        unsigned colour, const char *text)
{
    char line[160];
    text_at(d, x, y, colour, fit(line, sizeof line, text, width));
}

static void modal_footer(struct screen *d, const struct viewport *vp,
                         const char *text)
{
    int y = vp->height - KEYBAR;
    unsigned bg = ui_theme_rgb565(UI_COLOR_MODAL_BG);
    d->set_foreground(ui_theme_rgb565(UI_COLOR_MODAL_BORDER));
    d->hline(1, vp->width - 2, y);
    modal_label(d, Q_MARGIN, y + 2, vp->width - 2 * Q_MARGIN,
                color_fit_contrast(ui_theme_rgb565(UI_COLOR_MUTED), bg, 450), text);
}

static void modal_button(struct screen *d, int x, int y, int w, int h,
                         const char *label, unsigned accent, bool selected)
{
    unsigned bg = ui_theme_rgb565(UI_COLOR_MODAL_BG);
    unsigned fg = ui_theme_rgb565(UI_COLOR_MODAL_FG);
    int radius = agents_fruit_nav() ? 5 : 0;
    if (selected)
    {
        d->set_foreground(accent);
        fill_round_rect(d, x, y, w, h, radius);
        fg = color_fit_contrast(ui_theme_rgb565(UI_COLOR_MODAL_SELECTED_TEXT),
                                accent, 450);
    }
    else
    {
        d->set_foreground(accent);
        draw_round_rect(d, x, y, w, h, radius, 1);
        fg = color_fit_contrast(accent, bg, 450);
    }
    text_at(d, x + (w - text_w(d, label)) / 2,
            y + (h - font_get(agents_body_font)->height) / 2, fg, label);
}

static void draw_actions_body(struct screen *d, struct viewport *vp, int sel)
{
    unsigned bg = ui_theme_rgb565(UI_COLOR_MODAL_BG);
    unsigned fg = ui_theme_rgb565(UI_COLOR_MODAL_FG);
    unsigned muted = color_fit_contrast(ui_theme_rgb565(UI_COLOR_MUTED), bg, 450);
    unsigned edge = ui_theme_rgb565(UI_COLOR_MODAL_BORDER);
    int h = font_get(vp->font)->height;
    text_at(d, Q_MARGIN, 4, fg, "Actions");
    char state[48], age[16];
    snprintf(state, sizeof state, "%s%s%s", status_word(agent_state(&detail_agent)),
             detail_age() >= 0 ? " · " : "",
             detail_age() >= 0 ? fmt_age(age, sizeof age, detail_age()) : "");
    text_at(d, vp->width - Q_MARGIN - text_w(d, state), 4, muted, state);
    d->set_foreground(edge);
    d->hline(1, vp->width - 2, 26);
    int shown = (vp->height - 28) / ACTION_ROW_HEIGHT;
    int first = MAX(0, MIN(sel - shown + 1, nrows - shown));
    for (int i = first; i < nrows && i < first + shown; i++)
    {
        const struct row *r = &rows[i];
        int y = 28 + (i - first) * ACTION_ROW_HEIGHT;
        bool selected = i == sel;
        unsigned ink = fg;
        if (selected)
        {
            unsigned fill = ui_theme_rgb565(UI_COLOR_SELECTION);
            d->set_foreground(fill);
            if (agents_fruit_nav())
            {
                d->fillrect(2, y, vp->width - 4, ACTION_ROW_HEIGHT - 1);
                ink = color_fit_contrast(ui_theme_rgb565(UI_COLOR_SELECTED_TEXT),
                                         fill, 450);
            }
            else
                d->fillrect(3, y + 4, 3, ACTION_ROW_HEIGHT - 8);
        }
        char buf[100];
        const char *label = row_text(i, buf, sizeof buf);
        bool action = r->kind == ROW_ACTION;
        bool preset = action && preset_action(&model.actions[r->index]);
        bool confirm = action && confirm_action_required(&model.actions[r->index]);
        if (preset)
        {
            snprintf(buf, sizeof buf, "↳ %s", label);
            label = buf;
            if (!i || rows[i - 1].kind != ROW_ACTION ||
                !preset_action(&model.actions[rows[i - 1].index]))
            {
                d->set_foreground(edge);
                d->hline(Q_MARGIN, vp->width - Q_MARGIN - 1, y);
            }
        }
        int ty = y + (ACTION_ROW_HEIGHT - h) / 2;
        int x = Q_MARGIN;
        if (r->kind == ROW_VOICE)
        {
            draw_mic(d, x, y + 3, ink);
            x += 16;
        }
        modal_label(d, x, ty, vp->width - x - Q_MARGIN - (confirm ? 62 : 0),
                    ink, label);
        if (confirm)
            text_at(d, vp->width - Q_MARGIN - text_w(d, "confirm"), ty,
                    selected && agents_fruit_nav() ? ink : muted, "confirm");
    }
    if (nrows > shown)
    {
        int track = vp->height - 32;
        int thumb = track * shown / nrows;
        d->set_foreground(edge);
        d->fillrect(vp->width - 4, 28 + (track - thumb) * first / (nrows - shown),
                    2, thumb);
    }
}

static void draw_overlay_body(struct screen *d, struct viewport *vp, int sel)
{
    unsigned bg = ui_theme_rgb565(UI_COLOR_MODAL_BG);
    unsigned fg = ui_theme_rgb565(UI_COLOR_MODAL_FG);
    unsigned border = ui_theme_rgb565(UI_COLOR_MODAL_BORDER);
    unsigned accent = ui_theme_rgb565(UI_COLOR_MODAL_ACCENT);
    unsigned muted = color_fit_contrast(ui_theme_rgb565(UI_COLOR_MUTED), bg, 450);
    unsigned danger = color_fit_contrast(DOT_CORAL, bg, 450);
    bool fruit = agents_fruit_nav();
    const struct hr_question *q = current_question();
    int width = vp->width - 2 * Q_MARGIN;
    int body_font = vp->font;
    d->set_foreground(bg);
    int radius = fruit && vp->buffer == &lcd_framebuffer_default ? 6 : 0;
    fill_round_rect(d, 0, 0, vp->width, vp->height, radius);
    d->set_foreground(border);
    int stroke = MAX(1, (int)ui_theme_metric(UI_METRIC_MODAL_BORDER_WIDTH));
    draw_round_rect(d, 0, 0, vp->width, vp->height, radius, stroke);

    if (view == VIEW_ACTIONS)
    {
        draw_actions_body(d, vp, sel);
        return;
    }
    if (view == VIEW_CONFIRM)
    {
        char title[64], subtitle[112];
        snprintf(title, sizeof title, "%s?", confirm_action.label);
        const struct hr_workspace *w = find_space(space_id);
        snprintf(subtitle, sizeof subtitle, "%s%s%s", agent_name(&detail_agent),
                 w ? " · " : "", w ? w->label : "");
        d->set_foreground(danger);
        d->fillrect(1, 1, vp->width - 2, 3);
        if (font_get_ui_bold() >= 0)
            vp->font = font_get_ui_bold();
        modal_label(d, Q_MARGIN, 12, width, fg, title);
        vp->font = body_font;
        modal_label(d, Q_MARGIN, 36, width, muted, subtitle);
        int bw = (width - 8) / 2;
        modal_button(d, Q_MARGIN, 63, bw, 35, "Cancel", accent, !confirm_selected);
        modal_button(d, Q_MARGIN + bw + 8, 63, bw, 35,
                     !strcmp(confirm_action.id, "interrupt") ?
                     "Interrupt" : "Confirm", danger, confirm_selected);
        modal_footer(d, vp, fruit ? "Wheel Select · SEL Confirm · MENU Cancel" :
                                   "◀/▶ Select · SEL Confirm · MENU Cancel");
        return;
    }

    if (view == VIEW_INPUT)
    {
        modal_label(d, Q_MARGIN, 8, width, fg, question_agent);
        modal_label(d, Q_MARGIN, 35, width, muted, question_line);
        if (detail_ready && model.detail_valid && !strcmp(model.detail_pane, question_pane))
        {
            const struct hr_detail *det = &model.detail;
            struct viewport box = *vp;
            box.x += Q_MARGIN;
            box.width = width;
            d->set_viewport(&box);
            draw_input(d, &box, 63, det->input,
                       det->input + hr_utf8_offset(det->input, det->cursor), fg, muted);
            d->set_viewport(vp);
        }
        modal_footer(d, vp, "SELECT Send · MENU Cancel");
        return;
    }

    if (!q)
        return;
    unsigned stripe = !strncmp(q->id, "t3a", 3) ? DOT_RED :
                      color_fit_contrast(USE_WARN, bg, 450);
    d->set_foreground(stripe);
    d->fillrect(1, 1, vp->width - 2, 3);
    draw_disc(d, Q_MARGIN, 12, stripe);
    const struct hr_workspace *w = find_space(q->workspace);
    char title[60];
    snprintf(title, sizeof title, "%s%s", is_t3(q->id) ? "T3 · " : "",
             w ? w->label : q->agent);
    bool separate_title = w && q->agent[0];
    if (font_get_ui_bold() >= 0)
        vp->font = font_get_ui_bold();
    modal_label(d, Q_MARGIN + 14, 6, separate_title ? width / 2 - 14 :
                width - 14, fg, title);
    vp->font = body_font;
    if (separate_title)
    {
        char thread[60];
        fit(thread, sizeof thread, q->agent, width / 2 - 4);
        text_at(d, vp->width - Q_MARGIN - text_w(d, thread), 7, muted, thread);
    }
    d->set_foreground(border);
    d->hline(Q_MARGIN, vp->width - Q_MARGIN - 1, 29);

    join_question(q);
    const char *body = question_text;
    int y = 38;
    if (q->nlines > 1 && text_w(d, q->lines[0]) < width * 3 / 5)
    {
        modal_label(d, Q_MARGIN + 5, y, width - 10, stripe, q->lines[0]);
        y += USE_LINE + 2;
        body += strlen(q->lines[0]);
        while (*body == ' ' || *body == '\n')
            body++;
    }
    int by = vp->height - KEYBAR - (fruit ? 78 : 57);
    d->set_foreground(border);
    d->drawrect(Q_MARGIN, y - 2, width, MAX(22, by - y - 3));
    struct dialog_text_line lines[4];
    int count = dialog_wrap_text(body, width - 12, vp->font, lines, 4);
    int room = MAX(1, MIN(3, (by - y - 5) / USE_LINE));
    for (int i = 0; i < count && i < room; i++)
    {
        char line[160];
        snprintf(line, sizeof line, "%.*s%s", MIN(lines[i].len, 148), lines[i].str,
                 i == room - 1 && count > room ? "…" : "");
        modal_label(d, Q_MARGIN + 6, y + i * USE_LINE, width - 12, fg, line);
    }
    if (quick_choice(q, false) && quick_choice(q, true))
    {
        int bw = (width - 8) / 2;
        modal_button(d, Q_MARGIN, by, bw, 36, fruit ? "Allow" : "◀ Allow",
                     color_fit_contrast(DOT_GREEN, bg, 450), fruit && alert_choice == 0);
        modal_button(d, Q_MARGIN + bw + 8, by, bw, 36, fruit ? "Deny" : "Deny ▶",
                     danger, fruit && alert_choice == 1);
    }
    if (fruit)
        modal_button(d, Q_MARGIN, by + 41, width, 25, "All options… ›",
                     accent, alert_choice == 2);
    modal_footer(d, vp, fruit ? "Wheel Select · SEL Choose · MENU Later" :
                               "SELECT All options · MENU Later");
}

static void draw_question_overlay(struct gui_synclist *list, int motion)
{
    struct screen *d = &screens[SCREEN_MAIN];
    enum view overlay = view;
    int sel = gui_synclist_get_sel_pos(list);
    bool inhibited = ui_flush_inhibited();
    ui_inhibit_flush(true);
    if (overlay == VIEW_ACTIONS || overlay == VIEW_CONFIRM)
    {
        view = VIEW_DETAIL;
        list_chrome(list);
        gui_synclist_set_nb_items(list, 0);
        draw_detail(list);
    }
    else
    {
        view = question_from == VIEW_ACTIONS ? VIEW_DETAIL : question_from;
        build_rows(list);
        gui_synclist_set_nb_items(list, nrows);
        int i = 0;
        while (i < nrows && (rows[i].kind != question_from_row.kind ||
                             strcmp(rows[i].id, question_from_row.id)))
            i++;
        gui_synclist_select_item(list, i < nrows ? i : 0);
        gui_synclist_set_title(list, view_title(), Icon_NOICON);
        if (view == VIEW_DETAIL) draw_detail(list);
        else if (view == VIEW_USAGE) draw_usage(list);
        else gui_synclist_draw(list);
    }
    view = overlay;
    build_rows(list);
    gui_synclist_set_nb_items(list, nrows);
    gui_synclist_select_item(list, sel);
    gui_synclist_set_title(list, view_title(), Icon_NOICON);
    if (view == VIEW_CONFIRM)
    {
        struct viewport under = {0};
        viewport_set_fullscreen(&under, SCREEN_MAIN);
        under.y = LCD_HEIGHT - 194;
        under.height = 192;
        under.font = agents_body_font;
        under.drawmode = DRMODE_FG;
        under.x = 8;
        under.width = LCD_WIDTH - 16;
        struct viewport *previous = d->set_viewport(&under);
        view = VIEW_ACTIONS;
        draw_overlay_body(d, &under, sel);
        view = overlay;
        d->set_viewport(previous);
    }
    ui_inhibit_flush(inhibited);

    struct viewport card = {0};
    viewport_set_fullscreen(&card, SCREEN_MAIN);
    card.drawmode = DRMODE_FG;
    card.font = agents_body_font;
    card.width = view == VIEW_CONFIRM ? 276 : LCD_WIDTH - 16;
    card.height = view == VIEW_CONFIRM ? 132 : view == VIEW_ACTIONS ? 192 : LCD_HEIGHT - 22;
    card.x = (LCD_WIDTH - card.width) / 2;
    card.y = view == VIEW_ACTIONS ? LCD_HEIGHT - card.height - 2 :
                                  (LCD_HEIGHT - card.height) / 2;
    int handle = motion && !inhibited && ui_theme_duration(16) ?
        core_alloc_free_space(FRAMEBUFFER_SIZE + card.width * card.height * sizeof(fb_data)) : 0;
    if (handle > 0)
    {
        fb_data *back = core_get_data_pinned(handle);
        fb_data *tile = back + LCD_WIDTH * LCD_HEIGHT;
        memcpy(back, lcd_framebuffer_default.fb_ptr, FRAMEBUFFER_SIZE);
        struct frame_buffer_t buffer = {
            .fb_ptr = tile, .stride = card.width, .elems = card.width * card.height,
        };
        struct viewport local = card;
        local.x = local.y = 0;
        local.buffer = &buffer;
        struct viewport *previous = d->set_viewport(&local);
        draw_overlay_body(d, &local, sel);
        d->set_viewport(previous);
        struct ui_sheet sheet = { .light = UI_FULL_OPACITY };
        if (motion < 0)
            sheet.tween.from = sheet.tween.to = 1024;
        struct ui_rect bounds = { card.x, card.y, card.width, card.height };
        ui_sheet_begin(&sheet, motion > 0, "agents-modal");
        do {
            ui_sheet_paint(&sheet, back, tile, bounds, true);
        } while (ui_sheet_wait(&sheet));
        ui_sheet_cancel(&sheet);
        memcpy(lcd_framebuffer_default.fb_ptr, back, FRAMEBUFFER_SIZE);
        core_put_data_pinned(back);
        core_free(handle);
    }
    if (motion < 0)
        return;
    fb_data *pixels = lcd_framebuffer_default.fb_ptr;
    ui_blit_tint(pixels, pixels, LCD_WIDTH * LCD_HEIGHT, UI_FULL_OPACITY * 65 / 100,
                 ui_theme_rgb565(UI_COLOR_DIM));
    struct viewport *previous = d->set_viewport(&card);
    draw_overlay_body(d, &card, sel);
    d->set_viewport(previous);
    ui_mark_dirty(SCREEN_MAIN);
    ui_flush_dirty();
}

/* A microphone 11 pixels wide and 17 tall, its top left at x, y */
static void draw_mic(struct screen *d, int x, int y, unsigned colour)
{
    d->set_foreground(colour);
    d->hline(x + 4, x + 6, y);
    d->fillrect(x + 3, y + 1, 5, 9);
    d->hline(x + 4, x + 6, y + 10);
    d->vline(x, y + 5, y + 9);
    d->vline(x + 10, y + 5, y + 9);
    d->drawpixel(x + 1, y + 10);
    d->drawpixel(x + 9, y + 10);
    d->drawpixel(x + 2, y + 11);
    d->drawpixel(x + 8, y + 11);
    d->hline(x + 3, x + 7, y + 12);
    d->vline(x + 5, y + 13, y + 15);
    d->hline(x + 2, x + 8, y + 16);
}

/* Whether the band has anything to say: a hold, a recording the computer
 * reports, a request awaiting its result, or a result still shown */
static bool talk_is_short(void);

static bool talk_wanted(void)
{
    if ((talk_held && talk_is_short()) ||
        (native_play_pending && (unsigned long)current_tick - native_play_at <
                                (3 * HZ + 19) / 20))
        return false;
    if (talk_discarded && !talk_failed)
        return false;
    return talk_held || model.voice == 't' || talk_req[0] ||
           (talk_note[0] && TIME_BEFORE(current_tick, talk_until));
}

/* The band over the foot of the list: the microphone, what the voice input
 * is doing and, while it records, for how long */
static void draw_talk(struct gui_synclist *list)
{
    struct screen *d = &screens[SCREEN_MAIN];
    struct viewport band = *list->parent[SCREEN_MAIN];
    struct viewport *vp = &band;
    vp->font = agents_body_font;
    char secs[12] = "";
    const char *text = talk_note;
    unsigned bg = ui_theme_rgb565(UI_COLOR_MODAL_BG);
    unsigned muted = color_fit_contrast(ui_theme_rgb565(UI_COLOR_MUTED), bg, 450);
    unsigned colour = ui_theme_rgb565(UI_COLOR_MODAL_FG), mic = muted;
    unsigned err = color_fit_contrast(DOT_CORAL, bg, 450);
    char listening[32];
    /* Not held, a recording is shown only while V says one runs */
    bool recording = talk_held ? !talk_failed && model.voice != 'i' :
                     !talk_req[0] && (!talk_note[0] ||
                                      TIME_AFTER(current_tick, talk_until));
    if (recording)
    {
        text = "Talking…";
        mic = err;
        snprintf(secs, sizeof secs, "%lds", (current_tick - talk_since) / HZ);
        if (talk_toggle)
        {
            snprintf(listening, sizeof listening, "Listening… %s", secs);
            text = listening;
            secs[0] = '\0';
        }
    }
    else if (talk_held && !talk_failed)
        text = "Stopped on the computer";
    else if (!talk_held && talk_req[0] && !(talk_t3 && talk_failed))
    {
        text = "Sending…";
        colour = muted;
    }
    else if (talk_failed)
        mic = colour = err;
    d->set_viewport(vp);
    unsigned fg = vp->fg_pattern;
    int y = vp->height - TALK_BAND;
    int h = font_get(vp->font)->height;
    d->set_foreground(bg);
    d->fillrect(0, y, vp->width, TALK_BAND);
    d->set_drawmode(DRMODE_FG);
    d->set_foreground(ui_theme_rgb565(UI_COLOR_MODAL_BORDER));
    d->hline(0, vp->width - 1, y);
    if (talk_failed)
    {
        d->set_foreground(err);
        d->fillrect(0, y + 1, 3, TALK_BAND - 1);
    }
    draw_mic(d, 8, y + (TALK_BAND - 17) / 2, mic);
    const char *controls = recording && talk_toggle ? "SEL Send · MENU Cancel" : secs;
    int sw = controls[0] ? text_w(d, controls) : 0;
    char fitted[86];
    text_at(d, 28, y + (TALK_BAND - h) / 2, colour,
            fit(fitted, sizeof fitted, text, vp->width - 28 - sw - 16));
    if (controls[0])
        text_at(d, vp->width - 8 - sw, y + (TALK_BAND - h) / 2, muted, controls);
    d->set_drawmode(DRMODE_SOLID);
    d->set_foreground(fg);
    d->update_viewport_rect(0, y, vp->width, TALK_BAND);
    d->set_viewport(NULL);
    talk_shown = true;
}

/* What vd is about: the selected workspace or agent, or the question's
 * pane; "-" for none */
static void talk_target(struct gui_synclist *list, char *buf, size_t size)
{
    int sel = gui_synclist_get_sel_pos(list);
    const struct row *r = sel >= 0 && sel < nrows ? &rows[sel] : NULL;
    if (view == VIEW_QUESTION || view == VIEW_INPUT)
        snprintf(buf, size, "%s", question_pane);
    else if (view == VIEW_DETAIL)
        snprintf(buf, size, "%s", detail_pane);
    else if (r && r->kind == ROW_SPACE)
        snprintf(buf, size, "w:%s", r->id);
    else if (r && r->kind == ROW_AGENT)
        snprintf(buf, size, "%s", r->id);
    else if (r && r->kind == ROW_ALERT && model.nquestions)
        snprintf(buf, size, "%s", model.questions[0].pane);
    else
        snprintf(buf, size, "-");
}

static void talk_result(const char *text, bool failed)
{
    snprintf(talk_note, sizeof talk_note, "%s", text);
    talk_failed = failed;
    talk_discarded = false;
    talk_until = current_tick + (failed ? 5 * HZ / 2 : 2 * HZ);
}

/* Play pressed: start recording on the computer */
static void start_talk(struct gui_synclist *list, bool remote)
{
    char target[24];
    if (talk_held || view == VIEW_ACTIONS || view == VIEW_CONFIRM)
        return;
    if (!mutating_ready())
        return;
    if (nout >= OUTBOX - 1)
        return; /* Keep one slot for vu/vc while recording. */
    char *slot = outbox_slot();
    if (!slot)
        return;
    talk_target(list, target, sizeof target);
    take_req(talk_req, sizeof talk_req);
    talk_t3 = t3_mode() || is_t3(target) || !strncmp(target, "w:t3", 4);
    strcpy(t3_talk_start, talk_t3 ? talk_req : "");
    snprintf(slot, sizeof outbox[0], "vd\t%s\t%s", talk_req, target);
    talk_req_up = false;
    talk_held = true;
    talk_remote = remote;
    talk_select = talk_toggle = false;
    talk_discarded = false;
    if (!agents_fruit_nav())
        agents_shortcut_talking(true);
    talk_since = current_tick;
    talk_until = current_tick + TALK_REPLY;
    talk_note[0] = '\0';
    talk_failed = false;
    model.voice = 0;
}

/* Discard clears the band immediately; a normal stop submits the recording. */
static void finish_talk(bool discard)
{
    if (!talk_held)
        return;
    talk_held = talk_toggle = talk_select = false;
    talk_discarded = discard;
    if (discard)
    {
        model.voice = 0;
        talk_note[0] = '\0';
        talk_failed = false;
    }
    agents_shortcut_talking(false);
    char *slot = podlink_state() == PODLINK_CONNECTED ? outbox_slot() : NULL;
    if (!slot)
    {
        talk_req[0] = '\0';
        return;
    }
    take_req(talk_req, sizeof talk_req);
    snprintf(slot, sizeof outbox[0], "%s\t%s", discard ? "vc" : "vu", talk_req);
    talk_req_up = !discard;
    talk_until = current_tick + TALK_REPLY;
}

static bool talk_is_short(void)
{
    if (talk_select || talk_toggle) return false;
    unsigned long limit = talk_remote ? (3 * HZ + 9) / 10 :
                                       (3 * HZ + 19) / 20;
    return (unsigned long)current_tick - (unsigned long)talk_since < limit;
}

static void agents_playback(int button)
{
    if (!cider_wps_action(button))
        default_event_handler(button);
}

static void finish_native_play(void)
{
    bool tap = native_play_pending &&
               (unsigned long)current_tick - native_play_at < (3 * HZ + 19) / 20;
    native_play_pending = false;
    if (talk_held && !talk_select && !talk_toggle)
        finish_talk(talk_is_short());
    if (tap)
    {
        talk_note[0] = '\0';
        talk_failed = false;
        agents_playback(BUTTON_MULTIMEDIA_PLAYPAUSE);
    }
}

static void forget_keys(void)
{
    key_req[0] = '\0';
    key_runs.nruns = 0;
    wheel.dir = 0;
}

/* Send the oldest waiting run when no k is in flight. A run waits on while
 * the outbox is full, and in the detail view until its pane is focused. */
static void pump_keys(void)
{
    if (key_req[0] && TIME_AFTER(current_tick, key_sent + KEY_LOST))
        key_req[0] = '\0';
    if (key_req[0] || !key_runs.nruns)
        return;
    if ((view == VIEW_DETAIL || view == VIEW_INPUT) && !detail_ready)
        return;
    char *slot = outbox_slot();
    struct hr_key_run run;
    if (!slot || !hr_keys_take(&key_runs, &run))
        return;
    take_req(key_req, sizeof key_req);
    if (run.count == 1)
        snprintf(slot, sizeof outbox[0], "k\t%s\t%s", key_req, run.key);
    else
        snprintf(slot, sizeof outbox[0], "k\t%s\t%s\t%d",
                 key_req, run.key, run.count);
    key_sent = current_tick;
}

/* One key tap on the computer, from the usage or detail view: bs, left,
 * right or enter. A tap past the runs' caps is dropped. */
static void send_key(const char *key)
{
    if ((view == VIEW_DETAIL || view == VIEW_INPUT) && is_t3(detail_pane))
        return;
    if (podlink_state() != PODLINK_CONNECTED || !model.hello)
    {
        talk_result("No link to the computer", true);
        return;
    }
    hr_keys_add(&key_runs, key);
    pump_keys();
}

/* An R for k: silent when it worked, its reason in the band when not. The
 * computer's busy and rate refusals are not failures and stay silent.
 * Returns false for any other request's R. */
static bool take_key_result(void)
{
    if (!key_req[0] || strcmp(model.result_req, key_req))
        return false;
    model.result_new = false;
    key_req[0] = '\0';
    if (!model.result_ok && strcmp(model.result_text, "Key busy") &&
        strcmp(model.result_text, "Key rate limit"))
        talk_result(model.result_text, true);
    pump_keys();
    return true;
}

/* An R for fw, fp or ft: silent when it worked, its reason in the band
 * when not. The detail view's fp lets the waiting keys go, or on failure
 * drops them. Returns false for any other request's R. */
static bool take_focus_result(void)
{
    if (!focus_req[0] || strcmp(model.result_req, focus_req))
        return false;
    model.result_new = false;
    focus_req[0] = '\0';
    if (!model.result_ok && strcmp(model.result_text, "Focus superseded"))
        talk_result(model.result_text, true);
    if (focus_for_detail)
    {
        detail_ready = model.result_ok;
        if (detail_ready)
            pump_keys();
        else
            key_runs.nruns = 0;
    }
    return true;
}

/* tc 1 daemons omit quick choices for T3 approvals. Only map the
 * standard, unambiguous labels; other policies stay in All options. */
static int quick_choice(const struct hr_question *q, bool deny)
{
    if (!q || q->multi)
        return 0;
    int explicit = deny ? q->no : q->yes;
    if (explicit && !is_t3(q->id))
        return explicit;
    if (explicit)
        for (int i = 0; i < q->noptions; i++)
            if (q->options[i].number == explicit && !q->options[i].custom)
                return explicit;
    if (strncmp(q->id, "t3a", 3))
        return 0;
    for (int i = 0; i < q->noptions; i++)
    {
        const char *label = q->options[i].label;
        if (q->options[i].custom)
            continue;
        if (deny ? (!strcasecmp(label, "Decline") || !strcasecmp(label, "Deny")) :
                   (!strcasecmp(label, "Approve") || !strcasecmp(label, "Allow once") ||
                    !strcasecmp(label, "Allow")))
            return q->options[i].number;
    }
    return 0;
}

/* Focus herdr workspaces, panes and T3 threads as the wheel moves. */
static void want_focus(struct gui_synclist *list)
{
    int sel = gui_synclist_get_sel_pos(list);
    const struct row *r = sel >= 0 && sel < nrows ? &rows[sel] : NULL;
    bool focusable = r && ((view == VIEW_SPACES && r->kind == ROW_SPACE &&
                            !is_t3(r->id)) ||
                           (view == VIEW_AGENTS && r->kind == ROW_AGENT));
    snprintf(focus_want, sizeof focus_want, "%s", focusable ? r->id : "");
    focus_at = current_tick + (is_t3(focus_want) ? HZ / 10 : 0);
}

/* Forget the wanted and the done focus: the view has changed */
static void reset_focus(void)
{
    focus_want[0] = focus_done[0] = '\0';
    focus_at = current_tick;
}

/* Send the wanted focus when no request is out and Play is up with its
 * result in */
static void send_wanted_focus(void)
{
    if (!focus_want[0] || TIME_BEFORE(current_tick, focus_at) ||
        podlink_state() != PODLINK_CONNECTED || !model.hello ||
        link_stale() || t3_unavailable() || focus_req[0] || question_id[0] ||
        talk_held || talk_req[0])
        return;
    if (strcmp(focus_want, focus_done))
    {
        bool for_detail = view == VIEW_DETAIL || view == VIEW_ACTIONS ||
                          view == VIEW_CONFIRM;
        if (!queue_focus(view == VIEW_SPACES ? "fw" : "fp", focus_want,
                         for_detail))
            return;
        snprintf(focus_done, sizeof focus_done, "%s", focus_want);
    }
    focus_want[0] = '\0';
}

/* An R for vd or vu. Returns false for any other request's R. */
static bool take_talk_result(void)
{
    if (t3_talk_start[0] && !strcmp(model.result_req, t3_talk_start))
    {
        t3_talk_start[0] = '\0';
        if (!strcmp(model.result_req, talk_req))
            talk_req[0] = '\0';
        model.result_new = false;
        if (!model.result_ok && !talk_discarded)
        {
            talk_held = talk_toggle = talk_select = false;
            agents_shortcut_talking(false);
            talk_result(!strcmp(model.result_text, "Voice cannot reach T3") ?
                        "Voice unavailable" : model.result_text, true);
        }
        return true;
    }
    if (!talk_req[0] || strcmp(model.result_req, talk_req))
        return false;
    model.result_new = false;
    talk_req[0] = '\0';
    if ((!model.result_ok || talk_req_up) && !(talk_t3 && talk_failed))
        talk_result(model.result_text, !model.result_ok);
    return true;
}

/* Scrim gives the agents view a shorter list, with the selected row's
 * description (%LD) in a band under it, by switching the UI viewport on
 * the bar's own render, which reads %LD from the list's last draw; and the
 * list takes a switched viewport through a theme toggle, whose render finds
 * no list drawn and so switches back, until the next render and draw. A
 * list drawn once, before the bar has chosen, has rows under the band, and
 * their dots are drawn over its text at every frame. So the draw that is
 * seen is the third: two unseen draws and two renders bring the bar and
 * the list to agree, the way a wheel step after the band appears does. */
static void settle_viewport(struct gui_synclist *list)
{
    gui_synclist_inhibit_flush(true);
    gui_synclist_draw(list);
    sb_update(SCREEN_MAIN, true);
    gui_synclist_draw(list);
    sb_update(SCREEN_MAIN, true);
    gui_synclist_inhibit_flush(false);
    gui_synclist_layout(list);
}

static void update_volume_mode(void)
{
#ifdef IPOD_ACCESSORY_PROTOCOL
    bool want = view != VIEW_USAGE || !usage_legacy_keys();
    if (want != volume_paging)
    {
        iap_agents_volume_enable(want);
        volume_paging = want;
    }
    iap_agents_volume_host_keys(!agents_fruit_nav() &&
        ((view == VIEW_USAGE && usage_legacy_keys()) ||
         (view == VIEW_DETAIL && !is_t3(detail_pane))));
#endif
}

/* Rebuild and draw, selecting the first row that can be chosen from select
 * on. In the question view the rows above it stay in view as far as they
 * fit, so a question is read from its start; in a list the selection goes
 * to the middle. top, when not -1, is the row to keep at the top instead. */
static void show_at(struct gui_synclist *list, int select, int top)
{
    update_volume_mode();
    build_rows(list);
    gui_synclist_set_nb_items(list, nrows);
    gui_synclist_set_title(list, view_title(), Icon_NOICON);
    if (select < 0)
    {
        select = 0;
        for (int i = 0; i < nrows; i++)
            if (rows[i].kind == ROW_AGENT || rows[i].kind == ROW_SPACE)
            {
                select = i;
                break;
            }
    }
    while (select < nrows - 1 && !selectable(select))
        select++;
    if (select >= nrows)
        select = 0;
    gui_synclist_select_item(list, select);
    if (view == VIEW_ALERT || view == VIEW_ACTIONS ||
        view == VIEW_CONFIRM || view == VIEW_INPUT)
    {
        draw_question_overlay(list, modal_open);
        modal_open = false;
        return;
    }
    settle_viewport(list);
    int lines = gui_synclist_get_nb_lines(list, SCREEN_MAIN);
    if (top < 0)
        top = view == VIEW_QUESTION ? select - lines + 1 : select - lines / 2;
    gui_synclist_set_top_item(list, top);
    if (view == VIEW_USAGE)
        draw_usage(list);
    else if (view == VIEW_DETAIL)
        draw_detail(list);
    else
        gui_synclist_draw(list);
}

static void show(struct gui_synclist *list, int select)
{
    show_at(list, select, -1);
}

/* Rebuild the rows, keeping the selection on the same row when it is still
 * there: a status or alert row that comes or goes moves the rest. When the
 * row has gone the selection goes to the first row that can be chosen, never
 * to whatever took its place: on a question, that could be Cancel. */
static void show_same(struct gui_synclist *list)
{
    int sel = gui_synclist_get_sel_pos(list);
    if (sel < 0 || sel >= nrows || !selectable(sel))
    {
        show(list, 0);
        return;
    }
    struct row was = rows[sel];
    int top = list->start_item[SCREEN_MAIN];
    build_rows(list);
    int i = 0;
    while (i < nrows && (rows[i].kind != was.kind ||
                         (was.id[0] ? strcmp(rows[i].id, was.id) :
                                      rows[i].index != was.index)))
        i++;
    /* A row that has not moved keeps the view where the reader left it */
    if (i < nrows)
        show_at(list, i, i == sel ? top : -1);
    else
        show(list, view == VIEW_SPACES || view == VIEW_AGENTS ?
             MIN(sel, nrows - 1) : 0);
}

/* Rebuild and draw, selecting the row that stands for r when it is still
 * there, else the first */
static void show_row(struct gui_synclist *list, const struct row *r)
{
    build_rows(list);
    int i = 0;
    while (i < nrows && (rows[i].kind != r->kind ||
                         strcmp(rows[i].id, r->id)))
        i++;
    show(list, i < nrows ? i : -1);
}

static void enter_answer(struct gui_synclist *list);

/* The question view over the current view, which it goes back to, on the
 * row it was on */
static void open_question(struct gui_synclist *list,
                          const struct hr_question *q)
{
    ui_scene_cancel(&page_scene);
    int sel = gui_synclist_get_sel_pos(list);
    question_from = view;
    question_from_row = sel >= 0 && sel < nrows ? rows[sel] : (struct row){0};
    snprintf(question_saved_detail, sizeof question_saved_detail, "%s", detail_pane);
    question_choice = 0;
    question_custom = question_closing = question_resolving = false;
    question_next_until = 0;
    alert_at = current_tick;
    alert_choice = 2;
    alert_direction_ready = false;
    /* A SELECT already down belongs to the view that was just covered. */
    select_down = false;
    hr_popped_add(&popped, q->id);
    snprintf(question_pane, sizeof question_pane, "%s", q->pane);
    snprintf(question_id, sizeof question_id, "%s", q->id);
    snprintf(question_agent, sizeof question_agent, "%s", q->agent);
    snprintf(question_tab, sizeof question_tab, "%s",
             q->tab[0] ? q->tab : q->workspace);
    snprintf(question_line, sizeof question_line, "%s",
             q->nlines ? q->lines[0] : "Question");
    if (view == VIEW_SPACES || view == VIEW_AGENTS)
        focus_want[0] = '\0';
    if (!strncmp(q->id, "t3q", 3) && !(quick_choice(q, false) && quick_choice(q, true)))
        enter_answer(list);
    else
    {
        view = VIEW_ALERT;
        modal_open = true;
        show(list, 0);
    }
}

static void leave_question(struct gui_synclist *list)
{
    forget_keys();
    question_choice = 0;
    question_custom = question_closing = question_resolving = false;
    question_next_until = 0;
    if (strcmp(detail_pane, question_saved_detail))
    {
        snprintf(detail_pane, sizeof detail_pane, "%s", question_saved_detail);
        subscribe_detail(detail_pane);
        detail_ready = false;
        if ((question_from == VIEW_DETAIL || question_from == VIEW_ACTIONS ||
             question_from == VIEW_CONFIRM) && !is_t3(detail_pane))
            queue_focus("fp", detail_pane, true);
        if (question_from == VIEW_ACTIONS || question_from == VIEW_CONFIRM)
        {
            model.actions_valid = false;
            model.actions_target[0] = '\0';
            question_from = VIEW_DETAIL;
        }
    }
    view = question_from;
    question_id[0] = '\0';
    question_pane[0] = '\0';
    pending_req[0] = '\0';
    pending_answer = false;
    show_row(list, &question_from_row);
}

static void dismiss_question(void)
{
    if (pending_req[0] || question_resolving || strncmp(question_id, "t3q", 3) ||
        !current_question() || !mutating_ready())
        return;
    char args[16];
    snprintf(args, sizeof args, "%s\t-2", question_id);
    question_closing = queue_request("a", args);
}

/* A question that has not popped up yet does so now, the oldest first and
 * one at a time, unless Play is held or its result is still to come */
static void pop_question(struct gui_synclist *list)
{
    if (view == VIEW_QUESTION || view == VIEW_ALERT || view == VIEW_INPUT ||
        view == VIEW_ACTIONS || view == VIEW_CONFIRM || talk_held ||
        talk_req[0])
        return;
    const struct hr_question *q = hr_model_next_popup(&model, &popped);
    if (!q)
        return;
    hr_popped_add(&popped, q->id);
    backlight_on();
    if (!is_t3(q->pane))
        q = hr_model_question_for(&model, q->pane);
    open_question(list, q);
}

static void pan(const void *from, const void *to, int direction)
{
    struct ui_theme theme;
    ui_theme_snapshot(&theme);
    if (ui_scene_automatic())
        ui_scene_interrupt();
    ui_scene_capture_timed_page(&page_scene, from, to, true, direction,
                          theme.metric[UI_METRIC_STATUS_HEIGHT], ANIM_F(14));
}

static void slide(const void *from, const void *to, bool push)
{
    if (ui_scene_automatic())
        ui_scene_interrupt();
    ui_scene_capture_timed_page(&page_scene, from, to, push, 0, 0,
                          push ? ANIM_F(18) : ANIM_F(15));
}

static int tab_index(void)
{
    if (is_t3(space_id))
        return !strcmp(space_tab, "all");
    return MAX(0, hr_model_tab_index(&model, space_id, space_tab));
}

static void open_usage(struct gui_synclist *list)
{
    if (agents_fruit_nav())
        slide(&spaces_page, &usage_page, true);
    else
        pan(&spaces_page, &usage_page, -1);
    usage_scroll = 0;
    int sel = gui_synclist_get_sel_pos(list);
    usage_from = sel >= 0 && sel < nrows ? rows[sel] : (struct row){0};
    view = VIEW_USAGE;
    reset_focus();
    ask_usage();
    show(list, 0);
}

/* Back to the spaces view, on the row the usage view was opened from */
static void close_usage(struct gui_synclist *list)
{
    if (agents_fruit_nav())
        slide(&usage_page, &spaces_page, false);
    else
        pan(&usage_page, &spaces_page, 1);
    view = VIEW_SPACES;
    key_runs.nruns = 0;
    show_row(list, &usage_from);
}

/* Into a workspace: its first tab's agents, and the computer shows it */
static void enter_space(struct gui_synclist *list, const char *ws)
{
    slide(&spaces_page, &tab_page[0], true);
    snprintf(space_id, sizeof space_id, "%s", ws);
    strcpy(space_tab, is_t3(space_id) ? "active" : "");
    if (is_t3(space_id))
    {
        model.agents_valid = false;
        filter_loading = true;
    }
    subscribe(space_id, space_tab);
    if (!is_t3(space_id))
        queue_focus("fw", space_id, false);
    view = VIEW_AGENTS;
    reset_focus();
    show(list, -1);
}

/* Back to the spaces view, on the workspace's row */
static void close_space(struct gui_synclist *list)
{
    slide(&tab_page[tab_index()], &spaces_page, false);
    struct row r = { .kind = ROW_SPACE };
    snprintf(r.id, sizeof r.id, "%s", space_id);
    view = VIEW_SPACES;
    space_id[0] = space_tab[0] = '\0';
    subscribe("", "");
    reset_focus();
    show_row(list, &r);
}

/* LEFT or RIGHT in the agents view: the tab before or after, which the
 * computer shows at once; its agents come with the next A, and the
 * selection goes to the first of them */
static void select_tab(struct gui_synclist *list, int tab)
{
    if (is_t3(space_id))
        strcpy(space_tab, tab ? "all" : "active");
    else
    {
        char args[40];
        snprintf(space_tab, sizeof space_tab, "%s", model.tabs[tab].id);
        snprintf(args, sizeof args, "%s\t%s", space_id, space_tab);
        queue_focus("ft", args, false);
    }
    subscribe(space_id, space_tab);
    model.agents_valid = false;
    filter_loading = is_t3(space_id);
    reset_focus();
    show(list, -1);
}

static void switch_tab(struct gui_synclist *list, int dir)
{
    bool t3 = is_t3(space_id);
    int i = t3 ? tab_index() : hr_model_tab_index(&model, space_id, space_tab);
    if (i < 0 || i + dir < 0 || i + dir >= (t3 ? 2 : model.ntabs))
        return;
    pan(&tab_page[i], &tab_page[i + dir], dir);
    select_tab(list, i + dir);
}

static void back_from_tab(struct gui_synclist *list)
{
    int i = tab_index();
    if (i == 0 || is_t3(space_id))
        close_space(list);
    else
    {
        slide(&tab_page[i], &tab_page[0], false);
        select_tab(list, 0);
    }
}

/* Into an agent's detail: its pane's lines come with D, and its keys go
 * once the computer has shown the pane */
static void open_detail(struct gui_synclist *list, const struct hr_agent *a)
{
    slide(&tab_page[tab_index()], &detail_page, true);
    detail_scroll = detail_scroll_max = 0;
    detail_follow = detail_new = false;
    detail_wheel_at = current_tick - HZ;
    memset(&detail_layout, 0, sizeof detail_layout);
    detail_body_avail = 0;
    snprintf(detail_pane, sizeof detail_pane, "%s", a->pane);
    detail_agent = *a;
    detail_ready = false;
    task_text[0] = '\0';
    detail_shown_age = -1;
    input_top = 0;
    forget_keys();
    model.detail_valid = false;
    subscribe_detail(detail_pane);
    view = VIEW_DETAIL;
    reset_focus();
    if (is_t3(detail_pane))
        snprintf(focus_want, sizeof focus_want, "%s", detail_pane);
    else
        queue_focus("fp", detail_pane, true);
    show(list, 0);
}

/* Back to the agents view, on the agent's row */
static void close_detail(struct gui_synclist *list)
{
    slide(&detail_page, &tab_page[tab_index()], false);
    struct row r = { .kind = ROW_AGENT };
    snprintf(r.id, sizeof r.id, "%s", detail_pane);
    subscribe_detail("");
    forget_keys();
    detail_ready = false;
    detail_pane[0] = '\0';
    model.actions_target[0] = '\0';
    model.actions_valid = false;
    view = VIEW_AGENTS;
    reset_focus();
    show_row(list, &r);
}

/* Show the sheet only after its reply, so an older peer leaves RIGHT idle. */
static void request_actions(void)
{
    if (talk_held || talk_req[0] || action_req[0] || model.actions_target[0] ||
        !mutating_ready())
        return;
    char *slot = outbox_slot();
    if (!slot)
        return;
    strcpy(model.actions_target, detail_pane);
    model.actions_valid = false;
    actions_until = current_tick + 2 * HZ;
    snprintf(slot, sizeof outbox[0], "ks\t%s", detail_pane);
}

static void close_actions(struct gui_synclist *list)
{
    if (model.actions_valid)
        draw_question_overlay(list, -1);
    model.actions_target[0] = '\0';
    model.actions_valid = false;
    view = VIEW_DETAIL;
    show(list, 0);
}

static void run_action(struct gui_synclist *list, int choice, bool confirmed)
{
    if (!model.actions_valid || strcmp(model.actions_target, detail_pane) ||
        action_req[0] || choice < 0 || choice >= model.nactions ||
        !mutating_ready())
        return;
    const struct hr_action *a = &model.actions[choice];
    if (confirmed && (strcmp(a->id, confirm_action.id) ||
                       strcmp(a->label, confirm_action.label)))
    {
        close_actions(list);
        talk_result("Request expired", true);
        return;
    }
    if (confirm_action_required(a) && !confirmed)
    {
        confirm_action = *a;
        action_choice = choice;
        action_row = gui_synclist_get_sel_pos(list);
        confirm_selected = false;
        view = VIEW_CONFIRM;
        modal_open = true;
        show(list, action_row);
        return;
    }
    char *slot = outbox_slot();
    if (!slot)
        return;
    take_req(action_req, sizeof action_req);
    action_until = current_tick + FOCUS_REPLY;
    snprintf(slot, sizeof outbox[0], "act\t%s\t%s\t%s",
             action_req, detail_pane, a->id);
    close_actions(list);
}

/* Act on SELECT: enter the workspace, open the agent's detail or the
 * waiting question, or answer */
static void enter_answer(struct gui_synclist *list)
{
    if (!current_question() || pending_req[0]) return;
    if (!is_t3(question_pane))
    {
        forget_keys();
        snprintf(detail_pane, sizeof detail_pane, "%s", question_pane);
        detail_ready = false;
        subscribe_detail(detail_pane);
        queue_focus("fp", question_pane, true);
    }
    view = VIEW_QUESTION;
    show(list, 0);
}

static void answer_number(int number)
{
    const struct hr_question *q = current_question();
    if (!q || pending_req[0] || question_resolving ||
        (!is_t3(q->id) && (!detail_ready || focus_req[0])) ||
        !mutating_ready())
        return;
    char args[32];
    question_custom = false;
    for (int i = 0; i < q->noptions; i++)
        if (q->options[i].number == number)
            question_custom = q->options[i].custom;
    if (question_custom && is_t3(q->id))
        return;
    snprintf(args, sizeof args, "%s\t%d", q->id, number);
    if (queue_request("a", args))
        pending_answer = !q->multi || number == 0;
}

static void close_input(bool submit)
{
    if (!detail_ready || pending_req[0] || focus_req[0]) return;
    /* Review must include every queued edit before Enter can be sent. */
    if (submit && (key_req[0] || key_runs.nruns)) return;
    finish_talk(true);
    forget_keys();
    char args[32];
    snprintf(args, sizeof args, "%s\t%s", question_id, submit ? "enter" : "esc");
    question_closing = queue_request("qi", args);
}

static void alert_activate(struct gui_synclist *list, int choice)
{
    if (TIME_BEFORE(current_tick, alert_at + HZ / 4) || pending_req[0])
        return;
    const struct hr_question *q = current_question();
    if (!q)
        return;
    int number = choice == 2 ? 0 : quick_choice(q, choice == 1);
    if (!number)
    {
        draw_question_overlay(list, -1);
        enter_answer(list);
    }
    else if (is_t3(q->id))
        answer_number(number);
    else
    {
        question_choice = number;
        enter_answer(list);
    }
}

static void activate(struct gui_synclist *list)
{
    int sel = gui_synclist_get_sel_pos(list);
    if (!selectable(sel) || pending_req[0] || question_resolving)
        return;
    const struct row *r = &rows[sel];
    const struct hr_question *q;
    switch (r->kind)
    {
        case ROW_ALERT:
            open_question(list, &model.questions[0]);
            return;
        case ROW_SPACE:
            enter_space(list, model.ws[r->index].id);
            return;
        case ROW_AGENT:
            open_detail(list, &model.agents[r->index]);
            return;
        case ROW_RETRY:
            queue_handshake();
            return;
        case ROW_USAGE:
            open_usage(list);
            return;
        case ROW_FILTER:
            select_tab(list, !tab_index());
            return;
        case ROW_ANSWER_PENDING:
            q = hr_model_question_for(&model, detail_pane);
            if (q)
                open_question(list, q);
            return;
        case ROW_VOICE:
            close_actions(list);
            start_talk(list, false);
            talk_toggle = talk_held;
            return;
        case ROW_OPEN_COMPUTER:
        {
            if (action_req[0] || !mutating_ready())
                return;
            char *slot = outbox_slot();
            if (slot)
            {
                take_req(action_req, sizeof action_req);
                action_until = current_tick + FOCUS_REPLY;
                snprintf(slot, sizeof outbox[0], "fp\t%s\t%s", action_req, detail_pane);
                focus_want[0] = '\0';
                close_actions(list);
            }
            return;
        }
        case ROW_ACTION:
            run_action(list, r->index, false);
            return;
        case ROW_DISMISS:
            dismiss_question();
            return;
        case ROW_OPTION:
        case ROW_SUBMIT:
        {
            q = current_question();
            if (!q) return;
            answer_number(r->kind == ROW_SUBMIT ? 0 : q->options[r->index].number);
            show_same(list);
            return;
        }
    }
}

static void reset_source_view(void)
{
    ui_scene_cancel(&page_scene);
    view = VIEW_SPACES;
    space_id[0] = space_tab[0] = detail_pane[0] = '\0';
    question_id[0] = question_pane[0] = '\0';
    pending_req[0] = focus_req[0] = action_req[0] = '\0';
    pending_answer = question_resolving = question_custom = question_closing = false;
    detail_ready = filter_loading = received_projects = modal_open = false;
    select_down = native_play_pending = false;
    talk_req[0] = talk_note[0] = t3_talk_start[0] = '\0';
    talk_held = talk_toggle = talk_select = false;
    agents_shortcut_talking(false);
    reset_focus();
    forget_keys();
    memset(&popped, 0, sizeof popped);
}

/* Take whatever the link has. Returns true if anything on screen changed. */
static bool poll_link(void)
{
    bool changed = false;
    bool connected = podlink_state() == PODLINK_CONNECTED;
    uint32_t generation = podlink_generation();
    bool replaced = generation != cache_generation;
    if (replaced)
    {
        cache_valid = false;
        cache_generation = generation;
    }
    if (replaced || connected != was_connected)
    {
#ifdef HAVE_CRUMBS
        crumb(CR_AGENTS_STEP, 0x10 | connected);
#endif
        was_connected = connected;
        hr_model_link_lost(&model);
        memset(&popped, 0, sizeof popped);
        nout = 0;
        pending_req[0] = '\0';
        pending_answer = question_custom = question_closing = false;
        focus_req[0] = action_req[0] = '\0';
        reset_focus();
        /* The computer stops recording when the link goes */
        if (talk_held || talk_req[0])
            talk_result("Computer disconnected", true);
        talk_held = talk_toggle = talk_select = false;
        agents_background_disconnected();
        agents_shortcut_talking(false);
        talk_req[0] = '\0';
        t3_talk_start[0] = '\0';
        forget_keys();
        if (connected)
        {
            if (model.mode != HR_MODE_T3)
            {
                hr_model_reset(&model);
                reset_source_view();
            }
            queue_handshake();
            if (space_id[0])
                subscribe(space_id, space_tab);
            if (detail_pane[0])
            {
                detail_ready = !strcmp(detail_pane, question_pane) &&
                               is_t3(detail_pane);
                subscribe_detail(detail_pane);
                if (!is_t3(detail_pane))
                    queue_focus("fp", detail_pane, true);
            }
            if (view == VIEW_USAGE)
                ask_usage();
        }
        changed = true;
    }
    for (int i = 0; i < 16; i++)
    {
        struct podlink_message m;
        if (!podlink_poll(&m))
            break;
        if (m.type != PODLINK_TEXT)
            continue;
        int before = model.nquestions;
        bool had = question_pane[0] && current_question();
        bool health_known = model.t3_health_known, health_ok = model.t3_ok;
        bool workspaces_seen = model.workspaces_seen;
        enum hr_mode mode = model.mode;
        int type = hr_model_apply(&model, (const char *)m.data, m.length);
#ifdef HAVE_CRUMBS
        crumb(CR_AGENTS_STEP, 0x100 | (uint32_t)(type & 0xff));
#endif
        if (!type)
            continue;
        last_message = current_tick;
        if (type == 'H')
            link_seen = true;
        if (type == 'M' && mode != model.mode)
            reset_source_view();
        agents_background_update(&model, type, true);
        if (type == 'T' && (health_known != model.t3_health_known ||
                            health_ok != model.t3_ok))
            changed = true;
        /* Only what alters a row, the title or a drawn view is a change */
        if (type == 'W' || type == 'S')
            changed |= model.ws_shown_changed || workspaces_seen != model.workspaces_seen;
        else if (type != 'T' && type != 'R' && type != 'V' && type != 'H')
            changed = true;
        if (type == 'Q' && model.nquestions > before && !had)
            backlight_on();
        if (type == 'U' && model.usage.serial != usage_serial)
        {
            usage_serial = model.usage.serial;
            usage_at = current_tick;
        }
        if (type == 'V' && model.voice == 't' && !talk_held)
            talk_since = current_tick;
        if (type == 'D')
            detail_at = current_tick;
        /* The tab shown has gone: back to the first */
        if (type == 'B' && !is_t3(space_id) && space_tab[0] &&
            !strcmp(model.tabs_ws, space_id)
            && hr_model_tab_index(&model, space_id, space_tab) < 0)
        {
            space_tab[0] = '\0';
            subscribe(space_id, space_tab);
            model.agents_valid = false;
        }
        /* One result at a time: take_result reads it before the next */
        if (type == 'R' && !take_talk_result() && !take_key_result() &&
            !take_focus_result() && !take_action_result())
            break;
    }
    flush_outbox();
    return changed;
}

/* A command's result has come back: say so, and move on after an answer */
static void take_result(struct gui_synclist *list)
{
    if (pending_req[0] && TIME_AFTER(current_tick, pending_until))
    {
        pending_req[0] = '\0';
        pending_answer = question_closing = false;
        talk_result("No reply from computer", true);
        show_same(list);
    }
    if (!model.result_new)
        return;
    model.result_new = false;
    if (strcmp(model.result_req, pending_req))
        return;
    pending_req[0] = '\0';
    if (!model.result_ok)
        talk_result(model.result_text, true);
    else if (question_closing)
    {
        leave_question(list);
        return;
    }
    else if (question_custom)
    {
        view = VIEW_INPUT;
        model.detail_valid = false;
        input_top = 0;
        question_custom = false;
        show(list, 0);
        return;
    }
    else if (pending_answer)
    {
        question_resolving = true;
        question_next_until = current_tick + 3 * HZ / 2;
        talk_result(model.result_text, false);
    }
    question_custom = question_closing = false;
    pending_answer = false;
    show_same(list);
}

static void advance_question(struct gui_synclist *list)
{
    if (view != VIEW_ALERT && view != VIEW_QUESTION)
        return;
    const struct hr_question *q = current_question();
    if (question_next_until)
    {
        if (!q && !strncmp(question_id, "t3q", 3))
        {
            const struct hr_question *next = hr_model_question_for(&model, question_pane);
            if (next && !strncmp(next->id, "t3q", 3))
            {
                hr_copy(question_id, next->id, sizeof question_id);
                hr_popped_add(&popped, question_id);
                question_next_until = 0;
                question_resolving = false;
                view = VIEW_QUESTION;
                show(list, 0);
                return;
            }
            if (TIME_BEFORE(current_tick, question_next_until))
                return;
        }
        if (!TIME_BEFORE(current_tick, question_next_until))
        {
            leave_question(list);
            return;
        }
    }
    if (!q && !pending_req[0])
    {
        bool handled_elsewhere = !question_resolving && model.hello;
        leave_question(list);
        if (handled_elsewhere)
            talk_result("Handled on computer", false);
    }
}

/* A question result can arrive while the quick screen covers its page.
 * Keep that one result until the overlay closes; poll_link still drains
 * subsequent list/detail/voice traffic without drawing over the overlay. */
static struct
{
    bool ready, ok;
    char req[8], text[124];
} quickscreen_result;

static void quickscreen_keep_result(void)
{
    if (model.result_new && pending_req[0] &&
        !strcmp(model.result_req, pending_req))
    {
        quickscreen_result.ready = true;
        quickscreen_result.ok = model.result_ok;
        memcpy(quickscreen_result.req, model.result_req, sizeof model.result_req);
        memcpy(quickscreen_result.text, model.result_text, sizeof model.result_text);
        model.result_new = false;
    }
}

static void agents_quickscreen_poll(void)
{
    quickscreen_keep_result();
    poll_link();
    quickscreen_keep_result();
    pump_keys();
}

static int agents_quickscreen(struct gui_synclist *list, int action)
{
    ui_scene_cancel(&page_scene);
    native_play_pending = false;
    finish_talk(true);
    quickscreen_result.ready = false;
    long entered = current_tick;
#ifdef IPOD_ACCESSORY_PROTOCOL
    /* The same remote arrows now edit QS slots, never host keys or paging. */
    iap_agents_volume_enable(false);
    iap_agents_volume_host_keys(false);
    volume_paging = false;
#endif
    int ret = quick_screen_current(action, agents_quickscreen_poll);
    update_volume_mode();
    if (ret != QUICKSCREEN_IN_USB)
    {
        pending_until += current_tick - entered;
        if (quickscreen_result.ready)
        {
            model.result_new = true;
            model.result_ok = quickscreen_result.ok;
            memcpy(model.result_req, quickscreen_result.req, sizeof model.result_req);
            memcpy(model.result_text, quickscreen_result.text, sizeof model.result_text);
            take_result(list);
        }
        show_same(list);
    }
    return ret;
}

static bool agents_play_down(void)
{
    int mask = BUTTON_PLAY;
#ifdef IPOD_ACCESSORY_PROTOCOL
    mask |= BUTTON_RC_PLAY;
#endif
    return (button_status() & mask) != 0;
}

static bool agents_menu_exit(int action)
{
    if (action != ACTION_STD_MENU && action != ACTION_STD_CANCEL)
        return false;
    int raw;
    get_action_statuscode(&raw);
    int menu = BUTTON_MENU;
#ifdef IPOD_ACCESSORY_PROTOCOL
    menu |= BUTTON_RC_MENU;
#endif
    return (raw & menu) != 0;
}

static void agents_usb_exit(int result)
{
#ifdef HAVE_USB_CONFIRM
    if (result == AGENTS_EXIT_MENU) usb_confirm_reopen();
#else
    (void)result;
#endif
}

static bool voice_view(void)
{
    return view == VIEW_SPACES || view == VIEW_AGENTS || view == VIEW_DETAIL ||
           view == VIEW_USAGE;
}

static void activate_select(struct gui_synclist *list)
{
    if (talk_toggle)
    {
        finish_talk(false);
        return;
    }
    if (talk_held)
        return;
    switch (view)
    {
        case VIEW_SPACES:
        {
            int sel = gui_synclist_get_sel_pos(list);
            if (t3_unavailable() &&
                (sel < 0 || sel >= nrows || rows[sel].kind != ROW_USAGE))
            {
                queue_handshake();
                return;
            }
            activate(list);
            break;
        }
        case VIEW_USAGE:
            if (!usage_legacy_keys())
                ask_usage();
            else
                send_key("enter");
            break;
        case VIEW_DETAIL:
            if (is_t3(detail_pane) || agents_fruit_nav())
                request_actions();
            else
                send_key("enter");
            break;
        case VIEW_ALERT:
            alert_activate(list, agents_fruit_nav() ? alert_choice : 2);
            break;
        case VIEW_CONFIRM:
            if (confirm_selected)
                run_action(list, action_choice, true);
            else
            {
                draw_question_overlay(list, -1);
                view = VIEW_ACTIONS;
                show(list, action_row);
            }
            break;
        case VIEW_INPUT:
            if (!talk_req[0])
                close_input(true);
            break;
        default:
            activate(list);
            break;
    }
}

static void select_long(struct gui_synclist *list)
{
    select_held = true;
    if (select_view != view || talk_toggle)
        return;
    agents_shortcut_commit_select();
    if (agents_fruit_nav() && voice_view())
    {
        start_talk(list, false);
        talk_select = talk_held;
    }
    else if (!agents_fruit_nav() && view == VIEW_DETAIL)
    {
        if (is_t3(detail_pane))
        {
            const struct hr_question *q = hr_model_question_for(&model, detail_pane);
            if (q)
                open_question(list, q);
            else
                request_actions();
        }
        else
            send_key("enter");
    }
}

static void scroll_panel(struct gui_synclist *list, int dir, bool page)
{
    if (view == VIEW_DETAIL && is_t3(detail_pane))
    {
        bool fast = TIME_BEFORE(current_tick, detail_wheel_at + WHEEL_FAST);
        int step = page ? detail_page_lines : fast ? 3 : 1;
        detail_wheel_at = current_tick;
        detail_scroll = MAX(0, MIN(detail_scroll_max, detail_scroll + dir * step));
        detail_follow = detail_scroll == detail_scroll_max;
        draw_detail(list);
    }
    else if (view == VIEW_USAGE && !usage_legacy_keys())
    {
        int height = list->parent[SCREEN_MAIN]->height;
        int maximum = MAX(0, HR_USAGE_SVCS * USE_BLOCK + USE_LINE + 3 - height);
        usage_scroll = MAX(0, MIN(maximum, usage_scroll + dir * (page ? height : USE_LINE)));
        draw_usage(list);
    }
    else if (view == VIEW_USAGE || view == VIEW_DETAIL)
    {
        const char *key = hr_wheel_step(&wheel, current_tick, dir, WHEEL_FAST);
        if (key)
            send_key(key);
    }
    else if (view == VIEW_ACTIONS)
    {
        int sel = gui_synclist_get_sel_pos(list);
        show(list, MAX(0, MIN(sel + dir, nrows - 1)));
    }
    else if (view == VIEW_CONFIRM)
    {
        confirm_selected = !confirm_selected;
        draw_question_overlay(list, false);
    }
    else if (view == VIEW_ALERT && agents_fruit_nav())
    {
        if (quick_choice(current_question(), false) && quick_choice(current_question(), true))
            alert_choice = MAX(0, MIN(alert_choice + dir, 2));
        draw_question_overlay(list, false);
    }
}

static void horizontal_button(struct gui_synclist *list, int dir, bool held)
{
    switch (view)
    {
        case VIEW_SPACES:
            if (dir < 0 && !held)
                open_usage(list);
            break;
        case VIEW_USAGE:
            if (dir > 0 && !held)
                close_usage(list);
            else if (dir < 0 && usage_legacy_keys())
                send_key("bs");
            break;
        case VIEW_AGENTS:
            if (!held)
                switch_tab(list, dir);
            break;
        case VIEW_DETAIL:
            if (is_t3(detail_pane))
            {
                if (held)
                {
                    detail_scroll = dir < 0 ? 0 : detail_scroll_max;
                    detail_follow = dir > 0;
                    draw_detail(list);
                }
                else
                    scroll_panel(list, dir, true);
            }
            else if (dir < 0)
                send_key("bs");
            else if (!held)
                request_actions();
            break;
        case VIEW_ALERT:
            if (alert_direction_ready && !held && quick_choice(current_question(), false) &&
                quick_choice(current_question(), true))
                alert_activate(list, dir < 0 ? 0 : 1);
            alert_direction_ready = false;
            break;
        case VIEW_CONFIRM:
            if (!held)
            {
                confirm_selected = dir > 0;
                draw_question_overlay(list, false);
            }
            break;
        case VIEW_INPUT:
            if (dir < 0 && detail_ready && !question_closing)
                send_key("bs");
            break;
        default:
            break;
    }
}

void agents_screen_shortcut_entry(void)
{
    shortcut_entry = true;
}

static int agents_run(void);
int agents_screen(void)
{
    if (!agents_nav_enter(AGENTS_NAV_AGENTS)) return AGENTS_EXIT_DEFERRED;
    int result = agents_run();
    agents_nav_leave(AGENTS_NAV_AGENTS);
    return result;
}

static int agents_run(void)
{
    struct gui_synclist list;
    int action, ret = AGENTS_EXIT_OTHER;
    char buf[48];

#ifdef HAVE_CRUMBS
    crumb(CR_AGENTS_IN, 0);
#endif
    bool from_shortcut = shortcut_entry;
    shortcut_entry = false;
    agents_foreground = true;
    if (!podlink_open_agents(background_link))
    {
        agents_foreground = false;
        splash(HZ * 3, "Unplug USB, then open Agents");
        return AGENTS_EXIT_OTHER;
    }
    bool preloaded = cache_valid && !agents_visited && model.hello &&
                     model.mode == HR_MODE_T3 &&
                     podlink_state() == PODLINK_CONNECTED;
    link_seen = cache_valid && model.hello;
    bool resume = cache_valid && agents_visited && model.mode == HR_MODE_T3 &&
                  cache_generation == podlink_generation() &&
                  podlink_state() == PODLINK_CONNECTED;
    struct agents_background_target target;
    bool deep_link = from_shortcut && agents_background_take_target(&target);
    agents_background_enter();
#ifdef HAVE_CRUMBS
    crumb(CR_AGENTS_STEP, 1 | (uint32_t)resume << 8);
#endif
    if (!resume)
    {
        if (!preloaded)
            hr_model_reset(&model);
        view = VIEW_SPACES;
        space_id[0] = space_tab[0] = detail_pane[0] = '\0';
        question_id[0] = '\0';
        question_choice = 0;
        question_custom = question_closing = false;
        question_pane[0] = pending_req[0] = focus_req[0] = action_req[0] = '\0';
        pending_answer = question_resolving = detail_ready = false;
        question_next_until = 0;
        was_connected = preloaded && agents_background_handshake_complete();
        detail_shown_age = -1;
        task_text[0] = '\0';
        reset_focus();
        talk_held = talk_shown = talk_discarded = false;
        agents_shortcut_talking(false);
        talk_req[0] = talk_note[0] = t3_talk_start[0] = '\0';
        forget_keys();
        nout = 0;
        if (!preloaded)
            last_message = current_tick;
        usage_serial = model.usage.serial;
        usage_scroll = detail_scroll = detail_scroll_max = 0;
        memset(&detail_layout, 0, sizeof detail_layout);
        detail_body_avail = 0;
        detail_page_lines = 1;
    }
    cache_generation = podlink_generation();
    if (was_connected)
    {
        queue_handshake();
        if (space_id[0])
            subscribe(space_id, space_tab);
        if (detail_pane[0])
            subscribe_detail(detail_pane);
        if (view == VIEW_USAGE)
            ask_usage();
        flush_outbox();
    }
    agents_visited = true;
    received_projects = model.nws > 0;
    select_held = select_down = false;

    FOR_NB_SCREENS(i)
        viewportmanager_theme_enable(i, true, NULL);
    native_play_pending = false;
#ifdef IPOD_ACCESSORY_PROTOCOL
    volume_paging = false;
    update_volume_mode();
#endif
    push_current_activity(ACTIVITY_AGENTS);
    int body_font = ui_theme_current_font_load(UI_FONT_BODY, global_settings.glyphs_to_cache);
    agents_body_font = body_font >= 0 ? body_font : FONT_UI;
    /* Play is held to talk here, and held long enough it would power off */
    bool poweroff = button_get_sw_poweroff_state();
    button_set_sw_poweroff_state(false);
    gui_synclist_init(&list, row_name, &list, false, 1, NULL);
    gui_synclist_set_row_callbacks(&list, NULL, row_value, NULL);
    gui_synclist_set_description_callback(&list, row_description);
    gui_synclist_set_voice_callback(&list, speak_row);
    gui_synclist_set_status_callback(&list, draw_dot);
    if (resume)
    {
        show_row(&list, &cache_row);
        gui_synclist_set_top_item(&list, cache_top);
        if (view == VIEW_USAGE) draw_usage(&list);
        else if (view == VIEW_DETAIL) draw_detail(&list);
        else show_same(&list);
    }
    else
        show(&list, -1);
    if (deep_link)
    {
        if (target.question[0])
        {
            for (int i = 0; i < model.nquestions; i++)
                if (!strcmp(target.question, model.questions[i].id))
                {
                    if (view == VIEW_ALERT || view == VIEW_QUESTION || view == VIEW_INPUT)
                        leave_question(&list);
                    open_question(&list, &model.questions[i]);
                    break;
                }
        }
        else if (target.project[0] && target.thread[0])
        {
            if (view == VIEW_ALERT || view == VIEW_QUESTION || view == VIEW_INPUT)
                leave_question(&list);
            if (view == VIEW_USAGE)
                close_usage(&list);
            enter_space(&list, target.project);
            struct hr_agent a = {0};
            hr_copy(a.pane, target.thread, sizeof a.pane);
            hr_copy(a.name, target.thread_title, sizeof a.name);
            a.status = target.status;
            open_detail(&list, &a);
            ui_scene_cancel(&page_scene);
        }
    }

    long frame_at = current_tick;
    long redraw_at = current_tick + HZ;
    const char *shown_trouble = NULL;
    bool done = false, left_held = false, right_held = false;
    while (!done)
    {
        enum agents_nav_screen nav = agents_nav_pending();
        if (nav == AGENTS_NAV_AGENTS) agents_nav_take(nav);
        else if (nav != AGENTS_NAV_NONE)
        {
            finish_talk(true);
            ret = nav == AGENTS_NAV_WPS ? AGENTS_SWITCH_WPS : AGENTS_SWITCH_COVERS;
            break;
        }
        bool changed = poll_link();
        pump_keys();
        take_result(&list);
        advance_question(&list);
        if (!received_projects && model.nws)
        {
            received_projects = true;
            if (view == VIEW_SPACES)
                show(&list, -1);
        }
        if (view == VIEW_AGENTS && filter_loading && model.agents_valid &&
            !strcmp(model.agents_ws, space_id))
        {
            filter_loading = false;
            show(&list, -1);
        }
        if (view == VIEW_QUESTION && question_choice && detail_ready && !focus_req[0])
        {
            answer_number(question_choice);
            question_choice = 0;
        }
        if (view == VIEW_INPUT)
        {
            const struct hr_question *replacement = hr_model_question_for(&model, question_pane);
            bool stale = replacement && strcmp(replacement->id, question_id);
            if (stale ||
                podlink_state() != PODLINK_CONNECTED)
            {
                finish_talk(true);
                leave_question(&list);
            }
        }
        pop_question(&list);
        if (view == VIEW_DETAIL && model.actions_target[0])
        {
            if (model.actions_valid && !talk_held && !talk_req[0])
            {
                forget_keys();
                view = VIEW_ACTIONS;
                modal_open = true;
                show(&list, 0);
            }
            else if (!model.actions_valid &&
                     TIME_AFTER(current_tick, actions_until))
            {
                model.actions_target[0] = '\0';
                talk_result("Actions unavailable", true);
            }
        }
        else if ((view == VIEW_ACTIONS || view == VIEW_CONFIRM) &&
                 !model.actions_valid)
            close_actions(&list);
        if (action_req[0] && TIME_AFTER(current_tick, action_until))
        {
            action_req[0] = '\0';
            talk_discarded = false;
            talk_result("No reply from computer", true);
        }
        signal_usage_view(view == VIEW_USAGE ? 1 : view == VIEW_DETAIL ? 2 : 0, false);
        /* Status rows count seconds, so they redraw once a second */
        const char *t = trouble(buf, sizeof buf);
        if (view == VIEW_USAGE)
        {
            /* Its times count down once a second, and it goes stale with age */
            if (TIME_AFTER(current_tick, usage_next))
                ask_usage();
            if (changed || t != shown_trouble ||
                TIME_AFTER(current_tick, redraw_at))
            {
                shown_trouble = t;
                redraw_at = current_tick + HZ;
                draw_usage(&list);
            }
        }
        else if (changed || t != shown_trouble ||
                 (t && TIME_AFTER(current_tick, redraw_at)) ||
                 (view == VIEW_DETAIL && detail_shown_age >= 0 &&
                  detail_age() / 60 != detail_shown_age))
        {
            /* The detail's time in state is drawn again each minute */
            shown_trouble = t;
            redraw_at = current_tick + HZ;
            if (view == VIEW_DETAIL)
                draw_detail(&list);
            else
                show_same(&list);
        }

        /* Only the dots, and the detail's task line, are drawn again for a
         * frame, and only while one in view moves and the screen can be
         * seen */
        if (TIME_AFTER(current_tick, frame_at))
        {
            frame_at = current_tick + ANIM_TICKS - 1;
            if (is_backlight_on(true) && view == VIEW_DETAIL)
                tick_task(&list);
            else if (is_backlight_on(true) && animating(&list))
                gui_synclist_draw_status(&list);
        }

        /* A release the keymap did not see still ends the hold. A failure
         * stays in view while Play is held, and for a moment after. */
        bool play_down = agents_play_down();
        if ((native_play_pending || (talk_held && !talk_select && !talk_toggle)) &&
            !play_down)
            finish_native_play();
        if (select_down && !select_held && !select_remote &&
            (button_status() & BUTTON_SELECT) &&
            TIME_AFTER(current_tick, select_at + 3 * HZ / 10 - 1))
            select_long(&list);
        if (talk_held && talk_select && !select_remote && !(button_status() & BUTTON_SELECT))
            finish_talk(false);
        if (talk_req[0] && TIME_AFTER(current_tick, talk_until))
        {
            talk_req[0] = '\0';
            talk_result("No reply from computer", true);
        }
        if (focus_req[0] && TIME_AFTER(current_tick, focus_until))
        {
            focus_req[0] = '\0';
            talk_result("No reply from computer", true);
            if (focus_for_detail)
                key_runs.nruns = 0;
        }
        send_wanted_focus();
        /* The band is drawn over whatever the list drew last; once it has
         * gone the list is drawn again beneath */
        if (talk_wanted())
            draw_talk(&list);
        else if (talk_shown)
        {
            talk_shown = false;
            if (view == VIEW_USAGE)
                draw_usage(&list);
            else if (view == VIEW_DETAIL)
                draw_detail(&list);
            else
                show_same(&list);
        }

        int before = gui_synclist_get_sel_pos(&list);
        int before_top = list.start_item[SCREEN_MAIN];
        if (page_scene.handle > 0)
        {
            bool dirty = ui_take_dirty(SCREEN_MAIN);
            if (page_scene.capturing)
                ui_scene_start(&page_scene);
            else
                ui_scene_update(&page_scene, dirty);
        }
        int timeout = ui_scene_timeout(&page_scene, ANIM_TICKS);
        if (view == VIEW_USAGE || view == VIEW_DETAIL || view == VIEW_ALERT ||
            view == VIEW_INPUT || view == VIEW_ACTIONS || view == VIEW_CONFIRM)
        {
            action = get_action(CONTEXT_AGENTS, timeout);
            int dir = action == ACTION_STD_PREV || action == ACTION_STD_PREVREPEAT ? -1 :
                      action == ACTION_STD_NEXT || action == ACTION_STD_NEXTREPEAT ? 1 : 0;
            if (dir)
            {
                bool page = false;
#ifdef IPOD_ACCESSORY_PROTOCOL
                int raw;
                get_action_statuscode(&raw);
                page = !agents_fruit_nav() &&
                       (raw & (BUTTON_RC_VOL_UP | BUTTON_RC_VOL_DOWN));
#endif
                scroll_panel(&list, dir, page);
                continue;
            }
        }
        else if (list_do_action(CONTEXT_AGENTS, timeout, &list, &action))
        {
            int sel = gui_synclist_get_sel_pos(&list);
            int lines = gui_synclist_get_nb_lines(&list, SCREEN_MAIN);

            if (view == VIEW_QUESTION && sel > before &&
                before >= before_top + lines)
            {
                /* The selection is below the rows in view, which were
                 * scrolled up to read: scroll back down towards it. Only
                 * the question scrolls so; in the agents view a selection
                 * out of view is the band's doing, and the move itself
                 * brings it back. */
                gui_synclist_select_item(&list, before);
                gui_synclist_set_top_item(&list, before_top + 1);
                gui_synclist_draw(&list);
                continue;
            }
            /* Wheel moves skip the rows that cannot be chosen: on in the
             * direction of the move, else the other way */
            if (!selectable(sel))
            {
                int step = sel < before ? -1 : 1;
                int i = sel;
                while (i >= 0 && i < nrows && !selectable(i))
                    i += step;
                if (i < 0 && selectable(before))
                {
                    /* Nothing to choose above: the move scrolls the rows
                     * there into view, such as a long question's start */
                    gui_synclist_select_item(&list, before);
                    gui_synclist_set_top_item(&list, before_top - 1);
                    gui_synclist_draw(&list);
                    continue;
                }
                if (i < 0 || i >= nrows)
                    for (i = sel; i >= 0 && i < nrows && !selectable(i); )
                        i -= step;
                if (i >= 0 && i < nrows)
                {
                    gui_synclist_select_item(&list, i);
                    gui_synclist_draw(&list);
                }
            }
            if (gui_synclist_get_sel_pos(&list) != before && view != VIEW_QUESTION)
                want_focus(&list);
            continue;
        }
        if (action == ACTION_STD_QUICKSCREEN || action == ACTION_WPS_QUICKSCREEN)
        {
            if (view != VIEW_CONFIRM &&
                agents_quickscreen(&list, action) == QUICKSCREEN_IN_USB)
            {
                ret = AGENTS_EXIT_OTHER;
                done = true;
            }
            continue;
        }
        if (agents_menu_exit(action))
        {
            native_play_pending = false;
            bool recording = talk_held;
            finish_talk(true);
            if (recording)
                continue;
            if (view == VIEW_SPACES)
            {
                ret = AGENTS_EXIT_MENU;
                done = true;
            }
            else if (view == VIEW_USAGE)
                close_usage(&list);
            else if (view == VIEW_AGENTS)
                back_from_tab(&list);
            else if (view == VIEW_DETAIL)
                close_detail(&list);
            else if (view == VIEW_CONFIRM)
            {
                draw_question_overlay(&list, -1);
                view = VIEW_ACTIONS;
                show(&list, action_row);
            }
            else if (view == VIEW_ACTIONS)
                close_actions(&list);
            else if (view == VIEW_INPUT)
                close_input(false);
            else
            {
                if (view == VIEW_ALERT)
                    draw_question_overlay(&list, -1);
                leave_question(&list);
            }
            continue;
        }
        switch (action)
        {
            case ACTION_AGENTS_SELECT_DOWN:
                select_down = true;
                select_held = false;
                select_remote = (get_action_statuscode(NULL) & ACTION_REMOTE) != 0;
                select_at = get_action_tick();
                select_view = view;
                break;
            case ACTION_AGENTS_SELECT_UP:
                if (talk_select)
                    finish_talk(false);
                if (select_down && select_view == view)
                {
                    if (talk_toggle || (!select_held &&
                        (unsigned long)(get_action_tick() - select_at) < 3 * HZ / 10))
                        activate_select(&list);
                    else if (!select_held)
                    {
                        select_long(&list);
                        if (talk_select)
                            finish_talk(false);
                    }
                }
                else if (!select_down && !select_held && voice_view())
                    activate_select(&list); /* A lost down edge cannot answer a prompt. */
                select_down = select_held = false;
                break;
            case ACTION_AGENTS_SELECT_REPEAT:
                if (!select_down)
                    select_held = true;
                else if (!select_held &&
                    (select_remote ||
                     TIME_AFTER(get_action_tick(), select_at + 3 * HZ / 10 - 1)))
                    select_long(&list);
                break;
            case ACTION_STD_OK: /* USB remote SELECT is host-classified. */
                activate_select(&list);
                break;
            case ACTION_AGENTS_LEFT_DOWN:
            case ACTION_AGENTS_RIGHT_DOWN:
                if (action == ACTION_AGENTS_LEFT_DOWN)
                    left_held = false;
                else
                    right_held = false;
                alert_direction_ready = view == VIEW_ALERT &&
                    TIME_AFTER(current_tick, alert_at + HZ / 4 - 1);
                break;
            case ACTION_AGENTS_LEFT_REPEAT:
                left_held = true;
                horizontal_button(&list, -1, true);
                break;
            case ACTION_AGENTS_RIGHT_REPEAT:
                right_held = true;
                horizontal_button(&list, 1, true);
                break;
            case ACTION_AGENTS_LEFT:
                if (!left_held)
                    horizontal_button(&list, -1, false);
                left_held = false;
                break;
            case ACTION_AGENTS_RIGHT:
                if (!right_held)
                    horizontal_button(&list, 1, false);
                right_held = false;
                break;
            case ACTION_AGENTS_TALK:
                native_play_pending = true;
                native_play_at = get_action_tick();
                if (voice_view() || view == VIEW_INPUT)
                {
                    start_talk(&list, false);
                    if (talk_held)
                        agents_shortcut_talking(true);
                }
                break;
            case ACTION_AGENTS_REMOTE_TALK:
                if (voice_view() || view == VIEW_INPUT)
                    start_talk(&list, true);
                break;
            case ACTION_AGENTS_DISCARD:
                finish_talk(true);
                break;
            case ACTION_AGENTS_REMOTE_COVERS:
            case ACTION_AGENTS_COVERS:
                native_play_pending = false;
                finish_talk(true);
                ret = AGENTS_SWITCH_COVERS;
                done = true;
                break;
            case ACTION_AGENTS_REMOTE_SWITCH:
                native_play_pending = false;
                finish_talk(true);
                ret = (t3_mode() || agents_fruit_nav()) ?
                      AGENTS_SWITCH_WPS : AGENTS_SWITCH_REMOTE_WPS;
                done = true;
                break;
            case ACTION_AGENTS_SWITCH:
                native_play_pending = false;
                finish_talk(true);
                ret = AGENTS_SWITCH_WPS;
                done = true;
                break;
            case ACTION_AGENTS_TALK_END:
                finish_native_play();
                break;
            case ACTION_NONE:
                break;
            default:
                if (cider_wps_action(action))
                    break;
                if (default_event_handler(action) == SYS_USB_CONNECTED)
                {
                    ret = AGENTS_EXIT_OTHER;
                    done = true;
                }
                break;
        }
    }

    ui_scene_cancel(&page_scene);
    /* A hold ends with the screen */
    signal_usage_view(false, true);
    finish_talk(true);
    flush_outbox();
    /* The resident service owns the connection; exit keeps the cached view. */
    long deadline = current_tick + 2 * HZ;
    while (nout && podlink_state() == PODLINK_CONNECTED &&
           TIME_BEFORE(current_tick, deadline))
    {
        poll_link();
        flush_outbox();
        sleep(1);
    }
    forget_keys();
    focus_want[0] = '\0';
    int selected = gui_synclist_get_sel_pos(&list);
    cache_row = selected >= 0 && selected < nrows ? rows[selected] :
                                                   (struct row){0};
    cache_top = list.start_item[SCREEN_MAIN];
    gui_synclist_scroll_stop(&list);
    cache_valid = was_connected && model.hello && !nout;
    cache_generation = podlink_generation();
#ifdef HAVE_CRUMBS
    crumb(CR_AGENTS_STEP, 0xf0);
#endif
    podlink_close();
    agents_foreground = false;
    /* Play still held would power off at once: wait for its release */
    while (ret != AGENTS_SWITCH_WPS &&
           ret != AGENTS_SWITCH_REMOTE_WPS && agents_play_down())
        sleep(HZ / 20);
    button_set_sw_poweroff_state(poweroff);
    pop_current_activity();
    font_unload(body_font);
    agents_body_font = FONT_UI;
#ifdef IPOD_ACCESSORY_PROTOCOL
    iap_agents_volume_enable(false);
    volume_paging = false;
#endif
    native_play_pending = false;
#ifdef HAVE_CRUMBS
    crumb(CR_AGENTS_OUT, (uint32_t)ret);
#endif
    FOR_NB_SCREENS(i)
        viewportmanager_theme_undo(i, false);
    agents_usb_exit(ret);
    return ret;
}
