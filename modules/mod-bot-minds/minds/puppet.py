#!/usr/bin/env python3
"""puppet.py — hand-type ops for a watched bot (the scripted 'fake mind').

Appends one JSON line to <dir>/<Bot>.ops.jsonl; the module tails and executes.

Examples:
    puppet.py say Thargrim "Anyone have spare copper?" --channel yell
    puppet.py say Thargrim "psst" --channel whisper --to Grimzo
"""

import argparse
import json
import os
import sys

DEFAULT_DIR = "/home/twoabove/wow/percepts"


def emit(args, obj):
    path = os.path.join(args.dir, f"{args.bot}.ops.jsonl")
    with open(path, "a", encoding="utf-8") as f:
        f.write(json.dumps(obj, ensure_ascii=False) + "\n")
    print(f"-> {path}: {json.dumps(obj, ensure_ascii=False)}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", default=DEFAULT_DIR, help=f"percept dir (default {DEFAULT_DIR})")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("say")
    p.add_argument("bot")
    p.add_argument("text")
    p.add_argument("--channel", default="say", choices=["say", "yell", "party", "guild", "whisper"])
    p.add_argument("--to", default="", help="whisper target")

    args = ap.parse_args()

    if args.channel == "whisper" and not args.to:
        sys.exit("whisper needs --to")
    emit(args, {"op": "say", "channel": args.channel, "to": args.to, "text": args.text})


if __name__ == "__main__":
    main()
