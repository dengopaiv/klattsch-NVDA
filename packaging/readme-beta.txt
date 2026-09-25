Klattsch Native {VERSION} beta - for testers
============================================

This is a beta for testing, not a release. Please do not pass it on.


What this is
------------

A speech synthesizer you can type into. Klattsch Native is klattsch, Tony
Gies's parallel-formant synthesizer, rewritten in C to run natively, with an
English text front end in front of it. This beta is the sample generator: one
window where you type English (or phonemes), set the voice, and listen or save
a WAV file.

The NVDA add-on is not in this beta. It comes later, built on the same
engine, so what you hear here is what the add-on will speak with.

Nothing to install. Unpack the folder anywhere and run klattsch_gui.exe.
Windows 10 or 11, 64-bit.


Using it
--------

Everything works from the keyboard, and every control is labelled for screen
readers. Tab moves through the window in this order:

  Text to speak (Alt+T)      type English here, or phonemes in phoneme mode
  Phoneme mode (Alt+H)       tick it if the box holds klattsch phonemes
  Phoneme bank (Alt+B)       English (Klatt 1980), or one of two Japanese
  Sample rate                48000 Hz unless you want to hear it lower
  Base pitch (Alt+P)         the voice's pitch in Hz; 120 by default
  Rate (Alt+R)               milliseconds per phoneme; lower is faster
  Formant scale, vibrato, tremolo, aspiration, spectral tilt, effort
                             the rest of the voice; each shows its unit
  Comma pause                how long a comma pauses, in ms
  Speak (Alt+K)              also Enter, from anywhere but the text box
  Stop (Alt+O)               also Escape
  Convert to phonemes (Alt+C)
                             shows what the text became, as phonemes you
                             can edit and speak
  Save WAV (Alt+W)
  Reset defaults (Alt+D)
  Messages (Alt+M)           length, warnings, and the phonemes spoken

Escape stops speech; it does not close the window. Alt+F4 closes it.


What to tell us
---------------

Anything is useful. In particular:

- Words it says wrongly. Please send the word or the sentence. (Its English
  pronunciation comes from letter-to-sound rules, not a dictionary, so it
  will get some words wrong; the list of which ones is what we need.)
- Stress on the wrong syllable, or a sentence whose tune sounds wrong
  (questions, lists, long sentences).
- Anything in the window your screen reader reads badly, or that you cannot
  reach from the keyboard.
- Voice settings you liked. Messages shows the numbers; copy them.
- Crashes, and what you were doing.

Please include the version from the window title: Klattsch Native {VERSION} beta.


Credits and licences
--------------------

klattsch is Tony Gies's work (MIT licence), github.com/tgies/klattsch. This is
a fork of it; LICENSE is his licence and travels with every copy.

The English text front end is BSD-3-Clause and carries the NRL
letter-to-sound rules (Naval Research Laboratory, 1976), John A. Wasser's
public-domain arrangement of them, and Tamas Geczy's exception list.
NOTICE.md names every piece and gives the full terms.

Source and documentation: github.com/dengopaiv/klattsch-NVDA
