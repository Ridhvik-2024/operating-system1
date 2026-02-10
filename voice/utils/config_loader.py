# utils/config_loader.py
# Loads JSON config files safely with debug output

import json
from pathlib import Path

def load_json(path: str, debug=True) -> dict:
    path_obj = Path(path)

    if debug:
        print(f"[CONFIG] Loading config file: {path_obj}")

    if not path_obj.exists():
        raise FileNotFoundError(f"[CONFIG ERROR] File not found: {path}")

    try:
        with open(path_obj, "r", encoding="utf-8") as f:
            data = json.load(f)

        if debug:
            print(f"[CONFIG] Loaded keys from {path}: {list(data.keys())}")

        return data

    except json.JSONDecodeError as e:
        print(f"[CONFIG ERROR] Invalid JSON in {path}")
        raise e