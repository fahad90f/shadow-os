"""Speech-to-text: Moonshine base v2 (default) or Whisper base.en int8, via sherpa-onnx.

Measured in the voice lab on a 4-core AVX2 host (429 utterances, 2 threads):
Moonshine base v2 ~0.08 s per utterance and the best word error rate on the
synthetic set; Whisper base.en ~0.3-0.5 s (it always encodes a 30 s window) and
more robust to some accents, which is why `--calibrate` can pick it per user.
Whisper labels noise ("(wind blowing)", "[ Sound Effects ]"); wake.clean_transcript
removes those. Whisper accepts at most 30 s, so longer audio is split.
"""
import logging
import time

import numpy as np
import sherpa_onnx as so

from . import MIC_RATE
from .wake import clean_transcript

log = logging.getLogger("shadow-voiced")

WHISPER_MAX_S = 28.0


class Recognizer:
    def __init__(self, root, name, threads=2):
        self.name = name
        f = root.file
        t = time.monotonic()
        if name == "moonshine":
            d = "models/stt/moonshine/"
            self.rec = so.OfflineRecognizer.from_moonshine_v2(
                encoder=f(d + "encoder_model.ort"), decoder=f(d + "decoder_model_merged.ort"),
                tokens=f(d + "tokens.txt"), num_threads=threads)
            self.label = "moonshine-base-v2"
        elif name == "whisper":
            d = "models/stt/whisper/"
            self.rec = so.OfflineRecognizer.from_whisper(
                encoder=f(d + "encoder.int8.onnx"), decoder=f(d + "decoder.int8.onnx"),
                tokens=f(d + "tokens.txt"), language="en", task="transcribe", num_threads=threads)
            self.label = "whisper-base.en-int8"
        else:
            raise ValueError(f"unknown STT model {name!r}")
        self.load_s = time.monotonic() - t

    def _decode(self, x):
        s = self.rec.create_stream()
        s.accept_waveform(MIC_RATE, x)
        self.rec.decode_stream(s)
        return s.result.text

    def transcribe(self, x):
        """float32 mono 16 kHz -> (clean text, raw text, milliseconds)."""
        t = time.monotonic()
        x = np.asarray(x, dtype=np.float32)
        if len(x) < MIC_RATE // 10:
            return "", "", 0
        limit = int(WHISPER_MAX_S * MIC_RATE)
        if self.name == "whisper" and len(x) > limit:
            raw = " ".join(self._decode(x[i:i + limit]) for i in range(0, len(x), limit))
        else:
            raw = self._decode(x)
        return clean_transcript(raw), raw.strip(), int(round(1000 * (time.monotonic() - t)))
