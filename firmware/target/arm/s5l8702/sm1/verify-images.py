"""Reassemble the packaged X17 images and verify their published pins."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory

root = Path(__file__).resolve().parent
profiles = json.loads((root / 'profiles.json').read_text())
with TemporaryDirectory() as build:
    assembler = Path(build) / 'image-builder'
    x = 'x17-pinned-compat/src'
    subprocess.run([os.environ.get('ZIG', 'zig'), 'build-exe', '-OReleaseFast',
                    '--dep', 'asm', '--dep', 'image', '-Mroot=image-builder.zig',
                    '--dep', 'isa', f'-Masm={x}/asm.zig', '--dep', 'asm',
                    f'-Mimage={x}/image.zig', f'-Misa={x}/isa.zig',
                    f'-femit-bin={assembler}'], check=True, cwd=root)
    for name, profile in profiles.items():
        binary = Path(build) / (name + '.x17i')
        subprocess.run([assembler, profile['source'], binary], cwd=root, check=True)
        data = binary.read_bytes()
        if (hashlib.sha256(data).hexdigest() != profile['sha256'] or
                data != (root / 'images' / binary.name).read_bytes()):
            raise ValueError(f'{name} differs from its published image')
        print(name, profile['sha256'])
