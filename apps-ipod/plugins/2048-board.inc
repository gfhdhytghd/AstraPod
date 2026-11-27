/* 2048's board on the full screen, with each move drawn as a slide of
 * paced frames. Only the area that changes is drawn and sent to the LCD.
 * GNU GPL version 2 or later. */

#define TILE BMPHEIGHT__2048_tiles
#define BOARD BMPWIDTH__2048_background
#define BOARD_Y ((LCD_HEIGHT - BOARD) / 2)
#define BOARD_X (LCD_WIDTH - BOARD - BOARD_Y)
#define TEXT_X 0
#define TEXT_Y BOARD_Y
/* A slide lasts ten refreshes: five frames at every second refresh. */
#define SLIDE_REFRESHES 10

struct board_rect {
    int x, y, w, h;
};

/* The game's own full-screen viewport, and the one that clips a frame to
 * the area it changes. */
static struct viewport game_vp, clip_vp;
static int text_bottom;
static unsigned int shown_score, shown_best;

static int cell_pos(int i)
{
    return TILE * i + (BOARD - GRID_SIZE * TILE) * (i + 1) / (GRID_SIZE + 1);
}

static struct board_rect cell_rect(int x, int y)
{
    return (struct board_rect){ BOARD_X + cell_pos(x), BOARD_Y + cell_pos(y),
                                TILE, TILE };
}

static void rect_join(struct board_rect *r, struct board_rect add)
{
    if (r->w <= 0)
    {
        *r = add;
        return;
    }
    int right = MAX(r->x + r->w, add.x + add.w);
    int bottom = MAX(r->y + r->h, add.y + add.h);
    r->x = MIN(r->x, add.x);
    r->y = MIN(r->y, add.y);
    r->w = right - r->x;
    r->h = bottom - r->y;
}

static bool rect_meets(const struct board_rect *r, struct board_rect other)
{
    return other.x < r->x + r->w && r->x < other.x + other.w &&
           other.y < r->y + r->h && r->y < other.y + other.h;
}

/* Draws a tile at screen coordinates, clipped to clip_vp. */
static void draw_tile(unsigned int value, int x, int y)
{
    rb->lcd_bitmap_part(_2048_tiles,
                        BMPWIDTH__2048_tiles - TILE * ilog2(value), 0,
                        STRIDE(SCREEN_MAIN, BMPWIDTH__2048_tiles,
                               BMPHEIGHT__2048_tiles),
                        x - clip_vp.x, y - clip_vp.y, TILE, TILE);
}

/* Starts a frame's drawing in `area` with the empty board. */
static void board_begin(const struct board_rect *area)
{
    clip_vp = game_vp;
    clip_vp.x = area->x;
    clip_vp.y = area->y;
    clip_vp.width = area->w;
    clip_vp.height = area->h;
    rb->lcd_set_viewport(&clip_vp);
    rb->lcd_bitmap(_2048_background, BOARD_X - clip_vp.x,
                   BOARD_Y - clip_vp.y, BOARD, BOARD);
}

/* Each cell in `area` with the tile `value` gives it, 0 for none. */
static void board_cells(const struct board_rect *area,
                        unsigned int (*value)(int x, int y))
{
    for (int y = 0; y < GRID_SIZE; ++y)
        for (int x = 0; x < GRID_SIZE; ++x)
        {
            struct board_rect cell = cell_rect(x, y);
            if (rect_meets(area, cell))
                draw_tile(value(x, y), cell.x, cell.y);
        }
}

static unsigned int grid_value(int x, int y)
{
    return ctx->grid[x][y];
}

/* The title, score and best score in the column left of the board. Each
 * label falls back to a shorter text or the system font until it fits. */
static int draw_label(const char *full, const char *brief, unsigned int value,
                      int y)
{
    static const int fonts[] = { FONT_UI, FONT_SYSFIXED };
    const char *formats[] = { full, brief, "%d" };
    char buf[32];
    for (unsigned int f = 0; f < ARRAYLEN(formats); ++f)
        for (unsigned int i = 0; i < ARRAYLEN(fonts); ++i)
        {
            int w, h;
            rb->snprintf(buf, sizeof(buf), formats[f], value);
            rb->font_getstringsize(buf, &w, &h, fonts[i]);
            if (TEXT_X + w < BOARD_X)
            {
                rb->lcd_setfont(fonts[i]);
                rb->lcd_putsxy(TEXT_X, y, buf);
                return y + h + VERT_SPACING;
            }
        }
    return y;
}

/* Redraws the labels and returns the rows they may have changed. */
static struct board_rect draw_text(void)
{
    rb->lcd_set_viewport(&game_vp);
    rb->lcd_set_foreground(TEXT_COLOR);
    rb->lcd_set_background(BACKGROUND);
    int old_bottom = text_bottom;
    rb->lcd_set_drawmode(DRMODE_SOLID | DRMODE_INVERSEVID);
    rb->lcd_fillrect(0, 0, BOARD_X, old_bottom);
    rb->lcd_set_drawmode(DRMODE_SOLID);
    int y = draw_label("%d", "%d", WINNING_TILE, TEXT_Y);
    y = draw_label("Score: %d", "S: %d", ctx->score, y);
    text_bottom = draw_label("Best: %d", "B: %d", best_score, y);
    rb->lcd_setfont(WHAT_FONT);
    shown_score = ctx->score;
    shown_best = best_score;
    return (struct board_rect){ 0, 0, BOARD_X, MAX(old_bottom, text_bottom) };
}

/* draws game screen + updates LCD */
static void draw(void)
{
    struct board_rect board = { BOARD_X, BOARD_Y, BOARD, BOARD };
    rb->lcd_set_viewport(&game_vp);
    rb->lcd_set_background(BACKGROUND);
    rb->lcd_clear_viewport();
    text_bottom = 0;
    board_begin(&board);
    board_cells(&board, grid_value);
    draw_text();
    rb->lcd_update();
}

/*** slides ***/

struct slide {
    unsigned int value;
    int from_x, from_y, to_x, to_y;
};

static struct slide slides[SPACES];
static int slide_count;
static bool slide_still[GRID_SIZE][GRID_SIZE];

/* Marks every tile with its cell before a move's steps begin. */
static void track_begin(void)
{
    for (int x = 0; x < GRID_SIZE; ++x)
        for (int y = 0; y < GRID_SIZE; ++y)
            tile_at[x][y] = ctx->grid[x][y] ? x * GRID_SIZE + y : -1;
    memset(merged_into, -1, sizeof(merged_into));
}

/* Where the tile that started at `id` ended, following its merges. */
static bool track_end(int id, int *to_x, int *to_y)
{
    for (int hop = 0; hop < SPACES && id >= 0; ++hop)
    {
        for (int x = 0; x < GRID_SIZE; ++x)
            for (int y = 0; y < GRID_SIZE; ++y)
                if (tile_at[x][y] == id)
                {
                    *to_x = x;
                    *to_y = y;
                    return true;
                }
        id = merged_into[id];
    }
    return false;
}

static unsigned int still_value(int x, int y)
{
    return slide_still[x][y] ? (unsigned int)old_grid[x][y] : 0;
}

/* Eases out: fast at the start, settling into the cell. */
static int slide_offset(int distance, int elapsed)
{
    int rest = SLIDE_REFRESHES - elapsed;
    return distance - distance * rest * rest /
                      (SLIDE_REFRESHES * SLIDE_REFRESHES);
}

/* Shows the move from old_grid to ctx->grid, the new tile included, as
 * frames on every second refresh. Tiles keep their old values until the
 * last frame, which shows the merges, the new tile and the score. */
static void animate_move(void)
{
    struct board_rect moving = { 0, 0, 0, 0 };
    slide_count = 0;
    for (int x = 0; x < GRID_SIZE; ++x)
        for (int y = 0; y < GRID_SIZE; ++y)
        {
            int to_x, to_y;
            slide_still[x][y] = false;
            if (!old_grid[x][y] || !track_end(x * GRID_SIZE + y, &to_x, &to_y))
                continue;
            if (to_x == x && to_y == y)
            {
                slide_still[x][y] = true;
                continue;
            }
            struct board_rect from = cell_rect(x, y), to = cell_rect(to_x, to_y);
            slides[slide_count++] = (struct slide){
                old_grid[x][y], from.x, from.y, to.x, to.y,
            };
            rect_join(&moving, from);
            rect_join(&moving, to);
        }

    struct board_rect last = moving;
    for (int x = 0; x < GRID_SIZE; ++x)
        for (int y = 0; y < GRID_SIZE; ++y)
            if (ctx->grid[x][y] && tile_at[x][y] < 0)
                rect_join(&last, cell_rect(x, y));

    rb->frame_start();
    for (unsigned int frame = 0;; frame = rb->frame_next())
    {
        /* The first frame is one frame into the slide. */
        int elapsed = frame + 2;
        if (elapsed >= SLIDE_REFRESHES || moving.w <= 0)
            break;
        board_begin(&moving);
        board_cells(&moving, still_value);
        for (int i = 0; i < slide_count; ++i)
        {
            const struct slide *s = &slides[i];
            draw_tile(s->value,
                      s->from_x + slide_offset(s->to_x - s->from_x, elapsed),
                      s->from_y + slide_offset(s->to_y - s->from_y, elapsed));
        }
        rb->lcd_update_rect(moving.x, moving.y, moving.w, moving.h);
    }

    board_begin(&last);
    board_cells(&last, grid_value);
    if (ctx->score != shown_score || best_score != shown_best)
        rect_join(&last, draw_text());
    rb->lcd_set_viewport(&game_vp);
    rb->lcd_update_rect(last.x, last.y, last.w, last.h);
}
