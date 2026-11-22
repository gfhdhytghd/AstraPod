#!/usr/bin/env python3
"""Stage AstraPod utility adapters from upstream sources. GPL-2.0-or-later."""
from hashlib import sha256
from pathlib import Path
import sys


def replace(source, before, after):
    if source.count(before) != 1:
        raise ValueError(f"expected one upstream match for {before!r}")
    return source.replace(before, after, 1)


def folder_picker(source):
    start = '#include "plugin.h"'
    entry = 'enum plugin_status plugin_start(const void* parameter)'
    # AstraPod's folder picker uses the same selection format and persists it.
    # Keep the upstream entry protocol, using the native picker implementation.
    header, body = source.split(start)
    _, entry_body = body.split(entry)
    entry_body = replace(entry_body, '''folder_select(rb->str(LANG_AUTORESUME),
                              rb->global_settings->autoresume_paths,
                              MAX_PATHNAME+1)''', 'rb->select_db_folders(true)')
    entry_body = replace(entry_body, '''folder_select(rb->str(LANG_SELECT_FOLDER),
                           rb->global_settings->tagcache_scan_paths,
                           sizeof(rb->global_settings->tagcache_scan_paths))''',
                         'rb->select_db_folders(false)')
    return f'{header}{start}\n\n{entry}{entry_body}'


def dictionary(source):
    source = replace(source, '#include "lib/simple_viewer.h"\n', '')
    # Match the device index format even when the simulator uses LP64.
    source = replace(source, '    long offset;', '    uint32_t offset;')
    source = replace(source, 'static long reverse (long N)',
                     'static uint32_t reverse(uint32_t N)')
    for index, shift in enumerate((24, 16, 8)):
        source = replace(source, f'(B[{index}] << {shift})',
                         f'((uint32_t)B[{index}] << {shift})')
    source = replace(source, '    int filesize, high, low, probe;',
                     '    off_t filesize;\n    int high, low, probe;')
    source = replace(source, '    /* for the searching algorithm */',
                     '''    if (filesize < 0 || filesize > INT_MAX ||
        filesize % sizeof(struct stWord) != 0)
    {
        rb->close(fIndex);
        return PLUGIN_ERROR;
    }

    /* for the searching algorithm */''')
    source = replace(source, '#include "plugin.h"', '#include "plugin.h"\n#include <limits.h>')
    source = replace(source, '        rb->lseek(fIndex, sizeof(struct stWord) * probe, SEEK_SET);\n'
                     '        rb->read(fIndex, &word, sizeof(struct stWord));',
                     '''        if (rb->lseek(fIndex, sizeof(word) * probe, SEEK_SET) < 0 ||
            rb->read(fIndex, &word, sizeof(word)) != sizeof(word))
        {
            rb->close(fIndex);
            return PLUGIN_ERROR;
        }
        word.word[WORDLEN - 1] = '\\0';''')
    source = replace(source, '''    rb->lseek(fIndex, sizeof(struct stWord) * low, SEEK_SET);
    rb->read(fIndex, &word, sizeof(struct stWord));
    rb->close(fIndex);''',
                     '''    if (low >= 0 &&
        (rb->lseek(fIndex, sizeof(word) * low, SEEK_SET) < 0 ||
         rb->read(fIndex, &word, sizeof(word)) != sizeof(word)))
    {
        rb->close(fIndex);
        return PLUGIN_ERROR;
    }
    rb->close(fIndex);
    word.word[WORDLEN - 1] = '\\0';''')
    source = replace(source, '''    rb->lseek(fData, (off_t)reverse(word.offset), SEEK_SET);

    /* Read in the description */
    rb->read_line(fData, description, buffer_size);''',
                     '''    off_t offset = reverse(word.offset);
    if (offset < 0 || offset >= rb->ffilesize(fData) ||
        rb->lseek(fData, offset, SEEK_SET) < 0 ||
        rb->read_line(fData, description, buffer_size) < 0)
    {
        rb->close(fData);
        return PLUGIN_ERROR;
    }''')
    return replace(source, '    view_text(searchword, description);\n\n    return PLUGIN_OK;',
                   '    return rb->view_text(searchword, description);')


def credits(source):
    source = replace(source, 'static bool stop_autoscroll(int action)', """static int credit_action(int context, int timeout)
{
    int action = rb->get_action(context, timeout);
    if (IS_SYSEVENT(action) &&
        rb->default_event_handler(action) == SYS_USB_CONNECTED)
        return ACTION_STD_CANCEL;
    return action;
}

static bool stop_autoscroll(int action)""")
    if source.count('rb->get_action(CONTEXT_LIST,') != 7:
        raise ValueError('unexpected Credits input loops')
    source = source.replace('rb->get_action(CONTEXT_LIST,',
                            'credit_action(CONTEXT_LIST,')
    return replace(source, 'if(!rb->action_userabort(5*HZ))',
                   'if(credit_action(CONTEXT_STD, 5*HZ) != ACTION_STD_CANCEL)')


def alarmclock(source):
    source = replace(source, 'quit = false, usb = false, waiting',
                     'quit = false, waiting')
    source = replace(source, '    pause_audio();', '''    struct tm *now = rb->get_time();
    alarms[0] = (now->tm_hour + (now->tm_min == 59)) % 24;
    alarms[1] = (now->tm_min + 1) % 60;
    pause_audio();''')
    source = replace(source, '        button = get_button();', """        button = get_button();
        if (IS_SYSEVENT(button) &&
            rb->default_event_handler(button) == SYS_USB_CONNECTED)
            return PLUGIN_USB_CONNECTED;""")
    source = replace(source, '            quit = true;\n\n        if (waiting)',
                     '            break;\n\n        if (waiting)')
    source = replace(source, """                default:
                    if (rb->default_event_handler(button) == SYS_USB_CONNECTED)
                        quit = usb = true;
                    break;""", """                default:
                    break;""")
    return replace(source, '''    return (usb) ? PLUGIN_USB_CONNECTED
                 : (done ? PLUGIN_GOTO_WPS : PLUGIN_OK);''',
                   '    return done ? PLUGIN_GOTO_WPS : PLUGIN_OK;')


def check_digest(source, digest):
    if sha256(source.encode()).hexdigest() != digest:
        raise ValueError("review the AstraPod adapter for this source")


def calendar(source):
    check_digest(source, "d3d3a07fc8e4788dc149d61e54053b4b2ad3ffcbe76fb3cbe532bf6d0edd194d")
    source = replace(source, '#define TEMP_FILE PLUGIN_APPS_DATA_DIR "/~temp"',
                     '#define TEMP_FILE MEMO_FILE ".tmp"')
    source = replace(source, '    int     file_pointer_start;\n    int     file_pointer_end;',
                     '    off_t   file_pointer_start;\n    off_t   file_pointer_end;')
    start = source.index('static void load_memo(')
    end = source.index('static void add_memo(', start)
    memo_adapter = Path(__file__).with_name("calendar-memo.inc").read_text()
    source = source[:start] + memo_adapter + '\n' + source[end:]
    source = replace(source, '    shown->wday = tm->tm_wday - 1;',
                     '    shown->wday = (tm->tm_wday + 6) % 7;')
    source = replace(source, 'static void add_memo(struct shown *shown, int type)\n{',
                     '''static void add_memo(struct shown *shown, int type)
{
    if (memos_in_memory >= MAX_MEMOS_IN_A_MONTH)
    {
        rb->splash(HZ, "Too many events");
        return;
    }''')
    source = replace(source, '    while (!exit)\n    {\n        switch (rb->do_menu',
                     '    while (!exit && !rb->plugin_usb_taken())\n    {\n        switch (rb->do_menu')
    source = replace(source, '    while (!exit)\n    {\n        button = rb->get_action',
                     '    while (!exit && !been_in_usb_mode)\n    {\n        button = rb->get_action')
    source = replace(source, '                return edit_memo(pointer_array[selected], shown);',
                     '''                rb->gui_synclist_scroll_stop(&gui_memos);
                return edit_memo(pointer_array[selected], shown);''')
    source = replace(source, '            case ACTION_STD_CANCEL:\n                return false;',
                     '''            case ACTION_STD_CANCEL:
                rb->gui_synclist_scroll_stop(&gui_memos);
                return false;''')
    source = replace(source, '    while (!exit)\n    {\n        button = rb->button_get',
                     '    while (!exit && !been_in_usb_mode && !rb->plugin_usb_taken())\n    {\n        button = rb->button_get')
    source = replace(source, '    if (rb->memcmp(&old_info, &info, sizeof(struct info)))',
                     '''    if (rb->plugin_usb_taken() || been_in_usb_mode)
        return PLUGIN_USB_CONNECTED;
    if (rb->memcmp(&old_info, &info, sizeof(struct info)))''')
    return replace(source, '    return been_in_usb_mode?PLUGIN_USB_CONNECTED:PLUGIN_OK;',
                   '    return PLUGIN_OK;')


def shopper(source):
    check_digest(source, "dc1c5073281f224f034f8af00141f9754b23f6b531451ddeb483c608604e8f60")
    start = source.index('static bool save_changes(')
    end = source.index('static void create_view(', start)
    writer = Path(__file__).with_name("shopper-file.inc").read_text()
    source = source[:start] + writer + '\n' + source[end:]
    source = replace(source, '#define MAX_LINE_LEN (DESC_SIZE + 1)',
                     '#define MAX_LINE_LEN (DESC_SIZE + 4)')
    source = replace(source, 'static char temp_line[DESC_SIZE];',
                     'char temp_line[MAX_LINE_LEN];')
    source = replace(source, '    static int count = 0;\n    int linelen;',
                     '    int read, linelen;')
    source = replace(source, '    total_item_count = 0;\n\n    fd',
                     '''    total_item_count = 0;
    view = EDIT_SHOPPING_LIST;
    show_categories = true;
    update_category_string();

    fd''')
    source = replace(source, '    while (rb->read_line(fd,temp_line,MAX_LINE_LEN))\n    {',
                     '''    while ((read = rb->read_line(fd, temp_line, sizeof(temp_line))) > 0)
    {
        if (read >= MAX_LINE_LEN - 1)
            goto invalid;
        if (!temp_line[0])
            continue;''')
    source = replace(source, 'if (val <= 2)', 'if (val <= 1)')
    start = source.index('            new_item.id = count;')
    end = source.index('    }\n    rb->close(fd);', start)
    source = source[:start] + '''            const char *description = temp_line;
            new_item.flag = FL_CLEARED;
            switch (*description)
            {
                case '#': new_item.flag = FL_CATEGORY; description++; break;
                case '!': new_item.flag = FL_SET; description++; break;
                case ' ': description++; break;
            }
            if (total_item_count == MAX_LIST_SIZE ||
                rb->strlcpy(new_item.desc, description, sizeof(new_item.desc)) >=
                sizeof(new_item.desc))
                goto invalid;
            new_item.id = total_item_count;
            items_list[total_item_count++] = new_item;
        }
''' + source[end:]
    source = replace(source, '    rb->close(fd);\n    changed = false;\n    return PLUGIN_OK;',
                     '''    if (read < 0)
        goto invalid;
    rb->close(fd);
    changed = false;
    return PLUGIN_OK;

invalid:
    rb->close(fd);
    rb->splash(HZ * 2, "Shopping list cannot be read");
    return PLUGIN_ERROR;''')
    source = replace(source, 'static bool toggle(int selected_item)\n{',
                     '''static bool toggle(int selected_item)
{
    if (selected_item < 0 || selected_item >= view_item_count)
        return false;''')
    start = source.index('#if LCD_DEPTH > 1', source.index('enum plugin_status plugin_start'))
    end = source.index('    /* now dump it in the list */', start)
    source = source[:start] + '''    if (!parameter)
    {
        rb->splash(HZ, "Open a .shopper file in Files");
        return PLUGIN_OK;
    }
    if (rb->strlcpy(filename, parameter, sizeof(filename)) >= sizeof(filename) ||
        load_file() != PLUGIN_OK)
        return PLUGIN_ERROR;

''' + source[end:]
    source = replace(source, '    rb->lcd_update();\n\n    while (!exit)',
                     '    while (!exit && !rb->plugin_usb_taken())')
    menu = '                        switch (rb->do_menu(&menu, NULL, NULL, false))\n                        {'
    if source.count(menu) != 2:
        raise ValueError("expected two Shopper menus")
    source = source.replace(menu, menu + '\n                            case MENU_ATTACHED_USB:\n                                return PLUGIN_USB_CONNECTED;')
    source = replace(source, '                exit = 1;\n                break;\n            }\n        }',
                     '''                exit = 1;
                break;
            }
            default:
                if (rb->default_event_handler(button) == SYS_USB_CONNECTED)
                    return PLUGIN_USB_CONNECTED;
                break;
        }''')
    return replace(source, '    return PLUGIN_OK;\n}',
                   '    return rb->plugin_usb_taken() ? PLUGIN_USB_CONNECTED : PLUGIN_OK;\n}')


def darts(source):
    # A click scores a dart. A hold opens the pause menu without scoring.
    source = replace(source, """#define DARTS_QUIT PLA_SELECT_REPEAT
#else
#define DARTS_QUIT PLA_CANCEL
#endif
#define DARTS_SELECT PLA_SELECT""", """#define DARTS_QUIT PLA_SELECT_REPEAT
#define DARTS_SELECT PLA_SELECT_REL
#else
#define DARTS_QUIT PLA_CANCEL
#define DARTS_SELECT PLA_SELECT
#endif""")
    source = replace(source, """    int fd = rb->open(RESUME_FILE, O_WRONLY | O_CREAT, 0666);
    if (fd < 0)
        return;

    rb->write(fd, &settings, sizeof(struct settings_struct));

    rb->close(fd);
    rb->lcd_update();""", """    const char *temp = RESUME_FILE ".tmp";
    int fd = rb->open(temp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
    {
        rb->splash(HZ, "Could not save game");
        return;
    }

    ssize_t written = rb->write(fd, &settings, sizeof(settings));
    int closed = rb->close(fd);
    if (written != sizeof(settings) || closed < 0 ||
        rb->rename(temp, RESUME_FILE) < 0)
    {
        rb->remove(temp);
        rb->splash(HZ, "Could not save game");
    }""")
    source = replace(source, '        loaded = true;',
                     '''        const unsigned char *turn = (const unsigned char *)&settings.turn;
        loaded = *turn <= 1 &&
                 (settings.mode == five || settings.mode == three) &&
                 settings.throws >= 1 && settings.throws <= 3 &&
                 settings.history_ptr >= 0 && settings.history_ptr <= MAX_UNDO;
        for (int i = 0; loaded && i < NUM_PLAYERS; ++i)
            loaded = settings.scores[i] > 1 && settings.scores[i] <= 501;
        for (int i = 0; loaded && i < settings.history_ptr; ++i)
            loaded = settings.history[i] >= -1 && settings.history[i] <= 60;''')
    source = replace(source, 'settings.throws < 3 && settings.history[settings.history_ptr - 1] < 0;',
                     'settings.throws < 3 && settings.history_ptr > 0 &&\n'
                     '               settings.history[settings.history_ptr - 1] < 0;')
    source = replace(source, '                if (hit > settings.scores[settings.turn])',
                     '''                /* Drop a whole turn to keep the undo boundary aligned. */
                if (settings.history_ptr + settings.throws > MAX_UNDO)
                {
                    settings.history_ptr -= 3;
                    rb->memmove(settings.history, settings.history + 3,
                                settings.history_ptr * sizeof(settings.history[0]));
                }
                if (hit > settings.scores[settings.turn])''')
    source = replace(source, """    if (response == YESNO_NO)
        return false;
    else
        return true;""", '    return response == YESNO_YES;')
    source = replace(source, '                settings.mode = three;\n                break;\n            }',
                     '''                settings.mode = three;
                break;
            }
            case GO_TO_PREVIOUS:
            case GO_TO_ROOT:
                _exit(PLUGIN_OK);
            case MENU_ATTACHED_USB:
                _exit(PLUGIN_USB_CONNECTED);''')
    source = replace(source, '            switch (do_dart_scorer_pause_menu())\n            {',
                     '''            switch (do_dart_scorer_pause_menu())
            {
            case MENU_ATTACHED_USB:
                return PLUGIN_USB_CONNECTED;''')
    source = replace(source, """        case 5:
            return 3;
        default:""", """        case 5:
            return 3;
        case GO_TO_PREVIOUS:
        case GO_TO_ROOT:
            return 0;
        case MENU_ATTACHED_USB:
            return MENU_ATTACHED_USB;
        default:""")
    return replace(source, """        case 5:
            if (loaded)
                save_game();
            return PLUGIN_OK;
        default:""", """        case 5:
            if (loaded)
                save_game();
            return PLUGIN_OK;
        case GO_TO_PREVIOUS:
        case GO_TO_ROOT:
            return PLUGIN_OK;
        case MENU_ATTACHED_USB:
            return PLUGIN_USB_CONNECTED;
        default:""")


def resistor(source):
    source = replace(source, '#include "lib/helper.h"',
                     '#include "lib/helper.h"\n#include "lib/pluginlib_exit.h"')
    for button in ('button_press', 'button_input'):
        source = replace(source, f'        {button} = rb->button_get(true);',
                         f'        {button} = rb->button_get(true);\n        exit_on_usb({button});')
    source = replace(source, '        while ((button_press = rb->button_get(true)) & BUTTON_REL);',
                     '''        do {
            button_press = rb->button_get(true);
            exit_on_usb(button_press);
        } while (button_press & BUTTON_REL);''')
    return replace(source, '            case 4:\n                menuquit = true;',
                   '            case GO_TO_PREVIOUS:\n            case GO_TO_ROOT:\n            case 4:\n                menuquit = true;')


def vu_meter(source):
    source = replace(source, '#include "lib/playback_control.h"\n', '')
    source = replace(source, '''static void change_volume(int delta) {
    int minvol = rb->sound_min(SOUND_VOLUME);
    int maxvol = rb->sound_max(SOUND_VOLUME);
    int vol = rb->global_status->volume + delta;

    if (vol > maxvol) vol = maxvol;
    else if (vol < minvol) vol = minvol;
    if (vol != rb->global_status->volume) {
        rb->sound_set(SOUND_VOLUME, vol);
        rb->lcd_putsxyf(0,0, "%d", vol);
        rb->lcd_update();
        rb->sleep(HZ/12);
    }
}

''', '')
    source = replace(source, '''    int selection;
    bool menu_quit = false;''', '''    int selection = 0;
    bool menu_quit = false;''')
    source = replace(source, '''"Minimeters","Decay Speed","Playback Control",
                        "Quit");''', '''"Minimeters","Decay Speed","Quit");''')
    source = replace(source, '''    FOR_NB_SCREENS(i)
        rb->viewportmanager_theme_enable(i, true, NULL);
    while (!menu_quit) {''',
                     '    while (!menu_quit && !rb->plugin_usb_taken()) {')
    source = replace(source, '''            case 4:
                playback_control(NULL);
                break;

            case 5:''', '            case 4:')
    source = replace(source, '''    FOR_NB_SCREENS(i)
        rb->viewportmanager_theme_undo(i, false);
    /* the menu uses the userfont, set it back to sysfont */
    rb->lcd_setfont(FONT_SYSFIXED);
    return exit;''', '    return exit;')
    source = replace(source, '''            rb->read(fp, &vumeter_settings, sizeof(struct saved_settings));
            rb->close(fp);''', '''        ssize_t size = rb->read(fp, &vumeter_settings, sizeof(vumeter_settings));
        rb->close(fp);
        if (size != sizeof(vumeter_settings) ||
            (vumeter_settings.meter_type != ANALOG &&
             vumeter_settings.meter_type != DIGITAL) ||
            (unsigned)vumeter_settings.analog_decay > 6 ||
            (unsigned)vumeter_settings.digital_decay > 6 ||
            *(unsigned char *)&vumeter_settings.analog_use_db_scale > 1 ||
            *(unsigned char *)&vumeter_settings.digital_use_db_scale > 1 ||
            *(unsigned char *)&vumeter_settings.analog_minimeters > 1 ||
            *(unsigned char *)&vumeter_settings.digital_minimeters > 1)
            reset_settings();''')
    source = replace(source, '''    if(vumeter_settings.analog_use_db_scale) {''',
                     '''    /* A full-scale sample still falls in the last pixel column. */
    left_peak = MIN(left_peak, MAX_PEAK - 1);
    right_peak = MIN(right_peak, MAX_PEAK - 1);
    if(vumeter_settings.analog_use_db_scale) {''')
    for channel in ("left", "right"):
        source = replace(source, f'digital_db_scale[{channel}_peak * 44 / MAX_PEAK]',
                         f'digital_db_scale[{channel}_peak * (ARRAYLEN(digital_db_scale) - 1) / MAX_PEAK]')
    source = replace(source, '''static void draw_title(void)
{
    static unsigned int sampr;
    static int x;
    static char title[16];

    if (sampr != rb->mixer_get_frequency())
    {
        sampr = rb->mixer_get_frequency();
        if ((sampr % 1000) < 100)
            rb->snprintf(title, sizeof title, "VU %d kHz",
                         sampr / 1000);
        else
            rb->snprintf(title, sizeof title, "VU %d.%u kHz",
                         sampr / 1000, (sampr % 1000) / 100);

        rb->font_getstringsize(title, &x, NULL, FONT_SYSFIXED);
        x = half_width - (x/2); /* centered */
    }
    rb->lcd_putsxy(x, 0, title);
}''', '''static void draw_title(int status)
{
    char title[32];
    int width;

    if (!(status & AUDIO_STATUS_PLAY))
        rb->snprintf(title, sizeof(title), "Playback stopped");
    else if (status & AUDIO_STATUS_PAUSE)
        rb->snprintf(title, sizeof(title), "Playback paused");
    else
    {
        unsigned rate = rb->mixer_get_frequency();
        rb->snprintf(title, sizeof(title), "Playback %u.%u kHz",
                     rate / 1000, (rate % 1000) / 100);
    }
    rb->font_getstringsize(title, &width, NULL, FONT_SYSFIXED);
    rb->lcd_putsxy(half_width - width / 2, 0, title);
}''')
    source = replace(source, '''    load_settings();
    rb->lcd_setfont(FONT_SYSFIXED);''', '''    load_settings();
    rb->lcd_set_backdrop(NULL);
    rb->lcd_set_background(LCD_BLACK);
    rb->lcd_set_foreground(LCD_WHITE);
    rb->lcd_setfont(FONT_SYSFIXED);''')
    source = replace(source, '''        draw_title();

#ifdef USB_ENABLE_AUDIO
        if (rb->usb_audio_get_playing())
            rb->mixer_channel_calculate_peaks(PCM_MIXER_CHAN_USBAUDIO,
                                              &peaks);
        else
#endif
            rb->mixer_channel_calculate_peaks(PCM_MIXER_CHAN_PLAYBACK,
                                              &peaks);''', '''        int status = rb->audio_status();
        draw_title(status);

        if ((status & (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE)) == AUDIO_STATUS_PLAY)
            rb->mixer_channel_calculate_peaks(PCM_MIXER_CHAN_PLAYBACK, &peaks);
        else
        {
            peaks.left = peaks.right = 0;
            last_left_needle_top_x = last_num_left_leds = last_num_right_leds = 0;
            last_right_needle_top_x = half_width;
        }''')
    source = replace(source, '                rb->lcd_puts(0, 2, LABEL_VOLUME ": Volume");',
                     '                rb->lcd_puts(0, 2, "Playback levels (read only)");')
    return replace(source, '''                if(vu_meter_menu())
                    return PLUGIN_OK;
                break;

            case VUMETER_UP:
            case VUMETER_UP | BUTTON_REPEAT:
                change_volume(1);
                break;

            case VUMETER_DOWN:
            case VUMETER_DOWN | BUTTON_REPEAT:
                change_volume(-1);
                break;''', '''                {
                    bool quit = vu_meter_menu();
                    if (rb->plugin_usb_taken())
                        return PLUGIN_USB_CONNECTED;
                    if (quit)
                    {
                        save_settings();
                        return PLUGIN_OK;
                    }
                }
                break;''')


if __name__ == "__main__":
    infile, outfile = map(Path, sys.argv[1:])
    adapters = {
        "db_folder_select": folder_picker, "dict": dictionary,
        "credits": credits, "alarmclock": alarmclock, "calendar": calendar,
        "dart_scorer": darts, "resistor": resistor, "shopper": shopper,
        "vu_meter": vu_meter,
    }
    outfile.write_text(adapters[infile.stem](infile.read_text()))
