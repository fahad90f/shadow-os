"""shadow_voice — the offline voice engine behind Shadow Assistant (shadow-voiced).

The package is split so that the cheap parts (data-dir lookup, --probe, logging,
the protocol writer, text normalisation, the wake-word policy) import nothing
heavy; numpy and sherpa_onnx are imported only once a data root has been chosen
and its runtime/ directory put on sys.path (see datadir.activate_runtime).

Modules:
  datadir    data-root lookup order, manifest.json, model paths, --probe
  logsetup   voice.log (rotated at 1 MB) + stderr, crash/exception hooks
  logtee     tiny child process that copies the helper's stderr into voice.log
  protocol   newline-delimited JSON events on a private stdout fd
  audio      capture/playback tools (pw-* > pulse > alsa), WAV I/O, level meters
  textnorm   text normalisation for speech (numbers, units, paths, acronyms, emoji)
  g2p        English text -> Kokoro phoneme tokens (lexicon + morphology + espeak-ng)
  wake       wake-word policy, transcript clean-up, yes/no, word error rate
  stt        speech-to-text model loading (Moonshine v2 / Whisper)
  tts        Kokoro synthesis + the speaker (sentence streaming, playback, barge-in)
  listener   VAD + keyword spotter + STT state machine (wake / listen / finish / cancel)
  engine     the --serve process: commands, threads, capture supervision, shutdown
  selftest   --selftest (TTS -> VAD/wake/STT round trip, no audio devices)
  calibrate  --calibrate (record the mic, run every STT model, recommend one)
  cli        command-line entry point
"""

VERSION = "2.0.0"

# Audio formats on the pipes to/from the capture and playback tools.
MIC_RATE = 16000          # s16le mono from pw-record / parec / arecord
FRAME = 512               # samples per processing frame (32 ms, Silero VAD window)
