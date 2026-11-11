# SHADOW Assistant (`shadow-assistant`)

A personal AI agent ("Jarvis") for Shadow OS. It holds a conversation, answers
questions, and carries out tasks on the computer by calling tools — every
system-changing or network action gated behind an explicit in-UI confirmation.

- **Application id:** `org.shadow.assistant`
- **Binary:** `shadow-assistant`
- **Backend:** NVIDIA Nemotron models via the OpenAI-compatible API at
  `https://integrate.api.nvidia.com/v1` (chat/completions, SSE streaming,
  QtNetwork).

## Configuration (you must create this)

The NVIDIA API key is **never** stored in the application or this repository.
It is read at run time from, in order of precedence:

1. the environment variable `SHADOW_ASSISTANT_API_KEY`, or
2. `~/.config/shadow/assistant.conf` (INI; create it `chmod 600`).

File format — `~/.config/shadow/assistant.conf`:

```ini
[api]
key=nvapi-XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
# base_url=https://integrate.api.nvidia.com/v1   # usually leave unset

[models]
# Change these to the exact model IDs available on your NVIDIA account.
brain=nvidia/nemotron-3.5-super-vl-120b-a12b
planner=nvidia/nemotron-3-ultra-550b
fast=nvidia/nemotron-3.5-lightning-30b
omni=nvidia/nemotron-3-nano-omni-30b

[behaviour]
autonomy=ask_every        # ask_every | ask_risky

[tools]
disabled=                 # comma-separated tool names to turn off
```

Model-role routing: normal chat → **brain**; the "Think hard" toggle →
**planner**; **fast** and **omni** are configured for background and
voice/vision use. The default model IDs above are the ones supplied in the
brief; they are config, not constants — correct them to match your account if
the API returns 404.

If no key is configured the app does **not** crash or block: it shows a setup
state with the exact path and a copy-able template, and a "Reload" button.

The endpoint can be overridden for development with
`SHADOW_ASSISTANT_BASE_URL` (used by the mock server below).

## Capabilities

1. **Chat** — streaming responses (SSE), Markdown rendering, per-conversation
   history persisted under `~/.local/share/shadow/assistant/<id>.json`
   (new/rename/delete, stop generation, copy).
2. **Tool calling** — a safe framework the model can call:
   `run_shell_command` (argv, never a shell), `read_file`, `write_file`,
   `search_files`, `open_application`, `get_system_info`. Every call that
   changes the system or reaches the network shows the exact command/arguments
   and requires a Run/Deny click. Every call is logged (Settings → Activity).
3. **Settings** — API key status, model mapping, per-tool enable switches,
   autonomy level (Ask every time / Ask for risky only; default Ask every time).
4. **Voice** — *scaffolded and disabled.* QtQuick 3D is unavailable in this
   build and there is no tested speech backend (e.g. `speech-dispatcher` /
   `spd-say`), so push-to-talk/TTS are shown as an explicit "Unavailable"
   state rather than a control that does nothing.

## Data sources / services

- NVIDIA OpenAI-compatible chat/completions API (network, QtNetwork, Bearer key).
- Local filesystem for conversation history and the config file.
- Tool execution uses `Shadow::Command` (argv, `LC_ALL=C.UTF-8`, no shell):
  `grep` for search, `gtk-launch` to open apps, `/proc` for system info.

## UI

Shadow design system (`org.shadow.ui`, `Theme` tokens, `Nx*` components).
A reactive "presence orb" (`components/AssistantOrb.qml`) renders the
assistant's idle/thinking/speaking states. QtQuick 3D is **not installed** in
this build, so the orb is a GPU-friendly 2.5D `Canvas` (layered radial glows,
an oscillating energy ring, orbiting particles). It respects
`Appearance.reduceMotion` (renders one calm frame) and works under software
rendering.

## Diagnostics

```
shadow-assistant --dump config    # key configured? endpoint, models, autonomy (never the key)
shadow-assistant --dump models    # the four model-role IDs + base URL
shadow-assistant --dump tools     # enabled tools, changesSystem/risky flags, autonomy
```

## Runtime Debian dependencies

- `libqt6network6` (streaming API client), plus the base Shadow UI runtime
  (Qt 6 Quick/QML, `org.shadow.ui`, `org.shadow.style`).
- `grep` (coreutils/grep) — the `search_files` tool.
- `gtk-launch` (from `libgtk-3-bin`) — the `open_application` tool; if absent
  the tool reports it honestly instead of failing silently.
- Optional / not required: `speech-dispatcher` would be needed for the (still
  disabled) voice feature.

## Tested

See `tests/` and the final report. Because the live NVIDIA API is unreachable
from the build chroot, streaming, tool-call round-trips, and the 401/404/
timeout/rate-limit error paths were tested against the bundled mock SSE server
(`tests/mock_api.py`) with `SHADOW_ASSISTANT_BASE_URL` pointing at it.

- Build: `SHADOW_JOBS=2 scripts/dev.sh build --dir assistant --only assistant`
- Smoke (zero QML warnings): `tests/desktop/smoke.sh --dir assistant assistant`
- `--dump config|models|tools` output verified.

## Limitations / not verified

- **The live NVIDIA endpoint was never reached** (no outbound network in the
  chroot). The request/stream/tool-call formats follow the OpenAI spec and are
  verified against the mock, but the exact Nemotron model IDs and any
  NVIDIA-specific response quirks are unverified against the real service.
- **Voice is not implemented**, only scaffolded behind a disabled toggle.
- `open_application` depends on `gtk-launch` being present in the session.
- No vision/image input UI yet (the omni role is wired in config only).

## Changes needed outside this directory

- An application icon for `org.shadow.assistant` (e.g. a themed
  `org.shadow.assistant` icon in `desktop/ui/` / the icon theme). The launcher
  references `Icon=org.shadow.assistant`; without the artwork the launcher
  falls back to a generic icon. No symbolic (`Nx*`) glyph is missing — the UI
  uses only existing glyphs.
