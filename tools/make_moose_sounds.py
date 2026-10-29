"""Make the moose's sounds (game/SOUND/cowhit.WAV and cowland.WAV) from public domain
recordings of a cow moose by the US Fish and Wildlife Service (Minnesota), from SoundBible:
  https://soundbible.com/1225-Moose-Call.html  (moose_1225.wav: a series of short grunts)
  https://soundbible.com/1227-Moose.html       (moose_1227.wav: two long moans)

cowhit (hit by a car): the first grunt of 1225. cowland (lands after flying): the start of
the loud moan in 1227. Both are pitched down by PITCH (resampled, so also slower) to sound
like a big bull rather than a cow, softened in the highs, and matched to the loudness of the
original moos. cowhit starts with LEAD_SILENCE of silence: the game restarts the hit sound for
a few frames while the car pushes the animal, and a loud start would stutter.

usage: python tools/make_moose_sounds.py <dir with moose_1225.wav and moose_1227.wav>
The original moos are kept as game/SOUND/*.WAV.orig.
"""
import os
import shutil
import sys
import wave

import numpy as np

SOUND = os.path.join(os.path.dirname(__file__), "..", "game", "SOUND")
RATE = 22050
PITCH = 0.70  # playback speed of the source: 0.70 is about 6 semitones down
LEAD_SILENCE = 0.15


def load(path):
    w = wave.open(path)
    a = np.frombuffer(w.readframes(w.getnframes()), np.int16).reshape(-1, w.getnchannels()).mean(1) / 32768
    assert w.getframerate() == RATE
    return a


def clip(a, t0, t1, fade_in=0.02, fade_out=0.12):
    c = a[int(t0 * RATE):int(t1 * RATE)].copy()
    n, m = int(fade_in * RATE), int(fade_out * RATE)
    c[:n] *= np.linspace(0, 1, n)
    c[-m:] *= np.linspace(1, 0, m)
    return c


def deepen(c):
    # stretch by 1/PITCH (lower and slower), then a gentle low-pass to soften the nasal highs
    t = np.arange(0, len(c) - 1, PITCH)
    c = np.interp(t, np.arange(len(c)), c)
    k = np.array([1, 2, 3, 2, 1], float)
    return np.convolve(c, k / k.sum(), mode="same")


def original_rms(name):
    w = wave.open(os.path.join(SOUND, name + ".WAV.orig"))
    a = (np.frombuffer(w.readframes(w.getnframes()), np.uint8).astype(float) - 128) / 128
    return np.sqrt(np.mean(a ** 2))


def save(name, c, lead=0.0):
    target = original_rms(name)
    for drive in np.arange(1, 8, 0.25):  # soft-clip up to the original's loudness
        d = np.tanh(drive * c / np.abs(c).max()) / np.tanh(drive)
        if np.sqrt(np.mean(d ** 2)) >= target:
            break
    d *= min(1.0, target / np.sqrt(np.mean(d ** 2)))  # already louder at drive 1: turn it down
    d = np.concatenate([np.zeros(int(lead * RATE)), d * 0.98])
    w = wave.open(os.path.join(SOUND, name + ".WAV"), "wb")
    w.setnchannels(1)
    w.setsampwidth(1)
    w.setframerate(RATE)
    w.writeframes(np.clip(np.round(d * 127 + 128), 0, 255).astype(np.uint8).tobytes())
    w.close()
    print(f"{name}: {len(d) / RATE:.2f} s, loudness {np.sqrt(np.mean(d ** 2)):.3f} (original {target:.3f})")


if __name__ == "__main__":
    src = sys.argv[1]
    for name in ("cowhit", "cowland"):
        orig = os.path.join(SOUND, name + ".WAV.orig")
        if not os.path.exists(orig):
            shutil.copy(os.path.join(SOUND, name + ".WAV"), orig)
    save("cowhit", deepen(clip(load(os.path.join(src, "moose_1225.wav")), 0.19, 0.85)), LEAD_SILENCE)
    save("cowland", deepen(clip(load(os.path.join(src, "moose_1227.wav")), 1.02, 1.95)))
