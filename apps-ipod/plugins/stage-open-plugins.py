#!/usr/bin/env python3
"""Stage Open Plugins with AstraPod's foreground launch contract."""
from pathlib import Path
import sys


upstream, adapter, output = map(Path, sys.argv[1:])
source = upstream.read_text()
end = source.index('#include "plugin.h"') + len('#include "plugin.h"')
output.write_text(source[:end] + "\n" + adapter.read_text())
