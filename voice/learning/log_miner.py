# learning/log_miner.py

import json
from collections import defaultdict
from pathlib import Path

LOG_FILE = Path("data/logs.jsonl")
RULES_FILE = Path("intent/rules.json")

MIN_FAILURES = 3
MIN_SUCCESS_CONFIDENCE = 0.9


def load_logs():
    if not LOG_FILE.exists():
        return []

    logs = []
    with open(LOG_FILE, "r", encoding="utf-8") as f:
        for line in f:
            try:
                logs.append(json.loads(line))
            except json.JSONDecodeError:
                continue
    return logs


def mine_rules():
    logs = load_logs()
    if not logs:
        return

    failures = defaultdict(int)
    successes = {}

    # ---------------- ANALYZE LOGS ----------------
    for entry in logs:
        if entry.get("event_type") != "INTENT_PARSED":
            continue

        payload = entry.get("payload", {})
        text = payload.get("text")
        intent_id = payload.get("intent_id")
        confidence = payload.get("confidence", 0)

        if not text:
            continue

        if intent_id == "UNKNOWN":
            failures[text] += 1
        else:
            if confidence >= MIN_SUCCESS_CONFIDENCE:
                successes[text] = payload

    # ---------------- LOAD RULES ----------------
    if RULES_FILE.exists():
        try:
            rules = json.loads(RULES_FILE.read_text())
        except Exception:
            rules = []
    else:
        rules = []

    existing_patterns = {r.get("pattern") for r in rules if "pattern" in r}

    learned = 0

    # ---------------- PROMOTE RULES ----------------
    for text, payload in successes.items():
        if failures[text] < MIN_FAILURES:
            continue
        if text in existing_patterns:
            continue

        new_rule = {
            "pattern": text.lower().strip(),
            "intent_id": payload["intent_id"],
            "params": payload.get("params", {}),
            "confidence": payload.get("confidence", 0.95)
        }

        rules.append(new_rule)
        learned += 1
        print("[AUTO-LEARN] Promoted rule:", new_rule)

    if learned:
        RULES_FILE.write_text(json.dumps(rules, indent=2))