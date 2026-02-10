# utils/say.py

from utils.tts import speak

def say(message: str, debug=False):
    print(message)
    try:
        speak(message, debug=debug)
    except Exception as e:
        print("[TTS ERROR]", e)