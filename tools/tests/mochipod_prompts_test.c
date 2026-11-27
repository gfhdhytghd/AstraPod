/* MochiPod's yes/no prompts, as 01-confirm-buttons.patch leaves them.
 *
 * In both control modes Center answers Yes and Menu answers No; Classic V3's
 * Right also answers No. The wheel, Left and Play leave the prompt exactly as
 * it was. Every prompt is drawn under MOCHI_LAYOUT_TEST, which asserts that no
 * text leaves the screen or overlaps another drawing; given a directory, one
 * frame of each is written there as a PGM.
 *
 * Built and run by mochipod_prompts_test.py against the patched source. */
#include "src/input/wheel.h"
#include "src/render/lcd.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static Ui u;
static Pet p;
static WheelInput in;
static uint32_t tick;
static Display display;
static const char *output;

struct prompt
{
    enum Page page, parent;
    const char *name;
};

static const struct prompt prompts[] = {
    { EXIT_ASK,    OPTIONS,  "exit"    },
    { HATCH_ASK,   OPTIONS,  "hatch"   },
    { REPLACE_ASK, HATCH_ASK, "replace" },
    { DONATE_ASK,  DONATION, "donate"  },
    { CLAIM,       PASSWORD, "claim"   },
    { OFFER,       HOME,     "offer"   },
};

static void click(enum Control key)
{
    tick += 15;
    ui_command(&u, &p, wheel_command(&in, &u, key, PRESSED, tick, 100));
    ui_command(&u, &p, wheel_command(&in, &u, key, RELEASED, tick, 100));
}

static void open_prompt(const struct prompt *q, enum InputMode mode)
{
    pet_new(&p, 600);
    p.character = V3_LEGACY; p.generation = 0; p.setup = 0; p.stage = ADULT;
    p.points = 300;
    p.family.phase = VISIT; p.family.mate = V3_WARUSOTCHI;
    ui_start(&u);
    u.mode = mode; u.icon = IC_STATUS; u.amount = 5; u.code = 0;
    u.depth = 1; u.parents[0] = (MenuFocus){ q->parent, 0 };
    u.page = q->page; u.choice = 0;
    in = (WheelInput){ 0 }; tick = 0;
}

static void check_ignored(const struct prompt *q, enum InputMode mode)
{
    static const enum Control keys[] = { WHEEL_CW, WHEEL_CCW, CLICK_PLAY,
                                         CLICK_LEFT };
    open_prompt(q, mode);
    Ui ui_before = u;
    Pet pet_before = p;
    for (unsigned i = 0; i < sizeof keys / sizeof *keys; i++)
    {
        click(keys[i]);
        if (memcmp(&u, &ui_before, sizeof u) || memcmp(&p, &pet_before, sizeof p))
        {
            fprintf(stderr, "%s, mode %d: control %d changed the prompt\n",
                    q->name, mode, keys[i]);
            assert(0);
        }
    }
}

static void check_yes(const struct prompt *q, enum InputMode mode)
{
    open_prompt(q, mode);
    click(CLICK_CENTER);
    switch (q->page)
    {
    case EXIT_ASK:    assert(u.exit_requested); break;
    case HATCH_ASK:   assert(u.page == REPLACE_ASK && p.stage == ADULT); break;
    case REPLACE_ASK: assert(u.page == HOME && p.stage == EGG); break;
    case DONATE_ASK:  assert(u.page == NOTICE && u.amount == 0); break;
    case CLAIM:       assert(u.page == NOTICE); break;
    case OFFER:
        assert(u.page == WEDDING && p.family.phase == WITH_BABY); break;
    default: assert(0);
    }
}

static void check_no(const struct prompt *q, enum InputMode mode,
                     enum Control key)
{
    open_prompt(q, mode);
    Pet pet_before = p;
    click(key);
    assert(!u.exit_requested);
    if (q->page == OFFER)
    {
        assert(u.page == HOME && p.family.phase == SINGLE);
        return;
    }
    assert(u.page == q->parent);
    assert(!memcmp(&p, &pet_before, sizeof p));
    if (q->page == DONATE_ASK)
        assert(u.amount == 0);
}

static void sample(const char *name)
{
    if (!output)
        return;
    char path[512];
    snprintf(path, sizeof path, "%s/%s.pgm", output, name);
    FILE *file = fopen(path, "wb");
    assert(file);
    fprintf(file, "P5\n%d %d\n255\n", LCD_COLS, LCD_ROWS);
    for (int y = 0; y < LCD_ROWS; ++y)
        for (int x = 0; x < LCD_COLS; ++x)
            fputc(display.bits[y / 8][x] & (1u << (y % 8)) ? 0 : 255, file);
    assert(fclose(file) == 0);
}

static void check_drawn(const struct prompt *q)
{
    open_prompt(q, NATIVE_IPOD);
    assert(ui_count(&u, &p) == 0);      /* no choice dots */
    if (q->page == CLAIM)
    {   /* every reward name above the price and the keys */
        for (int code = 0; code < 9 + 2 * GOODS_COUNT + 32; ++code)
        {
            u.code = code;
            draw_frame(&display, &p, &u);
        }
        u.code = 0;
    }
    if (q->page == DONATE_ASK)
    {
        u.amount = 9999;
        draw_frame(&display, &p, &u);
    }
    if (q->page == OFFER)
        for (u.frame = 0; u.frame <= 48; ++u.frame)
            draw_frame(&display, &p, &u);
    draw_frame(&display, &p, &u);
    sample(q->name);
}

int main(int argc, char **argv)
{
    output = argc > 1 ? argv[1] : NULL;
    for (unsigned i = 0; i < sizeof prompts / sizeof *prompts; i++)
    {
        const struct prompt *q = &prompts[i];
        for (int mode = NATIVE_IPOD; mode <= CLASSIC_V3; mode++)
        {
            check_ignored(q, (enum InputMode)mode);
            check_yes(q, (enum InputMode)mode);
            check_no(q, (enum InputMode)mode, CLICK_MENU);
            if (mode == CLASSIC_V3)
                check_no(q, (enum InputMode)mode, CLICK_RIGHT);
        }
        check_drawn(q);
    }
    printf("PASS %u MochiPod prompts: Center yes, Menu no, wheel ignored, "
           "both control modes, layout\n",
           (unsigned)(sizeof prompts / sizeof *prompts));
    return 0;
}
