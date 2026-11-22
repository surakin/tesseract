#!/usr/bin/env python3
"""Every state event type the registry shows must have a phrase.

Usage: check_state_event_phrases.py STATE_EVENTS_RS PHRASES_CPP

Parses the `("<type>", StateTier::<Tier>)` entries of STATE_EVENT_KINDS and
requires each StateSetting or Always type to appear in StateEventPhrases.cpp,
except the Always types that keep a dedicated timeline row. Hidden types are
skipped. Exits 1 and prints each missing type otherwise.
"""
import re
import sys
from pathlib import Path

# Always-tier types rendered by their own rows, not by the phrase table.
DEDICATED_ROWS = {
    "m.room.name",
    "m.room.topic",
    "m.room.pinned_events",
    "m.room.tombstone",
}

ENTRY = re.compile(r'^\s*\("([^"]+)",\s*StateTier::(\w+)\)', re.M)


def main():
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    registry = Path(sys.argv[1]).read_text(encoding="utf-8")
    phrases = Path(sys.argv[2]).read_text(encoding="utf-8")
    entries = ENTRY.findall(registry)
    if not entries:
        print("no STATE_EVENT_KINDS entries found in " + sys.argv[1])
        return 1
    missing = [
        t for t, tier in entries
        if tier in ("StateSetting", "Always")
        and t not in DEDICATED_ROWS
        and f'"{t}"' not in phrases
    ]
    for t in missing:
        print(f"state event type has no phrase in StateEventPhrases.cpp: {t}")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
