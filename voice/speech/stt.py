# speech/stt.py
# Speech-to-Text with:
# - HARD timeout (no infinite hangs)
# - Better sentence completion handling
# - Debug output everywhere

import speech_recognition as sr
import threading

class SpeechToText:
    def __init__(
        self,
        language="en-IN",
        listen_timeout=3,
        phrase_time_limit=5,
        recognition_timeout=6,
        debug=True
    ):
        self.recognizer = sr.Recognizer()
        self.language = language
        self.listen_timeout = listen_timeout
        self.phrase_time_limit = phrase_time_limit
        self.recognition_timeout = recognition_timeout
        self.debug = debug

        # ---------- Recognition tuning ----------
        self.recognizer.energy_threshold = 300
        self.recognizer.dynamic_energy_threshold = True

        # IMPORTANT: These two control sentence cutoff
        self.recognizer.pause_threshold = 0.9          # wait longer before deciding "sentence ended"
        self.recognizer.non_speaking_duration = 0.4    # tolerate short pauses

        # ---------- One-time mic calibration ----------
        with sr.Microphone() as source:
            if self.debug:
                print("[STT] Calibrating microphone (1s)...")
            self.recognizer.adjust_for_ambient_noise(source, duration=1)

        if self.debug:
            print("[STT] Initialized")
            print(f"[STT] phrase_time_limit={self.phrase_time_limit}s")
            print(f"[STT] pause_threshold={self.recognizer.pause_threshold}s")

    # ---------------- INTERNAL ----------------

    def _recognize_worker(self, audio, result: dict):
        """Runs Google STT in a thread so it can't block forever"""
        try:
            text = self.recognizer.recognize_google(
                audio,
                language=self.language
            )
            result["text"] = text
        except Exception as e:
            result["error"] = str(e)

    # ---------------- PUBLIC ----------------

    def listen(self) -> str | None:
        with sr.Microphone() as source:
            if self.debug:
                print("[STT] Listening...")

            try:
                audio = self.recognizer.listen(
                    source,
                    timeout=self.listen_timeout,
                    phrase_time_limit=self.phrase_time_limit
                )
            except sr.WaitTimeoutError:
                if self.debug:
                    print("[STT] Listen timeout (no speech)")
                return None
            except Exception as e:
                print("[STT ERROR] Listen failed:", e)
                return None

        if self.debug:
            print("[STT] Audio captured, recognizing...")

        # ---------- HARD TIMEOUT PROTECTION ----------
        result = {}
        t = threading.Thread(
            target=self._recognize_worker,
            args=(audio, result),
            daemon=True
        )
        t.start()
        t.join(timeout=self.recognition_timeout)

        if t.is_alive():
            print("[STT ERROR] Recognition timeout (Google STT hung)")
            return None

        if "error" in result:
            print("[STT ERROR] Recognition failed:", result["error"])
            return None

        text = result.get("text")
        if text:
            text = text.strip()
            if self.debug:
                print("[STT RESULT]", text)
            return text

        if self.debug:
            print("[STT] Empty recognition result")
        return None