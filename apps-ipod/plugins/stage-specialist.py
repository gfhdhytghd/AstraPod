#!/usr/bin/env python3
"""Stage specialist service entry points. GPL-2.0-or-later."""
from hashlib import sha256
from pathlib import Path
import sys

DIGESTS = {
    "file_picker": "e5b74cbd2d30ec252c8081b771877a3d3824cafb24b2058a2c930ab50c09afff",
    "main_menu_config": "d275d5908cbaaaccb70039b87cb5a77d42f9f29bbc0da1f19d7604760dde5305",
    "properties": "fa50a92d228350a878de76d95bf4350dab2e0f2649928a50988569cd5be7a3b2",
    "theme_remove": "000f98068e0c47a934d92be1501dfd6c0f2e95f2846b2d79cca9b0c4b54b7aa6",
    "pitch_screen": "482bc4124219f0e6cc0a5d5575bbf9e9ec41382975d56418031b312ab8633ad7",
}


def splice(source, left, right, replacement):
    start = source.index(left)
    end = source.index(right, start)
    return source[:start] + replacement + source[end:]


def file_picker(source):
    source = source.replace('#include "lang_enum.h"', '''#include "lang_enum.h"
#include "pathfuncs.h"
#include "files/filetypes.h"''')
    source = splice(source, '#if defined(DEBUG)', '#define FIND_NODIRS',
                    '#define logf(...) DEBUGF(__VA_ARGS__)\n\n')
    source = source.replace('    long num;\n', '')
    source = source.replace('    const char* start = *parameter;\n'
                            '    while (*parameter[0] > \'/\' && ispunct(*parameter[0])) '
                            '(*parameter)++;',
                            "    if (**parameter == '=')\n        (*parameter)++;")
    source = splice(source, "        case 'r'", "        case 't'",
                    '''        case 'r':
            ret = string_parse(parameter, pfp->return_plugin,
                               sizeof(pfp->return_plugin));
            if (ret >= (int)sizeof(pfp->return_plugin))
                pfp->return_plugin[0] = 0;
            break;
''')
    source = source.replace("if (pfp->file_ext[1] == '*')",
                            'if (!rb->strcmp(pfp->file_ext, ".*"))')
    source = source.replace('if (pfp->flags & FIND_EXT)',
                            'if (pfp->flags & (FIND_EXT | FIND_WILDCARD))')
    source = splice(source, "        case 'a'", "        default :", r'''        case 'a':
        {
            char digits[8];
            ret = string_parse(parameter, digits, sizeof(digits));
            if (!ret || ret >= (int)sizeof(digits))
                break;
            unsigned int attr = 0;
            char *p = digits;
            while (*p >= '0' && *p <= '9')
                attr = attr * 10 + (*p++ - '0');
            if (!*p && attr && attr <= FILE_ATTR_MASK &&
                !(attr & ~FILE_ATTR_MASK))
            {
                pfp->tree_attr = attr;
                pfp->flags |= FIND_ATTRIB;
            }
            break;
        }
''')
    source = splice(source, 'static bool cb_show_item', 'enum plugin_status plugin_start',
                    r'''static int browse_file_dir(struct fpp *pfp)
{
    char path[MAX_PATH];
    const char *extensions = pfp->file_ext[0] ? pfp->file_ext : NULL;
    int ret = rb->pick_file(path, sizeof(path), pfp->start_dir, pfp->title,
                            extensions, pfp->tree_attr,
                            (pfp->flags & FIND_NODIRS) != 0);
    if (rb->plugin_usb_taken())
        return PLUGIN_USB_CONNECTED;
    if (ret < 0)
        return PLUGIN_ERROR;
    if (!ret)
        return PLUGIN_OK;

    if (pfp->return_plugin[0])
        return rb->queue_handoff(pfp->return_plugin, path);
    return rb->view_text("Selected file", path);
}

''')
    return splice(source, '    if (!parameter)', '    if (fpp.title',
                  '''    if (parameter)
        argparse((const char*) parameter, -1, &fpp, &arg_callback);

''')


original, entries, output = map(Path, sys.argv[1:])
raw = original.read_bytes()
if sha256(raw).hexdigest() != DIGESTS[original.stem]:
    sys.exit(f"{original}: review the AstraPod specialist adapter for this source")

source = raw.decode()
if original.stem == "file_picker":
    output.write_text(file_picker(source))
else:
    header = source[:source.index('#include "plugin.h"')]
    output.write_text(header + '#include "plugin.h"\n' +
                      f'#define ROCK_SPECIALIST_{original.stem.upper()}\n' +
                      entries.read_text())
