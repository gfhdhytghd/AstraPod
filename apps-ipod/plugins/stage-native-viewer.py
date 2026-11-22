#!/usr/bin/env python3
"""Route stock viewers through the native decoders and reading engine."""
from pathlib import Path
import sys


source, target = map(Path, sys.argv[1:3])
text = source.read_text()
entry = "enum plugin_status plugin_start(const void* "
if entry not in text:
    raise SystemExit(f"{source}: cannot locate upstream viewer entry")
header = text[:text.index('#include "plugin.h"')]
kind, title, extensions, attr = {
    "imageviewer": ("IMAGE", "Select an image", "", "FILE_ATTR_IMG"),
    "text_viewer": ("TEXT", "Select a document",
                    ".txt.md.nfo.html.htm.rtf.fb2.epub.mobi.prc.azw.azw3.pdb.docx.pdf",
                    "0"),
}[source.stem]
target.write_text(header + f'''#include "plugin.h"
#include "files/filetypes.h"

enum plugin_status plugin_start(const void *parameter)
{{
    char selected[MAX_PATH];
    const char *path = parameter;
    if (!path)
    {{
        int status = rb->pick_file(selected, sizeof(selected), "/",
                                  "{title}", "{extensions}", {attr}, false);
        if (status <= 0)
            return rb->plugin_usb_taken() ? PLUGIN_USB_CONNECTED : PLUGIN_OK;
        path = selected;
    }}
    return rb->queue_native_handoff(ROCK_HANDOFF_{kind}, path);
}}
''')
