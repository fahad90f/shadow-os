# Shadow Assistant: voice

Say **"Shadow"** and what you want ("Shadow, what is using my disk space?",
or "Shadow", a pause, then the request) and the assistant answers aloud. The
microphone button does the same without the wake word: the first click
listens, the second sends what you said. This page explains how the voice
works, what it does with your audio, what it needs, and what to do when it
does not work. The application itself is described in
[README.md](README.md#assistant) and
[`applications/assistant/README.md`](../../applications/assistant/README.md);
the engine's protocol and internals in
[`applications/assistant/voice/README.md`](../../applications/assistant/voice/README.md).

**If the voice does not work, run the one-shot fix first:**

```sh
cd ~/shadow-os-build          # your checkout of the Shadow OS sources
git pull
bash scripts/fix-assistant-voice.sh
```

It is described [below](#the-one-shot-fix-scriptsfix-assistant-voicesh).

## How it works

```
 Shadow Assistant (window, or in the background from login)
   VoiceController ── QProcess, JSON lines ──▶ shadow-voiced (Python 3, one process)
                                                 microphone: pw-record → parec → arecord
                                                 Silero VAD: where speech starts and ends
                                                 keyword spotter "Shadow" (early hint)
                                                 Moonshine (default) or Whisper: speech → text
                                                 Kokoro voice + pronunciation front end → speaker
                                                 speaker: pw-play → pacat → aplay
```

* **The wake word is decided by the words, not by a sound.** Every utterance
  is transcribed on this computer; it is meant for the assistant only when the
  transcript **starts** with "Shadow" ("I saw my shadow" and "Shadows grow
  longer" never wake it). The keyword spotter only lets the window light up
  early and accepts near misses such as "Shadda" when it also heard the word.
  `wake_sensitivity` (low, normal, high) sets how lenient that is.
* **Speech recognition**: Moonshine base (fast, the default) or Whisper
  base.en (more accurate in the lab, about 3.5 times slower). The fix script
  lets both transcribe *your* voice once and keeps the better one
  (`stt_model`).
* **The voice**: Kokoro-82M, six curated female voices (Heart, the default;
  Bella; Sarah; Emma, British; Nicole; Sky). Replies are spoken while they
  stream in, sentence by sentence; Markdown is not read out (code blocks
  become "code omitted", links their text, tables are skipped). Numbers,
  units, versions, file paths, acronyms and product names are put into words
  first, so nothing is silently skipped.
* **Half duplex**: while it speaks (and 0.4 s after) the microphone input is
  ignored, so it never answers itself. After a spoken reply it listens 5 s
  for a follow-up without the wake word (`follow_up=false` turns that off).
* **Without the engine data** the assistant still speaks its replies with
  eSpeak NG ("basic voice") and says what is missing; there is no speech
  recognition then. If the engine process crashes, the assistant restarts it
  (after 1, 2, 4, 8, 15, 30 s) and shows "error" with the engine's last
  messages after three failures in two minutes.

## Privacy

* **Audio never leaves the computer.** Wake word, speech recognition and the
  voice run locally. The only network use of the voice is the one-time
  download of the engine data (below).
* What you say **after "Shadow"** (or into the microphone button) becomes
  text on this computer, and that text is sent to Claude like a typed message
  (or to your local model, if `[api] local=true`).
* Speech that is not addressed to the assistant is transcribed in memory and
  dropped: it is not kept, not logged and not sent.
* **Nothing is recorded.** Audio is processed in memory only; transcripts
  appear in the engine log only when it runs with `--debug`.
* **The microphone is closed while muted**: the recording process is stopped
  (your desktop's microphone indicator goes off). It is also closed while the
  wake word is off and no microphone-button request is pending.
* The fix script's microphone test keeps no audio either.

## What it needs

| | |
|---|---|
| Processor | amd64 (x86-64). The pinned engine runtime (sherpa-onnx 2.0.2) exists for x86-64 only; arm64 images do not get the engine data |
| Python | 3.12 or 3.13 (Debian trixie: 3.13), with `python3-numpy` |
| Debian packages | `python3`, `python3-numpy`, `espeak-ng`, `pipewire-bin` (or `pulseaudio-utils`, or `alsa-utils`), `curl` (or `wget`) and `bzip2` for the download — all dependencies of the `shadow-assistant` package; recommended: `pipewire-pulse`, `wireplumber`, `pulseaudio-utils`, `alsa-ucm-conf` |
| Laptop microphones | many recent laptops (Intel "SOF" audio, digital microphone arrays) show no microphone at all without `firmware-sof-signed` and `alsa-ucm-conf` (both in the Shadow OS desktop image) |
| Disk | 693 MB for the engine data with both speech models (540 MB with Moonshine only); while installing, the download (707 MB) is needed as well |
| Memory | about 850–880 MB while the engine runs (measured in the lab) |

### Installing the engine data

| Where | How | Used by |
|---|---|---|
| `/var/lib/shadow-voice` | included in Shadow OS images built with network access ([../packaging/iso.md](../packaging/iso.md#voice-engine-data)); otherwise `sudo shadow-voice-setup --system`, or the fix script | every user |
| `~/.local/share/shadow/voice` | the **Install** button in the assistant's voice settings, or `shadow-voice-setup` | you only (found first) |

`shadow-voice-setup` downloads only pinned files (the k2-fsa/sherpa-onnx
GitHub releases and files.pythonhosted.org), checks the size and sha256 of
every file, resumes an interrupted download, installs atomically and runs the
engine's self-test. `--check` verifies an installation, `--uninstall` removes
it, `--stt moonshine` installs the smaller set; `--help` lists the rest.
Licences of everything it downloads:
[`applications/assistant/voice/THIRD-PARTY.md`](../../applications/assistant/voice/THIRD-PARTY.md).

## The one-shot fix: `scripts/fix-assistant-voice.sh`

For a Shadow OS system where the assistant is built from a source checkout
(`cmake --build build && sudo cmake --install build --component
shadow-assistant`), or where the voice "does nothing". Run it as your normal
user from a terminal inside the Shadow desktop; it asks for your password
(sudo) only to install. It is safe to run again: a second run only checks.

| Step | What it does |
|---|---|
| 0 | checks that it runs as you (not root), inside the graphical session, from a checkout with the new engine |
| 1 | builds `shadow-assistant` in your configured build directory (`build/` in the checkout, or `--build-dir`) and installs it with `sudo cmake --install … --component shadow-assistant`; reports the install prefix. With the default prefix `/usr/local` the wake-word autostart entry lands in `/usr/local/etc/xdg/autostart`, which the session does not read, so a copy goes to `~/.config/autostart` |
| 2 | installs what is missing of `python3 python3-numpy espeak-ng pipewire-bin pipewire-pulse wireplumber pulseaudio-utils alsa-ucm-conf curl bzip2` (and `firmware-sof-signed` on an SOF laptop) without recommends; removes nothing |
| 3 | installs the engine data into `/var/lib/shadow-voice` with progress (both speech models), or verifies the existing installation |
| 4 | repairs `~/.config/shadow/assistant.conf` after a backup: removes comments after values (Qt read `muted=false  # …` as muted) and turns `#` lines into `;` comments; shows every changed line. **The line with your API key is never changed, shown or logged**; the file is made private (mode 600). Your own switches in the assistant (its microphone mute, the wake word) are changed only when you answer yes |
| 5 | sound checks: PipeWire with pipewire-pulse running; the default input is a real microphone (not a "monitor" of the speakers); it is not muted (offers to unmute) and not nearly silent (offers 80 % when below 30 %); the output is not muted |
| 6 | the engine's self-test; a spoken greeting ("Did you hear it?"); a microphone test: you say *"Shadow, please open the terminal and check my email."* while a level meter runs; every installed speech model transcribes it and the one that understood you best is saved as `stt_model` (Moonshine on a tie) |
| 7 | stops every running copy of the assistant and starts the new one in the background; checks that it found the engine (`shadow-assistant --dump voice`) and that its engine opened the microphone |

It ends with PASS or FAIL for every check and the next step for each FAIL.

Options: `--yes` (no questions: the spoken check is skipped, the test
sentence follows a countdown, and the safe defaults are taken: install what
is missing, unmute the system microphone and raise a very low level, use a
real microphone instead of a monitor), `--no-build`, `--no-calibrate`,
`--build-dir DIR`.

Its log, the build output, the engine set-up log and the settings backup are
in `~/.local/state/shadow-fix-assistant-voice/`.

## Settings

`[voice]` in `~/.config/shadow/assistant.conf` (the Settings page writes
them; edit by hand only with comments on lines of their own, starting with
`;`):

| Key | Values (default first) | Meaning |
|---|---|---|
| `wake_word_enabled` | `true`, `false` | listen for "Shadow" (counts once you switched it yourself) |
| `muted` | `false`, `true` | privacy mute: the microphone is never opened while true |
| `speak_replies` | `true`, `false` | speak answers to spoken requests |
| `voice` | `af_heart`, `af_bella`, `af_sarah`, `bf_emma`, `af_nicole`, `af_sky` | the voice |
| `speech_rate` | `1.0` (0.7–1.4) | speaking rate |
| `stt_model` | `auto`, `moonshine`, `whisper` | speech recognition (`auto` = Moonshine) |
| `wake_sensitivity` | `normal`, `low`, `high` | how lenient the wake word is |
| `follow_up` | `true`, `false` | after a spoken reply, listen 5 s without the wake word |
| `input_device` | empty = the default input | a PipeWire/PulseAudio source name (`pactl list short sources`) or an ALSA device (`hw:1,0`) |
| `tts_engine` | `auto`, `neural`, `espeak` | speech output |

## Troubleshooting

| Symptom | Check / fix |
|---|---|
| Anything | `bash scripts/fix-assistant-voice.sh`; its log is `~/.local/state/shadow-fix-assistant-voice/last-run.log` |
| "Basic voice" / "voice engine not installed" | `sudo shadow-voice-setup --system` (or Install in the voice settings) |
| `shadow-voice-setup` says download failed | network or proxy; run it again: finished files are kept and the download resumes |
| No reaction to "Shadow" | is the wake word on and the assistant not muted (`shadow-assistant --dump voice`, `config`)? Does the microphone work (step 6 of the fix script)? Try `wake_sensitivity=high` |
| "The microphone is silent (muted, or the wrong input is selected)" | `wpctl status`: pick the right input, unmute it (`wpctl set-mute @DEFAULT_AUDIO_SOURCE@ 0`), raise its level (`wpctl set-volume @DEFAULT_AUDIO_SOURCE@ 0.8`) |
| No microphone at all on a laptop | `firmware-sof-signed` and `alsa-ucm-conf` installed? Then reboot. `sudo dmesg \| grep -i sof` shows firmware errors |
| It misunderstands you | the fix script's microphone test picks the better speech model for your voice; speak closer to the microphone; reduce background noise |
| Words are skipped when it speaks | `sudo apt install espeak-ng` |
| It answers "Add your Claude API key in Settings first" | the voice works; the assistant needs an API key to answer |
| Engine errors | `~/.local/state/shadow/voice.log` (rotated at 1 MB); `shadow-voiced --selftest` (at `/usr/lib/shadow-assistant/voice/` or `/usr/local/lib/shadow-assistant/voice/`); `shadow-voice-setup --system --check` |

## Performance

Measured in the build lab, not on a laptop: a shared 4-vCPU Xeon (AVX-512),
Python 3.13, sherpa-onnx 2.0.2. A 4-core laptop without AVX-512 may be
slower.

| What | Result |
|---|---|
| engine start (models loaded) | 3.5–4.6 s |
| idle, listening for "Shadow" | 5–6 % of one core |
| memory | 850–880 MB |
| first audio of a reply | about 0.5 s |
| end of speech → transcript | 0.75–1.3 s |
| speech recognition per utterance | Moonshine 55–90 ms, Whisper about 310 ms |
| word error rate on 489 synthetic lab utterances | Moonshine 0.148, Whisper 0.098 |

## What has been verified

All in the Debian trixie build chroot, none of it on real audio hardware:

* the engine: 63 automated tests (protocol, wake-word policy, text for
  speech, set-up tool) against a real installation, and a live run through a
  private PipeWire session with a virtual microphone and a null-sink speaker;
* the application side: 34 tests of the real `shadow-assistant` against a
  stand-in engine, and whole sessions with the real engine on recorded speech;
* `scripts/fix-assistant-voice.sh`: complete runs as a desktop user with sudo
  in a private PipeWire session (virtual microphone fed with speech in real
  time, null-sink speaker): a first run that installs everything, a second
  run that only checks (with the assistant already running), a corrupted
  settings file, a muted microphone at 10 %, a computer without any
  microphone, and the interactive questions; afterwards "Shadow, what time is
  it?" spoken into the virtual microphone woke the restarted assistant;
* the image step (`build/iso/voice-data.sh`) against a scratch image root,
  with and without network.

Not verified: a real microphone and speakers, a laptop's SOF digital
microphones, the Wayland desktop session (the tests used X11 under Xvfb),
whether KWin raises the window when the wake word is heard, a complete ISO
build with the engine data, arm64 (not supported).
