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
- **Backend:** **NVIDIA's hosted models** (<https://build.nvidia.com/>) through
  one OpenAI-compatible endpoint, `https://integrate.api.nvidia.com/v1`
  (`/chat/completions`, SSE streaming, QtNetwork), with one key for every
  model. Several models work on each request **at the same time** and one of
  them writes the answer — see [The model team](#the-model-team). Two other
  backends remain: a Claude key (`sk-ant-…`) makes the assistant talk to
  Anthropic's Messages API with one model, as before; and an OpenAI-compatible
  local server (ollama) runs it fully offline (`[api] local=true`).

## Configuration

**The API key is set in the assistant:** Settings → Models → paste the key →
**Save** (on first start the same field is on the welcome page). Save writes
the key to `~/.config/shadow/assistant.key`, readable only by you (mode 600),
and tries it at once with one tiny request; the page then says whether NVIDIA
accepted it. The field never shows a saved key again, only its last four
characters. **Remove saved key** deletes the file. Get a key at
<https://build.nvidia.com/> (keys start with `nvapi-`).

The key is never stored in the application, its logs or this repository.
Optional other places, in order of precedence:

1. the environment variable `SHADOW_ASSISTANT_API_KEY`, or `NVIDIA_API_KEY`;
2. the key saved from Settings (above);
3. `key=` under `[api]` in `~/.config/shadow/assistant.conf` (INI; `chmod 600`);
4. the environment variable `ANTHROPIC_API_KEY`.

A key is only ever sent to its own service. `NVIDIA_API_KEY` is NVIDIA's and
`ANTHROPIC_API_KEY` Anthropic's; any other key is NVIDIA's when it starts
with `nvapi-` and Anthropic's when it starts with `sk-ant-`. The service in
use follows the key in effect; `SHADOW_ASSISTANT_PROVIDER=nvidia|anthropic`
or `provider=` under `[api]` names it instead, and a key that is plainly the
other service's is then passed over (the next one that fits is used). The
endpoints are per service too: `nvidia_base_url=` for NVIDIA, `base_url=`
for Anthropic (a key of another shape with a `base_url` of its own is the
Anthropic-protocol gateway setup it was before NVIDIA was added). From a terminal, the same as the
Settings page: `shadow-assistant --set-api-key` (reads the key from standard
input, never from the command line) and `shadow-assistant --remove-api-key`.

```ini
; Comments start with ';' on a line of their own — never after a value.
[api]
; Optional: the key is normally saved from Settings.
; key=nvapi-XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
; Endpoint (usually leave unset):
; nvidia_base_url=https://integrate.api.nvidia.com/v1

[team]
; team: several models work on a request at the same time and a lead model
; combines their work.  solo: one model answers.
mode=team
; The groups, as comma-separated model ids (unset = the built-in lists):
; coding=moonshotai/kimi-k3.1-pro, minimaxai/minimax-m3.1-flash
; agents=
; chat=
; vision=
; The models preferred as lead, best first:
; lead_coding=
; lead_agents=
; lead_chat=
; How long the lead waits for the others, in seconds:
; advisor_seconds=20
; The most models per request, the lead included (unset = the whole group):
; max_models=4

[models]
; Only with a Claude key: the Claude model per role.
; brain=claude-sonnet-4-6
; planner=claude-sonnet-4-6

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

If no key is configured the app shows the welcome page with the key field (it
never crashes). The endpoint can be overridden for development with
`SHADOW_ASSISTANT_BASE_URL`.

## The model team

With NVIDIA's models, a request is not answered by one model. It goes to a
**team**, chosen by what was asked:

| Request | Team (default) | Lead |
|---|---|---|
| **Code** — writing, fixing, explaining code | kimi-k3.1-pro, minimax-m3.1-flash, nemotron-3.5-super-vl, laguna-xs-2.1, nemotron-3-ultra, gemma-4-31b-it | kimi-k3.1-pro |
| **Actions** — doing things on this computer with tools | kimi-k3.1-pro, nemotron-3.5-bolt, minimax-m3.1-flash, nemotron-3.5-super-vl, nemotron-3.5-lightning, laguna-xs-2.1, nemotron-3-ultra, gemma-4-31b-it | nemotron-3-ultra |
| **Conversation** — questions and everyday talk | mistral-large-4, nemotron-3.5-bolt, nemotron-3.5-super-vl, step-5-preview, glm-5.3-flash, muse-glimmer-30b, laguna-xs-2.1, nemotron-3-ultra | mistral-large-4 |
| **Seeing** — a picture or the screen (`look_at_image`) | nemotron-3.5-super-vl, kimi-k3.1-pro, step-5-preview, glm-5.3-flash, … (the first four that take pictures) | — |

How a turn runs (`council.cpp`):

1. **Everyone but the lead works at once.** Each of these *advisors* gets the
   conversation and writes its own answer, code or step-by-step plan, in
   parallel. None of them can run a tool — an action that changes the
   computer must happen once, not once per model — but they are told which
   tools the lead has, so a plan can name exact steps.
2. **The lead combines.** When all advisors are done, or the wait is over
   (15 s for conversation, 20 s for actions, 45 s for code; 10 s / 20 s when
   you asked by voice), the lead gets their notes with its instructions,
   writes the reply you see, and is the only one that calls tools. Further
   rounds of the same turn (after a tool's result) are the lead's alone.
3. **Nothing depends on one model.** An advisor that fails or is too slow is
   left out. A lead that cannot be used (not offered to the key, rejects
   tools, refuses) hands the same request, notes included, to the next model
   of the group, up to four. A rejected key stops at once with one message.

**Think hard** brings in every model of every group. **Settings → Models**
shows the teams, which model leads, and a switch to turn the team off (one
model answers alone: quicker, fewer requests). While a request is worked on,
a strip above the prompt shows one dot per model and what is happening.

The list of models is not fixed in the program: NVIDIA adds and retires
models, so the assistant reads what the service offers (`GET /models`, on
start and from Settings → Models → *Read again*) and leaves out what is not
there. A model that turns out to reject tools or pictures is remembered and
not asked that way again (`~/.cache/shadow/assistant-models.json`). That is
a conclusion from the wording of an error, so it is kept for a week and for
one endpoint only, *Read again* forgets it, and it never leaves a kind of
request without a lead.

A team makes one request per model for every message (eight for a question).
If NVIDIA answers "too many requests", the next messages go to the lead and
two others for a minute and a half; `max_models=` under `[team]` limits the
team for good.

**Seeing.** `look_at_image` looks at a picture file, or at the screen as it is
now. Up to four vision models get the picture at the same time (made small
first: longest side 1280 px, JPEG, at most 180 kB) and the lead gets what each
of them saw; several pictures asked for at once are looked at one after the
other. Sending a picture is asked about first unless autonomy is `autopilot`
(the default), where it is sent without a question, like every other action.
Stop ends it: a picture not yet sent stays on the computer.

Without a window: `shadow-assistant --ask "…"` sends one request (give it
several times for several turns) and prints the team's progress and the reply
as JSON lines; `--dump team` prints the teams.

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
   installed, no capture tool); closing the window keeps it listening. Switch the wake word off (Ctrl+Shift+W or
   Settings) and the assistant stops listening and quits when closed.
3. **Tool calling** — a safe framework the model invokes; every call is argv
   (never a shell) and logged; with autonomy `ask_risky`/`ask_every` a call
   that changes the system or reaches the network is confirmed by click or
   spoken yes first. Tools:
   `run_shell_command`, `read_file`, `write_file`, `search_files`,
   `open_application`, `get_system_info`, `open_url`, `web_search`,
   `make_directory`, `move_path`, `copy_path`, `delete_path`, `write_code`,
   `run_in_terminal`, `type_text`, `take_screenshot`, `look_at_image`.
   Destructive targets (filesystem root, `$HOME`, bare top-level system dirs)
   are refused at every autonomy level.
4. **Settings** — the API key (paste, save, test, remove), the model teams,
   autonomy, per-tool switches, and a
   Voice panel (status, privacy mute, wake toggle, speak-replies, speech
   output Auto/Natural/eSpeak, detected capabilities) plus an Activity log.

## Interface ("Persona")

One window, no sidebar. The assistant is a face drawn in light
(`components/PersonaFace.qml`) that shows what she is doing: waiting for
"Shadow", listening (your words appear as you speak), thinking, speaking (the
sentence being read is highlighted in the answer), asking, or microphone off.
She stands in the middle while there is nothing to read, moves to the corner
while you read, and next to the question card when an action needs your yes
or no.

| Part | File | What it holds |
|---|---|---|
| Stage | `pages/StagePage.qml` | greeting and starter prompts; the conversation as typography (`ScriptLine`, `ProseBlocks`: streaming Markdown, code with Copy, tool lines); the question card (`AskCard`: Run / Deny, also by voice); the prompt bar |
| Conversations | `components/ConversationSheet.qml` | the list with a filter; new, rename (F2), delete with confirmation |
| Settings | `pages/BackstagePage.qml` | key status, models, autonomy, tools, the voice panel (voice with preview, rate, recogniser, wake sensitivity, follow-up, microphone, install and re-check) and the activity log |
| History & undo | `pages/HistoryPage.qml` | the file changes the assistant made, each with Undo |
| First start | `pages/AwakeningPage.qml` | shown until an API key is configured; never displays the key |

State and intents live in `components/StageModel.qml`, the design tokens in the
`Pe` singleton (`Pe.qml`). The voice ⇄ agent wiring stays in C++
(`VoiceController`): QML only presents it. Answers are laid out as plain-text
blocks (`components/markdown.js`): remote images are never fetched, and only
http, https and mailto links open.

Keyboard: Enter send · Shift+Enter new line · Ctrl+Space talk · Esc stop (or
deny the question) · Alt+Y / Alt+N run / deny · Ctrl+N new conversation ·
Ctrl+K conversations · F2 rename · Ctrl+, settings · Ctrl+H history ·
Ctrl+M microphone on/off · Ctrl+Shift+W wake word on/off · Ctrl+T think hard ·
Ctrl+L prompt.

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
shadow-assistant --dump config    # key configured? where from, service, endpoint, team, autonomy, voice (never the key)
shadow-assistant --dump team      # the model teams: members, which are offered now, who leads
shadow-assistant --dump models    # the four model-role IDs + base URL (Claude key)
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
- `tests/team_tests.py [--bin build/bin/shadow-assistant]` — the model team,
  against the real binary and `tests/mock_nvidia.py` (a stand-in for NVIDIA's
  API that records every request; `python3 tests/mock_nvidia.py --port 8910`
  runs it by hand). 45 tests: all advisors asked at the same moment and the
  lead after them with every note; code / actions / conversation go to their
  teams; a tool runs once however many models planned it; yes/no before
  acting; Think hard; solo; `max_models`; custom groups; several turns;
  retired models left out; failing, slow or rate-limited advisors; a lead
  that rejects tools, is not offered, accepts only shorter replies, or all
  leads failing; a rejected key (answered as NVIDIA's real service answers
  one) and no key; the model list unreachable; reasoning text and one-piece
  replies; `look_at_image` (four models, the picture shrunk, a missing file,
  a model that takes no pictures, "no" to sending); the key saved from
  Settings (mode 600, never printed, removed again), its precedence, which
  service a key selects, a Claude key still working; a key never used with the other service and
  the key file private whatever the umask; several tool calls or pictures in
  one reply; what an error may and may not teach about a model; Stop while
  the team works or looks, and nothing from a stopped turn starting inside
  the next; no connection at all; and closing the app in mid-request.
- The understand → tool → confirm loop is driven end to end against the
  bundled mock servers with `SHADOW_ASSISTANT_BASE_URL`.

## Limitations / not verified

- Live microphone capture, wake-word latency, recognition accuracy and
  playback depend on the user's audio hardware; the app side is tested with
  the fake helper, the helper itself in `voice/`.
- **NVIDIA's real service was not used in any test**: no key was available
  in the test environment. The model ids and the shape of the model list were
  read from the real service (the list needs no key); everything that needs a
  key — streaming, tool calls, pictures, error answers, request limits — is
  verified against `tests/mock_nvidia.py` only. Which of the listed models
  really accept tools or pictures is not known in advance; the assistant
  learns it from their answers and moves on to the next model.
- A team of eight makes eight requests per message. Whether a given NVIDIA
  account allows that rate was not measured (see `max_models`, and the
  automatic smaller team after "too many requests").
- The live Claude Messages endpoint was never reached from the test
  environment either; that path is verified against `tests/mock_api.py`.

## Changes outside this directory

None are pending. What the voice needed elsewhere is done:

- `debian/control` (shadow-assistant): pocketsphinx, speech-dispatcher and
  python3-speechd are gone; it depends on `python3`, `python3-numpy`,
  `espeak-ng`, a capture/playback tool (`pipewire-bin | pulseaudio-utils |
  alsa-utils`) and `curl | wget` + `bzip2` for the engine download; the
  description is current. `debian/shadow-assistant.prerm`/`.postrm` remove byte
  code and, on purge, the engine data.
- The icon `org.shadow.assistant` is in the icon theme
  (`assets/icons/apps/org.shadow.assistant.svg`, generated by
  `assets/src/icons/gen-app-icons.py`).
- Images include the engine data (`build/iso/voice-data.sh`, called by
  `build/iso/stage-rootfs.sh`; `SHADOW_IMAGE_VOICE=0|1|required`); CI caches
  its download and leaves it out of pull-request builds.
- `scripts/fix-assistant-voice.sh` builds and installs this assistant from a
  checkout and proves the voice end to end
  ([docs/applications/assistant-voice.md](../../docs/applications/assistant-voice.md)).
