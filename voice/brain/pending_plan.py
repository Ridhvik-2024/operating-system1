class PendingPlan:
    def __init__(self):
        self.files = {}
        self.active = False

    def set(self, files: dict):
        self.files = files
        self.active = True

    def clear(self):
        self.files = {}
        self.active = False

    def is_active(self) -> bool:
        return self.active

    def summary(self) -> str:
        return "\n".join(self.files.keys())