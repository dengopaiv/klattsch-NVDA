#!/usr/bin/env python3
"""Step 6's first exit test: the NVDA driver, without NVDA.

    python tools/test-driver.py <klattschNative library>

Loads nvda-addon/addon/synthDrivers/klattsch.py against stand-ins for the
NVDA modules it imports, and the real library. The stand-ins model what was
read in NVDA 2026.1's source (docs/22-addon-driver.md §22.1): a wave player
that plays on its own thread and calls each chunk's onDone when that chunk
has played, whose stop() discards the callbacks still pending and whose
idle() waits for playback to end; settings as auto-properties; the two
notifications as actions.

What it checks, each against SCREEN-READER.md §2 or NVDA-ADDON.md step 6:

  * the audio the driver feeds is byte for byte what the API renders for the
    same input -- the driver adds nothing and loses nothing;
  * every index is notified once, in order, at the chunk that ends where the
    API placed it; then synthDoneSpeaking;
  * index-only and empty utterances, breaks, character mode, PitchCommand;
  * cancel: rendering stops, the next utterance speaks, its indexes come out,
    and a setting made before the cancel survives it;
  * a failure inside the worker: the indexes and done are still notified,
    and the worker lives to speak again;
  * speakable text that makes no sound is logged as an error;
  * a sample-rate change makes a new player at the new rate.

Runs in CI against the Linux build of the same library. Exit 0 is a pass.
"""

import ctypes
import importlib.util
import os
import shutil
import sys
import tempfile
import textwrap
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DRIVER = ROOT / "nvda-addon" / "addon" / "synthDrivers" / "klattsch.py"

# --- stand-ins for NVDA --------------------------------------------------------

STUBS = {
    "config.py": """
        conf = {"audio": {"outputDevice": "default"}, "speech": {"klattsch": {"pitch": 50}}}
    """,
    "logHandler.py": """
        class _Log:
            def __init__(self):
                self.records = []
            def _add(self, level, msg, *a, **k):
                self.records.append((level, msg % a if a else msg))
            def error(self, msg, *a, **k): self._add("error", msg, *a)
            def warning(self, msg, *a, **k): self._add("warning", msg, *a)
            def debugWarning(self, msg, *a, **k): self._add("debugWarning", msg, *a)
            def debug(self, msg, *a, **k): pass
        log = _Log()
    """,
    "autoSettingsUtils/__init__.py": "",
    "autoSettingsUtils/utils.py": """
        class StringParameterInfo:
            def __init__(self, id, displayName):
                self.id = id
                self.displayName = displayName
    """,
    "autoSettingsUtils/driverSetting.py": """
        class DriverSetting:
            def __init__(self, id, displayNameWithAccelerator, availableInSettingsRing=False,
                         defaultVal=None, displayName=None, useConfig=True):
                self.id = id
                self.defaultVal = defaultVal
                self.availableInSettingsRing = availableInSettingsRing
        class NumericDriverSetting(DriverSetting):
            def __init__(self, id, displayNameWithAccelerator, availableInSettingsRing=False,
                         defaultVal=50, minVal=0, maxVal=100, minStep=1, normalStep=5,
                         largeStep=10, displayName=None, useConfig=True):
                super().__init__(id, displayNameWithAccelerator, availableInSettingsRing, defaultVal)
        class BooleanDriverSetting(DriverSetting):
            def __init__(self, id, displayNameWithAccelerator, availableInSettingsRing=False,
                         displayName=None, defaultVal=False, useConfig=True):
                super().__init__(id, displayNameWithAccelerator, availableInSettingsRing, defaultVal)
    """,
    "speech/__init__.py": "",
    "speech/commands.py": """
        import config
        class SynthCommand: pass
        class IndexCommand(SynthCommand):
            def __init__(self, index): self.index = index
        class CharacterModeCommand(SynthCommand):
            def __init__(self, state): self.state = state
        class BreakCommand(SynthCommand):
            def __init__(self, time=0): self.time = time
        class LangChangeCommand(SynthCommand):
            def __init__(self, lang): self.lang = lang
        class PitchCommand(SynthCommand):
            # As NVDA's BaseProsodyCommand: newValue is the configured value
            # plus the offset, on the 0-100 scale.
            def __init__(self, offset=0, multiplier=1):
                self._offset, self._multiplier = offset, multiplier
            @property
            def newValue(self):
                base = config.conf["speech"]["klattsch"]["pitch"]
                if self._offset:
                    return base + self._offset
                return int(base * self._multiplier)
    """,
    "synthDriverHandler.py": """
        from autoSettingsUtils.driverSetting import DriverSetting, NumericDriverSetting, BooleanDriverSetting
        from autoSettingsUtils.utils import StringParameterInfo
        import time

        class Action:
            def __init__(self, name):
                self.name = name
                self.events = []
            def notify(self, **kw):
                self.events.append((self.name, kw.get("index"), time.monotonic()))

        synthIndexReached = Action("index")
        synthDoneSpeaking = Action("done")

        class VoiceInfo(StringParameterInfo):
            def __init__(self, id, displayName, language=None):
                self.language = language
                super().__init__(id, displayName)

        class _AutoProps(type):
            # NVDA's AutoPropertyObject: _get_x / _set_x become property x.
            def __new__(mcs, name, bases, ns):
                cls = super().__new__(mcs, name, bases, ns)
                names = {k[5:] for k in dir(cls) if k.startswith(("_get_", "_set_"))}
                for n in names:
                    g = getattr(cls, "_get_" + n, None)
                    s = getattr(cls, "_set_" + n, None)
                    setattr(cls, n, property(g, s))
                return cls

        class SynthDriver(metaclass=_AutoProps):
            name = ""
            supportedSettings = ()
            def __init__(self):
                # As NVDA's initSettings: a setting's default is applied only
                # where the driver has no attribute of that name; a driver with
                # properties keeps its own values, and NVDA saves those.
                for s in self.supportedSettings:
                    if not hasattr(self, s.id):
                        setattr(self, s.id, s.defaultVal)
            def terminate(self): pass
            def _get_availableVoices(self):
                if not hasattr(self, "_availableVoices"):
                    self._availableVoices = self._getAvailableVoices()
                return self._availableVoices
            @classmethod
            def VoiceSetting(cls): return DriverSetting("voice", "&Voice", True)
            @classmethod
            def RateSetting(cls, minStep=1): return NumericDriverSetting("rate", "&Rate", True)
            @classmethod
            def RateBoostSetting(cls): return BooleanDriverSetting("rateBoost", "Rate boos&t", True)
            @classmethod
            def PitchSetting(cls, minStep=1): return NumericDriverSetting("pitch", "&Pitch", True)
            @classmethod
            def InflectionSetting(cls, minStep=1): return NumericDriverSetting("inflection", "&Inflection", True)
            @classmethod
            def VolumeSetting(cls, minStep=1): return NumericDriverSetting("volume", "V&olume", True)
    """,
    "nvwave.py": """
        import queue, threading, time

        players = []

        class WavePlayer:
            # Plays on its own thread at SPEED times real time and calls each
            # chunk's onDone when that chunk has played, as WASAPI does.
            SPEED = 20.0

            def __init__(self, channels, samplesPerSec, bitsPerSample, outputDevice="default",
                         wantDucking=True, purpose=None):
                assert channels == 1 and bitsPerSample == 16
                self.samplesPerSec = samplesPerSec
                self.fed = bytearray()          # everything fed, in order
                self.played = bytearray()       # everything that finished playing
                self.marks = []                 # (bytes fed before it, onDone)
                self._q = queue.Queue()
                self._epoch = 0
                self._busy = 0
                self._cv = threading.Condition()
                self.closed = False
                threading.Thread(target=self._play, daemon=True).start()
                players.append(self)

            def _play(self):
                while True:
                    epoch, data, onDone = self._q.get()
                    time.sleep(len(data) / 2 / self.samplesPerSec / self.SPEED)
                    with self._cv:
                        live = epoch == self._epoch
                        if live:
                            self.played += data
                    if live and onDone:
                        onDone()
                    with self._cv:
                        self._busy -= 1
                        self._cv.notify_all()

            def feed(self, data, size=None, onDone=None):
                with self._cv:
                    self.fed += data
                    self._busy += 1
                    self._q.put((self._epoch, bytes(data), onDone))

            def idle(self):
                with self._cv:
                    self._cv.wait_for(lambda: self._busy == 0, timeout=30)

            def sync(self):
                self.idle()

            def stop(self):
                # As nvwave: what is still queued never plays, and its
                # callbacks are discarded.
                with self._cv:
                    self._epoch += 1

            def pause(self, switch):
                pass

            def close(self):
                self.stop()
                self.closed = True
    """,
}


def install_stubs(where):
    for rel, body in STUBS.items():
        p = where / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(textwrap.dedent(body), encoding="utf-8")


# --- the reference: the same input straight through the API -------------------

class Api:
    def __init__(self, path):
        lib = ctypes.CDLL(str(path))
        lib.kl_api_create.restype = ctypes.c_void_p
        lib.kl_api_create.argtypes = [ctypes.c_int]
        for f in ("kl_api_begin", "kl_api_end", "kl_api_destroy"):
            getattr(lib, f).argtypes = [ctypes.c_void_p]
        lib.kl_api_end.restype = ctypes.c_long
        lib.kl_api_text.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        lib.kl_api_index.argtypes = [ctypes.c_void_p, ctypes.c_int]
        lib.kl_api_render.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int16), ctypes.c_int,
                                      ctypes.POINTER(ctypes.c_int)]
        self.lib = lib

    def render(self, pieces, rate=22050):
        """(pcm bytes, [(index, byte offset)]) at the API's default settings."""
        lib = self.lib
        h = lib.kl_api_create(rate)
        lib.kl_api_begin(h)
        for p in pieces:
            if isinstance(p, int):
                lib.kl_api_index(h, p)
            else:
                lib.kl_api_text(h, p.encode())
        lib.kl_api_end(h)
        buf = (ctypes.c_int16 * 4096)()
        idx = ctypes.c_int()
        pcm, marks = bytearray(), []
        while True:
            n = lib.kl_api_render(h, buf, 4096, ctypes.byref(idx))
            pcm += ctypes.string_at(buf, n * 2)
            if idx.value >= 0:
                marks.append((idx.value, len(pcm)))
            if n == 0 and idx.value == -1:
                break
        lib.kl_api_destroy(h)
        return bytes(pcm), marks


# --- the checks -------------------------------------------------------------------

failures = 0


def check(cond, what):
    global failures
    print(("  ok    " if cond else "  FAIL  ") + what)
    if not cond:
        failures += 1


def main():
    if len(sys.argv) != 2:
        print(__doc__.split("\n\n")[1], file=sys.stderr)
        return 2
    lib_path = Path(sys.argv[1]).resolve()
    tmp = Path(tempfile.mkdtemp(prefix="klattsch-driver-"))
    try:
        install_stubs(tmp)
        drivers = tmp / "synthDrivers"
        drivers.mkdir()
        shutil.copy(DRIVER, drivers / "klattsch.py")
        shutil.copy(lib_path, drivers / ("klattschNative.dll" if os.name == "nt" else "klattschNative.so"))
        sys.path.insert(0, str(tmp))
        return run(drivers, Api(lib_path))
    finally:
        sys.path.remove(str(tmp)) if str(tmp) in sys.path else None
        shutil.rmtree(tmp, ignore_errors=True)


def run(drivers, api):
    spec = importlib.util.spec_from_file_location("klattsch", drivers / "klattsch.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    import nvwave
    import synthDriverHandler as sdh
    from logHandler import log
    from speech.commands import BreakCommand, CharacterModeCommand, IndexCommand, PitchCommand

    check(mod.SynthDriver.check(), "check() finds the library")
    synth = mod.SynthDriver()
    events = []

    def reset():
        sdh.synthIndexReached.events.clear()
        sdh.synthDoneSpeaking.events.clear()

    def collected():
        return sorted(sdh.synthIndexReached.events + sdh.synthDoneSpeaking.events, key=lambda e: e[2])

    def wait_done(n=1, timeout=30.0):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if len(sdh.synthDoneSpeaking.events) >= n:
                return True
            time.sleep(0.005)
        return False

    def player():
        return nvwave.players[-1]

    def speak(seq):
        reset()
        before = nvwave.players[-1] if nvwave.players else None
        start = len(before.fed) if before else 0
        synth.speak(seq)
        ok = wait_done()
        p = nvwave.players[-1] if nvwave.players else None
        if p is not before:
            start = 0      # a new player was made for this utterance
        return ok, (bytes(p.fed[start:]) if p else b"")

    print("Speaking, with indexes")
    pieces = ["Hello there, world.", 1, "This is the second line.", 2]
    want_pcm, want_marks = api.render(pieces)
    ok, got = speak(["Hello there, world.", IndexCommand(1), "This is the second line.", IndexCommand(2)])
    ev = collected()
    check(ok, "synthDoneSpeaking after the utterance")
    check(got == want_pcm, f"the audio fed is the API's, byte for byte ({len(got)} of {len(want_pcm)} bytes)")
    check([(e[0], e[1]) for e in ev] == [("index", 1), ("index", 2), ("done", None)],
          f"indexes 1, 2 once each, in order, then done: {[(e[0], e[1]) for e in ev]}")
    check(player().samplesPerSec == 22050, "the player runs at the default 22,050 Hz")

    print("Indexes with no audio")
    ok, got = speak([IndexCommand(5)])
    check(ok and got == b"" and [e[1] for e in sdh.synthIndexReached.events] == [5],
          "an index on its own: notified, no audio, done")
    ok, got = speak(["", "   ", IndexCommand(6), IndexCommand(7)])
    check(ok and got == b"" and [e[1] for e in sdh.synthIndexReached.events] == [6, 7],
          "empty and blank text: indexes 6 and 7, no audio, done")
    # "..." is three `.` pauses to the front end: silence, of a length, not
    # nothing (docs/21-stage-api.md §21.4, corrected).
    ok, got = speak(["...", IndexCommand(8)])
    check(ok and len(got) >= 2 * 22050 * 900 // 1000 and not any(got)
          and [e[1] for e in sdh.synthIndexReached.events] == [8],
          f"punctuation only: {len(got) // 2} samples of silence, then index 8")

    print("Commands")
    ok, plain = speak(["Aa bb."])
    ok2, broken = speak(["Aa", BreakCommand(300), "bb."])
    check(ok and ok2 and len(broken) >= len(plain) + 2 * 22050 * 290 // 1000,
          f"a 300 ms break adds at least 290 ms ({(len(broken) - len(plain)) // 2} samples)")
    ok, got = speak([CharacterModeCommand(True), "a", CharacterModeCommand(False), IndexCommand(40)])
    check(ok and len(got) > 0 and [e[1] for e in sdh.synthIndexReached.events] == [40],
          "character mode speaks a letter name, then index 40")
    ok, flat = speak(["Big", "letters."])
    ok2, raised = speak(["Big", PitchCommand(offset=30), "letters.", PitchCommand(), IndexCommand(50)])
    check(ok and ok2 and raised != flat and [e[1] for e in sdh.synthIndexReached.events] == [50],
          "PitchCommand changes the audio, and index 50 still comes out")
    ok, again = speak(["Big", "letters."])
    check(ok and again == flat, "after PitchCommand() the pitch is back to the setting")

    print("Cancel")
    long_text = " ".join(f"Sentence number {i} of a long paragraph." for i in range(12))
    reset()
    synth.speak([long_text, IndexCommand(10), long_text, IndexCommand(11)])
    time.sleep(0.05)
    synth.rate = 100                      # a setting made just before a cancel
    synth.cancel()
    ok, fast = speak(["Hello there, world.", IndexCommand(20)])
    idx = [e[1] for e in sdh.synthIndexReached.events]
    check(ok and idx and idx[-1] == 20, f"after a cancel the next utterance speaks, and index 20 comes out: {idx}")
    check(10 not in idx and 11 not in idx, "the cancelled utterance's indexes are not reported after it")
    slow, _ = api.render(["Hello there, world."])
    check(0 < len(fast) < len(slow) * 0.6, f"the rate set before the cancel survived it ({len(fast)} < {len(slow)})")
    synth.rate = 50

    print("Failure inside the worker")
    real_render = synth._lib.render
    calls = {"n": 0}

    def broken_render(*a):
        calls["n"] += 1
        if calls["n"] == 2:
            raise OSError("injected")
        return real_render(*a)

    synth._lib.render = broken_render
    log.records.clear()
    ok, _ = speak(["Hello there, world.", IndexCommand(30), "More.", IndexCommand(31)])
    synth._lib.render = real_render
    idx = sorted(e[1] for e in sdh.synthIndexReached.events)
    check(ok and idx == [30, 31], f"a failed utterance still notifies its indexes and done: {idx}")
    check(any(r[0] == "error" for r in log.records), "and logs the failure")
    ok, got = speak(["Still here.", IndexCommand(32)])
    check(ok and len(got) > 0 and [e[1] for e in sdh.synthIndexReached.events] == [32],
          "the worker lives to speak again")

    print("Silent success")
    real_sounding = synth._lib.sounding
    synth._lib.sounding = lambda h: 0
    log.records.clear()
    ok, _ = speak(["Words with letters."])
    synth._lib.sounding = real_sounding
    check(any(r[0] == "error" and "no sound" in r[1] for r in log.records),
          "speakable text that makes no sound is logged as an error")

    print("Settings")
    synth.sampleRate = "44100"
    ok, got = speak(["Hello."])
    check(ok and player().samplesPerSec == 44100 and len(got) > 0, "a new sample rate makes a new player")
    synth.sampleRate = "8000"
    check(synth.sampleRate == "22050", "a rate not offered is refused (falls back to 22,050)")
    check(list(synth.availableSamplerates) == ["22050", "44100", "48000"], "no rate below 16 kHz is offered")
    synth.volume = 0
    ok, got = speak(["Hello."])
    check(ok and len(got) > 0 and not any(got), "volume 0 is silence of the right length")
    synth.volume = 100

    synth.terminate()
    check(not synth._thread.is_alive(), "terminate() ends the worker")
    print(f"\n{'all checks pass' if not failures else f'{failures} failure(s)'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
