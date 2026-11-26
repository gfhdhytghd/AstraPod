#!/usr/bin/env python3
"""The list's wheel scrolling and letter jump against the stock firmware.

python3 tools/tests/list_quickscroll_test.py [OUT_DIR]

Builds apps-ipod/widgets/list_quickscroll.c on the host. Checks stock's
letter key, the stock rules on a made-up list, then replays every wheel log
recorded from the stock 1.1.2 firmware in tools/tests/quickscroll/*.json and
compares the selection, top row and overlay at each recorded checkpoint.
"""
import ctypes
import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WIDGETS = ROOT / "apps-ipod/widgets"
HERE = Path(__file__).resolve().parent / "quickscroll"


def build(out):
    cc = ["gcc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
          "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
          f"-I{WIDGETS}"]
    replay = out / "replay"
    subprocess.run(cc + [str(HERE / "replay.c"), str(WIDGETS / "list_quickscroll.c"),
                         "-o", str(replay)], check=True)
    lib = out / "quickscroll.so"
    subprocess.run(["gcc", "-std=gnu11", "-shared", "-fPIC", "-O1",
                    f"-I{WIDGETS}", str(WIDGETS / "list_quickscroll.c"), "-o", str(lib)],
                   check=True)
    return replay, ctypes.CDLL(str(lib))


def key_of(lib, text):
    out = ctypes.create_string_buffer(16)
    lib.quickscroll_key_of(text.encode(), out)
    return out.value.decode()


def check_keys(lib):
    cases = {
        "Apple": "A", "  apple": "A", "The Beatles": "T", "éclair": "E",
        "Ölmez": "O", "Æon": "A", "Ñu": "#", "ñu": "N", "1999": "#",
        "(Intro)": "#", "!Viva": "#", "Ωmega": "#", "Жизнь": "#",
        "漢字": "!", "マリーゴールド": "!", "한국": "!", "": "!", "   ": "!",
        "ßtraße": "S", "Łódź": "L", "ÿes": "Y", "Żubr": "Z",
    }
    for text, want in cases.items():
        got = key_of(lib, text)
        assert got == want, f"key of {text!r}: {got!r}, stock {want!r}"
    print(f"stock letter key: {len(cases)} names PASS")


def run(replay, names, events, visible, out, row_height=25):
    """Events are (t_us, "touch"), (t_us, "release"), (t_us, "moved",
    reports) or (t_us, "step", positions). Without "moved" events a step is
    one report a position, as the player's wheel sends them."""
    keys = out / "names.txt"
    keys.write_text("".join(n + "\n" for n in names), encoding="utf-8")
    timed = any(e[1] == "moved" for e in events)
    lines = []
    for e in events:
        if e[1] == "step" and not timed:
            lines.append((e[0], "moved", abs(e[2])))
        lines.append(e)
    text = "".join(" ".join(map(str, e)) + "\n" for e in lines)
    result = subprocess.run([str(replay), str(keys), str(visible), str(row_height)],
                            input=text, capture_output=True, text=True, check=True)
    rows = []
    for line in result.stdout.splitlines():
        t, sel, top, mode, shown = line.split()
        rows.append((int(t), int(sel), int(top), int(mode), shown))
    return rows


def check_rules(replay, out):
    # 26 letters with uneven groups: A has 40 rows, B 39, ...
    names = [f"{chr(65 + g)}{i:03d}" for g in range(26) for i in range(40 - g)]
    ms = 1000

    # Slow: one detent each 250 ms moves one row and never jumps.
    events = [(0, "touch")] + [(40 * ms + 250 * ms * i, "step", 4) for i in range(20)]
    rows = run(replay, names, events, 8, out)
    assert [r[1] for r in rows[1:]] == list(range(1, 21)), rows
    assert all(r[3] == 0 for r in rows)

    # Fast: a detent every 10 ms. The multiplier doubles every 350 ms to 16,
    # 16 rows an event, and the next event starts the jump.
    events = [(0, "touch")] + [(40 * ms + 10 * ms * i, "step", 4) for i in range(300)]
    rows = run(replay, names, events, 8, out)
    entered = next(i for i, r in enumerate(rows) if r[3])
    t_enter = rows[entered][0] - 40 * ms
    assert 1050 * ms <= t_enter <= 1450 * ms, t_enter
    letters = [r[4] for r in rows[entered:]]
    seen = [k for i, k in enumerate(letters) if i == 0 or k != letters[i - 1]]
    assert "".join(seen) == "".join(chr(c) for c in range(ord(seen[0]), 91)), seen
    for r in rows[entered + 1:]:
        assert names[r[1]][0] == r[4] and r[2] == min(r[1], len(names) - 8), r
    # Every detent in the jump is the next group's first row.
    starts = {i for i, n in enumerate(names) if n.endswith("000")}
    assert all(r[1] in starts or r[1] == len(names) - 1 for r in rows[entered + 1:])

    # Release leaves the jump, and slow detents then move a row each.
    t = rows[-1][0]
    events += [(t + 5 * ms, "release"), (t + 600 * ms, "touch"),
               (t + 640 * ms, "step", -4), (t + 900 * ms, "step", -4)]
    rows = run(replay, names, events, 8, out)
    assert rows[-4][3] == 0 and rows[-4][4] == "-"
    assert rows[-2][1] == rows[-4][1] - 1 and rows[-1][1] == rows[-4][1] - 2, rows[-4:]
    print("stock rules on a made-up list: PASS")


def check_stock(replay, out):
    """Each fixture holds stock's selection and mode as each wheel event
    left them, read from its memory, and the letter on every frame that
    showed the overlay. A frame follows its event by a redraw, so its
    letter must be one ours showed in the window_us before it."""
    fixtures = sorted(HERE.glob("*.json"))
    for path in fixtures:
        data = json.loads(path.read_text(encoding="utf-8"))
        rows = run(replay, data["names"], data["events"], data["visible"], out,
                   data["row_height"])
        after = {r[0]: r for r in rows}
        failures = []
        for want in data["states"]:
            got = after[want["t_us"]]
            if (got[1], got[3]) != (want["selected"], want["mode"]):
                failures.append(f"{want['t_us'] / 1000:.0f} ms: ours row {got[1]} "
                                f"mode {got[3]}, stock row {want['selected']} "
                                f"mode {want['mode']}")
        for want in data["overlays"]:
            t = want["t_us"]
            shown = [r[4] for r in rows if t - data["window_us"] < r[0] <= t]
            shown += [r[4] for r in rows if r[0] <= t - data["window_us"]][-1:]
            if want["overlay"] not in shown:
                failures.append(f"{t / 1000:.0f} ms: ours {'/'.join(shown)}, "
                                f"stock {want['overlay']}")
        checks = len(data["states"]) + len(data["overlays"])
        assert not failures, f"{path.name}: {len(failures)} of {checks} differ\n  " \
            + "\n  ".join(failures[:12])
        print(f"{path.stem}: {len(data['states'])} events and "
              f"{len(data['overlays'])} overlay frames as stock: PASS")
    if not fixtures:
        print("no stock replays found")


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(tempfile.mkdtemp())
    out.mkdir(parents=True, exist_ok=True)
    replay, lib = build(out)
    check_keys(lib)
    check_rules(replay, out)
    check_stock(replay, out)


if __name__ == "__main__":
    main()
