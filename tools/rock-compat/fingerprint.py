#!/usr/bin/env python3
"""Write a C initializer for the SHA-256 of the preprocessed SDK."""
from hashlib import sha256
from pathlib import Path
import sys

source, header = map(Path, sys.argv[1:])
digest = sha256(source.read_bytes()).digest()
initializer = ", ".join(f"0x{byte:02x}" for byte in digest)
result = f"#define ROCK_FINGERPRINT {{ {initializer} }}\n"
if not header.exists() or header.read_text() != result:
    header.write_text(result)
