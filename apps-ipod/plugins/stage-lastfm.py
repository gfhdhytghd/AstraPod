#!/usr/bin/env python3
"""Stage native Last.fm plugins while preserving the upstream sources."""
from pathlib import Path
import re
import sys


def part(source, start, end):
    before, sep, keep = source.partition(start)
    if not sep or not before or end not in keep:
        raise ValueError(f"review upstream source around {start!r}")
    return start + keep[:keep.index(end)]


source, adapter, output = map(Path, sys.argv[1:])
text = source.read_text()
header = text[:text.index("#include \"plugin.h\"")]
stock_code = ""
if source.stem == "lastfm_scrobbler":
    stock_code += part(text, "struct scrobbler_entry", "static struct scrobbler_cfg")
    stock_code += part(text, "static int sbl_create_entry", "static int sbl_check_or_open")
    stock_code = stock_code.replace("    struct mp3entry id3, *id;",
                                    "    static struct mp3entry id3;\n    struct mp3entry *id;")
    stock_code, checked = re.subn(
        r"(?m)^([ ]+)(rb->fdprintf\(output_fd,[^;]+\));",
        lambda m: (f"{m[1]}if ({m[2]} < 0)\n"
                   f"{m[1]}    return SCROBBLER_LOG_ERROR;"), stock_code)
    if checked != 2:
        raise ValueError("review upstream scrobbler write sites")
    stock_code = stock_code.replace(
        "    const char *path = rb->strrchr(entry->path, '/');\n"
        "    if (!path)\n        path = entry->path;\n"
        "    else\n        path++; /* remove slash */",
        "    const char *name = rb->strrchr(entry->path, '/');\n"
        "    static char path[MAX_PATH];\n"
        "    if (rb->strlcpy(path, name ? name + 1 : entry->path,\n"
        "                    sizeof(path)) >= sizeof(path))\n"
        "        return SCROBBLER_LOG_ERROR;\n"
        "    str_chk_valid(path, \"\");")
    stock_code = stock_code.replace(
        "    rb->snprintf(track_len_rate_timestamp_mbid, sizeof(track_len_rate_timestamp_mbid),\n"
        "        \"%d\"SEP",
        "    int n = rb->snprintf(track_len_rate_timestamp_mbid,\n"
        "                         sizeof(track_len_rate_timestamp_mbid),\n"
        "        \"%d\"SEP")
    stock_code = stock_code.replace(
        'str_chk_valid(id->mb_track_id, ""));',
        'str_chk_valid(id->mb_track_id, ""));\n'
        '    if (n < 0 || (size_t)n >= sizeof(track_len_rate_timestamp_mbid))\n'
        '        return SCROBBLER_LOG_ERROR;')

output.write_text(header + '#include "plugin.h"\n' +
                  adapter.read_text().replace("/* Include stock formatter here. */",
                                              stock_code))
