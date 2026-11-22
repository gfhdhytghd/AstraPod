#!/usr/bin/env python3
"""Stage bounded storage and foreground lifecycle adapters for Keybox."""
from hashlib import sha256
from pathlib import Path
import sys


source, storage, output = map(Path, sys.argv[1:])
data = source.read_bytes()
digest = "adf9211a9a72f3be093d3acd1d999724bdfb5b179ac01097dd1e93bc2450a439"
if sha256(data).hexdigest() != digest:
    sys.exit("Keybox upstream source changed; review the storage adapter")
text = data.decode()


def replace(old, new):
    global text
    if text.count(old) != 1:
        sys.exit(f"Keybox staging expected one match for {old!r}")
    text = text.replace(old, new)


def section(start, end, replacement):
    first = text.index(start)
    last = text.index(end, first)
    replace(text[first:last], replacement)


replace("#define MAX_ENTRIES 12*BLOCK_SIZE /* keep this a multiple of BLOCK_SIZE */",
        "#define MAX_ENTRIES 96")
replace("static char buffer[sizeof(struct pw_entry)*MAX_ENTRIES];",
        """#define MAX_FILE_SIZE ALIGN_UP(HEADER_LEN + MAX_ENTRIES * 3 * FIELD_LEN + 1, \\
                               BLOCK_SIZE)
static char buffer[MAX_FILE_SIZE];""")
replace("    FILE_OPEN_ERROR = -1", """\
    FILE_OPEN_ERROR = -1,
    FILE_SAVE_ERROR = -2,
    WRONG_PASSWORD = -3""")
section("static void init_ll(void)", "static void delete_entry", """\
static void init_ll(void)
{
    rb->memset(&pw_list, 0, sizeof(pw_list));
    rb->gui_synclist_set_nb_items(&kb_list, 0);
    rb->gui_synclist_select_item(&kb_list, 0);
}

static void close_keybox(void)
{
    rb->gui_synclist_scroll_stop(&kb_list);
    init_ll();
    rb->memset(master_pw, 0, sizeof(master_pw));
    rb->memset(buffer, 0, sizeof(buffer));
    rb->memset(&pwhash, 0, sizeof(pwhash));
    rb->memset(&key, 0, sizeof(key));
    bytes_read = 0;
    salt = 0;
    data_changed = false;
}

""")
replace("""    entry2->used = false;
    entry2->name[0] = '\\0';
    entry2->password[0] = '\\0';
    entry2->next = NULL;""", "    rb->memset(entry2, 0, sizeof(*entry2));")
replace("""    int i, j;
    struct pw_entry *entry = pw_list.first.next;""",
        """    int i, j;
    struct pw_entry *entry = &pw_list.first;""")
replace("    if (rb->kbd_input(pw_list.entries[i].title, FIELD_LEN, NULL) < 0)\n        return;",
        """    if (rb->kbd_input(pw_list.entries[i].title, FIELD_LEN, NULL) < 0 ||
        !pw_list.entries[i].title[0])
        return;""")
replace("""    if (entry->next == entry)
        entry->next = NULL;

""", "")
section("static void edit_title", "static void context_menu", """\
static void edit_field(char field[FIELD_LEN], bool required)
{
    char input[FIELD_LEN];
    rb->strlcpy(input, field, sizeof(input));
    if (rb->kbd_input(input, sizeof(input), NULL) == 0 &&
        (!required || input[0]))
    {
        rb->strlcpy(field, input, FIELD_LEN);
        data_changed = true;
    }
    rb->memset(input, 0, sizeof(input));
}

static struct pw_entry *entry_at(int selected)
{
    struct pw_entry *entry = pw_list.first.next;
    for (int i = 0; entry && i < selected; ++i)
        entry = entry->next;
    return entry;
}

""")
section("static void context_menu", "static void splash_pw", """\
static void context_menu(int selected_item)
{
    int selection = 0;
    struct pw_entry *entry = entry_at(selected_item);
    switch (rb->do_menu(&context_m, &selection, NULL, false))
    {
        case 0:
            add_entry(selected_item);
            break;
        case 1:
            if (entry)
                edit_field(entry->title, true);
            break;
        case 2:
            if (entry)
                edit_field(entry->name, false);
            break;
        case 3:
            if (entry)
                edit_field(entry->password, false);
            break;
        case 4:
            if (entry)
                delete_entry(selected_item);
            break;
        case 5:
            playback_control(NULL);
            break;
    }
}

""")
section("static void splash_pw", "static void hash_pw", """\
static void show_entry(int selected_item)
{
    struct pw_entry *entry = entry_at(selected_item);
    if (!entry)
        return;
    char details[2 * FIELD_LEN];
    if (entry->name[0])
        rb->snprintf(details, sizeof(details), "%s\\n%s",
                     entry->name, entry->password);
    else
        rb->strlcpy(details, entry->password, sizeof(details));
    rb->view_text(entry->title, details);
    rb->memset(details, 0, sizeof(details));
}

""")
section("static int parse_buffer(void)", "static int enter_pw",
        storage.read_text() + "\n\n")
section("static int enter_pw", "static void reset", """\
static bool enter_pw(bool new_file)
{
    rb->splash(HZ, new_file ? "Enter new master password" :
                              "Enter master password");
    if (rb->kbd_input(master_pw, sizeof(master_pw), NULL) < 0)
        return false;
    if (new_file)
    {
        char confirmation[FIELD_LEN] = {0};
        rb->splash(HZ, "Confirm master password");
        bool matches = rb->kbd_input(confirmation, sizeof(confirmation), NULL) == 0
                       && !rb->strcmp(master_pw, confirmation);
        rb->memset(confirmation, 0, sizeof(confirmation));
        if (!matches)
        {
            if (!rb->plugin_usb_taken())
                rb->splash(HZ, "Password not confirmed");
            return false;
        }
    }
    hash_pw(&pwhash);
    return true;
}

static int keybox(void)
{
    bool new_file = !rb->file_exists(KEYBOX_FILE);
    if (!new_file && !read_keybox())
        return FILE_OPEN_ERROR;
    if (!enter_pw(new_file))
        return PLUGIN_OK;
    if (!new_file)
    {
        int result = parse_buffer();
        if (result != PLUGIN_OK)
            return result;
    }

    while (!rb->plugin_usb_taken())
    {
        rb->gui_synclist_draw(&kb_list);
        int action = rb->get_action(CONTEXT_LIST, TIMEOUT_BLOCK);
        if (rb->gui_synclist_do_button(&kb_list, &action))
            continue;
        if (action == ACTION_STD_CANCEL || action == ACTION_STD_MENU)
        {
            if (data_changed && !save_keybox())
                return FILE_SAVE_ERROR;
            return PLUGIN_OK;
        }
        if (action == ACTION_STD_OK)
            show_entry(rb->gui_synclist_get_sel_pos(&kb_list));
        else if (action == ACTION_STD_CONTEXT)
            context_menu(rb->gui_synclist_get_sel_pos(&kb_list));
        else if (rb->default_event_handler(action) == SYS_USB_CONNECTED)
            break;
    }
    return PLUGIN_USB_CONNECTED;
}

""")
section("    static const char *yes_lines[]=", "static int main_menu(void)", """\
    static const struct text_message message = {message_lines, 2};
    if (rb->gui_syncyesno_run(&message, NULL, NULL) == YESNO_YES &&
        rb->file_exists(KEYBOX_FILE))
    {
        if (rb->remove(KEYBOX_FILE) == 0)
            rb->splash(HZ, "Keybox reset");
        else
            rb->splash(HZ * 2, "Keybox not reset");
    }
}

""")
replace("""            ret = keybox();
            if (ret)
                return ret;""",
        """            ret = keybox();
            close_keybox();
            if (rb->plugin_usb_taken())
                return PLUGIN_USB_CONNECTED;
            if (ret == FILE_OPEN_ERROR)
                rb->splash(HZ * 2, "Cannot read keybox file");
            if (ret == FILE_SAVE_ERROR)
                rb->splash(HZ * 2, "Keybox changes not saved");""")
replace("""        case 3:
            exit = true;""",
        """        case MENU_ATTACHED_USB:
            return PLUGIN_USB_CONNECTED;
        case GO_TO_PREVIOUS:
        case GO_TO_ROOT:
        case 3:
            exit = true;""")
replace("    } while (!exit);", "    } while (!exit && !rb->plugin_usb_taken());")
replace("    return 0;\n}\n\nenum plugin_status plugin_start",
        "    return rb->plugin_usb_taken() ? PLUGIN_USB_CONNECTED : PLUGIN_OK;\n}\n\nenum plugin_status plugin_start")
section("    int ret;\n\n    rb->gui_synclist_init", "\n    init_ll();", """\
    rb->gui_synclist_init(&kb_list, &kb_list_cb, NULL, false, 1, NULL);
    rb->gui_synclist_set_title(&kb_list, "Keybox", NOICON);
""")
text = text[:text.index("    ret = main_menu();")] + "    return main_menu();\n}\n"
output.write_text(text)
