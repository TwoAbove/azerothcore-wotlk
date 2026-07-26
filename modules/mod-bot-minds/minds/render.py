#!/usr/bin/env python3
"""render.py — JSONL percepts -> first-person prose transcript.

Usage:
    render.py <Bot>.percepts.jsonl            render whole file
    render.py <Bot>.percepts.jsonl --follow   render then tail (Ctrl-C to exit)

Collapses runs of xp/money/loot lines into single sentences so the transcript
reads like experience, not a ledger.
"""

import argparse
import json
import sys
import time
from datetime import datetime

COLLAPSE_KINDS = {"xp", "money", "loot"}
COLLAPSE_GAP_MS = 30_000

QUALITY = {0: "junk", 1: "", 2: "uncommon", 3: "rare", 4: "epic", 5: "legendary"}


def fmt_money(copper: int) -> str:
    copper = int(copper)
    sign = "-" if copper < 0 else ""
    copper = abs(copper)
    g, rem = divmod(copper, 10_000)
    s, c = divmod(rem, 100)
    parts = []
    if g:
        parts.append(f"{g}g")
    if s:
        parts.append(f"{s}s")
    if c or not parts:
        parts.append(f"{c}c")
    return sign + " ".join(parts)


def ts(ms: int) -> str:
    return datetime.fromtimestamp(ms / 1000).strftime("%H:%M:%S")


def render_one(e: dict) -> str | None:
    """Render a single (non-collapsed) percept to prose. None = drop."""
    k = e.get("kind", "?")
    bot = e.get("bot", "?")

    if k == "chat":
        who = e.get("from", "someone")
        chan = e.get("channel", "say")
        text = e.get("text", "")
        if who == bot:
            verb = {"yell": "yell", "whisper": "whisper"}.get(chan, "say")
            return f'I {verb}: "{text}"'
        if chan == "whisper":
            return f'{who} whispers to me: "{text}"'
        if chan == "channel":
            return f'[{e.get("channel_name", "channel")}] {who}: "{text}"'
        if chan in ("party", "guild"):
            return f'[{chan}] {who}: "{text}"'
        verb = "yells" if chan == "yell" else "says"
        return f'{who} {verb}: "{text}"'

    if k == "quest":
        st, title = e.get("status"), e.get("title", "?")
        return {
            "accepted": f'I took on the quest "{title}".',
            "completed": f'I completed the quest "{title}".',
            "abandoned": f'I gave up on the quest "{title}".',
        }.get(st, f'Quest "{title}": {st}.')

    if k == "level":
        return f"I reached level {e.get('to', '?')}."

    if k == "xp":
        src = f" from {e['from']}" if e.get("from") else ""
        return f"I gained {e.get('amount', 0)} experience{src}."

    if k == "money":
        d = int(e.get("delta", 0))
        verb = "earned" if d > 0 else "spent"
        return f"I {verb} {fmt_money(abs(d))} (I now carry {fmt_money(e.get('total', 0))})."

    if k == "loot":
        n = int(e.get("count", 1))
        q = QUALITY.get(int(e.get("quality", 1)), "")
        qs = f" ({q})" if q and q != "junk" else ""
        cnt = f" x{n}" if n > 1 else ""
        return f"I picked up {e.get('item', '?')}{cnt}{qs}."

    if k == "kill":
        return f"I killed {e.get('victim', '?')} (level {e.get('victim_level', '?')})."

    if k == "death":
        killer = f", killed by {e['killer']}" if e.get("killer") else ""
        return f"I died in {e.get('zone', '?')}{killer}."

    if k == "resurrect":
        return f"I came back to life in {e.get('zone', '?')}."

    if k == "zone":
        return f"I crossed into {e.get('area', '?')} ({e.get('zone', '?')})."

    if k == "combat":
        if e.get("status") == "start":
            enemy = f" — {e['enemy']} is on me" if e.get("enemy") else ""
            return f"A fight broke out{enemy}."
        return (
            f"The fight ended after {e.get('seconds', '?')}s — "
            f"{e.get('kills', 0)} kill(s), {e.get('hp_pct', '?')}% health left."
        )

    if k == "pulse":
        return (
            f"[pulse] Level {e.get('level', '?')}, {e.get('hp_pct', '?')}% hp, "
            f"{fmt_money(e.get('money', 0))}, in {e.get('area', '?')} ({e.get('zone', '?')}), "
            f"{e.get('bag_free', '?')} bag slots free."
        )

    if k == "task":
        reason = f" ({e['reason']})" if e.get("reason") else ""
        return f"I rejected an op: {e.get('text', '?')}{reason}."

    if k == "tell":
        return f"Game feedback: {e.get('text', '?')}"

    if k == "act":
        ok = e.get("ok")
        detail = f" ({e['detail']})" if e.get("detail") else ""
        return f"[say] {'done' if ok else 'failed'}{detail}."

    if k == "invite":
        return f"{e.get('from', 'Someone')} invited me to a group."

    # unknown kind — degrade gracefully, never drop information
    rest = {a: b for a, b in e.items() if a not in ("t", "bot", "kind")}
    return f"[{k}] {json.dumps(rest, ensure_ascii=False)}"


def render_collapsed(run: list[dict]) -> str | None:
    """Render a buffered run of same-kind percepts as one sentence."""
    if len(run) == 1:
        return render_one(run[0])
    k = run[0]["kind"]
    if k == "xp":
        total = sum(int(e.get("amount", 0)) for e in run)
        srcs: dict[str, int] = {}
        for e in run:
            if e.get("from"):
                srcs[e["from"]] = srcs.get(e["from"], 0) + 1
        detail = ", ".join(f"{n} x{c}" if c > 1 else n for n, c in srcs.items())
        return f"I gained {total} experience" + (f" ({detail})" if detail else "") + "."
    if k == "money":
        total = sum(int(e.get("delta", 0)) for e in run)
        last_total = run[-1].get("total", 0)
        if total == 0:
            return None
        verb = "earned" if total > 0 else "spent"
        return f"I {verb} {fmt_money(abs(total))} over a few exchanges (now {fmt_money(last_total)})."
    if k == "loot":
        items: dict[str, int] = {}
        for e in run:
            items[e.get("item", "?")] = items.get(e.get("item", "?"), 0) + int(e.get("count", 1))
        listing = ", ".join(f"{it} x{c}" if c > 1 else it for it, c in items.items())
        return f"I picked up {listing}."
    return "\n".join(filter(None, (render_one(e) for e in run)))


class Renderer:
    def __init__(self, out=sys.stdout):
        self.out = out
        self.run: list[dict] = []

    def _flush(self):
        if not self.run:
            return
        line = render_collapsed(self.run)
        if line:
            self.out.write(f"[{ts(self.run[0].get('t', 0))}] {line}\n")
        self.run = []

    def feed(self, raw: str):
        raw = raw.strip()
        if not raw:
            return
        try:
            e = json.loads(raw)
        except json.JSONDecodeError:
            self.out.write(f"[??:??:??] (unreadable percept: {raw[:80]})\n")
            return
        k = e.get("kind")
        if self.run and (
            k != self.run[0].get("kind")
            or k not in COLLAPSE_KINDS
            or e.get("t", 0) - self.run[-1].get("t", 0) > COLLAPSE_GAP_MS
        ):
            self._flush()
        if k in COLLAPSE_KINDS:
            self.run.append(e)
        else:
            line = render_one(e)
            if line:
                self.out.write(f"[{ts(e.get('t', 0))}] {line}\n")

    def close(self):
        self._flush()
        self.out.flush()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("file", help="<Bot>.percepts.jsonl")
    ap.add_argument("--follow", action="store_true", help="keep tailing for new percepts")
    args = ap.parse_args()

    r = Renderer()
    with open(args.file, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            r.feed(line)
        r.close()
        if not args.follow:
            return
        try:
            while True:
                line = f.readline()
                if line:
                    r.feed(line)
                    r.close()  # in follow mode, flush eagerly
                else:
                    time.sleep(0.5)
        except KeyboardInterrupt:
            r.close()


if __name__ == "__main__":
    main()
