#!/usr/bin/env python3
"""Stage rb_info's native list adapter. GPL-2.0-or-later."""
from hashlib import sha256
from pathlib import Path
import sys

upstream, adapter, output = map(Path, sys.argv[1:])
source = upstream.read_bytes()
if sha256(source).hexdigest() != (
    "92930c8b0742759048a80b4b65eb0fb59aeb75afd8a865266673ff6fa4f4545c"
):
    sys.exit(f"{upstream}: review rb_info source conversion")

# Keep the upstream attribution and diagnostic data. The adapter owns the
# lists and input loop, and has no dependency on stock UI internals.
header, body = source.decode().split('#include "plugin.h"\n', 1)
buffers = body[body.index("struct menu_buffer_t {"):body.index("/* stringify")]
paths = body[body.index("struct paths {"):body.index("#define TESTPUT_HEADER")]
buffers = buffers.replace("#ifndef CACHE_SIZE\n#define CACHE_SIZE (0)\n#endif\n", "")
buffers = buffers.replace('    {"cache", CACHE_SIZE},',
                          '#ifdef CACHE_SIZE\n    {"cache", CACHE_SIZE},\n#endif')
output.write_text(
    header + '#include "logf.h"\n#include "plugin.h"\n#include <limits.h>\n'
    '#include "rb-info-input.h"\n\n#define MACROSTR(x) #x\n'
    '#define MACROVAL(x) MACROSTR(x)\n\n'
    + buffers + paths + "\n" + adapter.read_text()
)
