#!/usr/bin/env python3
"""Stage source adapters for AstraPod's multi-source plugins."""
import re
import sys
from pathlib import Path

src, out = map(Path, sys.argv[1:])
source = src.read_text()


def replace(old, new, count=1):
    global source
    if source.count(old) != count:
        raise ValueError(f"{src}: expected {count} occurrences of {old!r}")
    source = source.replace(old, new)


if src.parent.name == "goban" and src.name in ("goban.c", "game.c"):
    replace('#include "goban.h"', '''#include "goban.h"

#undef DEFAULT_SAVE_DIR
#define DEFAULT_SAVE_DIR ROCKBOX_DIR "/goban"''')

if src.name == "chessbox.c":
    replace("rb->talk_shutup()", "rb->talk_force_shutup()")
    replace("int selection;", "int selection = 0;", count=2)
    replace("""        button = rb->button_get(true);
        switch (button) {""", """        button = rb->button_get(true);
        if (IS_SYSEVENT(button) && button != SYS_POWEROFF &&
            button != SYS_REBOOT &&
            rb->default_event_handler(button) == SYS_USB_CONNECTED)
            button = SYS_USB_CONNECTED;
        switch (button) {""", count=2)
    replace("            case SYS_POWEROFF:", "            case SYS_USB_CONNECTED:\n            case SYS_POWEROFF:", count=2)
    replace("        case SYS_POWEROFF:\n        case SYS_REBOOT:", "        case SYS_USB_CONNECTED:\n        case SYS_POWEROFF:\n        case SYS_REBOOT:")
    replace("    switch(rb->do_menu(&menu, &selection, NULL, false))\n    {",
            """    switch(rb->do_menu(&menu, &selection, NULL, false))
    {
        case MENU_ATTACHED_USB:
            return COMMAND_QUIT;""", count=2)
    replace("    if (cb_sysevent)\n        rb->default_event_handler(cb_sysevent);",
            """    if (cb_sysevent &&
        rb->default_event_handler(cb_sysevent) == SYS_USB_CONNECTED)
        return PLUGIN_USB_CONNECTED;
    if (rb->plugin_usb_taken())
        return PLUGIN_USB_CONNECTED;""")
    start = source.index("static void cb_saveposition (")
    end = source.index("/* ---- Restore saved position", start)
    save = source[start:end]
    save = save.replace("    int fd;", "    int fd;\n    bool write_ok = true;")
    save = save.replace(
        "    fd = rb->open(SAVE_FILE, O_WRONLY|O_CREAT|O_TRUNC, 0666);",
        """    if (cb_sysevent || rb->plugin_usb_taken())
        return;
    fd = rb->open(SAVE_FILE ".tmp", O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0)
    {
        rb->splash(HZ, "Could not save position");
        return;
    }""")
    save = re.sub(r"rb->write\(fd, ([^;]+), ([^,;]+)\);",
                  r"write_ok &= rb->write(fd, \1, \2) == (ssize_t)(\2);", save)
    save = save.replace("    rb->close(fd);", """    if (rb->file_sync(fd) < 0)
        write_ok = false;
    if (rb->close(fd) < 0)
        write_ok = false;
    if (!write_ok || rb->rename(SAVE_FILE ".tmp", SAVE_FILE) < 0)
    {
        rb->remove(SAVE_FILE ".tmp");
        rb->splash(HZ, "Could not save position");
    }""")
    replace(source[start:end], save)
    start = source.index("static struct pgn_game_node* cb_restoreposition (")
    end = source.index("/* ---- show menu in viewer mode", start)
    restore = source[start:end]
    restore = re.sub(r"^([ ]+)rb->read\(fd, ([^;]+), ([^,;]+)\);",
                     r"\1if (rb->read(fd, \2, \3) != (ssize_t)(\3))\n\1    goto invalid;",
                     restore, flags=re.M)
    restore = restore.replace("    char buf[4];", "    char buf[4];\n    int kings[2] = {0};")
    restore = restore.replace("        for (sq = 0; sq < 64; sq++) {",
                              """        if (computer < 1 || computer > 2 ||
            opponent < 1 || opponent > 2 || computer == opponent)
            goto invalid;
        for (sq = 0; sq < 64; sq++) {""")
    restore = restore.replace("            if (color[sq] == 0)",
                              """            if (board[sq] > king || color[sq] > 2 ||
                (color[sq] == 0) != (board[sq] == no_piece))
                goto invalid;
            if (board[sq] == king)
                kings[color[sq] - 1]++;
            if (color[sq] == 0)""")
    restore = restore.replace("        n++;\n        n &= 0xFF;",
                              """        if (kings[0] != 1 || kings[1] != 1 || n < -1 ||
            n >= MAX_GAME_CNT)
            goto invalid;
        n = (n + 1) & 0xFF;""")
    restore = restore.replace("            if (GameList[GameCnt].color == 0)",
                              """            if ((GameList[GameCnt].gmove & 0xC0C0) ||
                GameList[GameCnt].color > 2 ||
                GameList[GameCnt].piece > king)
                goto invalid;
            if (GameList[GameCnt].color == 0)""")
    restore = restore.replace(
        """        while (rb->read(fd, buf, 4) > 0)
            pgn_append_ply(game, ((n++) & 1) ? black : white, buf, false);""",
        """        ssize_t got;
        while ((got = rb->read(fd, buf, 4)) == 4)
        {
            if (buf[0] < 'a' || buf[0] > 'h' ||
                buf[1] < '1' || buf[1] > '8' ||
                buf[2] < 'a' || buf[2] > 'h' ||
                buf[3] < '1' || buf[3] > '8')
                goto invalid;
            pgn_append_ply(game, ((n++) & 1) ? black : white, buf, false);
        }
        if (got != 0)
            goto invalid;""")
    restore = restore.replace("    return game;\n}",
                              """    return game;

invalid:
    rb->close(fd);
    rb->splash(2 * HZ, "Invalid saved position");
    GNUChess_Initialize();
    return pgn_init_game();
}""")
    replace(source[start:end], restore)
elif src.name == "chessbox_pgn.c":
    replace("""                return NULL;
            break;
        }
    }
}""", """                return NULL;
            default:
                if (rb->default_event_handler(button) == SYS_USB_CONNECTED)
                    return NULL;
                break;
        }
    }
}""")
    replace("    if (bufleft < size)", """    const size_t align = __alignof__(struct pgn_ply_node);
    size = (size + align - 1) & ~(align - 1);
    if (!bufptr || bufleft < size)""")
    replace("        bufleft -= size;\n        return ptr;",
            "        bufleft -= size;\n        rb->memset(ptr, 0, size);\n        return ptr;")
    replace("size_t bufleft;", """size_t bufleft;
static void *pgn_body;
static size_t pgn_body_size;""")
    replace("""    if (bufptr == NULL){
        pl_malloc_init();
    }""", "    pl_malloc_init();", count=2)
    replace("""    if ( (fhandler = rb->open(filename, O_RDONLY)) < 0 ) return NULL;

    pl_malloc_init();""",
            """    if ( (fhandler = rb->open(filename, O_RDONLY)) < 0 ) return NULL;
    off_t size = rb->ffilesize(fhandler);
    if (size < 0 || size > 1024 * 1024)
    {
        rb->close(fhandler);
        return NULL;
    }

    pl_malloc_init();""")
    replace("""    rb->close(fhandler);
    return first_game;""", """    rb->close(fhandler);
    pgn_body = bufptr;
    pgn_body_size = bufleft;
    return first_game;""")
    start = source.index("    char tag_type[20];")
    end = source.index("\n}\n", start)
    replace(source[start:end], """    const char *split = rb->strchr(buffer, ' ');
    if (!split || split[1] != '"')
        return;
    size_t tag_length = split - buffer - 1;
    const char *value = split + 2;
    const char *end = rb->strrchr(value, '"');
    if (!end)
        return;

    const struct {
        const char *tag;
        char *field;
        size_t size;
    } fields[] = {
        { "White", game->white_player, sizeof(game->white_player) },
        { "Black", game->black_player, sizeof(game->black_player) },
        { "Result", game->result, sizeof(game->result) },
        { "Date", game->game_date, sizeof(game->game_date) },
    };
    for (size_t i = 0; i < ARRAYLEN(fields); i++)
    {
        if (rb->strlen(fields[i].tag) == tag_length &&
            !rb->memcmp(fields[i].tag, buffer + 1, tag_length))
        {
            size_t count = MIN((size_t)(end - value), fields[i].size - 1);
            rb->memcpy(fields[i].field, value, count);
            fields[i].field[count] = '\\0';
            break;
        }
    }""")
    start = source.index("    unsigned short pos, token_pos=0;")
    end = source.index("\n}\n", start)
    replace(source[start:end], r"""    unsigned short pos = initial_pos;
    size_t len = 0;
    while (line_buffer[pos] == ' ' || line_buffer[pos] == '.')
        pos++;
    while (line_buffer[pos] && line_buffer[pos] != ' ' &&
           line_buffer[pos] != '.')
    {
        if (len < 8)
            token_buffer[len] = line_buffer[pos];
        pos++;
        len++;
    }
    if (len > 8)
        return 0;
    while (len && (token_buffer[len - 1] == '!' ||
                   token_buffer[len - 1] == '?'))
        len--;
    token_buffer[len] = '\0';
    return pos;""")
    replace("    char str[9];", "    char str[9] = {0};")
    replace("    rb->strcpy(str,ply->pgn_text);", """    if (str_length >= sizeof(str))
        return false;
    rb->strcpy(str, ply->pgn_text);""")
    replace("""            if (board[locn[ply->row_to][ply->column_to]] == no_piece){
                /* en-passant, remove the pawn */""",
            """            if ((ply->row_from | ply->row_to | ply->column_to) >= 8)
                return false;
            if (board[locn[ply->row_to][ply->column_to]] == no_piece){
                /* en-passant, remove the pawn */""")
    replace("        if (board[locn[ply->row_from][ply->column_from]] == no_piece){",
            """        if ((ply->row_from | ply->row_to | ply->column_to) >= 8)
            return false;
        if (board[locn[ply->row_from][ply->column_from]] == no_piece){""")
    replace("        if (ply->row_to == 7 || ply->row_to == 0){",
            """        if (ply->row_from >= 8)
            return false;
        if (ply->row_to == 7 || ply->row_to == 0){""")
    start = source.index("        /* the other pieces */")
    end = source.index("        if (piece == knight)", start)
    change = source[start:end]
    change = change.replace("        piece = piece_from_pgn(str[0]);",
                            """        piece = piece_from_pgn(str[0]);
        if (piece == no_piece)
            return false;""")
    change = change.replace("            ply->taken_piece = board",
                            """            if ((ply->row_to | ply->column_to) >= 8)
                return false;
            ply->taken_piece = board""")
    replace(source[start:end], change)
    replace("                if (!temp_node) return NULL;",
            """                if (!temp_node || game_count == 65535)
                {
                    rb->close(fhandler);
                    return NULL;
                }""")
    replace("    fhandler = rb->open(filename, O_RDONLY);\n",
            """    selected_game->first_ply = NULL;
    fhandler = rb->open(filename, O_RDONLY);
    if (fhandler < 0)
        return;
    bufptr = pgn_body;
    bufleft = pgn_body_size;
""")
    replace("    char token_buffer[9];", "    char token_buffer[9] = {0};")
    replace("            pos = get_next_token(line_buffer, pos, token_buffer);",
            """            pos = get_next_token(line_buffer, pos, token_buffer);
            if (!pos)
            {
                success = false;
                break;
            }""")
    replace("                else\n                    first_ply = NULL;",
            """                else
                {
                    success = false;
                    break;
                }""")
    replace("    }\n\n    rb->close(loghandler);",
            """        if (!success)
        {
            first_ply = NULL;
            break;
        }
    }

    rb->close(loghandler);""")
    replace("            curr_node->next_node = temp_ply;\n        }\n    }",
            """            curr_node->next_node = temp_ply;
        }
        else
            first_ply = NULL;
    }""")
    replace("    if (is_mate){\n        ply->prev_node->checkmate = true;",
            """    if (!ply->prev_node)
        return;
    if (is_mate){
        ply->prev_node->checkmate = true;""")
elif src.name == "goban.c":
    replace("static void global_setup (void);", "static bool global_setup (void);")
    replace("    global_setup ();", """    if (!global_setup ())
        return PLUGIN_ERROR;""")
    start = source.index("    else\n    {\n        /* game loaded */")
    end = source.index("\n    draw_screen_display ();", start)
    replace(source[start:end], "")
    replace("                save_game (DEFAULT_SAVE);", """                if (rb->plugin_usb_taken())
                    return PLUGIN_USB_CONNECTED;
                if (!save_game (DEFAULT_SAVE))
                    rb->splash (2 * HZ, "Autosave failed");""", count=1)
    replace("static void\nglobal_setup (void)", "static bool\nglobal_setup (void)")
    replace("    setup_sgf ();", """    if (!setup_sgf ())
    {
        rb->splash (2 * HZ, "Goban cannot open its workspace");
        return false;
    }""")
    replace("\n}\n\nenum main_menu_selections", "\n    return true;\n}\n\nenum main_menu_selections")
    replace("            if (!rb->kbd_input(new_save_file, SAVE_FILE_LENGTH, NULL))",
            "            if (rb->kbd_input(new_save_file, SAVE_FILE_LENGTH, NULL) < 0)")
elif src.parent.name == "goban" and src.name == "game.c":
    replace("""        rb->splash (3 * HZ, "Unable to parse SGF file.  Will overwrite.");""",
            """        rb->splash (2 * HZ, "Unable to parse SGF file.");""")
    replace("    post_game_setup_sgf ();\n\n    if (!parse_failed)", """    if (parse_failed)
        return false;

    post_game_setup_sgf ();

    if (!parse_failed)""")
elif src.name == "sgf_storage.c":
    start = source.index("    if (!find_free (&buffer_num, &handle))")
    end = source.index("\n    set_free (buffer_num,", start)
    replace(source[start:end], """    if (!find_free (&buffer_num, &handle))
        return -1;
""")
    replace("    char *new_storage_buffer;\n    size_t size;\n\n", "")
    start = source.index("    if (total_storage_size < MIN_STORAGE_BUFFER_SIZE)")
    end = source.index("    if (total_storage_size < MIN_STORAGE_BUFFER_SIZE)", start + 1)
    replace(source[start:end], "")
    replace("    unhandled_fd = create_or_open_file (UNHANDLED_PROP_LIST_FILE);",
            "    unhandled_fd = rb->open(UNHANDLED_PROP_LIST_FILE, O_RDWR | O_CREAT);")
    replace("""    rb->lseek (unhandled_fd, 0, SEEK_SET);
    rb->ftruncate (unhandled_fd, 0);

    empty_stack (&parse_stack);

    return true;""", """    if (rb->ftruncate (unhandled_fd, 0) < 0)
    {
        close_file (&unhandled_fd);
        return false;
    }
    empty_stack (&parse_stack);
    return true;""")
elif src.name == "sgf_output.c":
    source = source.replace("write_file (", "write_output (")
    source = source.replace("write_char (", "write_output_char (")
    replace("static void pos_to_sgf", """static bool output_failed;

static void write_output(int fd, const void *data, size_t size)
{
    if (!output_failed && write_file(fd, data, size) != (ssize_t)size)
        output_failed = true;
}

static void write_output_char(int fd, char value)
{
    write_output(fd, &value, 1);
}

static void pos_to_sgf""")
    start = source.index("    sgf_fd = create_or_open_file (filename);")
    end = source.index("    while (pop_int_stack", start)
    replace(source[start:end], """    char temp[MAX_PATH];
    int len = rb->snprintf(temp, sizeof(temp), "%s.tmp", filename);
    if (len < 0 || len >= (int)sizeof(temp) || tree_head < 0)
        return false;

    close_file (&sgf_fd);
    sgf_fd = rb->open(temp, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (sgf_fd < 0)
        return false;

    empty_stack (&parse_stack);
    output_failed = !push_int_stack (&parse_stack, tree_head);

""")
    replace("            push_int_stack (&parse_stack, current);",
            """            if (!push_int_stack (&parse_stack, current))
                output_failed = true;""")
    replace("""            add_or_set_prop_sgf (current,
                                 PROP_VARIATION_TO_PROCESS, temp_data);""",
            """            if (add_or_set_prop_sgf (current,
                                     PROP_VARIATION_TO_PROCESS, temp_data) < 0)
                output_failed = true;""")
    replace("        push_int_stack (&parse_stack, current_node);",
            """        if (!push_int_stack (&parse_stack, current_node))
            output_failed = true;""")
    replace("""    current_node = saved;
    close_file (&sgf_fd);
    DEBUGF ("done outputting, file closed\\n");
    return true;""", """    current_node = saved;
    if (rb->file_sync(sgf_fd) < 0)
        output_failed = true;
    if (rb->close(sgf_fd) < 0)
        output_failed = true;
    sgf_fd = -1;
    if (!output_failed && rb->rename(temp, filename) == 0)
        return true;

    rb->remove(temp);
    return false;""")
elif src.parent.name == "goban" and src.name == "util.c":
    replace("        if (result < 0)\n", "        if (result <= 0)\n", count=1)
    replace("        rb->splash(0, buffer);", '        rb->splashf(0, "%s", buffer);')
elif src.name == "sgf.c":
    replace("        rb->splash(HZ / 3, comment_buffer);",
            '        rb->splashf(HZ / 3, "%s", comment_buffer);')
elif src.name == "sgf_parse.c":
    start = source.index("\nbool\nparse_sgf (")
    end = source.index("\n\nstatic void\nparse_node (", start)
    parser = source[start:end]
    parser = parser.replace("return false;", "goto failed;")
    parser = parser.replace("    current_node = start_node;",
                            """    off_t length = rb->ffilesize(sgf_fd);
    if (length < 1 || length > 256 * 1024)
        goto failed;

    current_node = start_node;""")
    parser = re.sub(
        r"(?m)^( +)push_int_stack \(&parse_stack, ([^\n]+)\);",
        r"\1if (!push_int_stack (&parse_stack, \2))\n\1    goto failed;", parser)
    parser = parser.replace("                parse_node ();",
                            """                if (rb->default_event_handler(rb->button_get(false))
                    == SYS_USB_CONNECTED)
                    goto failed;
                parse_node ();""")
    parser = parser.replace("    current_node = get_node (tree_head)->next;",
                            "    if (sgf_fd < 0)\n        goto failed;\n\n"
                            "    current_node = get_node (tree_head)->next;", 1)
    parser = parser.replace("    return true;\n}", """    return true;

failed:
    current_node = saved;
    close_file (&sgf_fd);
    empty_stack (&parse_stack);
    return false;
}""")
    replace(source[start:end], parser)
    replace('                DEBUGF ("invalid range value!\\n");',
            '                return;')
    replace('            DEBUGF ("invalid LaBel property \'%s\'", buffer);',
            '            return;')
    replace('            DEBUGF ("LaBel set on invalid position!\\n");',
            '            return;')
    replace("buffer[0] > 'z'", "buffer[0] >= 'a' + MAX_BOARD_SIZE")
    replace("buffer[1] > 'z'", "buffer[1] >= 'a' + MAX_BOARD_SIZE")
    replace("    rb->memset (buffer, 0, buffer_size);\n    --buffer_size;",
            """    if (buffer_size)
    {
        rb->memset (buffer, 0, buffer_size);
        --buffer_size;
    }""")

out.write_text(source)
