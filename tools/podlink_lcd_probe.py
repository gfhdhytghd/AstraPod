#!/usr/bin/env python3
"""Run the Classic TYPE 1 LCD scroll probe through the PodLink Shell."""

import argparse
import errno
import secrets
import struct
import time


def run(serial, geometry):
    from podlink_shell import (
        LCD_INDEX, LCD_REPLY, REQUEST, ep0_transfer, find_player,
    )

    player = find_player(serial)
    cookie = secrets.randbelow(0xFFFFFFFF) + 1
    payload = struct.pack("<I4H", cookie, *geometry)
    if ep0_transfer(player, 0x40, REQUEST, 16, LCD_INDEX, payload) != len(payload):
        raise RuntimeError("Short PodLink Shell LCD command write")

    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        reply = ep0_transfer(player, 0xC0, REQUEST, 0, LCD_INDEX, LCD_REPLY.size)
        version, completed, error, _ = LCD_REPLY.unpack(bytes(reply))
        if version != 1:
            raise RuntimeError(f"Unexpected LCD reply version {version}")
        if completed == cookie:
            if error:
                raise RuntimeError(f"Firmware returned {errno.errorcode.get(error, error)}")
            print("LCD redrawn. Use the panel image to judge scroll support.")
            return
        time.sleep(0.05)
    raise RuntimeError("Timed out waiting for the LCD probe to finish")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--lines", type=int, choices=(240, 320), default=240)
    parser.add_argument("command", choices=("lcd-vscroll",))
    parser.add_argument("geometry", nargs="+", metavar="TOP HEIGHT START | off")
    args = parser.parse_args()
    if args.geometry == ["off"]:
        geometry = (0, 0, 0, 0)
    else:
        try:
            top, height, start = map(int, args.geometry)
            if not (0 <= top <= start < top + height <= args.lines):
                raise ValueError
        except ValueError:
            parser.error("use 0 <= TOP <= START < TOP + HEIGHT <= --lines")
        geometry = (args.lines, top, height, start)
    try:
        run(args.serial, geometry)
    except (RuntimeError, ImportError, OSError, struct.error) as error:
        parser.exit(1, f"lcd-vscroll: {error}\n")


if __name__ == "__main__":
    main()
