# shadow-voiced — the offline voice engine of Shadow Assistant

Everything the assistant hears and says is processed locally. The engine is a small,
long-running Python 3 helper that the app (VoiceController, C++) starts with QProcess and
talks to in newline-delimited JSON. Nothing leaves the machine; network is only used by
`shadow-voice-setup` to download the pinned model files once.

```
 QML UI ──binds──▶ VoiceController (C++)
                        │ QProcess: JSON lines on stdin / stdout, diagnostics on stderr
                        ▼
              shadow-voiced --serve
                mic  : pw-record → parec → arecord      16 kHz s16le mono, raw pipe
                VAD  : Silero (+0.3 s pre-roll)          utterance boundaries
                wake : KWS zipformer "▁SHA D OW" (early hit) + transcript check (decision)
                STT  : Moonshine base v2 (default) | Whisper base.en int8
                TTS  : Kokoro-82M fp32 + g2p front end (lexicon, morphology, espeak-ng)
                out  : pw-play → pacat → aplay          24 kHz s16le mono, raw pipe
```

## Files

| Path | What |
|---|---|
| `shadow-voiced` | entry point (`#!/usr/bin/python3 -IB`: isolated, and no byte code, so a run as root leaves no `__pycache__` the user cannot delete), installed to `<prefix>/lib/shadow-assistant/voice/` |
| `shadow_voice/datadir.py` | data-root lookup, manifest, `--probe` (imports no model) |
| `shadow_voice/audio.py` | capture/playback tools, device mapping, WAV I/O, level meters |
| `shadow_voice/listener.py` | VAD + keyword spotter + STT state machine (wake / listen / finish / cancel) |
| `shadow_voice/wake.py` | wake-word policy, transcript clean-up, yes/no, word error rate |
| `shadow_voice/textnorm.py`, `g2p.py` | text → speakable words → Kokoro phoneme tokens |
| `shadow_voice/tts.py` | Kokoro engine and the speaker (sentence streaming, playback, barge-in) |
| `shadow_voice/engine.py` | `--serve`: commands, threads, capture supervision, shutdown |
| `shadow_voice/selftest.py`, `calibrate.py`, `cli.py` | the other modes |
| `shadow_voice/logsetup.py`, `logtee.py`, `protocol.py` | logging, JSON event output |
| `shadow-voice-setup` | bash installer for the engine data (installed to `<prefix>/bin/`) |
| `tests/` | `run-tests.sh` + unittest suites (wake policy, text normalisation/g2p, protocol) |
| `THIRD-PARTY.md` | licence of every downloaded artifact |

## Modes

```
shadow-voiced --serve [--data DIR] [--input-wav WAV [--fast]] [--output-dir DIR]
shadow-voiced --probe                     one JSON line, < 1 s, exit 0 usable / 3 no data / 4 Python deps / 5 no capture tool
shadow-voiced --selftest                  TTS → VAD/KWS/wake/STT round trip without audio devices, one JSON line
shadow-voiced --transcribe WAV [--stt moonshine|whisper]
shadow-voiced --say TEXT --out WAV [--voice af_heart] [--speed 1.0]
shadow-voiced --calibrate SECONDS --expect TEXT [--input-wav WAV] [--input-device DEV] [--wake-sensitivity S]
```

`--calibrate` records the microphone (one JSON line): level statistics (`mic`: peak, RMS,
noise floor, clipping, digital silence), seconds of speech, and per installed STT model what
it heard and its word error rate on the **command words** (TEXT without its leading wake
word, `command`). `wake` is the wake-word check: the whole recording goes through the real
listener (Silero VAD + keyword spotter + wake policy + an STT model) at `--wake-sensitivity`,
as in use; `wake.ok` is true only when that produced `final{id:"wake"}`. If it did not, the
same audio is tried at `high`: `wake.needs_high` says that only that setting works for this
voice. `recommended` is the model with the lowest command WER (Moonshine on a tie), unless a
model at most 12 points worse is the only one that hears "Shadow". Exit 0 recommended, 1 no
speech, 2 no microphone.

Useful options: `--voice`, `--speed`, `--stt auto|moonshine|whisper`, `--wake-sensitivity
low|normal|high`, `--no-wake`, `--muted`, `--input-device`, `--output-device`, `--capture`,
`--playback` (force a tool), `--tts-threads` (default min(4, physical cores)),
`--stt-threads` (default 2), `--debug` (log transcripts), `--no-log-file`.
Test-only: `--input-wav` (a WAV replaces the microphone, paced in real time unless
`--fast`), `--output-dir` (speech is written to `DIR/speak-<id>.wav`), `--capture-command
SH` (a shell command that writes 16 kHz s16le mono replaces the capture tool).

## Protocol (newline-delimited JSON, UTF-8)

Every event has `"event"` and `"t"` (seconds since the helper started). The first line is
always `hello{version,pid}` (before the models load, ~3 s); `ready` follows. Commands sent
before `ready` are queued and run in order once the models are loaded (`ping` and `quit`
are answered at once).

Commands (stdin):

| cmd | fields | notes |
|---|---|---|
| `config` | `voice`, `speed` (0.7–1.4, clamped), `wake`, `wake_sensitivity` (low/normal/high), `stt` (auto/moonshine/whisper), `input_device` ("" = default), `muted`, `follow_up_secs` | reply `config_ok{applied, rejected?{key: why}, ignored?[keys]}`; each rejected key also gets a non-fatal `error` |
| `listen` | `id`, `mode` (command/yesno/followup), `timeout` (s until speech must start; default 8, followup = `follow_up_secs` or 5) | next utterance without the wake word. **Sent while the helper speaks, it starts when the speech and its 0.4 s tail are over** (send `stop` first to barge in). While muted: `error` + `timeout{id, reason:"muted"}` |
| `finish` | — | transcribe what was said so far → `final`, or `timeout` when nothing was said |
| `cancel` | — | drop the listen (and an early wake) — no transcript, no `timeout` |
| `speak` | `id`, `text`, `more` | see "Speaking" below |
| `stop` | — | barge-in: `speak_end{interrupted:true}` for every accepted id |
| `preview` | `voice`, `text`? | interrupts current speech, speaks a sample with id `preview`, config unchanged |
| `mute` | `value` | true: capture process stopped (mic indicator off), listens cancelled |
| `ping` / `quit` | — | `pong` / exit 0 |

Events (stdout):

| event | fields |
|---|---|
| `hello` | `version`, `pid` |
| `ready` | `load_s`, `stt`, `stt_models`, `tts`, `voice`, `voices` (English Kokoro ids), `kws`, `capture`, `playback`, `data`, `version`, `sherpa_onnx` |
| `state` | `value`: idle / listening / transcribing / speaking / muted |
| `level` | `source` mic/tts, `rms` 0..1 (perceptual: −60 dBFS → 0, −12 dBFS → 1), ~30 Hz; mic only while capture runs and the helper is not speaking |
| `vad` | `speech` |
| `wake` | `via`: `kws` (early acoustic hit, UI may light up) / `stt` (confirmed by the transcript; always sent for an addressed utterance, also after a `kws` one) |
| `partial` | `text` (wake word stripped) while the user addresses Shadow |
| `final` | `text` (wake word stripped), `heard`, `stt_ms`, `id` ("wake" or the listen id), `mode`, `seconds`, `stt`; `wake_match` strict/fuzzy when it came through the wake word; `answer` yes/no/null in yesno mode |
| `wake_rejected` | `heard` — the KWS fired but the transcript did not start with the wake word |
| `timeout` | `id` (+ `reason` muted / no-input when the listen could not even start) |
| `speak_start` / `first_audio` / `sentence` / `speak_end` | `id` / `id, latency_ms` / `id, text` / `id, interrupted` |
| `mic` | `status` ok/error/stopped, `tool`, `message` |
| `error` | `message`, `fatal` |
| `config_ok`, `pong` | |
| `eof` | test mode only: the `--input-wav` file has been played completely (followed by `mic{status:"stopped",tool:"input-wav"}`; the helper keeps running until `quit` / stdin EOF) |

### Listening and the wake word

* Idle: the keyword spotter runs on every 32 ms frame; Silero VAD cuts utterances and every
  utterance is transcribed locally. It is addressed to Shadow only when the transcript
  **starts** with the wake word: "Shadow, open the terminal" → `wake{stt}` + `final{id:"wake"}`;
  a bare "Shadow." → `wake{stt}`, state `listening`, and the next utterance within 8 s is the
  command (`final{id:"wake"}`) or `timeout{id:"wake"}`. Anything else is dropped unheard and
  never changes the visible state. "I saw my shadow", "Shadows grow longer" never wake.
* Near-miss spellings ("Shadda", "Chadow", "Pay shadow") are accepted only when the keyword
  spotter also fired. `wake_sensitivity`: **low** KWS threshold 0.30 and only a short curated
  near-miss list; **normal** 0.20 and one-edit near misses, also after a misheard "hey";
  **high** 0.12, and curated near misses even without a KWS hit.
* `wake` only from idle: during a listen "Shadow, …" is simply that listen's utterance.
* Whisper's non-speech labels ("(wind blowing)", "[ Sound Effects ]") are removed; an empty
  transcript is ignored on the wake path; in a listen the helper keeps listening until the
  timeout (a cough does not end a push-to-talk), `finish` ends it at once.
* Half duplex: frames captured while the helper speaks and 0.4 s after are discarded and the
  detectors reset — the assistant never answers its own voice.

### Speaking

* `speak{id,text,more}` chunks are split into sentences; long sentences are cut at commas or
  before conjunctions. The first chunk of a reply is short (≤ ~10 words) for a fast first
  audio, later ones grow (synthesised while earlier ones play) so there are no gaps.
* `more:true` keeps the id open; while no text is left the player is closed and `state`
  falls back to idle/muted (the reply waits for the assistant, it is not talking).
  `speak_end` comes after the audio of the `more:false` chunk has really played. A chunk with
  a new id while the previous id is open ends the previous one first.
* Every accepted id gets exactly one `speak_start` and one `speak_end` — also when nothing is
  speakable (emoji only) or playback is impossible (`error` + `speak_end{interrupted:true}`).
* After `stop`, late chunks of an interrupted, still-open id are dropped until its
  `more:false` chunk (the user cut it off).
* Text is normalised for speech: numbers, units, currency, times, ISO dates, versions
  ("Python 3.13" → "three point thirteen"), file paths and URLs, CamelCase ("PipeWire"),
  acronyms ("USB" spelled, "RAM" said), emoji/odd Unicode removed, Markdown leftovers stripped.

### Microphone health

* `input_device` per tool: `pw-record --target NODE`, `parec --device=SOURCE`, `arecord -D PCM`.
  An ALSA name (`hw:1,0`, `plughw:…`, `default`) makes arecord the first choice. pw-record and
  parec silently use the default input when the node/source does not exist, so the helper
  checks first (`pw-dump` / `pactl list short sources`) and says so in the `mic` ok message
  ("input device 'X' was not found; pw-record uses the default input").
* The capture tool runs only while needed: not muted, and the wake word is on or a listen is
  active (it is closed 2 s after the last reason went away).
* Capture died / never delivered audio / stalled for 3 s → `mic{status:"error", message:
  "Microphone capture failed (… exited with code 1: <its stderr>). Retrying in N s."}` with
  backoff 1, 2, 4, 8, 15 s; a tool that never delivered audio is replaced by the next one.
* 10 s of digital silence (AC level below −84 dBFS) → one `mic{status:"error", message:"The
  microphone is silent (muted, or the wrong input is selected)"}`; when sound returns →
  `mic{status:"ok", message:"Microphone sound is back"}`.

### Lifecycle

stdin EOF (the app died), `quit`, SIGTERM, SIGHUP → orderly shutdown, exit 0; SIGINT → 130.
Capture and playback children are killed first; they also carry a parent-death signal
(`setpriv --pdeathsig`), so even a SIGKILLed helper leaves no recorder running. A fatal
problem is always reported as `error{fatal:true}` before the process exits non-zero (model
load failure: exit 2; missing data/deps: the `--probe` codes). An unexpected exception in
any thread emits `error{fatal:true}` before its traceback is logged, then the helper exits
(the app restarts it with backoff).

## Data

`shadow-voice-setup` installs 693 MB with both STT models (540 MB with Moonshine only; +60 MB when the numpy wheel is needed), downloading ~707 MB (~500 MB with Moonshine only):

```
manifest.json      {"format":1,"layout":2,"installed":{component:{version,sha256,artifacts,files}},"stt_default":..}
runtime/           sherpa_onnx/ (Python modules for cp312 AND cp313 side by side), sherpa_onnx.libs/,
                   numpy/ + numpy.libs/ only when Debian's python3-numpy is not importable
models/vad/silero_vad.onnx
models/kws/{encoder,decoder,joiner}.int8.onnx, tokens.txt, keywords.txt
models/stt/moonshine/{encoder_model.ort,decoder_model_merged.ort,tokens.txt}
models/stt/whisper/{encoder.int8.onnx,decoder.int8.onnx,tokens.txt}        (optional)
models/tts/kokoro/{model.onnx,voices.bin,tokens.txt,lexicon-us-en.txt,lexicon-gb-en.txt}
licenses/          upstream licence texts
```

Lookup: `--data DIR` > `$SHADOW_VOICE_DATA` > `${XDG_DATA_HOME:-~/.local/share}/shadow/voice`
> `/var/lib/shadow-voice` > `/usr/share/shadow-voice`; the first root whose manifest lists
vad + an STT model + Kokoro + the runtime (all files present with their recorded sizes)
wins. `--data` and `$SHADOW_VOICE_DATA` are authoritative: when they point at an incomplete
root the helper says so instead of silently using another installation.

### shadow-voice-setup

```
shadow-voice-setup                     per user (default when not root)
sudo shadow-voice-setup --system       /var/lib/shadow-voice (default for root)
shadow-voice-setup --dest DIR          any root, e.g. a chroot's /usr/share/shadow-voice for an ISO
  --stt moonshine|whisper|both (both)  --offline-cache DIR   --cache DIR   --keep-cache
  --numpy auto|system|wheel            --python PATH         --install-deps   --no-selftest
  --progress-json                      --check               --uninstall      --yes
```

Every artifact is pinned (URL, size, sha256; wheels with PyPI's digests); every file taken
out of an archive is pinned too. The pinned runtime is x86-64 only: on any other machine the
setup stops before downloading anything (exit 13). Before anything is downloaded, every
installed component is checked (its pinned artifacts and the sha256 of each of its files):
the intact ones are hard-linked into the new tree and neither downloaded nor unpacked again,
so a repair fetches only what is damaged or missing, and `curl`/`wget` is only required when
something must be downloaded (a full `--cache` needs neither). Downloads resume (`curl -C -`
/ `wget -c`); the new tree is staged next to the destination, the engine's selftest runs
against the staged tree, and only then is it swapped in atomically (`mv --exchange`, or two
renames): a failing selftest leaves the previous installation and the downloads as they
were. `manifest.json` records the sha256 of every installed file (`--check` verifies them).
`--progress-json` prints `{"step","progress","message"}` lines (steps: check, deps, download,
verify, unpack, manifest, install, selftest, done, error{code,failed_step}) for the in-app
installer. Exit codes: 0 ok, 1 unexpected, 2 usage, 3 missing dependency, 4 download failed,
5 checksum mismatch, 6 disk space, 7 permission, 8 unpack/install, 9 the selftest of the new
files failed (nothing was replaced), 10 `--check` found problems, 11 nothing installed, 12
another setup running, 13 not an x86-64 machine, 130 interrupted. Missing Debian packages are named with the exact `sudo apt install …` command;
`--install-deps` installs them (`sudo apt-get install -y --no-install-recommends`).

## Logging

Everything the helper writes to stderr — its own log lines and native messages from
onnxruntime/sherpa-onnx — goes to the app (QProcess stderr) **and** to
`$XDG_STATE_HOME/shadow/voice.log` (default `~/.local/state/shadow/voice.log`, rotated at
1 MB, one `voice.log.1`). A tiny tee process (`python3 -I -B logtee.py`) does the copying so
the last lines before a crash are never lost. Transcripts are only logged with `--debug`.

## Troubleshooting

| Symptom | Check / fix |
|---|---|
| `--probe` exit 3 | engine data missing: `shadow-voice-setup` (or the Install button in the app's voice settings) |
| `--probe` exit 4 | `sudo apt install python3-numpy`, or re-run `shadow-voice-setup` (it falls back to the pinned numpy wheel); a Python other than 3.12/3.13 is not supported by the pinned runtime |
| `--probe` exit 5 | no recorder: `sudo apt install pipewire-bin` (or pulseaudio-utils / alsa-utils) |
| "The microphone is silent …" | the input is muted or the wrong source is selected: `wpctl status`, `pactl list short sources`; set `input_device` to a source name |
| mic errors with "Retrying in …" | the message carries the recorder's own error; check that PipeWire runs: `systemctl --user status pipewire wireplumber` |
| wake word not recognised | `shadow-voiced --calibrate 6 --expect "Shadow, open the terminal"` compares both STT models on your voice, checks the input level and runs the real wake-word check on it (`wake.ok`; `wake.needs_high`: it works with `wake_sensitivity=high`) |
| words skipped when speaking | `sudo apt install espeak-ng` (pronounces words missing from the voice lexicon) |
| anything else | `~/.local/state/shadow/voice.log`, `shadow-voiced --selftest`, `shadow-voice-setup --check` |

## Performance (measured in the lab, not on the target laptop)

Host: 4-vCPU Intel Xeon @ 2.1 GHz **with AVX-512**, shared with other jobs (load average 3–4
during the measurements); the target is a 4-core/8-thread Comet Lake laptop with AVX2 only, so
treat these as indicative. Debian trixie chroot, Python 3.13.5, sherpa-onnx 2.0.2.

| What | Result |
|---|---|
| `--probe` | 0.05 s warm (0.8 s with a cold page cache) |
| start-up (`hello` → `ready`, Kokoro load + warm-up, STT loaded in parallel) | 3.5–4.6 s |
| idle CPU (wake word armed: VAD + KWS every 32 ms, 31 Hz mic levels) | 5–6 % of one core + 0.3 % for pw-record |
| memory | RSS 846 MB after `ready`, 880 MB peak after speaking (Kokoro fp32 + Moonshine + numpy) |
| first audio of a reply (Kokoro fp32, first clause) | median 469 ms with 4 threads, 614 ms with 2 (real-time factor 0.36 / 0.52); 430 ms in the live PipeWire test |
| STT per utterance (489 lab utterances, 2 threads) | Moonshine base v2: median 89 ms, p90 148 ms, WER 0.148 · Whisper base.en: median 311 ms, p90 502 ms, WER 0.098 |
| end of speech → `final` | 0.75 s from a WAV (0.6 s of silence ends an utterance + STT); 1.1–1.3 s through PipeWire's pipe-source virtual mic |

## Tests

```
applications/assistant/voice/tests/run-tests.sh            # all (protocol tests need an installed data root)
applications/assistant/voice/tests/run-tests.sh test_wake  # one module
```

Session WAVs are synthesised at test time with the engine's own voices (+ silence and light
noise) into a temp directory; capture failures and silent microphones use fake recorders
(`--capture-command`). Nothing is written into the repository.
