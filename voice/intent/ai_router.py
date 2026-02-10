# intent/ai_router.py

import os
import json
import requests


HF_MODEL = "HuggingFaceH4/zephyr-7b-beta"
HF_API_URL = f"https://api-inference.huggingface.co/models/{HF_MODEL}"


def _canonical_intent(raw: dict) -> dict:
    """
    Force intent into canonical schema so validators never fail.
    """
    return {
        "intent_id": raw.get("intent_id", "UNKNOWN"),
        "params": raw.get("params", {}),
        "confidence": float(raw.get("confidence", 0.4)),
        "source": "AI"
    }


def ai_route(text: str):
    api_key = os.getenv("HUGGINGFACE_API_KEY")

    if not api_key:
        print("[AI ROUTER] HuggingFace API key missing")
        return None

    headers = {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json"
    }

    prompt = f"""
Return ONLY valid JSON. No text. No explanations.

User input: "{text}"

Schema:
{{
  "intent_id": "OPEN_WEBSITE | OPEN_APP | SEARCH_WEB | MODE_SWITCH | NAVIGATION | UNKNOWN",
  "params": {{}},
  "confidence": 0.0
}}
"""

    payload = {
        "inputs": prompt,
        "parameters": {
            "temperature": 0.2,
            "max_new_tokens": 200
        }
    }

    try:
        r = requests.post(
            HF_API_URL,
            headers=headers,
            json=payload,
            timeout=60
        )
        r.raise_for_status()

        raw_text = r.json()[0].get("generated_text", "")
        print("[AI RAW OUTPUT]", raw_text[:500])

        # -------- HARD JSON EXTRACTION --------
        start = raw_text.find("{")
        end = raw_text.rfind("}") + 1

        if start == -1 or end <= start:
            print("[AI ROUTER ERROR] No JSON found")
            return _canonical_intent({"intent_id": "UNKNOWN"})

        json_text = raw_text[start:end]

        parsed = json.loads(json_text)

        intent = _canonical_intent(parsed)

        print("[AI ROUTER] Parsed intent:", intent)
        return intent

    except Exception as e:
        print("[AI ROUTER ERROR]", e)
        return _canonical_intent({"intent_id": "UNKNOWN"})