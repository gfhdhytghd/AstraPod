"""Offline PodLink fixture for the SDL random walker."""
import contextlib
from pathlib import Path
import runpy
import shutil
import socket
import struct
import subprocess
import tempfile
import threading

GENERATION = 42
LIBRARY = 7
ROWS = {
    "albums": [("album", "Fixture Album", "", "", 0, 1, "album", "album/a")],
    "artists": [("artist", "Fixture Artist", "", "", 0, 1, "artist", "artist/a")],
    "songs": [("track", "Fixture Track", "Fixture Artist", "", 180000, 0, "song",
               "play/library-songs/track")],
    "playlists": [("list", "Fixture Playlist", "", "", 0, 1, "playlist", "playlist/a")],
}


def build_mock(repo, output, jobs, desktop):
    helper = desktop / "tests/firmware/fu79_remote_art_sim.py"
    if not helper.is_file():
        raise RuntimeError(f"Offline peer fixture not found at {helper}. Set --desktop.")
    instrument = runpy.run_path(str(helper))["instrument"]
    source = output / "remote-src"
    source.mkdir()
    # Preserve the current tracked and untracked source, without copying any
    # build products or changing the checkout under test.
    files = subprocess.check_output(["git", "-C", str(repo), "ls-files", "-z",
                                     "--cached", "--others", "--exclude-standard"])
    for name in files.decode().split("\0"):
        if not name:
            continue
        path = repo / name
        if not path.is_file() and not path.is_symlink():
            continue
        target = source / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target, follow_symlinks=False)

    instrument(source)
    peer = source / "apps-ipod/sim/fu79-peer.c"
    code = peer.read_text()
    marker = "    cider_remote_source();"
    assert code.count(marker) == 1
    peer.write_text(code.replace(marker, '    if (!strcmp(scene, "home")) return screen;\n'
                                         + marker))

    build = output / "remote-sim"
    build.mkdir()
    commands = (
        [str(source / "tools/configure"), "--appsdir=apps-ipod", "--target=ipod6g",
         "--type=s", "--ram=64", "--rbdir=/.rockbox"],
        ["make", f"-j{jobs}", "bin", "codecs"],
    )
    log_path = output / "remote-build.log"
    with log_path.open("wb") as log:
        for command in commands:
            result = subprocess.run(command, cwd=build, stdout=log, stderr=subprocess.STDOUT)
            if result.returncode:
                raise RuntimeError(f"Offline simulator build failed: {log_path}")
    return build / "rockboxui"


def serve(connection, backend, trace):
    def send(*fields):
        body = "\t".join(map(str, fields)).encode()
        connection.sendall(struct.pack("<BH", 2, len(body)) + body)

    def page(request, offset, route):
        route = route.split("?", 1)[0]
        key = {"album/a": "songs", "playlist/a": "songs", "artist/a": "albums",
               "now/queue": "songs", "now/artist": "artists", "now/album": "albums",
               "library-songs": "songs", "root/Album": "albums"}.get(route, route)
        rows = ROWS.get(key, [])
        if key == "now/lyrics":
            rows = [("lrc1", "[00:00.00]Simulator lyric"), ("lrc1", "[00:15.00]Next line")]
        fetched = rows[offset:offset + 50]
        body = "\n".join("\t".join(map(str, row + ("",) * (10 - len(row))))
                         for row in fetched)
        send("CL", GENERATION, request, LIBRARY, offset, len(rows), 0,
             f"{len(fetched)}\n{body}\n")

    with connection, connection.makefile("rb") as stream:
        while header := stream.read(2):
            record = stream.read(int.from_bytes(header, "little")).decode()
            trace.write(record + "\n")
            fields = record.split("\t")
            if fields[0] == "ci":
                send("CA", GENERATION, 1, "")
                send("CF", GENERATION, "remote", backend)
                send("CG", GENERATION, LIBRARY)
                send("CN", GENERATION, 1, 1000, 180000, 10000, 0, 50, 0, 0,
                     "track", "", "000000", "Fixture Track", "Fixture Artist", "Fixture Album")
            elif fields[0] == "cq":
                page(fields[2], int(fields[3]), fields[5])
            elif fields[0] == "cc":
                send("CK", fields[1], fields[2], 1, "OK")
            elif fields[0] == "cr":
                send("CE", fields[1], fields[2], "no-art")


@contextlib.contextmanager
def offline_peer(backend, log):
    with tempfile.TemporaryDirectory(prefix="podbox-walk-") as tmp, \
         socket.socket(socket.AF_UNIX) as server, log.open("a", buffering=1) as trace:
        path = str(Path(tmp) / "peer")
        server.bind(path)
        server.listen(1)
        server.settimeout(2)
        ready = threading.Event()

        def worker():
            try:
                connection, _ = server.accept()
                ready.set()
                serve(connection, backend, trace)
            except (TimeoutError, BrokenPipeError, ConnectionResetError):
                pass

        thread = threading.Thread(target=worker, daemon=True)
        thread.start()
        yield {"FU79_PEER": path, "FU79_SCENE": "home"}, ready
        thread.join(timeout=2)
