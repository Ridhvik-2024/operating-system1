# utils/tts.py
# Robust pyttsx3 TTS for long-running loops (Windows-safe)
"""
import pyttsx3
import threading

_engine = None
_lock = threading.Lock()


def speak(text, debug=False):
    global _engine

    with _lock:
        try:
            if _engine is None:
                _engine = pyttsx3.init()
                _engine.setProperty("rate", 180)

                # force a known-good voice
                voices = _engine.getProperty("voices")
                if voices:
                    _engine.setProperty("voice", voices[0].id)

            if debug:
                print("[TTS]", text)

            _engine.say(text)

            # 🔥 CRITICAL: force event loop restart
            _engine.runAndWait()
            _engine.stop()

        except Exception as e:
            print("[TTS ERROR]", e)
            _engine = None  # force re-init next time
            """

# utils/tts.py
# Direct Windows SAPI (no pyttsx3)

import win32com.client
import threading

_speaker = win32com.client.Dispatch("SAPI.SpVoice")
_lock = threading.Lock()

def speak(text, debug=False):
    with _lock:
        if debug:
            print("[TTS]", text)
        _speaker.Speak(text)