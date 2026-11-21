# SHADOW Assistant (`shadow-assistant`)

**Shadow** — a hands-free, voice-driven personal AI agent for Shadow OS. Say
"**Shadow**" (or press the mic) to wake it, speak a command, and it understands
the speech, carries out the task by calling tools, and speaks the result back.
It also remains a full streaming text chat. By default it runs what you ask
without stopping for approval (autonomy `autopilot`). Settings → Autonomy can
turn confirmations back on: a clickable Run/Deny card **and**, hands-free, a
spoken "I'd like to run a command. Shall I go ahead?" answered with yes or no.

- **Application id:** `org.shadow.assistant`
- **Binary:** `shadow-assistant`
- **Backend:** Anthropic **Claude** models via the native Messages API at
  `https://api.anthropic.com/v1` (`/messages`, SSE streaming, QtNetwork). The
  default is a single model for every role — **Claude Sonnet 4.6** (1M
  context) run at **high effort** with adaptive thinking, a strong all-rounder
  for chat, planning, vision and tool use. Any role can be pointed at a
  different Claude model ID in the conf file. An OpenAI-compatible local server
  (ollama) is still supported for fully offline use (`[api] local=true`).

## Configuration

The Claude (Anthropic) API key is **never** stored in the application or this
repository. It is read at run time from, in order of precedence:

1. the environment variable `SHADOW_ASSISTANT_API_KEY`, or
2. the environment variable `ANTHROPIC_API_KEY`, or
3. `~/.config/shadow/assistant.conf` (INI; create it `chmod 600`).

Get a key at <https://console.anthropic.com/> (keys start with `sk-ant-`).

```ini
; Comments start with ';' on a line of their own — never after a value.
[api]
key=sk-ant-XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
; Endpoint (usually leave unset) and Messages API version header:
; base_url=https://api.anthropic.com/v1
; anthropic_version=2023-06-01

[models]
; brain: chat, tools, vision (the default) · planner: "Think hard"
; fast: quick background helpers · omni: voice and image
brain=claude-sonnet-4-6
planner=claude-sonnet-4-6
fast=claude-sonnet-4-6
omni=claude-sonnet-4-6

[behaviour]
; autopilot (default) | ask_risky | ask_every
autonomy=autopilot

[tools]
; Comma-separated tool names to turn off
disabled=

[voice]
; Listen for the wake word "Shadow" (on by default; the toggle turns it off)
wake_word_enabled=true
; Privacy mute: the microphone is never opened while true
muted=false
; Speak answers to spoken requests aloud
speak_replies=true
; af_heart (Heart, default) | af_bella | af_sarah | bf_emma | af_nicole | af_sky
voice=af_heart
; Speaking rate, 0.7 … 1.4
speech_rate=1.0
; Speech recognition: auto | moonshine | whisper
stt_model=auto
; Wake word sensitivity: low | normal | high
wake_sensitivity=normal
; After a spoken reply, listen 5 s for a follow-up without the wake word
follow_up=true
; Microphone (PipeWire/PulseAudio source name); empty = the default input
input_device=
; Speech output: auto | neural | espeak
tts_engine=auto
```

Qt's INI reader knows only `;` comments: a `#` line becomes a key and a
`# comment` after a value becomes part of the value. The assistant therefore
reads every value leniently — an inline `  # comment` or `  ; comment` is
ignored — so a file made from an older template (which put `#` comments after
values: `muted=false   # …` then read as *true*) works again without editing,
and the first saved setting rewrites it clean (stray `#` keys removed). Keys of the old pocketsphinx/piper voice (`wake_phrase`,
`wake_threshold`, `tts_voice`, `piper_bin`, `piper_model`, `stt_mode`,
`capture_tool`, `max_utterance_secs`, `pocketsphinx_model`) are ignored;
`tts_engine=piper|speechd` reads as `auto`.

If no key is configured the app shows a setup state (it never crashes). The
endpoint can be overridden for development with `SHADOW_ASSISTANT_BASE_URL`.

## Capabilities

1. **Chat** — streaming SSE responses, Markdown, per-conversation history under
   `~/.local/share/shadow/assistant/<id>.json`.
2. **Voice (hands-free "Shadow")** — say "Shadow, what's using my disk
   space?" (or "Shadow", pause, then the request), or press the mic: the
   speech is recognised **on this computer**, sent as the user message, and
   the reply is spoken **while it streams in**, sentence by sentence, in a
   natural female voice (Kokoro; Markdown is not read out: code blocks become
   "code omitted", links their text, tables are skipped). Afterwards it
   listens 5 s for a follow-up without the wake word. Push-to-talk: the first
   click listens, the second sends what was said. With autonomy
   `ask_risky`/`ask_every` an action is asked aloud ("I'd like to … Shall I go
   ahead?") and a spoken yes/no answers the card (no answer: the card waits).
   An error is announced briefly; Stop also stops the voice. Nothing heard
   before the wake word is kept or sent, and nothing is captured while muted.
   The wake word starts at login
   (`/etc/xdg/autostart/org.shadow.assistant-listen.desktop`, which runs
   `shadow-assistant --background`: no window until you say "Shadow"; it does
   not stay in the background when the wake word cannot be heard — voice not
   installed, no capture tool); closing the window keeps it listening. Switch the wake word off (sidebar or
   Settings) and the assistant stops listening and quits when closed.
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
   Voice panel (status, privacy mute, wake toggle, speak-replies, speech
   output Auto/Natural/eSpeak, detected capabilities) plus an Activity log.

## Voice subsystem (design)

```
 QML ──binds──▶ VoiceController (voicecontroller.cpp) ◀── wiring ──▶ Assistant
                    │ VoiceEngine (voiceengine.cpp): QProcess, JSON lines on stdin/stdout
                    ▼
             shadow-voiced (voice/, Python 3, one long-running process)
               mic → Silero VAD → wake word "Shadow" (KWS + transcript) → Moonshine/Whisper STT
               text → Kokoro TTS (female voices) → speaker
```

* **The helper** `shadow-voiced` owns the microphone and the speaker and runs
  all speech models offline; its protocol is in `voice/`. It is installed with
  the app (`<prefix>/lib/shadow-assistant/voice/`), its models (~550 MB) by
  `shadow-voice-setup` (per user: `shadow-voice-setup --user`, or **Install**
  in Settings).
* **VoiceController** (QML type, `voice.agent = agent`) presents state, engine,
  live mic/speech levels (≤ 30 Hz, smoothed), the partial transcript and
  honest capability flags, and does the voice ⇄ agent wiring in C++: a final
  transcript becomes a message (or answers a waiting card with yes/no), the
  streamed reply is cut into sentences (`speechtext.cpp`) and spoken while it
  arrives (`speak{more:true}` … `speak{more:false}`), confirmations are asked
  aloud, errors and Stop end the voice turn, a follow-up listen runs after a
  spoken reply.
* **Lookup:** helper `$SHADOW_VOICED` (exclusively, when set) →
  `<appdir>/../lib/shadow-assistant/voice/shadow-voiced` → `/usr/lib/…` →
  `/usr/local/lib/…`; setup tool `$SHADOW_VOICE_SETUP` → `<appdir>/` →
  `/usr/bin/` → `/usr/local/bin/`.
* **Honesty and fallbacks:** `shadow-voiced --probe` is asked first; when the
  helper, its models or its Python modules are missing the engine is
  **basic** (replies spoken with `espeak-ng -v en-us+f3`, no listening) or
  **missing**, and `engineMessage` names what is missing and the fix. A
  helper that dies is restarted after 1, 2, 4, 8, 15, 30 s; after 3 failures
  within 2 minutes the engine is **error** with its last stderr lines (refresh
  restarts it). A microphone failure is reported ("Microphone not available:
  …"). Nothing blocks the GUI thread; every listen has a watchdog.
* **Privacy:** the mute (and the wake setting) reach the helper before it
  opens the microphone; muting cancels a listen at once.

## Diagnostics

```
shadow-assistant --dump config    # key configured? endpoint, models, autonomy, voice (never the key)
shadow-assistant --dump models    # the four model-role IDs + base URL
shadow-assistant --dump tools     # enabled tools, changesSystem/risky flags
shadow-assistant --dump voice     # helper path + its --probe answer, eSpeak NG, setup tool, voice config;
                                  # probe only: the engine is not started, the microphone not opened
```

Headless voice entry points (no window; never hand over to, or block, a
running assistant):

```
shadow-assistant --voice-selftest            # the helper's own TTS → STT round trip, its JSON
shadow-assistant --voice-session INPUT.wav [--voice-out DIR] [--voice-timeout S]
    # the whole app-side loop: the helper hears INPUT.wav instead of the
    # microphone and writes its speech to DIR; a real Assistant answers (point
    # SHADOW_ASSISTANT_BASE_URL at tests/mock_api.py); one JSON event per line:
    # wake, final, sent, speak (every chunk), speak_start/_end, reply,
    # follow_up, confirm, yesno, error, done …
    # --voice-actions "0.3:start;1.2:finish" drives push-to-talk etc. (tests)
shadow-assistant --voice-speech-text FILE.md # how a Markdown reply is spoken (JSON)
```

Test hooks for screenshots: `SHADOW_ASSISTANT_TEST_VIEW`, `_TEST_PROMPT`
(sent as a voice turn), `_TEST_ORB`, `_TEST_PARTIAL` (a fake live
transcript), `_TEST_LEVEL` (a fake mic/speech level 0..1);
`SHADOW_VOICE_DEBUG=1` copies the helper's stderr to the app's.

## Runtime Debian dependencies

Base: `libqt6network6` plus the Shadow UI runtime (Qt 6 Quick/QML,
`org.shadow.ui`, `org.shadow.style`), `grep`, `gtk-launch` (`libgtk-3-bin`).

Voice (each part degrades honestly when absent):

- `python3` — runs the `shadow-voiced` helper (models and runtime wheels are
  fetched by `shadow-voice-setup`, sha256-verified)
- capture/playback: `pipewire-bin` (`pw-record`/`pw-play`), or
  `pulseaudio-utils` (`parec`/`pacat`), or `alsa-utils` (`arecord`/`aplay`)
- `espeak-ng` — pronunciation of words missing from the voice's lexicon, and
  the "basic voice" when the natural voice is not installed
- `wtype` (Wayland) / `xdotool` (X11) — `type_text`
- `shadow-terminal`, `shadow-editor`, `shadow-screenshot`, and `xdg-open`
  (`xdg-utils`) or `gio` (`libglib2.0-bin`) — used by the respective tools

pocketsphinx, speech-dispatcher and piper are no longer used.

## Tested

- Build: `SHADOW_JOBS=2 scripts/dev.sh build --dir assistant --only assistant`.
- Smoke (zero QML warnings): `tests/desktop/smoke.sh --dir assistant assistant` —
  runs the UI and `tests/voice_tests.py --smoke` (see `smoke-tests.list`).
- `tests/voice_tests.py [--bin build/bin/shadow-assistant]` — headless voice
  suite against the real binary with `tests/fake_voiced.py` (a deterministic
  stand-in for the helper: no models, no audio) and `tests/mock_api.py`:
  wake → reply spoken sentence by sentence → follow-up; push-to-talk finish,
  toggle, cancel; spoken yes/no (yes, no, timeout, a later "Shadow, yes");
  busy; API error; Stop; helper crash → backoff → error, crash → recovery;
  helper/models/eSpeak missing; mute; microphone failure; in-app install;
  Markdown for speech; the old corrupted conf templates; `--dump voice` is
  probe-only; `--voice-selftest`.
- The understand → tool → confirm loop is driven end to end against the
  bundled mock server with `SHADOW_ASSISTANT_BASE_URL`.

## Limitations / not verified

- Live microphone capture, wake-word latency, recognition accuracy and
  playback depend on the user's audio hardware; the app side is tested with
  the fake helper, the helper itself in `voice/`.
- The live Claude Messages endpoint was never reached from the test
  environment; streaming/tool-calls are verified against the mock only.

## Changes needed outside this directory

- `debian/control` (shadow-assistant): drop `pocketsphinx`, `pocketsphinx-en-us`,
  `speech-dispatcher`, `python3-speechd`; depend on `python3`, `espeak-ng` and a
  capture/playback tool (`pipewire-bin | pulseaudio-utils | alsa-utils`); refresh
  the description (it still mentions NVIDIA models).
- An application icon for `org.shadow.assistant` in the icon theme (the launcher
  references `Icon=org.shadow.assistant`). No `Nx*` glyph is missing.
