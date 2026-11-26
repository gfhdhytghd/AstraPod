/* Replays wheel input through the list's letter jump (list_quickscroll.c).
 *
 *     replay KEYS VISIBLE ROW_HEIGHT < EVENTS
 *
 * KEYS holds one row name per line, in list order. Each EVENTS line is
 * "t_us touch", "t_us moved REPORTS", the wheel reporting motion, "t_us step
 * POSITIONS", the list's wheel event, or "t_us release". After every event
 * the program prints "t_us selected top mode overlay", the overlay being
 * the key shown or "-". The selection starts on row 0 at the top. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "list_quickscroll.h"

static char **names;
static int count;

static void key(int item, char *out, void *ctx)
{
    (void)ctx;
    quickscroll_key_of(names[item], out);
}

int main(int argc, char **argv)
{
    if (argc != 4)
        return 2;
    FILE *f = fopen(argv[1], "r");
    if (!f)
        return 2;
    char line[512];
    while (fgets(line, sizeof(line), f))
    {
        line[strcspn(line, "\n")] = '\0';
        names = realloc(names, (count + 1) * sizeof(*names));
        names[count++] = strdup(line);
    }
    fclose(f);
    int visible = atoi(argv[2]);
    int row_height = atoi(argv[3]);

    struct quickscroll qs;
    quickscroll_reset(&qs);
    int selected = 0, top = 0;
    bool touched = false;
    unsigned long t;
    char op[16];
    while (scanf("%lu %15s", &t, op) == 2)
    {
        if (!strcmp(op, "touch"))
            touched = true;
        else if (!strcmp(op, "release"))
        {
            touched = false;
            quickscroll_release(&qs);
        }
        else if (!strcmp(op, "moved"))
        {
            int moves;
            if (scanf("%d", &moves) != 1)
                return 2;
            quickscroll_moved(&qs, (uint32_t)t, moves);
            continue;
        }
        else if (!strcmp(op, "step"))
        {
            int positions;
            if (scanf("%d", &positions) != 1)
                return 2;
            struct quickscroll_list list = { count, selected, row_height, key,
                                             NULL };
            struct quickscroll_move move = quickscroll_wheel(&qs, &list,
                (uint32_t)t, positions, touched);
            selected = move.selected;
            if (move.jumped)
                top = selected;
        }
        if (top > count - visible)
            top = count - visible;
        if (selected < top)
            top = selected;
        if (selected >= top + visible)
            top = selected - visible + 1;
        if (top < 0)
            top = 0;
        struct quickscroll_list list = { count, selected, row_height, key,
                                         NULL };
        char shown[QUICKSCROLL_KEY_SIZE];
        bool on = quickscroll_overlay(&qs, &list, shown);
        printf("%lu %d %d %d %s\n", t, selected, top, qs.mode, on ? shown : "-");
    }
    return 0;
}
