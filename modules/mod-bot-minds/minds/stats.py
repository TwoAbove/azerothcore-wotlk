#!/usr/bin/env python3
"""stats.py — the sizing number this probe exists to measure.

Reads a percepts JSONL file and reports:
  - events/hour by kind
  - rendered-prose tokens/hour (chars/4 estimate)  <- feeds episode-length math
"""

import argparse
import io
import json
from collections import Counter

from render import Renderer


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("file", help="<Bot>.percepts.jsonl")
    args = ap.parse_args()

    kinds = Counter()
    first_t = last_t = None
    raw_lines = []
    with open(args.file, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            raw_lines.append(line)
            try:
                e = json.loads(line)
            except json.JSONDecodeError:
                kinds["<unparseable>"] += 1
                continue
            kinds[e.get("kind", "?")] += 1
            t = e.get("t")
            if t:
                first_t = t if first_t is None else min(first_t, t)
                last_t = t if last_t is None else max(last_t, t)

    total = sum(kinds.values())
    if not total:
        print("no percepts")
        return

    span_h = ((last_t - first_t) / 3_600_000) if (first_t and last_t and last_t > first_t) else 0.0

    # render everything through the real renderer to measure prose volume
    buf = io.StringIO()
    r = Renderer(out=buf)
    for line in raw_lines:
        r.feed(line)
    r.close()
    prose = buf.getvalue()
    prose_tokens = len(prose) / 4  # ~4 chars/token heuristic

    print(f"percepts: {total} over {span_h:.2f} h  ({total / span_h:.0f}/h)" if span_h else f"percepts: {total} (span too short to rate)")
    print("\nby kind:")
    for k, n in kinds.most_common():
        rate = f"  {n / span_h:8.1f}/h" if span_h else ""
        print(f"  {k:12} {n:6}{rate}")
    print(f"\nrendered prose: {len(prose)} chars, ~{prose_tokens:.0f} tokens")
    if span_h:
        tokens_per_hour = prose_tokens / span_h
        print(f"** percept-tokens/hour: ~{tokens_per_hour:.0f} **")
        if tokens_per_hour:
            print(f"   (at 12k usable context: one episode ≈ {12_000 / tokens_per_hour:.1f} h of raw stream)")
        else:
            print("   (episode length unavailable: renderer produced no prose)")


if __name__ == "__main__":
    main()
