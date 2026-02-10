import os


def write_files(base_dir: str, files: dict):
    os.makedirs(base_dir, exist_ok=True)

    for name, content in files.items():
        path = os.path.join(base_dir, name)
        folder = os.path.dirname(path)

        if folder:
            os.makedirs(folder, exist_ok=True)

        with open(path, "w", encoding="utf-8") as f:
            f.write(content)

        print("[FILE CREATED]", path)