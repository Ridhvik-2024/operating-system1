# ai/proposal_schema.py

AI_PROPOSAL_SCHEMA = {
    "type": "object",
    "required": ["type", "confidence"],
    "properties": {
        "type": {
            "type": "string",
            "enum": [
                "IMPROVE_TEXT",
                "CODE_GENERATION",
                "UNKNOWN"
            ]
        },
        "confidence": {
            "type": "number",
            "minimum": 0.0,
            "maximum": 1.0
        },
        "result": {
            "type": "object"
        }
    }
}