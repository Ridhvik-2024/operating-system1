# main.py
# Main control loop for AI Assistant
# Unified print + TTS output
# Deterministic, safe, debuggable


from speech.stt import SpeechToText
from intent.rule_router import route as rule_route
from brain.state import State
from brain.keyboard_brain import KeyboardBrain
from utils.normalizer import normalize
from utils.validators import validate_intent, is_confidence_acceptable
from utils.logger import log_event
from utils.config_loader import load_json
from intent.learner import learn
from brain.dictation_buffer import DictationBuffer
from brain.pending_plan import PendingPlan
from utils.file_writer import write_files
from ai.ai_engine import ai_propose_improvement, ai_generate_code
from intent.ai_router import ai_route
from learning.log_miner import mine_rules
from utils.say import say   # 🔥 unified output (print + TTS)

# ---------- Load Config ----------
settings = load_json("config/settings.json")
keys = load_json("config/keys.json")
ai_settings = load_json("config/ai.json")

CONF_LADDER = settings.get("confidence_ladder", {})

SOURCE_PRIORITY = CONF_LADDER.get("priority", {})
MIN_CONFIDENCE_BY_SOURCE = CONF_LADDER.get("min_confidence", {})

DEBUG = settings["debug"]["enabled"]

print("[MAIN] System starting...")

# ---------- Initialize State ----------
state = State(
    default_mode=settings["execution"]["default_mode"],
    debug=DEBUG
)

keyboard_brain = KeyboardBrain(
    state=state,
    settings=settings,
    keys=keys
)

stt = SpeechToText(
    language=settings["speech"]["language"],
    listen_timeout=settings["speech"]["listen_timeout_sec"],
    phrase_time_limit=settings["speech"]["phrase_time_limit_sec"],
    debug=DEBUG
)

dictation_buffer = DictationBuffer()
pending_plan = PendingPlan()


# =========================
# MAIN LOOP
# =========================

def main_loop():
    print("[MAIN] Entering main loop")

    while True:
        if DEBUG:
            print("[DEBUG] main loop tick")

        text = stt.listen()
        if DEBUG:
            print("[DEBUG] raw STT text:", text)

        if not text:
            continue

        normalized = normalize(text, debug=DEBUG)

        # =========================
        # STATE PARAM COMPLETION
        # =========================
        if state.awaiting_param:
            if DEBUG:
                print(f"[STATE] Completing awaiting param: {state.awaiting_param}")

            # Handle SEARCH_WEB follow-up
            if state.last_intent == "SEARCH_WEB" and state.awaiting_param == "query":
                intent = {
                    "intent_id": "SEARCH_WEB",
                    "params": {"query": normalized},
                    "confidence": 0.99,
                    "source": "STATE"
                }

                state.clear_awaiting()

                result = keyboard_brain.execute(intent)
                if isinstance(result, str) and result.strip():
                    say(result, debug=DEBUG)

                continue


        if DEBUG:
            print("[DEBUG] normalized:", normalized)

        # =========================
        # APPROVAL GATE
        # =========================
        if pending_plan.is_active():
            if normalized == "approve":
                write_files("generated_project", pending_plan.files)
                pending_plan.clear()
                say("Files created", debug=DEBUG)
                continue

            if normalized == "cancel":
                pending_plan.clear()
                say("Plan cancelled", debug=DEBUG)
                continue

        # =========================
        # DICTATION MODE
        # =========================
        if state.mode == "DICTATION":

            if normalized == "make this better":
                if dictation_buffer.is_empty():
                    say("Nothing to improve", debug=DEBUG)
                    continue

                proposal = ai_propose_improvement(
                    dictation_buffer.get(),
                    ai_settings
                )

                if proposal and proposal["confidence"] >= 0.8:
                    dictation_buffer.replace(
                        proposal["result"]["text"]
                    )
                    say("Updated dictation", debug=DEBUG)
                    print(dictation_buffer.get())
                else:
                    say("Rewrite failed", debug=DEBUG)

                continue

            dictation_buffer.append(text)
            print("[DICTATION BUFFER]")
            print(dictation_buffer.get())
            continue

        # =========================
        # COMMAND MODE
        # =========================

        intent = rule_route(normalized)

        # ---------- PARTIAL INTENT HANDLING ----------
        if intent and intent["intent_id"] == "SEARCH_WEB":
            query = intent["params"].get("query")

            if not query:
                if DEBUG:
                    print("[PARTIAL] SEARCH_WEB missing query")

                state.set_awaiting("query")
                state.set_last_intent("SEARCH_WEB")
                say("What should I search?", debug=DEBUG)
                continue

        # ---------- AI FALLBACK ----------
        if not intent:
            intent = ai_route(normalized)

        # ---------- VALIDATE + EXECUTE ----------
        # =========================
        # CONFIDENCE GATE (LADDER)
        # =========================
        if intent and validate_intent(intent, debug=DEBUG):

            source = intent.get("source", "UNKNOWN")
            confidence = intent.get("confidence", 0.0)

            min_required = MIN_CONFIDENCE_BY_SOURCE.get(source, 1.1)

            if confidence >= min_required:
                if DEBUG:
                    print(
                        f"[CONFIDENCE] ACCEPTED "
                        f"source={source} confidence={confidence}"
                    )

                result = keyboard_brain.execute(intent)

                if isinstance(result, str) and result.strip():
                    say(result, debug=DEBUG)

                # ❗ Learn ONLY from RULES or STATE
                if source in ("RULES", "STATE"):
                    learn(intent, normalized)

                continue

            else:
                if DEBUG:
                    print(
                        f"[CONFIDENCE] REJECTED "
                        f"source={source} confidence={confidence} "
                        f"(min={min_required})"
                    )
                    result = keyboard_brain.execute(intent)

            #  ALWAYS speak if there is user-facing output
            if isinstance(result, str) and result.strip():
                say(result, debug=DEBUG)

            learn(intent, normalized)
            continue

        # =========================
        # AI CODE GENERATION (SAFE)
        # =========================
        if normalized.startswith("create "):
            proposal = ai_generate_code(normalized, ai_settings)

            if (
                proposal
                and proposal["type"] == "CODE_GENERATION"
                and proposal["confidence"] >= 0.8
            ):
                files = proposal["result"].get("files", {})
                pending_plan.set(files)

                say("I have a plan ready. Say approve or cancel.", debug=DEBUG)
                print(pending_plan.summary())
            else:
                say("No valid code proposal", debug=DEBUG)

            continue

        # =========================
        # NAVIGATION MODE
        # =========================
        if state.mode == "NAVIGATION":
            intent = rule_route(normalized)
            if intent and intent["intent_id"] == "NAVIGATION":
                result = keyboard_brain.execute(intent)
                if isinstance(result, str) and result.strip():
                    say(result, debug=DEBUG)
            continue

        # =========================
        # NOTHING MATCHED
        # =========================
        if DEBUG:
            print("[MAIN] No rule matched")


# =========================
# ENTRY POINT
# =========================

if __name__ == "__main__":
    try:
        print("[MAIN] Mining logs for new rules...")
        mine_rules()
        main_loop()
    except KeyboardInterrupt:
        print("\n[MAIN] Shutdown requested")