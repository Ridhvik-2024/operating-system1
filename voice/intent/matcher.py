import re

def normalize_for_match(text: str) -> str:
    """
    Normalize text for fuzzy rule matching
    """
    text = text.lower().strip()

    # remove filler words
    fillers = ["please", "the", "a", "an", "to"]
    for f in fillers:
        text = re.sub(rf"\b{f}\b", "", text)

    # collapse spaces
    text = re.sub(r"\s+", " ", text)

    return text.strip()