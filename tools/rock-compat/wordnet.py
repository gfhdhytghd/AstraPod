#!/usr/bin/env python3
"""Build Rockbox dictionary data from Princeton WordNet 3.0."""
from collections import defaultdict
from hashlib import sha256
from io import BytesIO
from pathlib import Path
import struct
import sys
import tempfile
import tarfile
from urllib.request import urlopen


URL = "https://wordnetcode.princeton.edu/3.0/WordNet-3.0.tar.bz2"
SHA256 = "6c492d0c7b4a40e7674d088191d3aa11f373bb1da60762e098b8ee2dda96ef22"
PREFIX = "WordNet-3.0/"
PARTS = {"noun": b"noun", "verb": b"verb", "adj": b"adjective", "adv": b"adverb"}


def source_archive(cache):
    if cache.is_file():
        data = cache.read_bytes()
    else:
        with urlopen(URL, timeout=60) as response:
            data = response.read()
    if sha256(data).hexdigest() != SHA256:
        raise ValueError(f"WordNet archive checksum mismatch: {cache}")
    if not cache.is_file():
        cache.parent.mkdir(parents=True, exist_ok=True)
        # Publish only complete archives. Concurrent packagers use separate files.
        with tempfile.NamedTemporaryFile(dir=cache.parent, delete=False) as temp:
            temp.write(data)
        Path(temp.name).replace(cache)
    return data


def build(archive, output):
    entries = defaultdict(list)
    files = {}
    names = {PREFIX + f"dict/{kind}.{part}"
             for part in PARTS for kind in ("data", "index")}
    names.add(PREFIX + "LICENSE")
    with tarfile.open(fileobj=BytesIO(archive), mode="r|bz2") as source:
        for member in source:
            if member.name in names:
                files[member.name] = source.extractfile(member).read()
    for part, label in PARTS.items():
        data = files[PREFIX + f"dict/data.{part}"]
        for line in files[PREFIX + f"dict/index.{part}"].splitlines():
            if line.startswith(b" "):
                continue
            lemma, _, count, pointers, *fields = line.split()
            word = lemma.replace(b"_", b" ").lower()
            if len(word) >= 32:
                continue
            offsets = fields[int(pointers) + 2:]
            if len(offsets) != int(count):
                raise ValueError(f"Invalid index record: {lemma!r}")
            for offset in map(int, offsets):
                end = data.index(b"\n", offset)
                gloss = data[data.index(b"|", offset, end) + 1:end].strip()
                entries[word].append(b"(" + label + b") " + gloss)

    output.mkdir(parents=True, exist_ok=True)
    with (output / "dict.index").open("wb") as index, \
            (output / "dict.desc").open("wb") as definitions:
        for word, senses in sorted(entries.items()):
            index.write(struct.pack(">32sI", word, definitions.tell()))
            definitions.write(b"  ".join(senses) + b"\n")
    (output / "dict.LICENSE").write_bytes(files[PREFIX + "LICENSE"])


if __name__ == "__main__":
    cache, output = map(Path, sys.argv[1:])
    build(source_archive(cache), output)
