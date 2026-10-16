#!/usr/bin/env python3
"""A fake benchprobe on a pseudo-terminal, for testing the CLI without hardware.

    python3 fake_probe.py            # prints the pty path, then serves forever
    benchprobe --port /dev/pts/N info

With --reset-on-first, it swallows the first command and sends a boot event,
the way a classic Arduino resets when the port opens.
"""

import json
import os
import pty
import sys
import tty


def reply(cmd: list[str]) -> dict:
    c = cmd[0] if cmd else ""
    if c == "info":
        return {"ok": True, "fw": "benchprobe", "ver": "0.1.0", "board": "FAKE",
                "digital_pins": 20, "analog_pins": 6, "max_samples": 400}
    if cmd[:2] == ["i2c", "scan"]:
        return {"ok": True, "found": [60, 104]}
    if cmd[:2] == ["i2c", "read"]:
        n = int(cmd[3], 0)
        return {"ok": True, "addr": int(cmd[2], 0), "n": n, "data": list(range(n))}
    if c == "cap":
        pins = [int(p) for p in cmd[1].split(",")]
        n, us = int(cmd[2]), int(cmd[3])
        data = "".join(f"{(s // 4) % (1 << len(pins)):02x}" for s in range(n))
        return {"ok": True, "pins": pins, "n": n, "interval_us": us, "elapsed_us": n * us, "data": data}
    if c == "read":
        return {"ok": True, "pin": int(cmd[1]), "v": 1}
    return {"ok": False, "err": "unknown command, try help"}


def main() -> None:
    reset_first = "--reset-on-first" in sys.argv
    master, slave = pty.openpty()
    tty.setraw(slave)
    print(os.ttyname(slave), flush=True)
    buf = b""
    while True:
        data = os.read(master, 1024)
        if not data:
            break
        buf += data
        while b"\n" in buf:
            raw, buf = buf.split(b"\n", 1)
            words = raw.decode().split()
            if reset_first:
                reset_first = False
                os.write(master, b'{"ok":true,"event":"boot","fw":"benchprobe","ver":"0.1.0"}\n')
                continue
            tag = None
            if words and words[0].startswith("#"):
                tag = words.pop(0)[1:]
            msg = reply(words)
            if tag:
                msg = {"ok": msg.pop("ok"), "id": tag, **msg}
            os.write(master, (json.dumps(msg, separators=(",", ":")) + "\n").encode())


if __name__ == "__main__":
    main()
