# intent/learner.py
# STRICT learner: enforces single-pattern schema only

import json
from pathlib import Path
from intent.moderation import is_safe_to_learn

RULE_FILE = Path("data/learned_rules.json")


def learn(intent: dict, normalized_text: str):
    print("[LEARNER] Attempting to learn")

    if not is_safe_to_learn(intent, normalized_text):
        print("[LEARNER] Learning rejected")
        return

    # -------- load existing rules safely --------
    if RULE_FILE.exists():
        try:
            rules = json.loads(RULE_FILE.read_text())
            if not isinstance(rules, list):
                rules = []
        except Exception:
            rules = []
    else:
        rules = []

    # -------- duplicate check (STRICT) --------
    for r in rules:
        if r.get("pattern") == normalized_text:
            print("[LEARNER] Rule already exists")
            return

    # -------- enforce canonical schema --------
    rule = {
        "pattern": normalized_text,
        "intent_id": intent["intent_id"],
        "params": intent.get("params", {}),
        "confidence": intent.get("confidence", 0.95)
    }

    rules.append(rule)

    RULE_FILE.parent.mkdir(parents=True, exist_ok=True)
    RULE_FILE.write_text(json.dumps(rules, indent=2))

    print("[LEARNER] Rule learned:", rule)