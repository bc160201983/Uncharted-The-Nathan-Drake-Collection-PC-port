"""Capture an isolated test instance through its existing renderer diagnostic API.

Optional --press uses the instance's user/probe-pad.txt configured by the probe
launcher, never OS keyboard injection.
"""
import argparse
from pathlib import Path
import re
import time
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("instance")
    parser.add_argument("output", type=Path)
    buttons = ("cross", "circle", "up", "down", "left", "right", "options",
               "square", "triangle", "l1", "r1")
    action = parser.add_mutually_exclusive_group()
    action.add_argument("--press", choices=buttons)
    action.add_argument("--sequence", nargs="+", choices=buttons,
                        help="Ordered presses through the runtime's diagnostic pad API")
    parser.add_argument("--sticks", nargs=4, type=int, metavar=("LX", "LY", "RX", "RY"),
                        help="Optional stick values 0..255; neutral is 128")
    parser.add_argument("--hold", type=float, default=0.15)
    parser.add_argument("--settle", type=float, default=0.6)
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,32}", args.instance):
        parser.error("Invalid isolated instance name")
    if not 0 <= args.settle <= 10:
        parser.error("--settle must be in 0..10 seconds")
    if not 0.05 <= args.hold <= 5:
        parser.error("--hold must be in 0.05..5 seconds")
    if args.sticks and any(not 0 <= value <= 255 for value in args.sticks):
        parser.error("--sticks values must be in 0..255")
    if args.sequence and len(args.sequence) > 20:
        parser.error("At most 20 sequential presses per call")
    work = ROOT / "work/local-lobby/instances" / args.instance
    dumps = work / "analysis102/big2-ps4_Shipping/dumps"
    if not dumps.is_dir():
        parser.error("Instance renderer dump directory does not exist")
    if args.press or args.sequence or args.sticks:
        pad = work / "user/probe-pad.txt"
        if not pad.is_file():
            parser.error("Create/configure this instance's GOW3_PAD_FILE first")
        axes = "" if not args.sticks else " ".join(
            f"{key}={value}" for key, value in zip(("lx", "ly", "rx", "ry"), args.sticks))
        for button in args.sequence or [args.press or ""]:
            try:
                pad.write_text(f"{button} {axes}".strip(), encoding="ascii")
                time.sleep(args.hold)
            finally:
                pad.write_text("", encoding="ascii")
            time.sleep(args.settle)
    before = {p: p.stat().st_mtime_ns for p in dumps.glob("output_*.raw")}
    (dumps / "trigger").touch()
    deadline = time.monotonic() + 6
    while time.monotonic() < deadline:
        for raw in sorted(dumps.glob("output_*.raw"), reverse=True):
            match = re.fullmatch(r"output_\d+_(\d+)x(\d+)\.raw", raw.name)
            if not match or raw.stat().st_mtime_ns == before.get(raw):
                continue
            width, height = map(int, match.groups())
            data = raw.read_bytes()
            if len(data) != width * height * 4:
                continue
            args.output.parent.mkdir(parents=True, exist_ok=True)
            Image.frombytes("RGBA", (width, height), data).save(args.output)
            print(args.output.resolve())
            return
        time.sleep(0.05)
    raise SystemExit("No fresh complete frame within six seconds; inspect boot.log")


if __name__ == "__main__":
    main()
