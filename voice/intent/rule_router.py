# intent/rule_router.py
# STRICT rule-first router
# NO guessing. NO fallback intents.
# If unsure → return None → AI handles it
import difflib
import json
import re
from pathlib import Path

RULES_FILE = Path("intent/rules.json")


# ---------------- HELPERS ----------------

def _intent(intent_id, params=None, confidence=0.95, source="RULES"):
    if params is None:
        params = {}
    print(f"[RULE ROUTER] Matched intent: {intent_id}")
    return {
        "intent_id": intent_id,
        "params": params,
        "confidence": confidence,
        "source": source
    }


def load_rules():
    if not RULES_FILE.exists():
        return []

    try:
        data = json.loads(RULES_FILE.read_text(encoding="utf-8"))
        if not isinstance(data, list):
            return []

        # canonical single-pattern rules only
        return [
            r for r in data
            if isinstance(r, dict)
            and isinstance(r.get("pattern"), str)
            and isinstance(r.get("intent_id"), str)
        ]

    except Exception as e:
        print("[RULE ROUTER ERROR] Failed to load rules:", e)
        return []


# ---------------- FUZZY MATCH ----------------

FUZZY_THRESHOLD = 0.92  # very strict on purpose


def fuzzy_match(normalized: str, rules: list):
    best_score = 0.0
    best_rule = None

    for rule in rules:
        pattern = rule["pattern"]
        score = difflib.SequenceMatcher(None, normalized, pattern).ratio()

        if score > best_score:
            best_score = score
            best_rule = rule

    if best_score >= FUZZY_THRESHOLD:
        print(
            f"[FUZZY MATCH] '{normalized}' → '{best_rule['pattern']}' "
            f"(score={best_score:.2f})"
        )
        return best_rule

    return None

# ---------------- ROUTER ----------------

def route(normalized: str):
    print("[RULE ROUTER] Input:", normalized)

    if not normalized:
        return None

    normalized = normalized.lower().strip()

    # 🔁 Reload rules every time (safe, rules can change)
    rules = load_rules()

    # ==================================================
    # 1️⃣ EXACT MATCH — absolute priority
    # ==================================================
    for rule in rules:
        if normalized == rule["pattern"]:
            return _intent(
                rule["intent_id"],
                rule.get("params", {}),
                rule.get("confidence", 0.95),
                source="RULES"
            )
        
    # ==================================================
    # 1️⃣.5️⃣ FUZZY MATCH (STRICT, RULES ONLY)
    # ==================================================
    fuzzy_rule = fuzzy_match(normalized, rules)
    if fuzzy_rule:
        return _intent(
            fuzzy_rule["intent_id"],
            fuzzy_rule.get("params", {}),
            fuzzy_rule.get("confidence", 0.9),
            source="RULES"
        )

    # ==================================================
    # 2️⃣ MODE SWITCH (safe hardcoded)
    # ==================================================
    if normalized in ("command mode", "dictation mode", "navigation mode"):
        return _intent(
            "MODE_SWITCH",
            {"mode": normalized.replace(" mode", "").upper()},
            confidence=0.99
        )

    # ==================================================
    # 3️⃣ SEARCH — explicit partials only
    # ==================================================
    if normalized in ("search", "search google", "search on google"):
        return _intent(
            "SEARCH_WEB",
            {"query": ""},
            confidence=0.9
        )

    m = re.match(r"^(search|search for|google)\s+(.+)$", normalized)
    if m:
        return _intent(
            "SEARCH_WEB",
            {"query": m.group(2)},
            confidence=0.99
        )

    # ==================================================
    # 4️⃣ NAVIGATION (safe deterministic)
    # ==================================================
    NAV = {
        "scroll up": ("UP", 3),
        "scroll down": ("DOWN", 3),
        "go left": ("LEFT", 1),
        "go right": ("RIGHT", 1),
    }

    if normalized in NAV:
        direction, count = NAV[normalized]
        return _intent(
            "NAVIGATION",
            {"direction": direction, "count": count},
            confidence=0.99
        )

    # ==================================================
    # 5️⃣ NOTHING MATCHED → AI decides
    # ==================================================
    print("[RULE ROUTER] No rule matched")
    return None