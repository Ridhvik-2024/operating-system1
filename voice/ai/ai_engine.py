import os
import json
import requests
from jsonschema import validate
from ai.proposal_schema import AI_PROPOSAL_SCHEMA

GROQ_ENDPOINT = "https://api.groq.com/openai/v1/chat/completions"


def ai_propose_improvement(text: str, settings: dict) -> dict | None:
    """
    AI rewrites text only.
    No execution, no OS access.
    """

    if not settings.get("enabled"):
        return None

    api_key = os.getenv(settings["api_key_env"])
    if not api_key:
        print("[AI] API key missing")
        return None

    headers = {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json"
    }

    payload = {
        "model": settings["model"],
        "messages": [
            {
                "role": "system",
                "content": (
                    "Rewrite the given text.\n"
                    "Return STRICT JSON only:\n"
                    "{ type, confidence, result: { text } }\n"
                    "No explanations."
                )
            },
            {"role": "user", "content": text}
        ],
        "temperature": 0.4
    }

    try:
        resp = requests.post(
            GROQ_ENDPOINT,
            headers=headers,
            json=payload,
            timeout=settings["timeout_sec"]
        )
        resp.raise_for_status()

        raw = resp.json()["choices"][0]["message"]["content"]
        proposal = json.loads(raw)

        validate(instance=proposal, schema=AI_PROPOSAL_SCHEMA)
        return proposal

    except Exception as e:
        print("[AI ERROR]", e)
        return None


def ai_generate_code(task: str, settings: dict) -> dict | None:
    """
    AI generates code as TEXT ONLY.
    Files are proposed, never written automatically.
    """

    if not settings.get("enabled"):
        return None

    api_key = os.getenv(settings["api_key_env"])
    if not api_key:
        print("[AI] API key missing")
        return None

    headers = {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json"
    }

    payload = {
        "model": settings["model"],
        "messages": [
            {
                "role": "system",
                "content": (
                    "Generate source code only.\n"
                    "Return STRICT JSON:\n"
                    "{ type:'CODE_GENERATION', confidence, result:{ files:{filename:code} } }\n"
                    "No markdown. No explanations."
                )
            },
            {"role": "user", "content": task}
        ],
        "temperature": 0.3
    }

    try:
        resp = requests.post(
            GROQ_ENDPOINT,
            headers=headers,
            json=payload,
            timeout=settings["timeout_sec"]
        )
        resp.raise_for_status()

        raw = resp.json()["choices"][0]["message"]["content"]
        proposal = json.loads(raw)

        validate(instance=proposal, schema=AI_PROPOSAL_SCHEMA)
        return proposal

    except Exception as e:
        print("[AI CODE ERROR]", e)
        return None