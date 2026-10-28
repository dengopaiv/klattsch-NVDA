# klattsch.py -- the NVDA synth driver for Klattsch Native.
#
# Everything that speaks is in klattschNative.dll: the klattsch engine (Tony
# Gies's, translated to C; MIT), the English text front end (BSD-3-Clause;
# see NOTICE.md) and the streaming API of csrc/klattsch_api.h. This file only
# translates between NVDA and that API, on one thread.
#
# Written for this project from NVDA 2026.1's documented driver interface and
# the requirements in docs/SCREEN-READER.md; not adapted from another
# synthesizer's driver. What was read in NVDA's source, and why each rule
# below is the way it is, is in docs/22-addon-driver.md.
#
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Päiv Dengo (this driver)
# Copyright (c) 2026 Tony Gies (the engine it drives)

import ctypes
import os
import queue
import threading
from collections import OrderedDict

import config
import nvwave
from autoSettingsUtils.driverSetting import DriverSetting, NumericDriverSetting
from autoSettingsUtils.utils import StringParameterInfo
from logHandler import log
from speech.commands import BreakCommand, CharacterModeCommand, IndexCommand, PitchCommand
from synthDriverHandler import SynthDriver as BaseSynthDriver
from synthDriverHandler import VoiceInfo, synthDoneSpeaking, synthIndexReached

try:
	import addonHandler

	addonHandler.initTranslation()
except Exception:  # outside NVDA (tools/test-driver.py), or no translations
	pass

try:
	_  # noqa: B018 -- NVDA installs it as a builtin
except NameError:

	def _(s):
		return s


_LIB_NAME = "klattschNative.dll" if os.name == "nt" else "klattschNative.so"
_LIB_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), _LIB_NAME)

# csrc/klattsch_api.h, the KL_SET_* order.
(
	_SET_BASE_F0,
	_SET_RATE,
	_SET_SCALE,
	_SET_VIBRATO_DEPTH,
	_SET_VIBRATO_RATE,
	_SET_TREMOLO_DEPTH,
	_SET_TREMOLO_RATE,
	_SET_ASPIRATION,
	_SET_TILT,
	_SET_EFFORT,
	_SET_INFLECTION,
	_SET_VOLUME,
	_SET_COMMA_MS,
) = range(13)

#: The sample rates offered. None below 16 kHz: below about 12.2 kHz the
#: formant clamp moves /s/ onto /sh/ (docs/NVDA-ADDON.md, "Two decisions").
_RATES = ("22050", "44100", "48000")


# --- from NVDA's 0-100 sliders to the engine ---------------------------------
#
# Stand-ins, chosen so that 50 is the engine's default and every step is
# audible; each is replaced by a listened-to curve in step 6's NVDA test.
# docs/NVDA-ADDON.md, "From NVDA's sliders to the engine".


def _rate_ms(rate, boost):
	"""Milliseconds per phoneme: 220 at 0, 110 at 50, 35 at 100; halved by boost."""
	r = min(max(rate, 0), 100)
	if r <= 50:
		ms = 110.0 * 2.0 ** ((50 - r) / 50.0)
	else:
		ms = 110.0 * (35.0 / 110.0) ** ((r - 50) / 50.0)
	return ms / 2.0 if boost else ms


def _pitch_hz(pitch):
	"""Base F0: an octave either side of 120 Hz."""
	return 120.0 * 2.0 ** ((min(max(pitch, 0), 100) - 50) / 50.0)


def _unit(v):
	return min(max(v, 0), 100) / 100.0


class _Lib:
	"""The ctypes bindings, declared once. Only ints, doubles and pointers."""

	def __init__(self, path):
		lib = ctypes.CDLL(path)
		h = ctypes.c_void_p
		sig = {
			"kl_api_version": (ctypes.c_char_p, []),
			"kl_api_create": (h, [ctypes.c_int]),
			"kl_api_destroy": (None, [h]),
			"kl_api_sample_rate": (ctypes.c_int, [h]),
			"kl_api_set": (ctypes.c_int, [h, ctypes.c_int, ctypes.c_double]),
			"kl_api_begin": (ctypes.c_int, [h]),
			"kl_api_text": (ctypes.c_int, [h, ctypes.c_char_p]),
			"kl_api_spell": (ctypes.c_int, [h, ctypes.c_char_p]),
			"kl_api_break": (ctypes.c_int, [h, ctypes.c_int]),
			"kl_api_index": (ctypes.c_int, [h, ctypes.c_int]),
			"kl_api_end": (ctypes.c_long, [h]),
			"kl_api_sounding": (ctypes.c_long, [h]),
			"kl_api_warnings": (ctypes.c_char_p, [h]),
			"kl_api_render": (
				ctypes.c_int,
				[h, ctypes.POINTER(ctypes.c_int16), ctypes.c_int, ctypes.POINTER(ctypes.c_int)],
			),
		}
		for name, (res, args) in sig.items():
			fn = getattr(lib, name)
			fn.restype = res
			fn.argtypes = args
			short = name[len("kl_api_"):]
			# `break` is a keyword, so that one is break_.
			setattr(self, short + "_" if short == "break" else short, fn)


class SynthDriver(BaseSynthDriver):
	name = "klattsch"
	# Translators: the synthesizer's name in NVDA's synthesizer list.
	description = _("Klattsch Native")

	supportedSettings = (
		BaseSynthDriver.VoiceSetting(),
		BaseSynthDriver.RateSetting(),
		BaseSynthDriver.RateBoostSetting(),
		BaseSynthDriver.PitchSetting(),
		BaseSynthDriver.InflectionSetting(),
		BaseSynthDriver.VolumeSetting(),
		# Translators: a setting: noise in the voice, 0 is none.
		NumericDriverSetting("breathiness", _("&Breathiness"), availableInSettingsRing=True, defaultVal=0),
		# Translators: a setting: the shape of the glottal pulse, lax to tense.
		NumericDriverSetting("effort", _("&Effort"), availableInSettingsRing=True, defaultVal=50),
		# Translators: a setting: all formants scaled together; larger is a smaller head.
		NumericDriverSetting("headSize", _("&Head size"), availableInSettingsRing=True, defaultVal=50),
		# Translators: a setting: spectral tilt, darker below 50, brighter above.
		NumericDriverSetting("tilt", _("Spectral &tilt"), defaultVal=50),
		# Translators: a setting: how far the pitch wavers; 0 is none.
		NumericDriverSetting("vibratoDepth", _("Vibrato &depth"), defaultVal=0),
		# Translators: a setting: how fast the pitch wavers.
		NumericDriverSetting("vibratoSpeed", _("Vibrato &speed"), defaultVal=50),
		# Translators: a setting: the audio sample rate.
		DriverSetting("sampleRate", _("Sa&mple rate"), defaultVal="22050"),
	)
	supportedCommands = {IndexCommand, CharacterModeCommand, BreakCommand, PitchCommand}
	supportedNotifications = {synthIndexReached, synthDoneSpeaking}

	@classmethod
	def check(cls):
		return ctypes.sizeof(ctypes.c_void_p) == 8 and os.path.isfile(_LIB_PATH)

	def __init__(self):
		self._lib = _Lib(_LIB_PATH)
		self._checkVersion()
		self._values = {
			"rate": 50,
			"rateBoost": False,
			"pitch": 50,
			"inflection": 50,
			"volume": 100,
			"breathiness": 0,
			"effort": 50,
			"headSize": 50,
			"tilt": 50,
			"vibratoDepth": 0,
			"vibratoSpeed": 50,
			"sampleRate": "22050",
		}
		self._voice = "default"
		self._gen = 0
		self._speakPending = 0
		self._lock = threading.Lock()
		self._player = None
		self._handle = None
		self._queue = queue.Queue()
		self._thread = threading.Thread(target=self._run, name="klattsch", daemon=True)
		self._thread.start()
		super().__init__()

	def _checkVersion(self):
		"""Refuse a library that is not the one the manifest names."""
		lib = self._lib.version().decode("ascii")
		try:
			want = addonHandler.getCodeAddon().manifest["version"]
		except Exception:
			return  # not running from an installed add-on
		if not want.startswith(lib):
			raise RuntimeError(f"klattschNative is {lib}, the add-on is {want}")

	def terminate(self):
		self.cancel()
		self._queue.put(None)
		self._thread.join(5.0)
		with self._lock:
			if self._player:
				self._player.close()
				self._player = None
		super().terminate()

	# --- settings: stored here, read by the worker at each utterance --------
	#
	# A setting is a value, not a queued item, so it survives a cancel by
	# construction (SCREEN-READER.md §2.3).

	def _getAvailableVoices(self):
		# Translators: the one voice until voices come from presets.
		return OrderedDict(default=VoiceInfo("default", _("Default"), "en"))

	def _get_voice(self):
		return self._voice

	def _set_voice(self, value):
		self._voice = value if value in self.availableVoices else "default"

	def _get_availableSamplerates(self):
		return OrderedDict((r, StringParameterInfo(r, f"{int(r):,} Hz")) for r in _RATES)

	def _get_sampleRate(self):
		return self._values["sampleRate"]

	def _set_sampleRate(self, value):
		self._values["sampleRate"] = value if value in _RATES else "22050"

	def _get_rateBoost(self):
		return self._values["rateBoost"]

	def _set_rateBoost(self, value):
		self._values["rateBoost"] = bool(value)

	def _numeric(name):  # noqa: N805 -- a class-body helper, not a method
		def get(self):
			return self._values[name]

		def set_(self, value):
			self._values[name] = min(max(int(value), 0), 100)

		return get, set_

	_get_rate, _set_rate = _numeric("rate")
	_get_pitch, _set_pitch = _numeric("pitch")
	_get_inflection, _set_inflection = _numeric("inflection")
	_get_volume, _set_volume = _numeric("volume")
	_get_breathiness, _set_breathiness = _numeric("breathiness")
	_get_effort, _set_effort = _numeric("effort")
	_get_headSize, _set_headSize = _numeric("headSize")
	_get_tilt, _set_tilt = _numeric("tilt")
	_get_vibratoDepth, _set_vibratoDepth = _numeric("vibratoDepth")
	_get_vibratoSpeed, _set_vibratoSpeed = _numeric("vibratoSpeed")
	del _numeric

	# --- NVDA's calls: queue and return --------------------------------------

	def speak(self, speechSequence):
		with self._lock:
			self._speakPending += 1
			gen = self._gen
		self._queue.put((gen, list(speechSequence)))

	def cancel(self):
		# NVDA's speech manager resets itself before calling this and ignores
		# indexes from the cancelled speech, so they are dropped, not reported
		# (docs/22-addon-driver.md §22.2). The worker sees the new generation
		# between chunks and stops rendering.
		with self._lock:
			self._gen += 1
			if self._player:
				self._player.stop()

	def pause(self, switch):
		with self._lock:
			if self._player:
				self._player.pause(switch)

	# --- the worker ----------------------------------------------------------

	def _run(self):
		while True:
			item = self._queue.get()
			if item is None:
				break
			gen, seq = item
			indexes = [c.index for c in seq if isinstance(c, IndexCommand)]
			reported = set()
			try:
				if gen == self._gen:
					self._speak(gen, seq, reported)
			except Exception:
				log.error("klattsch: speaking failed", exc_info=True)
				# An utterance that failed still owes NVDA its indexes, or
				# say-all waits forever (SCREEN-READER.md §2.1).
				if gen == self._gen:
					for i in indexes:
						if i not in reported:
							self._notifyIndex(i, reported)
			finally:
				with self._lock:
					self._speakPending -= 1
					last = self._speakPending == 0
					player = self._player
				if last:
					# Nothing more queued: let the audio play out, then say so.
					# idle() blocks until the last chunk has played, so it waits
					# here and not between utterances. Its onDone callbacks are
					# driven by WASAPI, not by feeding, so no periodic flush is
					# needed (docs/22-addon-driver.md §22.2).
					if player:
						try:
							player.idle()
						except Exception:
							log.debugWarning("klattsch: idle failed", exc_info=True)
					synthDoneSpeaking.notify(synth=self)
		with self._lock:
			if self._handle:
				self._lib.destroy(self._handle)
				self._handle = None

	def _notifyIndex(self, index, reported):
		reported.add(index)
		synthIndexReached.notify(synth=self, index=index)

	def _prepare(self):
		"""The handle and the player at the current sample rate; on this thread."""
		rate = int(self._values["sampleRate"])
		if self._handle and self._lib.sample_rate(self._handle) != rate:
			self._lib.destroy(self._handle)
			self._handle = None
		if not self._handle:
			self._handle = self._lib.create(rate)
			if not self._handle:
				raise MemoryError("kl_api_create failed")
		with self._lock:
			if self._player and self._player.samplesPerSec != rate:
				self._player.idle()
				self._player.close()
				self._player = None
			if not self._player:
				self._player = nvwave.WavePlayer(
					channels=1,
					samplesPerSec=rate,
					bitsPerSample=16,
					outputDevice=config.conf["audio"]["outputDevice"],
				)
		return rate

	def _apply(self, pitch=None):
		v, h, s = self._values, self._handle, self._lib.set
		s(h, _SET_BASE_F0, _pitch_hz(v["pitch"] if pitch is None else pitch))
		s(h, _SET_RATE, _rate_ms(v["rate"], v["rateBoost"]))
		s(h, _SET_INFLECTION, v["inflection"] / 50.0)
		s(h, _SET_VOLUME, _unit(v["volume"]))
		s(h, _SET_ASPIRATION, _unit(v["breathiness"]))
		s(h, _SET_EFFORT, _unit(v["effort"]))
		s(h, _SET_SCALE, 1.25 ** ((v["headSize"] - 50) / 50.0))
		s(h, _SET_TILT, (v["tilt"] - 50) / 50.0 * 0.95)
		s(h, _SET_VIBRATO_DEPTH, _unit(v["vibratoDepth"]) * 10.0)
		s(h, _SET_VIBRATO_RATE, 1.0 + _unit(v["vibratoSpeed"]) * 8.0)

	def _speak(self, gen, seq, reported):
		rate = self._prepare()
		chunk = rate // 25  # 40 ms
		buf = (ctypes.c_int16 * chunk)()
		idx = ctypes.c_int()
		lib, h = self._lib, self._handle
		# Audio is held back one chunk, so an index that comes out with no
		# audio of its own can still ride on the chunk before it.
		held = [None, []]
		spelling = False
		letters = False
		pitch = None

		def feed(data, idxs):
			with self._lock:
				if gen != self._gen or not self._player:
					return
				player = self._player
			onDone = None
			if idxs:

				def onDone(idxs=tuple(idxs)):
					for i in idxs:
						self._notifyIndex(i, reported)

			player.feed(data, onDone=onDone)

		def render():
			total = lib.end(h)
			if total < 0:
				raise RuntimeError(f"kl_api_end failed ({total})")
			if letters and lib.sounding(h) == 0:
				log.error(f"klattsch: speakable text made no sound; warnings: {lib.warnings(h)!r}")
			while gen == self._gen:
				n = lib.render(h, buf, chunk, ctypes.byref(idx))
				if n < 0:
					raise RuntimeError(f"kl_api_render failed ({n})")
				if n > 0:
					if held[0] is not None:
						feed(*held)
					held[0] = ctypes.string_at(buf, n * 2)
					held[1] = [idx.value] if idx.value >= 0 else []
				elif idx.value >= 0:
					if held[0] is not None:
						held[1].append(idx.value)
					else:
						self._notifyIndex(idx.value, reported)
				else:
					return

		def begin():
			self._apply(pitch)
			lib.begin(h)

		begin()
		for item in seq:
			if gen != self._gen:
				return
			if isinstance(item, str):
				data = item.encode("utf-8", "replace")
				if any(ch.isalnum() for ch in item):
					letters = True
				if spelling and item.isascii():
					lib.spell(h, data)
				else:
					lib.text(h, data)
			elif isinstance(item, IndexCommand):
				lib.index(h, item.index)
			elif isinstance(item, BreakCommand):
				lib.break_(h, item.time)
			elif isinstance(item, CharacterModeCommand):
				spelling = item.state
			elif isinstance(item, PitchCommand):
				# The front end resets each sentence to the compiler's base
				# pitch, so a pitch change starts a new compile.
				render()
				letters = False
				pitch = item.newValue
				begin()
		render()
		if gen == self._gen and held[0] is not None:
			feed(*held)
