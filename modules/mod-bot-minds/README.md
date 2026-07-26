# mod-bot-minds

Stage 0/1 of the streaming-consciousness architecture: a **percept tap** and an
**op mailbox** for individual playerbots. No LLM runs in this module — it turns
a bot's life into an append-only JSONL stream and accepts ops back.

## Shape

```
game hooks (map threads)          IO worker thread              Python (llm-lab / minds/)
PlayerScript / PlayerbotScript --> percept queue --> <Bot>.percepts.jsonl --> mind.py / render.py
OnPlayerUpdate op drain         <-- op queue      <-- <Bot>.ops.jsonl      <-- mind.py / puppet.py
```

Two hard walls, both honored here:
1. All game-state mutation happens on the owning map thread (`OnPlayerUpdate`
   drains the op queue; the IO thread only does file I/O and JSON parsing).
2. No file I/O or JSON parsing runs in the tick path.

## Config (`mod_bot_minds.conf`)

| key | meaning |
|---|---|
| `BotMinds.Enable` | master switch; inert when off or watch list empty |
| `BotMinds.Watch` | comma-separated exact character names |
| `BotMinds.PerceptDir` | where `<Name>.percepts.jsonl` / `<Name>.ops.jsonl` live |
| `BotMinds.PulseMinutes` | cadence of the periodic state snapshot |
| `BotMinds.OpsPollMs` | IO worker cadence |

Reloadable live via `reload config`. Ops written before the module attaches to
a file are never replayed (tail starts at EOF).

## Percept kinds

`pulse` (level/hp/money/zone/pos/bags), `chat` (what the bot hears, incl.
own-speech echo), `quest` (accepted/completed/abandoned), `level`, `xp`,
`money` (delta+total), `loot` (via `OnPlayerStoreNewItem` — includes vendor
buys and system restocks, see findings), `kill`, `death` (with killer if known),
`resurrect`, `zone`, `combat` (start/end with duration+kills+hp), `invite`,
`act` (say results), and `task` (rejected malformed or unsupported ops).

## Ops

The mailbox accepts three ops:

```json
{"op":"say","channel":"say|yell|party|guild|whisper","to":"Name","text":"..."}
{"op":"state","id":"opaque"}
{"op":"command","text":"<playerbots chat command>","id":"opaque"}
```

`to` is required only for whispers. `state` answers with a `state` percept
carrying the echoed `id` and the bot's full probe record under `data`
(LlmProbe::AppendBotJson, on the bot's map thread). `command` routes down the
same pipeline a master whisper takes (fromPlayer = master, else the bot
itself) and acks dispatch with an `act` percept; playerbots command feedback
("Casting X on Y", "Cannot cast ...") that would be whispered to a master is
tapped (LlmProbe::SetTellSink) and arrives as `tell` percepts, which the mind
daemon folds into the command's tool result together with a fresh state
render. Unknown kinds produce a rejected `task` percept; malformed lines are
rejected with a parse reason.

## minds/

- `llm-lab/mind.py --dir <PerceptDir>` — the production mind daemon: chat wakes
  -> state round-trip -> tool loop -> say/command ops (transcripts in the lab shape).
  Fetch tools: `recall_state`, `verb_help`, and `spellbook` (probe-exported known
  spells + Spell.dbc tooltips via `llm-lab/spelldbc.py`, filterable by
  `usable_on: others|self|enemy|any`)
- `render.py file [--follow]` — JSONL → first-person prose (collapses xp/money/loot runs)
- `puppet.py say <Bot> ...` — hand-typed say ops
- `stats.py file` — events/hour + rendered-tokens/hour (the episode-sizing number)
- `tests/sample.jsonl` — synthetic stream covering every emitted percept kind

## Probe findings (2026-07-09, bot Kianesta, Westfall)

- Measured **~1.2k percept-tokens/hour** idle-ish, ~3k/h while actively
  questing/fighting → one 12k-token episode ≈ 4–10 h of raw stream. Streaming
  math closes with huge margin against the measured serving envelope.
- **Combat flag can stick for 15+ minutes** (evading mob), freezing the bot's
  non-combat AI. Stage 1 wants a differ edge: "in combat N s without damage
  dealt/taken".
- `OnPlayerStoreNewItem` fires for playerbots' synthetic consumable restock
  (20× food/water/potions at login) — a provenance gap: looted vs granted is
  indistinguishable at this hook. Attribution capture is a Stage 1 concern.
- `AiPlayerbot.DisabledWithoutRealPlayer` was flipped to 0 in the live conf so
  bots exist without a real player online — revert if idle-CPU matters more
  than the probe/minds running unattended.
