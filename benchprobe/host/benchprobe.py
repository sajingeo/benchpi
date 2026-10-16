#!/usr/bin/env python3
"""benchprobe: talk to the benchprobe Arduino firmware from the bench host.

The command line *is* the protocol: every argument after the options is sent
as one command line, and the device's JSON reply is printed.

    benchprobe info
    benchprobe i2c scan
    benchprobe i2c read 0x68 6 0x3b
    benchprobe cap 2,3,4 256 50 --ascii
    benchprobe cap 2,3 400 10 --vcd capture.vcd
    benchprobe run "pin 7 pullup" "read 7" "watch 7 1000"
    benchprobe shell                      # interactive
    benchprobe ports                      # find the probe

Exit codes: 0 ok, 1 device returned an error, 2 connection problem.
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import sys
import time

try:
    import serial  # pyserial
except ImportError:  # pragma: no cover
    sys.stderr.write("benchprobe needs pyserial: pip install pyserial\n")
    sys.exit(2)

BAUD = 115200
DEFAULT_PORT = "/dev/benchprobe"


class ProbeError(Exception):
    pass


# ------------------------------------------------------------------ transport

def candidate_ports() -> list[str]:
    ports = sorted(glob.glob("/dev/serial/by-id/*"))
    if not ports:
        ports = sorted(glob.glob("/dev/ttyACM*") + glob.glob("/dev/ttyUSB*"))
    return ports


def resolve_port(explicit: str | None) -> str:
    if explicit:
        if not os.path.exists(explicit):
            raise ProbeError(f"port {explicit} not found")
        return explicit
    for p in (os.environ.get("BENCHPROBE_PORT"), DEFAULT_PORT):
        if p and os.path.exists(p):
            return p
    found = candidate_ports()
    if len(found) == 1:
        return found[0]
    if not found:
        raise ProbeError("no serial ports found; is the probe plugged in?")
    raise ProbeError("several serial ports found, pick one with --port: " + ", ".join(found))


def disable_hupcl(fd: int) -> None:
    """Keep DTR asserted on close so the next open doesn't reset an Uno."""
    try:
        import termios
        attrs = termios.tcgetattr(fd)
        attrs[2] &= ~termios.HUPCL
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
    except Exception:
        pass


class Probe:
    def __init__(self, port: str, timeout: float = 15.0):
        self.port = port
        self.timeout = timeout
        try:
            self.ser = serial.Serial(port, BAUD, timeout=0.1)
        except serial.SerialException as e:
            raise ProbeError(f"cannot open {port}: {e}") from e
        disable_hupcl(self.ser.fileno())
        self._seq = 0

    def close(self) -> None:
        self.ser.close()

    def _readline(self) -> str | None:
        raw = self.ser.readline()
        if not raw:
            return None
        return raw.decode("utf-8", "replace").strip()

    def command(self, line: str) -> dict:
        """Send one command and return its JSON reply.

        Classic Arduinos reset when the port opens. Bytes sent during the
        bootloader are lost, so if a boot event arrives before our reply we
        resend once. Commands are never resent otherwise.
        """
        self._seq += 1
        tag = f"h{self._seq}"
        payload = f"#{tag} {line.strip()}\n".encode()
        self.ser.write(payload)
        deadline = time.monotonic() + self.timeout
        resent = False
        while time.monotonic() < deadline:
            text = self._readline()
            if text is None:
                continue
            try:
                msg = json.loads(text)
            except json.JSONDecodeError:
                continue  # bootloader noise or partial line
            if msg.get("event") == "boot":
                if resent:
                    raise ProbeError("probe rebooted twice while handling the command")
                resent = True
                self.ser.reset_input_buffer()
                self.ser.write(payload)
                continue
            if msg.get("id") == tag:
                msg.pop("id", None)
                return msg
        raise ProbeError(f"no reply to '{line}' within {self.timeout:.0f}s")


# ------------------------------------------------------------------ helpers

def annotate(cmd: str, reply: dict) -> dict:
    """Add hex views that are handy for humans and agents."""
    words = cmd.split()
    if words[:2] == ["i2c", "scan"] and "found" in reply:
        reply["found_hex"] = [f"0x{a:02x}" for a in reply["found"]]
    for k in ("data", "rx"):
        if isinstance(reply.get(k), list):
            reply[k + "_hex"] = " ".join(f"{b:02x}" for b in reply[k])
    if words[:1] == ["cap"] and isinstance(reply.get("data"), str):
        reply["channels"] = decode_capture(reply)
    return reply


def decode_capture(reply: dict) -> dict:
    raw = bytes.fromhex(reply["data"])
    return {str(pin): "".join("1" if b >> i & 1 else "0" for b in raw)
            for i, pin in enumerate(reply["pins"])}


def ascii_waveform(reply: dict, width: int = 120) -> str:
    chans = decode_capture(reply)
    n = reply["n"]
    step = max(1, -(-n // width))
    out = [f"{n} samples, {reply['elapsed_us'] / max(n, 1):.1f} us/sample actual, "
           f"{step} sample(s) per column"]
    for pin, bits in chans.items():
        cols = bits[::step]
        out.append(f"D{pin:>3} " + "".join("‾" if c == "1" else "_" for c in cols))
    return "\n".join(out)


def write_vcd(reply: dict, path: str) -> None:
    chans = decode_capture(reply)
    n = reply["n"]
    period = reply["interval_us"] or max(1, round(reply["elapsed_us"] / max(n, 1)))
    ids = {pin: chr(33 + i) for i, pin in enumerate(chans)}
    with open(path, "w") as f:
        f.write("$timescale 1us $end\n$scope module benchprobe $end\n")
        for pin, sym in ids.items():
            f.write(f"$var wire 1 {sym} D{pin} $end\n")
        f.write("$upscope $end\n$enddefinitions $end\n")
        last = {}
        for s in range(n):
            changes = [(sym, chans[pin][s]) for pin, sym in ids.items() if last.get(pin) != chans[pin][s]]
            if changes:
                f.write(f"#{s * period}\n")
                for sym, v in changes:
                    f.write(f"{v}{sym}\n")
                for pin in ids:
                    last[pin] = chans[pin][s]
        f.write(f"#{n * period}\n")


def emit(reply: dict, pretty: bool) -> None:
    print(json.dumps(reply, indent=2 if pretty else None))


# ------------------------------------------------------------------ main

def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        prog="benchprobe",
        description="Drive the benchprobe Arduino. Arguments are sent as one protocol command.",
        epilog="Protocol reference: benchprobe/PROTOCOL.md")
    ap.add_argument("--port", help=f"serial port (default: $BENCHPROBE_PORT, {DEFAULT_PORT}, or the only port found)")
    ap.add_argument("--timeout", type=float, default=15.0, help="seconds to wait for a reply")
    ap.add_argument("--pretty", action="store_true", help="indent JSON output")
    ap.add_argument("--ascii", action="store_true", help="cap: also print an ASCII waveform to stderr")
    ap.add_argument("--vcd", metavar="FILE", help="cap: also write a VCD file (open in PulseView/GTKWave)")
    ap.add_argument("command", nargs=argparse.REMAINDER, help="e.g. info | i2c scan | run CMD... | shell | ports")
    args = ap.parse_args(argv)

    if not args.command:
        ap.print_help()
        return 2

    if args.command[0] == "ports":
        emit({"ok": True, "ports": candidate_ports(),
              "default": DEFAULT_PORT if os.path.exists(DEFAULT_PORT) else None}, args.pretty)
        return 0

    try:
        probe = Probe(resolve_port(args.port), args.timeout)
    except ProbeError as e:
        emit({"ok": False, "err": str(e)}, args.pretty)
        return 2

    try:
        if args.command[0] == "run":
            lines = args.command[1:] or [l for l in sys.stdin.read().splitlines() if l.strip()]
            status = 0
            for line in lines:
                reply = annotate(line, probe.command(line))
                emit({"cmd": line, **reply}, args.pretty)
                if not reply.get("ok"):
                    status = 1
            return status

        if args.command[0] == "shell":
            print(f"benchprobe on {probe.port}. Type commands, Ctrl-D to quit.", file=sys.stderr)
            for line in sys.stdin:
                if line.strip():
                    emit(annotate(line, probe.command(line)), args.pretty)
                    sys.stdout.flush()
            return 0

        line = " ".join(args.command)
        reply = annotate(line, probe.command(line))
        emit(reply, args.pretty)
        if reply.get("ok") and args.command[0] == "cap":
            if args.ascii:
                print(ascii_waveform(reply), file=sys.stderr)
            if args.vcd:
                write_vcd(reply, args.vcd)
        return 0 if reply.get("ok") else 1
    except ProbeError as e:
        emit({"ok": False, "err": str(e)}, args.pretty)
        return 2
    finally:
        probe.close()


if __name__ == "__main__":
    sys.exit(main())
