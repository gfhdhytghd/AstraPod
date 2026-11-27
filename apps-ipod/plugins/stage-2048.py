#!/usr/bin/env python3
"""Stage 2048 with a full-screen board and paced slides.

Arguments: SOURCE BOARD_INC OUTPUT. SOURCE is 2048.c as stage-usb.py leaves
it. The colour drawing code is replaced by BOARD_INC; the move logic and the
saves are unchanged apart from recording where each tile goes.
"""
from pathlib import Path
import sys


def replace(old, new, count=1):
    global source
    if source.count(old) != count:
        raise SystemExit(f"{source_path}: expected {count} matches for {old!r}")
    source = source.replace(old, new)


source_path = Path(sys.argv[1])
source = source_path.read_text()
board = Path(sys.argv[2]).read_text()

replace("static const int ANIM_SLEEPTIME = (HZ/20);\n", "")
replace('''#if LCD_DEPTH > 1
/* where to draw the background bitmap */
static const int BACKGROUND_X = (BASE_X-MIN_SPACE);
static const int BACKGROUND_Y = (BASE_Y-MIN_SPACE);
#endif
''', "")
replace("static int old_grid[GRID_SIZE][GRID_SIZE];\n", '''static int old_grid[GRID_SIZE][GRID_SIZE];
/* The starting cell of the tile in each cell, -1 for none, and the tile
 * each starting cell merged into. */
static signed char tile_at[GRID_SIZE][GRID_SIZE];
static signed char merged_into[SPACES];
''')
replace('''                ctx->score += ctx->grid[x + lookx][y + looky];
                ctx->grid[x][y] = 0;
''', '''                ctx->score += ctx->grid[x + lookx][y + looky];
                ctx->grid[x][y] = 0;
                /* check_gameover() also slides, after a tile is placed. */
                if(tile_at[x][y] >= 0)
                    merged_into[tile_at[x][y]] = tile_at[x + lookx][y + looky];
                tile_at[x][y] = -1;
''')
replace('''                ctx->grid[x + lookx][y + looky] = ctx->grid[x][y];
                ctx->grid[x][y] = 0;
''', '''                ctx->grid[x + lookx][y + looky] = ctx->grid[x][y];
                ctx->grid[x][y] = 0;
                tile_at[x + lookx][y + looky] = tile_at[x][y];
                tile_at[x][y] = -1;
''')

start = "/* low-depth displays resort to text drawing, see the #else case below */"
end = "#endif /* LCD_DEPTH > 1 */\n"
if source.count(start) != 1 or source.count(end) != 1:
    raise SystemExit(f"{source_path}: drawing code not found")
first = source.index(start)
last = source.index(end) + len(end)
source = source[:first] + board + source[last:]

replace('''        unsigned int grid_before_anim_step[GRID_SIZE][GRID_SIZE];

''', "")
replace('''        memcpy(&old_grid, &ctx->grid, sizeof(int)*SPACES);
''', '''        memcpy(&old_grid, &ctx->grid, sizeof(int)*SPACES);
        track_begin();
''')
for step in ("up", "down", "left", "right"):
    replace(f'''                memcpy(grid_before_anim_step, ctx->grid, sizeof(ctx->grid));
                {step}(true);
                if(memcmp(grid_before_anim_step, ctx->grid, sizeof(ctx->grid)))
                {{
                    rb->sleep(ANIM_SLEEPTIME);
                    draw();
                }}
''', f'''                {step}(true);
''')
replace('''            if(memcmp(&old_grid, ctx->grid, sizeof(ctx->grid)))
            {
                place_random();
            }
            memcpy(&old_grid, ctx->grid, sizeof(ctx->grid));
            if(check_gameover())
                return PLUGIN_OK;
            draw();
''', '''            if(memcmp(&old_grid, ctx->grid, sizeof(ctx->grid)))
            {
                place_random();
                animate_move();
            }
            memcpy(&old_grid, ctx->grid, sizeof(ctx->grid));
            if(check_gameover())
                return PLUGIN_OK;
''')
# The game screen hides the status bar; the menus around it keep theirs.
replace('''            enum plugin_status ret = do_game(sel == 1);
''', '''            rb->viewportmanager_theme_enable(SCREEN_MAIN, false, &game_vp);
            enum plugin_status ret = do_game(sel == 1);
            rb->viewportmanager_theme_undo(SCREEN_MAIN, false);
            rb->lcd_set_viewport(NULL);
''')

Path(sys.argv[3]).write_text(source)
