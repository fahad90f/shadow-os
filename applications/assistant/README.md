# SHADOW Assistant (`shadow-assistant`)

**Shadow** — a hands-free, voice-driven personal AI agent for Shadow OS. Say
"**Shadow**" (or press the mic) to wake it, speak a command, and it understands
the speech, carries out the task by calling tools, and speaks the result back.
It also remains a full streaming text chat. By default it runs what you ask
without stopping for approval (autonomy `autopilot`). Settings → Autonomy can
turn confirmations back on: a clickable Run/Deny card **and**, hands-free, a
spoken "shall I run it? say yes or no".

- **Application id:** `org.shadow.assistant`
- **Binary:** `shadow-assistant`
- **Backend:** Anthropic **Claude** models via the native Messages API at
  `https://api.anthropic.com/v1` (`/messages`, SSE streaming, QtNetwork). The
  defaults use the best current models — **Claude Opus 5.5** for chat, planning
  and vision, **Claude Haiku 5.5** for fast background helpers. An
  OpenAI-compatible local server (ollama) is still supported for fully offline
  use (`[api] local=true`).

## Configuration

The Claude (Anthropic) API key is **never** stored in the application or this
repository. It is read at run time from, in order of precedence:

1. the environment variable `SHADOW_ASSISTANT_API_KEY`, or
2. the environment variable `ANTHROPIC_API_KEY`, or
3. `~/.config/shadow/assistant.conf` (INI; create it `chmod 600`).

Get a key at <https://console.anthropic.com/> (keys start with `sk-ant-`).

```ini
[api]
key=sk-ant-XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
# base_url=https://api.anthropic.com/v1   # usually leave unset
# anthropic_version=2023-06-01            # Messages API version header

[models]
brain=claude-opus-5-5      # chat, tools, vision — the default
planner=claude-opus-5-5    # "Think hard" deep reasoning
fast=claude-haiku-5-5      # quick background helpers
omni=claude-opus-5-5       # voice & image

[behaviour]
autonomy=autopilot        # autopilot (default) | ask_risky | ask_every

[tools]
disabled=                 # comma-separated tool names to turn off

[voice]
wake_word_enabled=true    # listen for the wake phrase (on; the toggle turns it off)
wake_phrase=shadow        # word that wakes the assistant
wake_threshold=1e-20      # keyphrase-spotting threshold, used only with pocketsphinx 5
muted=false               # privacy mic mute — nothing is captured when true
speak_replies=true        # speak answers aloud
tts_engine=auto           # auto | piper | speechd | espeak
tts_voice=                # speechd/espeak voice name (e.g. en-us)
piper_bin=piper           # piper executable (neural TTS, if installed)
piper_model=              # path to a piper .onnx voice model (required for piper)
stt_mode=auto             # voice is transcribed offline (Claude has no speech-to-text)
capture_tool=auto         # auto | parecord | pw-record | arecord
input_device=             # optional capture device (PulseAudio/PipeWire source)
max_utterance_secs=12     # give up waiting for a command after this long
pocketsphinx_model=       # optional path to an acoustic model directory
```

If no key is configured the app shows a setup state (it never crashes). The
endpoint can be overridden for development with `SHADOW_ASSISTANT_BASE_URL`.

## Capabilities

1. **Chat** — streaming SSE responses, Markdown, per-conversation history under
   `~/.local/share/shadow/assistant/<id>.json`.
2. **Voice (hands-free "Shadow")** — wake word → listen → transcribe (shown as
   the user message) → model understands and calls tools (with autonomy
   `ask_risky`/`ask_every`, actions are first spoken and confirmed by voice
   and by card) → a concise result is spoken.
   A mic button gives push-to-talk; the wake toggle and a privacy mute live in
   the sidebar and Settings. The orb shows idle → wake → listening → thinking →
   speaking. Say it in one breath ("Shadow, open the terminal") or pause after
   "Shadow" and then give the command; the command ends when you stop talking.
   The listener transcribes each phrase **on this computer** with pocketsphinx
   and drops every phrase without the wake word at once: **nothing is recorded,
   stored or sent until after the wake word, and nothing runs while muted.**
   The wake word is on by default and starts at login
   (`/etc/xdg/autostart/org.shadow.assistant-listen.desktop`, which runs
   `shadow-assistant --background`: no window until you say "Shadow"); closing
   the window keeps it listening. Switch the wake word off (sidebar or
   Settings) and the assistant stops listening and quits when closed.
   `shadow-assistant --voice-listen 30` shows what the listener hears.
3. **Tool calling** — a safe framework the model invokes; every call is argv
   (never a shell) and logged; with autonomy `ask_risky`/`ask_every` a call
   that changes the system or reaches the network is confirmed by click or
   spoken yes first. Tools:
   `run_shell_command`, `read_file`, `write_file`, `search_files`,
   `open_application`, `get_system_info`, `open_url`, `web_search`,
   `make_directory`, `move_path`, `copy_path`, `delete_path`, `write_code`,
   `run_in_terminal`, `type_text`, `take_screenshot`.
   Destructive targets (filesystem root, `$HOME`, bare top-level system dirs)
   are refused at every autonomy level.
4. **Settings** — key status, model mapping, autonomy, per-tool switches, and a
   full Voice panel (privacy mute, wake toggle, speak-replies, speech-engine
   picker, detected-engine badges) plus an Activity log.

## Voice subsystem (design)

The app links **no** extra Qt/native libraries for voice; it drives the verified
runtime packages as subprocesses through `Shadow::Command` (argv, never a shell)
and `QProcess`. Each engine is detected at run time and reported honestly when
absent (an "unavailable" state with a reason — never a dead control):

| Role | Engine (runtime package) | Invocation |
|------|--------------------------|------------|
| Wake word | `pocketsphinx` + `pocketsphinx-en-us` | keyphrase spotting on the live mic; reports only a hit |
| Mic capture | `parecord` / `pw-record` / `arecord` | 16 kHz mono WAV, bounded by `max_utterance_secs` |
| Command STT | `pocketsphinx` (offline) | transcribes the captured WAV; works with no network |
| Speak back | `piper` (neural) → `spd-say` (speech-dispatcher) → `espeak-ng` | first available, piper needs a `.onnx` voice model |
| Typing | `wtype` (Wayland) → `xdotool` (X11) | `type_text` tool |

**STT is offline-only.** Claude has no speech-to-text, so spoken commands are
always transcribed **on this computer** with **pocketsphinx**; only the
resulting text is sent to the model. There is no cloud ASR path.

## Diagnostics

```
shadow-assistant --dump config    # key configured? endpoint, models, autonomy, voice (never the key)
shadow-assistant --dump models    # the four model-role IDs + base URL
shadow-assistant --dump tools     # enabled tools, changesSystem/risky flags
shadow-assistant --dump voice     # detected engines, capture tool, resolved TTS, wake config, capabilities
```

Headless engine hooks (used by `tests/voice_tests.sh`; honest JSON):

```
shadow-assistant --voice-detect-wake FILE.wav      # keyphrase spotting on a WAV
shadow-assistant --voice-transcribe  FILE.wav      # offline STT of a WAV
shadow-assistant --voice-synthesize "text" --voice-out OUT.wav   # TTS to a WAV
```

## Runtime Debian dependencies

Base: `libqt6network6` plus the Shadow UI runtime (Qt 6 Quick/QML,
`org.shadow.ui`, `org.shadow.style`), `grep`, `gtk-launch` (`libgtk-3-bin`).

Voice (install for the voice features; each degrades honestly if absent):

- `pocketsphinx` + `pocketsphinx-en-us` — wake word + offline STT
- `speech-dispatcher` + `espeak-ng` (and `python3-speechd`) — always-available TTS
- `piper` **neural TTS is *not* the Debian `piper` package** (that is a
  gaming-mouse configurator); provide the rhasspy/piper binary and a `.onnx`
  voice model via `piper_bin` / `piper_model`, or leave it unset and use
  speech-dispatcher/espeak-ng
- `wtype` (Wayland) / `xdotool` (X11) — `type_text`
- a capture tool: `pulseaudio-utils` (`parecord`) / `pipewire-bin` (`pw-record`) / `alsa-utils` (`arecord`)
- an audio player for piper playback: `pulseaudio-utils` (`paplay`) / `alsa-utils` (`aplay`) / `pipewire-bin` (`pw-play`)
- `shadow-terminal`, `shadow-editor`, `shadow-screenshot`, and `xdg-open`
  (`xdg-utils`) or `gio` (`libglib2.0-bin`) — used by the respective tools

**Qt Multimedia note.** The brief suggested `QAudioSource`/`QMediaDevices`
(`qt6-multimedia-dev` / `libqt6multimedia6`). Those build-dev packages are **not**
in the current build chroot, so capture is done via the CLI capture tools above
instead; no `Qt6::Multimedia` link is required.

## Tested

- Build: `SHADOW_JOBS=2 scripts/dev.sh build --dir assistant --only assistant` — **passes**.
- Smoke (zero QML warnings): `tests/desktop/smoke.sh --dir assistant assistant` — **passes**.
- `--dump config|models|tools|voice` verified; `--dump voice` reports the real
  engine availability of the host.
- The understand → tool → confirm loop was driven end to end against the bundled
  mock server (`tests/mock_api.py`) with `SHADOW_ASSISTANT_BASE_URL`; screenshots
  `assistant-voice-*` show the orb states (idle/listening/thinking/speaking), the
  spoken-confirmation card, and the Voice settings.
- `tests/voice_tests.sh` runs the real wake-word / STT / TTS engines on
  espeak-generated WAVs via the `--voice-*` hooks. **It skips honestly in the
  build chroot** (espeak-ng/pocketsphinx/piper are not installed there); run it
  on the built image for live results.

## Limitations / not verified

- **No audio hardware or voice packages in the build chroot**, so the live
  microphone capture, real-time wake-word latency, STT accuracy, TTS playback
  and barge-in could not be exercised here. The subprocess invocations and the
  `--voice-*` hooks are real and correct in form; they need the user's image
  (with the packages above) and a microphone to verify end to end.
- **Cloud ASR** is not available with Claude (it has no speech-to-text); the
  offline pocketsphinx path is the implemented STT.
- The live Claude Messages endpoint was never reached (no outbound network in
  the chroot); streaming/tool-calls are verified against the mock only.
- The exact pocketsphinx CLI flags target the classic `pocketsphinx_continuous`
  and the 5.x `pocketsphinx` binaries; the resolved binary is auto-detected but
  the precise flags/model layout should be confirmed on the image.

## Changes needed outside this directory

- **Runtime package dependencies** to add to `debian/control` for the voice
  features (verified present in the archive except where noted): `pocketsphinx`,
  `pocketsphinx-en-us`, `speech-dispatcher`, `espeak-ng`, `python3-speechd`,
  `wtype`, `xdotool`, plus a capture tool (`pulseaudio-utils` **or**
  `pipewire-bin` **or** `alsa-utils`) and `xdg-utils`. Piper neural TTS has no
  suitable Debian package (the `piper` package is unrelated) — ship the
  rhasspy/piper binary + a voice model separately, or rely on speech-dispatcher.
- No `Qt6::Multimedia` build-dep is required by this implementation.
- An application icon for `org.shadow.assistant` in the icon theme (the launcher
  references `Icon=org.shadow.assistant`). No `Nx*` glyph is missing.
</content>
