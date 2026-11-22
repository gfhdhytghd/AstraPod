#!/usr/bin/env python3
"""Stage upstream XRick with the bounds checks in xrick-stage.patch.

Each hunk's old text must follow the previous hunk exactly, so a change
upstream stops the build instead of applying beside it. GPL v2 or later.
"""
from pathlib import Path
import shutil
import sys

src, patch, dst = map(Path, sys.argv[1:4])
if dst.exists():
    shutil.rmtree(dst)
shutil.copytree(src, dst)

hunks = {}
name = None
for line in patch.read_text().splitlines():
    if line.startswith("diff --git "):
        name = None
    elif line.startswith("+++ b/apps/plugins/xrick/"):
        name = line[len("+++ b/apps/plugins/xrick/"):]
        hunks.setdefault(name, [])
    elif line.startswith("@@"):
        hunks[name].append(([], []))
    elif name and hunks[name] and line[:1] in (" ", "-", "+", ""):
        before, after = hunks[name][-1]
        text = line[1:] + "\n"
        if line[:1] != "+":
            before.append(text)
        if line[:1] != "-":
            after.append(text)

for name, edits in hunks.items():
    path = dst / name
    code = path.read_text(encoding="latin-1")
    cursor = 0
    for before, after in edits:
        old, new = "".join(before), "".join(after)
        at = code.find(old, cursor)
        if at < 0:
            raise SystemExit(f"{src / name}: expected {old!r}")
        code = code[:at] + new + code[at + len(old):]
        cursor = at + len(new)
    path.write_text(code, encoding="latin-1")
