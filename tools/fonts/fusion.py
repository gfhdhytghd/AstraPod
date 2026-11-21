"""Read pinned Fusion Pixel BDF assets used by PodBox."""
from io import BytesIO
import hashlib
from pathlib import Path
import sys
from urllib.request import urlopen
from zipfile import ZipFile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "arcade"))
from bdf import read as read_bdf

FUSION_CACHE = Path.home() / ".cache/podbox-font"
FUSION_VERSION = "2026.11.10"
FUSION_SHA256 = {
    8: "f4eb1511153c43019883bd4402457e52ba8ef2062a1445ad9d7cf8ab0e6c5dd3",
    10: "a6c58b0d23eb0e0dc011ab11c49974b9fccd069f22a89a49e889d205dbd4a2f3",
    12: "bbf8efa39f165dee5e52ed1c2ccc21f7d7d32dd6e536190cba55065220b2c0a5",
}


def load_fusion(size, cache, scratch):
    name = f"fusion-pixel-font-{size}px-monospaced-bdf-v{FUSION_VERSION}.zip"
    path = cache / name
    if path.is_file():
        data = path.read_bytes()
    else:
        url = f"https://github.com/TakWolf/fusion-pixel-font/releases/download/{FUSION_VERSION}/{name}"
        with urlopen(url, timeout=30) as response:
            data = response.read()
    if hashlib.sha256(data).hexdigest() != FUSION_SHA256[size]:
        raise ValueError(f"Unexpected checksum: {name}")
    if not path.is_file():
        cache.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    member = f"fusion-pixel-{size}px-monospaced-zh_hans.bdf"
    with ZipFile(BytesIO(data)) as archive:
        bdf = scratch / member
        bdf.write_bytes(archive.read(member))
    return read_bdf(bdf)[2]


def glyph_bitmap(glyph):
    ink = bytes(255 if row & (1 << bit) else 0
                for row in glyph.rows for bit in reversed(range(glyph.w)))
    return (glyph.xo, glyph.yo + glyph.h, glyph.w, glyph.h), ink
