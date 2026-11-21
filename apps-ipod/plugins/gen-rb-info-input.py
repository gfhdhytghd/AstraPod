#!/usr/bin/env python3
"""Name the compiled AstraPod action, context and button values. GPL-2.0-or-later."""
from pathlib import Path
import re
import sys

action_path, button_path, output = map(Path, sys.argv[1:])
enums = action_path.read_text()
macros = button_path.read_text()

tables = []
for kind in ("ACTION", "CONTEXT"):
    names = re.findall(rf"^\s*({kind}_\w+)(?:\s*=.*)?[,]\s*$", enums, re.M)
    if not names or len(names) != len(set(names)):
        sys.exit(f"Missing or duplicate {kind} enumeration")
    # Let the compiler evaluate assigned values. The size check in C catches
    # array indexes outside the native enumeration.
    items = "".join(f'    [{name}] = "{name}",\n' for name in names)
    tables.append(
        f"static const char *const {kind.lower()}_names[LAST_{kind}_PLACEHOLDER] = {{\n"
        f"{items}}};\n"
    )

buttons = sorted(re.findall(r"^#define (BUTTON_\w+) .+$", macros, re.M))
if "BUTTON_NONE" not in buttons or "BUTTON_SELECT" not in buttons:
    sys.exit("Missing native button definitions")
items = "".join(f'    {{"{name}", {name}}},\n' for name in buttons)
tables.append("static const struct { const char *name; unsigned int value; } "
              f"buttons[] = {{\n{items}}};\n")
output.write_text("\n".join(tables))
