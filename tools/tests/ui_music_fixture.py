"""Create isolated SDL media and build its database with Rockbox itself."""

from pathlib import Path
import os
import shutil
import struct
import subprocess
import time
from zipfile import ZipFile

from PIL import Image, ImageDraw


CONFIG = """\
start in screen: root
root menu order: database,files
backlight timeout: off
backlight on button hold: normal
voice menus: off
keyclick: off
show icons: on
animations: on
native theme: scrim
playlist catalog directory: /Playlists
"""

MUSIC_ALBUMS = (
    ("Aurora Sessions", "Aurora Duo", "Ambient", "2018"),
    ("Blue Hour", "Aurora Duo", "Jazz", "2020"),
    ("Cloud Atlas", "Belltower Quartet", "Classical", "2023"),
    ("Daybreak", "Belltower Quartet", "Jazz", "2024"),
    ("远方的回声与午夜车站现场录音珍藏版", "山间回声四重奏", "Folk", "2025"),
    ("红叶与雪原旅行现场录音特别版", "山间回声四重奏", "Folk", "2026"),
    ("Letters from the Coast", "Harbor Audio", "Audiobook", "2022"),
    ("In the Listening Room", "Fixture Radio", "Podcast", "2024"),
)
ENCODINGS = (("mp3", "libmp3lame"), ("flac", "flac"), ("m4a", "aac"))
TRACK_COUNT = len(MUSIC_ALBUMS) * len(ENCODINGS)
DATABASE = Path(".rockbox/library/database")


def create_fixture(destination, resource_zip, config=CONFIG):
    """Create a new fixture. Candidate host codecs can be added separately."""
    destination = Path(destination)
    destination.mkdir(parents=True)
    rockbox = destination / ".rockbox"
    with ZipFile(resource_zip) as resources:
        for name in resources.namelist():
            if name == ".rockbox/tagnavi.config" or name.startswith(
                (".rockbox/fonts/", ".rockbox/langs/", ".rockbox/codepages/",
                 ".rockbox/docs/", ".rockbox/games/")
            ):
                resources.extract(name, destination)

    (rockbox / "config.cfg").write_text(config, encoding="utf-8")
    (rockbox / "tagnavi_custom.config").write_text(
        '#! rockbox/tagbrowser/2.0\n\n%menu_start "custom" "Fixture Music"\n'
        '"By Composer" -> composer -> album -> title\n', encoding="utf-8"
    )

    media = []
    for number, (album, artist, genre, year) in enumerate(MUSIC_ALBUMS):
        folder = destination / "Music" / "Library" / "Collection" / year / artist / album / "Disc 1"
        folder.mkdir(parents=True)
        cover = Image.new("RGB", (192, 192), (36 + number * 20, 57, 110))
        ImageDraw.Draw(cover).ellipse((30, 30, 162, 162), fill=(215, 160, 78))
        cover.save(folder / "folder.jpg")
        for track, (extension, codec) in enumerate(ENCODINGS, 1):
            title = (f"Track {track:02d} – {album}" if number < 4 or number > 5 else
                     f"第{track:02d}章 都市夜景下的一段很长的乐章与现场鸣谢名单")
            path = folder / f"{track:02d} {title}.{extension}"
            tags = {
                "title": title,
                "artist": artist if track < 3 else f"{artist} feat. Guest",
                "album_artist": artist, "album": album, "genre": genre,
                "composer": f"Composer {number % 3 + 1}", "date": year,
                "track": f"{track}/3", "disc": "1/1", "comment": "SDL fixture",
            }
            metadata = [arg for key, value in tags.items()
                        for arg in ("-metadata", f"{key}={value}")]
            frequency = 196 + 14 * number + 11 * track
            subprocess.run([
                "ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error",
                "-filter_threads", "1", "-f", "lavfi", "-i",
                f"sine=frequency={frequency}:sample_rate=44100:duration=16",
                "-threads", "1", "-c:a", codec, *metadata, str(path),
            ], check=True)
            media.append(path.relative_to(destination).as_posix())

    playlist_dir = destination / "Playlists"
    playlist_dir.mkdir()
    (playlist_dir / "Fixture Mix.m3u8").write_text(
        "#EXTM3U\n" + "\n".join("/" + name for name in media[::2]) + "\n",
        encoding="utf-8",
    )
    (playlist_dir / "夜曲选集.m3u8").write_text(
        "#EXTM3U\n" + "\n".join("../" + name for name in media[12:18]) + "\n",
        encoding="utf-8",
    )
    documents = destination / "Documents"
    documents.mkdir()
    (documents / "阅读与收听须知.txt").write_text("SDL fixture\n测试文本\n", encoding="utf-8")
    (documents / "unrecognised.file").write_bytes(bytes(range(16)))
    (documents / ".hidden").touch()
    (documents / "empty.mp3").touch()
    (documents / "truncated.flac").write_bytes(b"fLaC\x00")
    pictures = destination / "Images"
    pictures.mkdir()
    shutil.copyfile(folder / "folder.jpg", pictures / "Fixture.jpg")


def install_sim_codecs(simulator, destination):
    """Copy only host modules from the directory that built this simulator."""
    codecs = Path(destination) / ".rockbox/codecs"
    codecs.mkdir(exist_ok=True)
    compiled = Path(simulator).parent / "lib/rbcodec/codecs"
    required = {"mpa.codec", "flac.codec", "aac.codec"}
    missing = required.difference(path.name for path in compiled.glob("*.codec"))
    if missing:
        raise RuntimeError(f"Build the SDL simulator codecs first: {', '.join(sorted(missing))}")
    for codec in compiled.glob("*.codec"):
        shutil.copyfile(codec, codecs / codec.name)


def database_count(destination):
    database = Path(destination) / DATABASE
    master = database / "database_idx.tcd"
    if (database / "database_swap.tcd").exists() or not master.exists():
        return 0
    with master.open("rb") as handle:
        header = handle.read(24)
    if len(header) != 24:
        return 0
    magic, _, count, _, commits, dirty = struct.unpack("<6i", header)
    if magic != 0x54434810 or commits < 1 or dirty:
        return 0
    return count


def build_tagcache(simulator, destination, log_path=None, min_tracks=TRACK_COUNT):
    """Build a database by opening the database screen in a private SDL root."""
    destination = Path(destination).resolve()
    config = destination / ".rockbox/config.cfg"
    saved = config.read_bytes()
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy",
               SDL_RENDER_DRIVER="software")
    command = [str(Path(simulator).resolve()), "--root", ".",
               "--nobackground", "--zoom", "1", "--screenshot"]
    config.write_bytes(saved + b"\nstart in screen: db\n")
    try:
        with open(log_path or os.devnull, "wb") as log:
            with subprocess.Popen(command, cwd=destination, env=env,
                                  stdout=log, stderr=subprocess.STDOUT) as firmware:
                try:
                    end = time.monotonic() + 45
                    while time.monotonic() < end and firmware.poll() is None:
                        count = database_count(destination)
                        if count >= min_tracks:
                            return count
                        time.sleep(0.05)
                    raise RuntimeError(f"Tagcache did not commit {min_tracks} tracks; log: {log_path}")
                finally:
                    if firmware.poll() is None:
                        firmware.terminate()
                        try:
                            firmware.wait(timeout=4)
                        except subprocess.TimeoutExpired:
                            firmware.kill()
                            firmware.wait()
    finally:
        config.write_bytes(saved)
