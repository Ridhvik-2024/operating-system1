class DictationBuffer:
    def __init__(self):
        self.buffer = ""

    def append(self, text: str):
        if self.buffer:
            self.buffer += " "
        self.buffer += text

    def replace(self, text: str):
        self.buffer = text

    def clear(self):
        self.buffer = ""

    def get(self) -> str:
        return self.buffer

    def is_empty(self) -> bool:
        return not self.buffer.strip()