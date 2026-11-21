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
  text on this computer, and that text is sent to the AI models like a typed
  message (NVIDIA's hosted models by default; Claude with an `sk-ant-` key;
  your local model if `[api] local=true`).
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

For a Shadow OS system whose assistant does not hear "Shadow", or that still
runs the old (pocketsphinx) assistant. Run it as your normal user from a
terminal inside the Shadow desktop, from your checkout of the sources; it
asks for your password (sudo) only to install. You speak twice: a test
sentence, then "Shadow, what time is it?" to the restarted assistant. It ends
with a summary and one verdict:

| Verdict | Meaning | Exit |
|---|---|---|
| **PASS** | every link was tested: your voice reached the engine and was understood, "Shadow" woke it, the API key reaches the assistant in the background and at login, and the restarted assistant answered aloud | 0 |
| **FAIL** | something does not work; each FAIL line says what to do next | 1 |
| **NOT VERIFIED** | nothing failed, but a test was skipped (`s` at a prompt, `--no-calibrate`), or a switch of yours keeps the assistant from listening (its microphone mute, the wake word off) | 3 |

A run that cannot continue (no build tools, a failed build or install) stops
with one FAIL naming the cause; it never tests the old assistant instead.

| Step | What it does |
|---|---|
| 0 | checks that it runs as you (not root), inside the graphical session, on x86-64, from a checkout with the new engine; notes whether a systemd user manager runs |
| 1 | builds the assistant. It uses `--build-dir`, else the configured build directory of the checkout (`build/cmake` first; a tree whose configuration never finished is ignored). **When there is none, it creates `build/cmake`** (`cmake -G Ninja -DSHADOW_ONLY=assistant -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX=/usr/local`; `build/cmake` is git-ignored), after installing the build tools with apt if they are missing (`cmake ninja-build g++ qt6-declarative-dev qt6-svg-dev`: on the 0.2.0 image 67 packages, 57 MB to download, 261 MB on disk, measured against the Debian archive on 2026-11-21; asked first, done with `--yes`). Build files owned by root (an earlier `sudo` build) are given back to you first (asked). Then `cmake --build … --target shadow-assistant` and, only if a staged install differs from the installed files, `sudo cmake --install … --component shadow-assistant`. The running assistant is stopped only right before that install. The new binary must report its voice engine (`--dump voice`) |
| 2 | installs what is missing of `python3 python3-numpy espeak-ng pipewire-bin pipewire-pulse wireplumber pulseaudio-utils alsa-ucm-conf curl bzip2` (and `firmware-sof-signed` on an SOF laptop) without recommends; removes nothing |
| 3 | installs the engine data into `/var/lib/shadow-voice` with the installed `shadow-voice-setup` (both speech models; only damaged or missing parts are downloaded again), or verifies it |
| 4 | repairs `~/.config/shadow/assistant.conf` after a backup: removes comments after values (Qt read `muted=false  # …` as muted) and turns `#` lines into `;` comments; shows every changed line. **Lines with your API key, a token, a password or a URL with a password are never changed, shown or logged**; the file is made private (mode 600); a symlinked file stays a symlink. Your own switches in the assistant are asked about: its microphone mute (`muted=true`) and a wake word you switched off there; Enter answers yes (the voice needs both on), `--yes` leaves them as the assistant reads them and writes nothing. A `wake_word_enabled=false` without `wake_word_chosen` was written by the old assistant without asking: the new one ignores it and listens, so the script writes nothing either (it only offers to switch the wake word off, default no) |
| 5 | sound checks: PipeWire; the default input is a real microphone (not a "monitor"), not muted (offers to unmute), not nearly silent (offers 80 % below 30 %); the default output is a real device (not "Dummy Output") and not muted (offers to unmute) |
| 6 | the engine's self-test; a spoken greeting (you must answer "Did you hear it?" with y or n; with `--yes` its sound is measured on the output); the microphone test: you say *"Shadow, the quick brown fox jumps over the lazy dog."* (no instruction, should any assistant listen). Every copy of the assistant is stopped right before. PASS needs speech in the recording, at most 34 % of the command words wrong, and "Shadow" taking the engine's real wake-word path (keyword spotter + transcript); a very quiet input is offered 100 % and a second try; if "Shadow" only works with `wake_sensitivity=high` that is offered. The better speech model is saved as `stt_model` |
| 7 | checks that the API key reaches an assistant started by your user manager (as at login): a key only exported in your shell (`SHADOW_ASSISTANT_API_KEY`, `NVIDIA_API_KEY` or `ANTHROPIC_API_KEY` in `~/.profile`) is offered to be saved in `assistant.conf` (never without asking; otherwise it is passed by name for this run and the login check fails). With no key anywhere it offers to take one now: you paste it (it is not shown) and `shadow-assistant --set-api-key` saves it as the Settings page does, in a file only you can read. Starts the new assistant in the background, waits until its engine records, then you say **"Shadow, what time is it?"**: PASS only when the engine heard it and the assistant's spoken answer started (a failed request to the AI models, which it answers with "Sorry, that didn't work", is a FAIL). Finally the wake word at login: `~/.config/autostart/org.shadow.assistant-listen.desktop` starting the new binary; PASS only when the unit your user manager makes of it (`app-org.shadow.assistant\x2dlisten@autostart.service`) starts the new binary with `--background`, otherwise WARN |

If the script is interrupted (Ctrl+C, a closed terminal) or cannot start the
new assistant, an assistant it stopped is started again. The summary says
how to go back to the packaged assistant: the copy in `/usr/local` takes
precedence over the package, also after package updates
(`sudo xargs -d '\n' rm -f < ~/.local/state/shadow-fix-assistant-voice/install_manifest_shadow-assistant.txt`
and removing the autostart file).

Options: `--yes` (no questions: you speak after a countdown; the safe
defaults are taken: install what is missing, unmute and raise the system
microphone, unmute the speakers, use a real microphone instead of a monitor;
your switches in the assistant and your API key are left alone; without `--yes`, Enter answers every question with what makes the voice work, except "Did you hear it?"),
`--no-build` (use the installed assistant; it must have the new engine),
`--no-calibrate` (no tests where you speak: NOT VERIFIED), `--build-dir DIR`.

Its log (with what the speech models heard you say), the build output, the
engine set-up log and the settings backup are in
`~/.local/state/shadow-fix-assistant-voice/` (mode 700). No audio is kept.
Running it again is safe: it rebuilds and reinstalls only what changed, repeats
the tests and restarts the assistant.

## Settings

`[voice]` in `~/.config/shadow/assistant.conf` (the Settings page writes
them; edit by hand only with comments on lines of their own, starting with
`;`):

| Key | Values (default first) | Meaning |
|---|---|---|
| `wake_word_enabled` | `true`, `false` | listen for "Shadow"; counts only once you switched it yourself in the assistant (`wake_word_chosen=true`): a `false` the old assistant wrote by itself is ignored |
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
| It answers "Add your API key in Settings first" | the voice works; the assistant needs an API key to answer: open it, Settings → Models, paste the key, Save |
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

None of it on real audio hardware:

* the engine: 66 automated tests (protocol, wake-word policy, text for
  speech, the set-up tool, including: a full cache needs no download tool, a
  repair downloads only the damaged component, a failing self-test of new
  files replaces nothing, other architectures are refused, and the
  microphone test's wake-word check on a misheard "Shadow") against a real
  installation in the Debian trixie build chroot;
* the application side: 34 tests of the real `shadow-assistant` against a
  stand-in engine, and whole sessions with the real engine on recorded speech;
* `scripts/fix-assistant-voice.sh`, run exactly as a user runs it (`bash
  scripts/fix-assistant-voice.sh` in `~/shadow-os-build`, answered through a
  terminal) on the **root filesystem of the Shadow OS 0.2.0 image** with the
  old 0.2.0 assistant package, a desktop user with sudo, a `systemd --user`
  session with PipeWire and WirePlumber as user services, KWin Wayland, the
  session's environment import and its autostart target, a virtual
  microphone fed with synthetic speech in real time and a null-sink speaker,
  and a stand-in for the Claude API (the network cut). 27 scenarios, among
  them: a fresh checkout without a build directory and without build tools
  with the old assistant running (build tools installed, `build/cmake`
  created and built, PASS; after a simulated next login the autostarted
  assistant answered too); a second run; the old settings template (key line
  untouched, mode 600); every question answered; no microphone; a microphone
  that hears no speech, one that is too quiet, an empty headset jack chosen
  as input; a voice whose "Shadow" is misheard; a skipped test; no API key, a
  key only in the shell (saved when allowed, otherwise a FAIL for the login);
  the privacy mute on; muted speakers, no speakers; an assistant opened
  during the test; Ctrl+C; passwords in URLs; a symlinked settings file; an
  older copy first on `PATH`; root-owned build files; a wrong `--build-dir`;
  no systemd user manager;
* the image step (`build/iso/voice-data.sh`) against a scratch image root,
  with and without network, and `SHADOW_IMAGE_VOICE=required`.

Not verified: a real microphone and speakers, a laptop's SOF digital
microphones and the WirePlumber restart after `alsa-ucm-conf` on real
hardware, the real Claude API (the round trip used a stand-in), the time a
real reply takes, whether KWin raises the window when the wake word is heard,
a complete ISO build with the engine data, arm64 (not supported).
