"""oraclepet: drive an OraclePet from a terminal (`oraclepet ...` or `python -m oraclepet ...`).

Every command takes --pet SUFFIX (D350 for Claude-D350, or an address); without it the first
pet found is used. A pet takes one host at a time: disconnect it in the OraclePet app first.
"""
from __future__ import annotations

import argparse
import asyncio
import json
import logging
import os
import sys
import time
from pathlib import Path
from typing import Any

from . import __version__
from . import protocol as P
from .pet import Pet, PetError, Progress, scan

_PHASES = {"starting": "starting", "sending": "sending", "finishing": "pet is checking",
           "rebooting": "pet is restarting into the new firmware"}


class _ProgressLine:
    """One updating line on a terminal; a line per 10 % when piped, so logs stay readable."""

    def __init__(self, stream: Any = sys.stderr):
        self.stream = stream
        self.tty = stream.isatty()
        self.t0 = time.monotonic()
        self.last: tuple | None = None
        self.open = False

    def __call__(self, p: Progress) -> None:
        pct = int(p.overall * 100)
        rate = p.acked / max(0.001, time.monotonic() - self.t0) / 1024
        files = f"  file {p.file_index}/{p.file_count}" if p.file_count > 1 else ""
        text = f"{_PHASES.get(p.phase, p.phase)} {pct:3d}%  {p.acked / 1024:.0f}/{p.total / 1024:.0f} KB  {rate:.1f} KB/s{files}"
        if self.tty:
            self.stream.write("\r" + text.ljust(76))
            self.stream.flush()
            self.open = True
        elif (p.phase, pct // 10) != self.last:
            self.last = (p.phase, pct // 10)
            print(text, file=self.stream, flush=True)

    def end(self) -> None:
        if self.open:
            self.stream.write("\n")
            self.stream.flush()
            self.open = False


def _pet(args: argparse.Namespace) -> Pet:
    return Pet(getattr(args, "pet", None) or os.environ.get("ORACLEPET_PET") or None,
               timeout=getattr(args, "timeout", None) or 10.0)


async def _job(pet: Pet, job: Any) -> int:
    line = _ProgressLine()
    pet.on_progress = line
    try:
        outcome = await job
    finally:
        line.end()
    print(outcome.message)
    return 0


# ---- commands ----
async def cmd_scan(args: argparse.Namespace) -> int:
    pets = await scan(getattr(args, "timeout", None) or 5.0, getattr(args, "pet", None))
    if not pets:
        print("no Claude-* pet found (a pet that is connected to another host does not advertise)")
        return 1
    for f in pets:
        print(f"{f.name:14} {f.address}  rssi {f.rssi}")
    return 0


async def cmd_info(args: argparse.Namespace) -> int:
    async with _pet(args) as pet:
        fw = pet.firmware or await pet.info()
        if args.json:
            print(json.dumps(fw.raw, ensure_ascii=False, indent=2))
            return 0
        print(f"{pet.name}  {pet.address}")
        rows = [("board", f"{fw.board} ({fw.project})" if fw.project else "?"),
                ("firmware", fw.version), ("built", fw.built), ("slot", f"{fw.slot} ({fw.state or '?'})"),
                ("sha", fw.sha), ("pack", f"{fw.pack or '-'}  of {', '.join(fw.packs) or 'none'}"),
                ("app", f"{fw.app or '-'}  of {', '.join(fw.apps) or 'none'}")]
        if fw.app_error:
            rows.append(("app error", fw.app_error))
        rows += [("free", f"{fw.free_kb} KB"), ("write size", f"{pet.link.max_write(P.OTA_DATA)} bytes"),
                 ("mic", ("yes" if P.has_mic(fw.project) else "no (this board has none)")
                         if pet.link.has(P.OTA_MIC) else "no (firmware older than LISTEN)")]
        for k, v in rows:
            print(f"  {k:10} {v}")
    return 0


async def cmd_ota(args: argparse.Namespace) -> int:
    data = Path(args.file).read_bytes()
    img = P.describe(data)
    if img is not None:
        print(f"image: {img.project} {img.version}, built {img.built}, sha {img.sha}")
    async with _pet(args) as pet:
        fw = pet.firmware
        # the Firmware page's rule: an old pet without a sha still gets the update
        if img is not None and fw is not None and fw.same_build(img) and not (not fw.sha and img.sha) and not args.force:
            print(f"{pet.name} already runs this build ({fw.version}, sha {fw.sha}); --force sends it anyway")
            return 0
        return await _job(pet, pet.ota(data))


async def cmd_pack(args: argparse.Namespace) -> int:
    async with _pet(args) as pet:
        return await _job(pet, pet.pack(args.dir, name=args.name))


async def cmd_app(args: argparse.Namespace) -> int:
    async with _pet(args) as pet:
        return await _job(pet, pet.app(args.file, run=not args.no_run, name=args.name))


async def cmd_run(args: argparse.Namespace) -> int:
    async with _pet(args) as pet:
        return await _job(pet, pet.run(args.name))


async def cmd_stop(args: argparse.Namespace) -> int:
    async with _pet(args) as pet:
        return await _job(pet, pet.stop_app())


async def cmd_select(args: argparse.Namespace) -> int:
    async with _pet(args) as pet:
        return await _job(pet, pet.select_pack(args.name))


async def cmd_play(args: argparse.Namespace) -> int:
    async with _pet(args) as pet:
        return await _job(pet, pet.play(args.name))


async def cmd_say(args: argparse.Namespace) -> int:
    async with _pet(args) as pet:
        return await _job(pet, pet.say(" ".join(args.text), language=args.lang, voice=args.voice))


async def cmd_selftest(args: argparse.Namespace) -> int:
    async with _pet(args) as pet:
        print((await pet.selftest())["summary"])
    return 0


def _save(path: str, samples: list) -> None:
    Path(path).write_bytes(P.wav(samples))
    print(f"saved {path} ({len(samples)} samples, {len(samples) / P.SAMPLE_RATE:.2f} s)")


async def cmd_listen(args: argparse.Namespace) -> int:
    async with _pet(args) as pet:
        try:
            levels, samples = await pet.listen(args.ms, send_audio=not args.no_audio)
        except PetError as e:
            if e.samples and args.wav:
                _save(args.wav, e.samples)
            raise
    print(f"heard {levels['ms']} ms: rms {levels['rms']} (floor {levels['floor']}), peak {levels['peak']}, "
          f"strongest {levels['freq']} Hz {levels['db']:+.1f} dB")
    if samples is None:
        if not args.no_audio:
            print("no audio came back (firmware without the mic characteristic?)")
    elif args.wav:
        _save(args.wav, samples)
    else:
        print(f"audio: {len(samples)} samples ({len(samples) / P.SAMPLE_RATE:.2f} s); --wav FILE saves it")
    return 0


async def cmd_heartbeat(args: argparse.Namespace) -> int:
    total, running, waiting = P.STATE_COUNTS[args.state]
    beat = P.Heartbeat(args.total if args.total is not None else total,
                       args.running if args.running is not None else running,
                       args.waiting if args.waiting is not None else waiting,
                       msg=args.msg, who=args.who, pct5h=args.pct5h, reset5h=args.reset5h,
                       pct7d=args.pct7d, reset7d=args.reset7d)
    if args.dry_run:
        sys.stdout.write(beat.json_line().decode("utf-8"))
        return 0
    async with _pet(args) as pet:
        line = await pet.heartbeat(beat)
        name = pet.name
    print(f"sent to {name}: {line.decode('utf-8').rstrip()}")
    print(f"the pet shows {beat.state}; with no newer beat it counts as idle after 30 s")
    return 0


# ---- parser ----
def build_parser() -> argparse.ArgumentParser:
    # the shared options work before or after the command (SUPPRESS keeps one from hiding the other)
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--pet", metavar="SUFFIX", default=argparse.SUPPRESS,
                        help="the pet's name suffix (D350 for Claude-D350) or its address; "
                             "default: $ORACLEPET_PET, else the first pet found")
    common.add_argument("--timeout", type=float, metavar="S", default=argparse.SUPPRESS,
                        help="seconds to look for the pet (default 10; scan 5)")
    common.add_argument("-v", "--verbose", action="count", default=argparse.SUPPRESS,
                        help="log the protocol (-vv: bleak too)")

    p = argparse.ArgumentParser(prog="oraclepet", parents=[common],
                                description="Drive an OraclePet over Bluetooth LE (protocol v1).")
    p.add_argument("--version", action="version", version=f"oraclepet {__version__}")
    sub = p.add_subparsers(dest="command", required=True, metavar="COMMAND")

    def cmd(name: str, fn: Any, help: str) -> argparse.ArgumentParser:
        sp = sub.add_parser(name, parents=[common], help=help, description=help)
        sp.set_defaults(func=fn)
        return sp

    cmd("scan", cmd_scan, "list the pets that are advertising")
    sp = cmd("info", cmd_info, "what the pet runs: board, firmware, packs, apps")
    sp.add_argument("--json", action="store_true", help="print the pet's JSON as it is")
    sp = cmd("ota", cmd_ota, "full firmware update with an app image (.bin); the pet reboots into it")
    sp.add_argument("file")
    sp.add_argument("--force", action="store_true", help="send even when the pet already runs this build")
    sp = cmd("pack", cmd_pack, "send an animation pack folder (manifest.json + GIFs) and show it")
    sp.add_argument("dir")
    sp.add_argument("--name", help="pack name on the pet (default: the folder name)")
    sp = cmd("app", cmd_app, "send a pet app (.wasm) and run it")
    sp.add_argument("file")
    sp.add_argument("--name", help="app name on the pet (default: the file name)")
    sp.add_argument("--no-run", action="store_true", help="only copy it")
    sp = cmd("run", cmd_run, "run an app that is on the pet")
    sp.add_argument("name")
    cmd("stop", cmd_stop, "stop the running app")
    sp = cmd("select", cmd_select, "show a pack that is on the pet")
    sp.add_argument("name")
    sp = cmd("play", cmd_play, "play sounds/<name>.ima from the pet's storage")
    sp.add_argument("name")
    sp = cmd("say", cmd_say, "speak through the pet (this OS's speech, streamed)")
    sp.add_argument("text", nargs="+")
    sp.add_argument("--lang", choices=["th", "en"], help="voice language (default: Thai when the text is Thai)")
    sp.add_argument("--voice", help="a voice name of this OS's synthesizer")
    cmd("selftest", cmd_selftest, "the pet beeps; with a mic it checks it heard itself")
    sp = cmd("listen", cmd_listen, "record the pet's mic and report the levels")
    sp.add_argument("--ms", type=int, default=3000, help="milliseconds to record, 100..10000 (default 3000)")
    sp.add_argument("--wav", metavar="FILE", help="save the recording as a 16 kHz WAV")
    sp.add_argument("--no-audio", action="store_true", help="levels only; do not send the audio back")
    sp = cmd("heartbeat", cmd_heartbeat, "send one heartbeat (a sample beat; the pet shows its state)")
    sp.add_argument("--state", choices=list(P.STATE_COUNTS), default="busy",
                    help="counts for this state (default busy)")
    for k in ("total", "running", "waiting"):
        sp.add_argument(f"--{k}", type=int, help=f"override {k}")
    sp.add_argument("--msg", default="hello from oraclepet", help="status line (63 bytes at most)")
    sp.add_argument("--who", help="name under the pet (20 characters at most)")
    for k in ("pct5h", "reset5h", "pct7d", "reset7d"):
        sp.add_argument(f"--{k}", type=int, help="usage window field")
    sp.add_argument("--dry-run", action="store_true", help="print the line, no Bluetooth")
    return p


def main(argv: list | None = None) -> int:
    args = build_parser().parse_args(argv)
    verbose = getattr(args, "verbose", 0) or 0
    logging.basicConfig(level=logging.DEBUG if verbose > 1 else logging.WARNING,
                        format="%(levelname)s %(name)s: %(message)s")
    if verbose:
        logging.getLogger("oraclepet").setLevel(logging.DEBUG)
    try:
        return asyncio.run(args.func(args))
    except KeyboardInterrupt:
        print("\nstopped.", file=sys.stderr)
        return 130
    except PetError as e:
        print(f"oraclepet: {e}", file=sys.stderr)
        return 1
    except Exception as e:
        if verbose:
            raise
        print(f"oraclepet: {type(e).__name__}: {e}", file=sys.stderr)
        if sys.platform == "darwin":
            print("macOS: the terminal app needs Bluetooth permission "
                  "(System Settings > Privacy & Security > Bluetooth).", file=sys.stderr)
        return 1
