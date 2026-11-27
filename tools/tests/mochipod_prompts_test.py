"""MochiPod's yes/no prompts answer Center = Yes and Menu = No.

Stages MochiPod's pinned source with apps-ipod/plugins/mochipod/*.patch
applied, as mochipod.make does, then builds and runs mochipod_prompts_test.c
on the host. Pass a directory to also get one PGM frame of each prompt.
The source comes from $MOCHIPOD_DIR or ~/.cache/podbox-mochipod/<commit>,
fetched there the first time like the firmware build does."""
from pathlib import Path
import os, re, shutil, subprocess, sys, tempfile

R = Path(__file__).resolve().parents[2]
make = (R / 'apps-ipod/plugins/mochipod.make').read_text()
url = re.search(r'^MOCHIPOD_URL := (\S+)', make, re.M).group(1)
commit = re.search(r'^MOCHIPOD_COMMIT := (\S+)', make, re.M).group(1)
src = Path(os.environ.get('MOCHIPOD_DIR',
           Path.home() / '.cache/podbox-mochipod' / commit))

if not (src / '.fetched').exists():
    shutil.rmtree(src, ignore_errors=True)
    src.mkdir(parents=True)
    git = ['git', '-C', str(src)]
    subprocess.run(git + ['init', '-q'], check=True)
    subprocess.run(git + ['fetch', '-q', '--depth', '1', url, commit], check=True)
    subprocess.run(git + ['checkout', '-q', commit], check=True)
    (src / '.fetched').touch()

out = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else None
with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp) / 'mochipod'
    shutil.copytree(src / 'src', root / 'src')
    for patch in sorted((R / 'apps-ipod/plugins/mochipod').glob('*.patch')):
        with open(patch, 'rb') as f:
            subprocess.run(['patch', '-s', '-p1', '-d', str(root)], stdin=f,
                           check=True)
    units = ['core/sound', 'core/shop', 'core/family', 'core/rtc_guard',
             'core/growth', 'core/characters', 'core/game', 'core/ui',
             'core/play', 'input/wheel', 'render/lcd']
    exe = Path(tmp) / 'prompts'
    subprocess.run([os.environ.get('CC', 'gcc'), '-std=c99', '-O1', '-Wall',
                    '-Wextra', '-Werror', '-DMOCHI_LAYOUT_TEST',
                    '-I', str(root),
                    str(R / 'tools/tests/mochipod_prompts_test.c'),
                    *[str(root / 'src' / (u + '.c')) for u in units],
                    '-o', str(exe)], check=True)
    if out:
        out.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(exe)] + ([str(out)] if out else []), check=True,
                   timeout=60)
