#!/usr/bin/env bash
# fix-assistant-voice.sh - make the Shadow Assistant hear "Shadow" and speak,
# on this computer, in one go.
#
#     bash scripts/fix-assistant-voice.sh [--yes] [--no-build] [--no-calibrate]
#                                         [--build-dir DIR]
#
# Run it as your normal user (not with sudo) from a terminal inside the Shadow
# desktop, from your checkout of the Shadow OS sources (it finds the checkout
# from its own path). It asks for your password (sudo) only to install: the
# assistant, Debian packages and the voice engine data. It is safe to run
# again: a second run only checks, and says PASS when everything works.
#
#   --yes           no questions: skip the spoken "did you hear it?" check, say
#                   the test sentence after a countdown, and take the safe
#                   defaults (install what is missing, unmute the system
#                   microphone and raise a very low input level, use a real
#                   microphone instead of a monitor). Choices you made in the
#                   assistant itself (its mute button, the wake word switch)
#                   are only changed when you answer yes to a question.
#   --no-build      do not build and install the assistant from this checkout
#   --no-calibrate  skip the microphone sentence test (speech recognition)
#   --build-dir DIR the CMake build directory (default: build/ in the checkout,
#                   or the one configured build directory found in it)
#
# Why the old voice failed, and what each step does about it:
#  1. The voice ran on pocketsphinx, whose Debian package has no acoustic model
#     without a "recommended" extra (Shadow installs without recommends), and
#     on speech-dispatcher, which hung ~30 s per reply. The new engine
#     (shadow-voiced: Silero VAD, a "Shadow" keyword spotter, Moonshine or
#     Whisper speech recognition, the Kokoro voice) runs offline from
#     /var/lib/shadow-voice.
#     -> Steps 1-3 build and install the assistant from this checkout (the
#        engine comes with it), install the Debian packages it runs on, and
#        download the engine data once (~700 MB, every file sha256-checked).
#  2. The settings template wrote "value  # comment" lines. Qt reads those
#     comments as part of the value: "muted=false  # ..." meant MUTED; and a
#     "# ..." line is a setting to Qt, not a comment (only ';' starts one).
#     -> Step 4 removes inline comments from every value line and turns '#'
#        lines into ';' comments (backup first). The line holding your API key
#        is never changed, printed or logged.
#  3. Laptop microphones are often a digital array behind the audio DSP (Intel
#     SOF): without firmware-sof-signed and alsa-ucm-conf there is no
#     microphone at all; a muted or near-silent input, or a "monitor" chosen as
#     the default input, looks like a deaf assistant.
#     -> Step 5 checks PipeWire, the default input, its mute switch and level.
#  4. Nothing ever proved that the voice worked.
#     -> Step 6 runs the engine's self-test, plays a greeting, and records you
#        saying one sentence (with a live level meter): every installed speech
#        model transcribes it, and the one that understands you best is saved
#        as stt_model (Moonshine on a tie: it is ~3x faster).
#  5. A running copy of the old assistant keeps the old behaviour.
#     -> Step 7 stops every running copy and starts the new one in the
#        background, so "Shadow" works without opening it first, then checks
#        that it found the engine and opened the microphone.
#
# Everything is written to ~/.local/state/shadow-fix-assistant-voice/ (this
# run's log, the build output, a backup of assistant.conf before any change).
# Nothing is ever recorded: the microphone test keeps no audio.

if [ -z "${BASH_VERSION:-}" ]; then exec bash "$0" "$@"; fi
set -u
export SYSTEMD_PAGER=cat LC_ALL=C.UTF-8

SCRIPT=$(readlink -f -- "${BASH_SOURCE[0]}")
REPO=$(cd "$(dirname -- "$SCRIPT")/.." && pwd)
CONF_HOME=${XDG_CONFIG_HOME:-$HOME/.config}
STATE_DIR=${XDG_STATE_HOME:-$HOME/.local/state}/shadow-fix-assistant-voice
CONF=$CONF_HOME/shadow/assistant.conf
VOICE_LOG=${XDG_STATE_HOME:-$HOME/.local/state}/shadow/voice.log
LISTEN_DESKTOP=org.shadow.assistant-listen.desktop
USER_AUTOSTART=$CONF_HOME/autostart/$LISTEN_DESKTOP
SYSTEM_DATA=/var/lib/shadow-voice
# First line of every file this script writes.
MARK='# Written by Shadow OS scripts/fix-assistant-voice.sh.'
# Spoken by you in the microphone test: starts with the wake word, plain words
# only (numbers and contractions would blur the word error rate).
SENTENCE='Shadow, please open the terminal and check my email.'
CAL_SECS=6
GREETING="Hello, I'm Shadow. If you can hear me, my voice works."
# What the engine and this script run on. curl and bzip2: the engine download.
# pulseaudio-utils: pactl (the checks below) and the parec/pacat fallback.
PACKAGES="python3 python3-numpy espeak-ng pipewire-bin pipewire-pulse wireplumber pulseaudio-utils alsa-ucm-conf curl bzip2"
ENGINE_COMPONENTS="runtime vad kws stt-moonshine stt-whisper tts-kokoro"

YES=0
DO_BUILD=1
DO_CALIBRATE=1
BUILD_ARG=""
while [ $# -gt 0 ]; do
    case "$1" in
        --yes|-y) YES=1 ;;
        --no-build) DO_BUILD=0 ;;
        --no-calibrate) DO_CALIBRATE=0 ;;
        --build-dir) [ $# -ge 2 ] || { echo "--build-dir needs a directory" >&2; exit 2; }; BUILD_ARG=$2; shift ;;
        --build-dir=*) BUILD_ARG=${1#--build-dir=} ;;
        -h|--help) sed -n '2,24p' "$SCRIPT"; exit 0 ;;
        *) echo "unknown option '$1' (see --help)" >&2; exit 2 ;;
    esac
    shift
done
[ -z "$BUILD_ARG" ] || BUILD_ARG=$(readlink -m -- "$BUILD_ARG")

# Questions need a terminal; without one (or with --yes) the safe defaults apply.
INTERACTIVE=0
if [ "$YES" = 0 ] && [ -t 0 ]; then INTERACTIVE=1; fi

# Colours only on a terminal; the log file gets plain text.
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ] && [ "${TERM:-dumb}" != dumb ]; then
    C_OK=$'\033[32m' C_WARN=$'\033[33m' C_FAIL=$'\033[1;31m' C_STEP=$'\033[1;36m' C_OFF=$'\033[0m'
    TTY_OUT=/dev/tty
else
    C_OK="" C_WARN="" C_FAIL="" C_STEP="" C_OFF=""
    TTY_OUT=""
fi

FAILS=0
CHECKS=()        # "STATUS|what|next step" for the summary
BACKUP=""
TEE_PID=""
TMPD=""
SUDO_STATE=""    # "", ok, fail
STOPPED=0
PREFIX=""        # install prefix of the assistant built from this checkout
BIN=""           # the shadow-assistant binary to run
HELPER=""        # shadow-voiced
SETUP=""         # shadow-voice-setup
INPUT_DEVICE=""

step() { printf '\n%s== %s%s\n' "$C_STEP" "$*" "$C_OFF"; }
ok()   { printf '  %s[ OK ]%s %s\n' "$C_OK" "$C_OFF" "$*"; }
warn() { printf '  %s[WARN]%s %s\n' "$C_WARN" "$C_OFF" "$*"; }
fail() { printf '  %s[FAIL]%s %s\n' "$C_FAIL" "$C_OFF" "$*"; FAILS=$((FAILS + 1)); }
note() { printf '         %s\n' "$*"; }
die()  { printf '\n%sERROR:%s %s\n' "$C_FAIL" "$C_OFF" "$*"; exit 1; }
# One line of the final summary: PASS, WARN, FAIL or SKIP, what, next step.
check() { CHECKS+=("$1|$2|${3:-}"); }

wait_until() {   # seconds command... : poll every 0.25 s
    local n=$(($1 * 4))
    shift
    while [ "$n" -gt 0 ]; do
        "$@" && return 0
        sleep 0.25
        n=$((n - 1))
    done
    return 1
}

ask() {   # question default(Y|N) [answer without questions] -> 0 = yes
    local q=$1 def=$2 auto=${3:-$2} a
    if [ "$INTERACTIVE" != 1 ]; then
        if [ "$auto" = Y ]; then note "$q -> yes (no questions asked)"; return 0; fi
        note "$q -> left as it is (no questions asked)"
        return 1
    fi
    if [ "$def" = Y ]; then printf '         %s [Y/n] ' "$q"; else printf '         %s [y/N] ' "$q"; fi
    read -r a || a=""
    case "$a" in
        "") [ "$def" = Y ] ;;
        [Yy]*) return 0 ;;
        *) return 1 ;;
    esac
}

# Hand the rest of the log to the terminal before the script ends, also after
# die; never wait for it for more than two seconds.
close_log() {
    [ -z "$TMPD" ] || rm -rf -- "$TMPD"
    [ -n "$TEE_PID" ] || return 0
    exec >&- 2>&-
    local n=40
    while [ "$n" -gt 0 ] && kill -0 "$TEE_PID" 2>/dev/null; do
        sleep 0.05
        n=$((n - 1))
    done
}

# sudo once, when the first step needs root. It may ask for the password
# (also with --yes, as long as there is a terminal).
need_sudo() {
    [ "$SUDO_STATE" = fail ] && return 1
    # Also after a long build: sudo forgets the password after 15 minutes.
    if sudo -n true 2>/dev/null; then SUDO_STATE=ok; return 0; fi
    if [ -t 0 ]; then
        note "(sudo asks for your password; it may also print 'unable to resolve host ...', which is harmless)"
        if sudo -v; then SUDO_STATE=ok; return 0; fi
    fi
    SUDO_STATE=fail
    return 1
}

# apt without questions: no debconf dialogs, changed configuration files are
# kept, and nothing reads the terminal (so it cannot swallow what you type).
apt_get() {
    sudo env DEBIAN_FRONTEND=noninteractive apt-get -o DPkg::Lock::Timeout=120 \
        -o Dpkg::Options::=--force-confdef -o Dpkg::Options::=--force-confold "$@" < /dev/null
}
pkg_installed() { [ "$(dpkg-query -W -f='${db:Status-Abbrev}' "$1" 2>/dev/null)" = "ii " ]; }
pkg_available() { LC_ALL=C apt-cache policy "$1" 2>/dev/null | grep -q 'Candidate: [^(]'; }

backup_dir() {
    if [ -z "$BACKUP" ]; then
        BACKUP=$STATE_DIR/backup-$(date +%Y%m%d-%H%M%S)
        (umask 077 && mkdir -p "$BACKUP") || { fail "cannot create $BACKUP"; BACKUP=""; return 1; }
    fi
}
backup_conf_once() {   # copy assistant.conf (it holds the API key: mode 0600) before the first change
    [ -f "$CONF" ] || return 0
    [ -n "$BACKUP" ] && [ -f "$BACKUP/assistant.conf" ] && return 0
    backup_dir || return 1
    install -m 600 -- "$CONF" "$BACKUP/assistant.conf"
}

# Python does the careful text work (INI with a secret in it, JSON, the level
# meter). It is the engine's own interpreter; step 2 installs it if missing.
PYHELP=$(cat <<'PY'
import json, math, os, re, sys, tempfile, time

# A line that holds (or held) a secret: never changed, never printed.
SECRET_KEY = re.compile(r'^\s*[#;]?\s*[\w.-]*(key|token|secret|passw(or)?d)[\w.-]*\s*=', re.I)
SECRET_VALUE = re.compile(r'sk-ant-|\bsk-[A-Za-z0-9_-]{20,}')
# The assistant's own rule (AssistantConfig::sanitizeValue): a comment starts
# at '#' or ';' at the start of the value or after a blank.
INLINE = re.compile(r'(?:^|\s)[#;].*$', re.S)


def secret(line):
    return bool(SECRET_KEY.search(line) or SECRET_VALUE.search(line))


def read_text(path):
    with open(path, 'rb') as f:
        return f.read().decode('utf-8', 'surrogateescape')


def replace_file(path, text):
    """Write atomically, mode 0600 (the file holds the API key)."""
    d = os.path.dirname(path) or '.'
    os.makedirs(d, mode=0o700, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix='.assistant.conf.', dir=d)
    try:
        with os.fdopen(fd, 'wb') as f:
            f.write(text.encode('utf-8', 'surrogateescape'))
            f.flush()
            os.fsync(f.fileno())
        os.chmod(tmp, 0o600)
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def repaired(text):
    out, changes, secrets, section = [], [], 0, ''
    for n, line in enumerate(text.splitlines(keepends=True), 1):
        body = line.rstrip('\r\n')
        eol = line[len(body):]
        s = body.strip()
        new = body
        if secret(body):
            secrets += 1
        elif s.startswith('[') and s.endswith(']'):
            section = s[1:-1].strip()
        elif s.startswith('#'):
            # Qt's INI format has no '#' comments ("# x=y" becomes a key named
            # "# x"); ';' is its comment character.
            i = body.index('#')
            new = body[:i] + ';' + body[i + 1:]
        elif s and not s.startswith(';') and '=' in body:
            eq = body.index('=')
            value = body[eq + 1:]
            m = INLINE.search(value)
            if m:
                new = (body[:eq + 1] + value[:m.start()]).rstrip()
        if new != body:
            changes.append({'line': n, 'section': section, 'old': body, 'new': new})
        out.append(new + eol)
    return ''.join(out), changes, secrets


def parse(text):
    """{(section, key): sanitized value}; the last occurrence wins, as in Qt."""
    vals, section = {}, ''
    for line in text.splitlines():
        s = line.strip()
        if not s or s[0] in '#;':
            continue
        if s.startswith('[') and s.endswith(']'):
            section = s[1:-1].strip()
            continue
        if '=' in s:
            k, v = s.split('=', 1)
            vals[(section, k.strip())] = INLINE.sub('', v).strip()
    return vals


def cmd_conf_repair(path, mode):
    text = read_text(path)
    new, changes, secrets = repaired(text)
    if mode == '--write' and new != text:
        replace_file(path, new)
    print(json.dumps({'changes': changes, 'secret_lines': secrets}))


def cmd_conf_get(path, section, key, default=''):
    if secret(key + '='):
        sys.exit('refusing to read a secret')
    try:
        vals = parse(read_text(path))
    except FileNotFoundError:
        vals = {}
    print(vals.get((section, key), default))


def cmd_conf_set(path, section, key, value):
    """Set one value in [section], keeping every other line (and the key line) as it is."""
    try:
        text = read_text(path)
    except FileNotFoundError:
        text = ''
    lines = text.splitlines(keepends=True)
    nl = '\r\n' if lines and lines[0].endswith('\r\n') else '\n'   # keep the file's line endings
    if lines and not lines[-1].endswith('\n'):
        lines[-1] += nl
    old, cur, start, end, done = None, None, None, None, False
    for i, line in enumerate(lines):
        s = line.strip()
        if s.startswith('[') and s.endswith(']'):
            if cur == section and end is None:
                end = i
            cur = s[1:-1].strip()
            if cur == section and start is None:
                start = i
            continue
        if cur == section and s and s[0] not in '#;' and '=' in s and not secret(line):
            k, v = s.split('=', 1)
            if k.strip() == key:
                old = INLINE.sub('', v).strip()
                lines[i] = key + '=' + value + nl
                done = True
    if not done:
        if start is None:
            if lines and lines[-1].strip():
                lines.append(nl)
            lines += ['[' + section + ']' + nl, key + '=' + value + nl]
        else:
            j = end if end is not None else len(lines)
            while j > start + 1 and not lines[j - 1].strip():
                j -= 1
            lines.insert(j, key + '=' + value + nl)
    replace_file(path, ''.join(lines))
    print('(not set)' if old is None else old)


def load_json(path):
    """The last line of the file that is a JSON object (tools print one line)."""
    obj = None
    with open(path, encoding='utf-8', errors='replace') as f:
        data = f.read()
    try:
        return json.loads(data)
    except ValueError:
        pass
    for line in data.splitlines():
        line = line.strip()
        if line.startswith('{'):
            try:
                obj = json.loads(line)
            except ValueError:
                pass
    return obj


def cmd_field(path, *names):
    """One output line per dotted field name; missing -> empty; bools true/false."""
    obj = load_json(path)
    for name in names:
        v = obj
        for part in name.split('.'):
            v = v.get(part) if isinstance(v, dict) else None
        if isinstance(v, bool):
            v = 'true' if v else 'false'
        elif isinstance(v, (list, tuple)):
            v = ' '.join(str(x) for x in v)
        elif isinstance(v, dict):
            v = json.dumps(v)
        print('' if v is None else str(v).replace('\n', ' '))


def cmd_manifest(path):
    """Installed engine components (line 1) and their number of files (line 2), from manifest.json."""
    try:
        with open(path, encoding='utf-8') as f:
            m = json.load(f)
    except (OSError, ValueError):
        m = {}
    inst = m.get('installed') or {}
    print(' '.join(sorted(inst)))
    print(sum(len(v.get('files') or {}) for v in inst.values()))


def cmd_devices(kind):
    """pactl -f json list sources|sinks on stdin -> name, monitor, mute, volume %, description."""
    try:
        items = json.load(sys.stdin)
    except ValueError:
        sys.exit(1)
    for d in items:
        name = d.get('name') or ''
        props = d.get('properties') or {}
        mon = d.get('monitor_of_sink')
        monitor = (kind == 'sources' and (name.endswith('.monitor') or props.get('device.class') == 'monitor'
                                          or (mon not in (None, '', 'n/a'))))
        vols = []
        for ch in (d.get('volume') or {}).values():
            try:
                vols.append(int(str(ch.get('value_percent', '0')).rstrip('%')))
            except ValueError:
                pass
        desc = (d.get('description') or props.get('device.description') or name).replace('\t', ' ')
        print('\t'.join([name, '1' if monitor else '0', '1' if d.get('mute') else '0',
                         str(max(vols) if vols else -1), desc]))


def cmd_calib(path):
    """The microphone test result as KEY=value lines for the shell, plus what each model heard."""
    o = load_json(path) or {}
    mic = o.get('mic') or {}
    models = o.get('models') or {}
    rec = o.get('recommended')

    def q(v):
        return "'" + str(v).replace("'", "'\\''") + "'"
    print('CAL_OK=' + ('1' if o.get('ok') else '0'))
    print('CAL_ERROR=' + q(o.get('error') or ''))
    print('CAL_SOURCE=' + q(o.get('source') or ''))
    print('CAL_SILENCE=' + ('1' if mic.get('silence') else '0'))
    print('CAL_CLIPPING=' + ('1' if mic.get('clipping') else '0'))
    print('CAL_PEAK=' + q(mic.get('peak_dbfs', '')))
    print('CAL_FLOOR=' + q(mic.get('noise_floor_dbfs', '')))
    print('CAL_SPEECH_S=' + q(o.get('speech_s', 0)))
    print('CAL_RECOMMENDED=' + q(rec or ''))
    best = models.get(rec) if rec else None
    print('CAL_BEST_WER=' + q('' if not best or best.get('wer') is None else best['wer']))
    heard = (best or {}).get('text', '')
    first = re.sub(r'[^a-z]', '', (heard.split() or [''])[0].lower())
    print('CAL_WAKE_HEARD=' + ('1' if first == 'shadow' else '0'))
    print('CAL_ADVICE=' + q(' '.join(o.get('advice') or [])))
    lines = []
    for name in sorted(models, key=lambda k: (k != 'moonshine', k)):
        r = models[name]
        lines.append('%-9s heard: "%s"  (%s of %s words wrong, %s ms)' % (
            name, r.get('text', ''), r.get('errors', '?'), r.get('words', '?'), r.get('stt_ms', '?')))
    print('CAL_MODELS=' + q('\n'.join(lines)))


def cmd_meter(seconds, tty):
    """Live input level from raw s16le mono 16 kHz on stdin, drawn on the terminal."""
    import array
    end = time.monotonic() + float(seconds)
    out = None
    if tty:
        try:
            out = open(tty, 'w')
        except OSError:
            out = None
    peak_db, blk = -120.0, 1600            # 100 ms blocks
    while time.monotonic() < end:
        data = sys.stdin.buffer.read(blk * 2)
        if not data:
            break
        a = array.array('h')
        a.frombytes(data[:len(data) // 2 * 2])
        if sys.byteorder != 'little':
            a.byteswap()
        rms = math.sqrt(sum(x * x for x in a) / max(1, len(a))) / 32768.0
        db = 20 * math.log10(rms) if rms > 1e-9 else -120.0
        peak_db = max(peak_db, db)
        if out:
            n = max(0, min(30, int((db + 60) / 2)))    # -60 dBFS .. 0 dBFS
            left = max(0.0, end - time.monotonic())
            out.write('\r         mic %s%s %6.1f dBFS  %3.1f s ' % ('#' * n, '.' * (30 - n), db, left))
            out.flush()
    if out:
        out.write('\r' + ' ' * 70 + '\r')
        out.close()
    print('%.1f' % peak_db)


cmd, args = sys.argv[1], sys.argv[2:]
{'conf-repair': cmd_conf_repair, 'conf-get': cmd_conf_get, 'conf-set': cmd_conf_set, 'field': cmd_field,
 'manifest': cmd_manifest, 'devices': cmd_devices, 'calib': cmd_calib, 'meter': cmd_meter}[cmd](*args)
PY
)
py() { python3 -I -c "$PYHELP" "$@"; }

# This user's shadow-assistant processes, found by their executable: the
# process name is cut to 15 characters ("shadow-assistan", so pkill -x never
# matches) and a command-line match (pkill -f) would also hit "cmake --build
# ... --target shadow-assistant" or an editor with that file open.
assistant_pids() {
    local p exe out=""
    for p in /proc/[0-9]*; do
        [ -O "$p" ] || continue
        exe=$(readlink "$p/exe" 2>/dev/null) || continue
        exe=${exe% (deleted)}
        if [ "${exe##*/}" = shadow-assistant ]; then out="$out ${p#/proc/}"; fi
    done
    printf '%s' "${out# }"
}
# A process's command line, words separated by spaces ("" when it is gone:
# processes come and go while /proc is read).
cmdline_of() { { tr '\0' ' ' < "/proc/$1/cmdline"; } 2>/dev/null; }
# This user's engine processes (Python running shadow-voiced --serve).
helper_pids() {
    local p out=""
    for p in /proc/[0-9]*; do
        [ -O "$p" ] || continue
        case "$(cmdline_of "${p#/proc/}")" in
            *shadow-voiced*--serve*) out="$out ${p#/proc/}" ;;
        esac
    done
    printf '%s' "${out# }"
}
ppid_of() { sed -n 's/^PPid:[[:space:]]*//p' "/proc/$1/status" 2>/dev/null; }
children_of() {   # pid -> its child processes
    local p out=""
    for p in /proc/[0-9]*; do
        [ "$(ppid_of "${p#/proc/}")" = "$1" ] && out="$out ${p#/proc/}"
    done
    printf '%s' "${out# }"
}
pids_gone() {
    local p
    for p in "$@"; do kill -0 "$p" 2>/dev/null && return 1; done
    return 0
}

# Stop every running copy of the assistant (and so its engine), once: before
# its files are replaced and its settings repaired; step 7 starts it again.
stop_assistant() {
    [ "$STOPPED" = 1 ] && return 0
    STOPPED=1
    local pids hp
    pids=$(assistant_pids)
    hp=$(helper_pids)
    if [ -z "$pids" ] && [ -z "$hp" ]; then
        note "the assistant is not running"
        return 0
    fi
    note "stopping the running assistant (PID ${pids:-none}); it is started again at the end"
    # Its child processes (the engine; with the old voice also pocketsphinx,
    # parecord, spd-say) must not outlive it, holding the microphone.
    local kids="" p
    for p in $pids; do kids="$kids $(children_of "$p")"; done
    if [ -n "$pids" ]; then
        # shellcheck disable=SC2086
        kill -TERM $pids 2>/dev/null
        # shellcheck disable=SC2086
        if ! wait_until 8 pids_gone $pids; then
            # shellcheck disable=SC2086
            kill -KILL $pids 2>/dev/null
            # shellcheck disable=SC2086
            wait_until 3 pids_gone $pids || fail "could not stop shadow-assistant (PID $pids)"
        fi
    fi
    # The engine leaves when its input closes and old children usually end
    # with their parent; any that do not are stopped.
    hp="$(helper_pids) ${kids# }"
    hp=${hp# }
    # shellcheck disable=SC2086
    if [ -n "$hp" ] && ! wait_until 5 pids_gone $hp; then
        # shellcheck disable=SC2086
        kill -TERM $hp 2>/dev/null
        # shellcheck disable=SC2086
        wait_until 5 pids_gone $hp || kill -KILL $hp 2>/dev/null
    fi
}

# Start the assistant detached, listening for the wake word. A transient user
# service (systemd-run) survives closing this terminal and logs to the
# journal; without a systemd user manager it is detached with setsid.
start_background() {
    local envs=() v unit
    if command -v systemd-run >/dev/null 2>&1 && systemctl --user show-environment >/dev/null 2>&1; then
        for v in WAYLAND_DISPLAY DISPLAY XAUTHORITY XDG_CURRENT_DESKTOP XDG_SESSION_TYPE XDG_SESSION_DESKTOP \
                 XDG_MENU_PREFIX QT_QPA_PLATFORM QT_QPA_PLATFORMTHEME DBUS_SESSION_BUS_ADDRESS; do
            if [ -n "${!v:-}" ]; then envs+=("--setenv=$v=${!v}"); fi
        done
        unit=shadow-assistant-wake-$(date +%s)
        if systemd-run --user --quiet --collect --unit="$unit" \
                --description="Shadow Assistant listening for the wake word (started by fix-assistant-voice.sh)" \
                "${envs[@]}" "$BIN" --background < /dev/null >> "$STATE_DIR/assistant.log" 2>&1; then
            START_HOW="systemd user unit $unit.service"
            START_UNIT=$unit
            return 0
        fi
    fi
    : > "$STATE_DIR/assistant.log"
    if setsid -f "$BIN" --background < /dev/null >> "$STATE_DIR/assistant.log" 2>&1; then
        START_HOW="detached process, output in $STATE_DIR/assistant.log"
        return 0
    fi
    return 1
}
start_output() {   # what the background assistant printed (its error, when it stopped)
    if [ -n "$START_UNIT" ]; then
        journalctl --user -u "$START_UNIT.service" -o cat --no-pager -n 15 2>/dev/null
    else
        tail -n 15 "$STATE_DIR/assistant.log" 2>/dev/null
    fi
}

sof_device() {   # an audio DSP run by Intel's Sound Open Firmware driver -> its PCI address
    local d drv
    for d in /sys/bus/pci/devices/*; do
        [ -e "$d/class" ] || continue
        case "$(cat "$d/class" 2>/dev/null)" in 0x0401*|0x0403*) ;; *) continue ;; esac
        drv=$(readlink "$d/driver" 2>/dev/null) || continue
        case "${drv##*/}" in sof-audio-pci*) printf '%s\n' "${d##*/}"; return 0 ;; esac
    done
    grep -qsi 'sof' /proc/asound/cards && { echo "(see /proc/asound/cards)"; return 0; }
    return 1
}

autostart_dirs() {   # the XDG autostart directories the session reads (systemd's generator)
    local dirs v
    dirs=$(systemctl --user show-environment 2>/dev/null | sed -n 's/^XDG_CONFIG_DIRS=//p')
    [ -n "$dirs" ] || dirs=${XDG_CONFIG_DIRS:-/etc/xdg}
    printf '%s\n' "$CONF_HOME/autostart"
    local IFS=:
    for v in $dirs; do printf '%s\n' "${v%/}/autostart"; done
}

# Find the configured CMake build directory of this checkout.
find_build_dir() {
    local c home cands=()
    if [ -n "$BUILD_ARG" ]; then
        cands=("$BUILD_ARG")
    else
        cands=("$REPO/build")
        for c in "$REPO"/*/CMakeCache.txt; do
            [ -f "$c" ] && [ "${c%/CMakeCache.txt}" != "$REPO/build" ] && cands+=("${c%/CMakeCache.txt}")
        done
    fi
    for c in "${cands[@]}"; do
        [ -f "$c/CMakeCache.txt" ] || continue
        home=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$c/CMakeCache.txt")
        if [ "$(readlink -f -- "$home" 2>/dev/null)" = "$REPO" ]; then
            printf '%s\n' "$c"
            return 0
        fi
        [ -n "$BUILD_ARG" ] && { BUILD_WHY="$c was configured for $home, not for $REPO"; return 1; }
    done
    if [ -n "$BUILD_ARG" ]; then BUILD_WHY="$BUILD_ARG has no CMakeCache.txt (not a configured build directory)"; fi
    return 1
}

# cmake --build output: everything to build.log, on the terminal one line per
# 10 % and the error lines.
build_progress() {
    local line pct shown=-1 n t
    while IFS= read -r line; do
        printf '%s\n' "$line" >> "$STATE_DIR/build.log"
        pct=""
        if [[ $line =~ ^\[([0-9]+)/([0-9]+)\] ]]; then
            n=${BASH_REMATCH[1]}; t=${BASH_REMATCH[2]}
            [ "$t" -gt 0 ] && pct=$((n * 100 / t))
        elif [[ $line =~ ^\[\ *([0-9]+)%\] ]]; then
            pct=${BASH_REMATCH[1]}
        fi
        # ([0/n] is CMake re-checking its file lists, not a compile step.)
        if [ -n "$pct" ] && [ "${n:-1}" != 0 ] && [ $((pct / 10)) -gt "$shown" ]; then
            shown=$((pct / 10))
            note "building: $pct %"
        fi
        case "$line" in
            *"error:"*|FAILED:*|*"Error "[0-9]*) note "$line" ;;
            "ninja: no work to do."*) note "nothing to rebuild" ;;
        esac
    done
}

# shadow-voice-setup --progress-json lines -> one line per step and per 10 %.
setup_progress() {
    local line step prog msg last="" tenth prev=-1
    while IFS= read -r line; do
        printf '%s\n' "$line" >> "$STATE_DIR/engine-setup.log"
        step=$(sed -n 's/^{"step":"\([^"]*\)".*/\1/p' <<<"$line")
        [ -n "$step" ] || continue
        prog=$(sed -n 's/.*"progress":\([0-9.]*\).*/\1/p' <<<"$line")
        msg=$(sed -n 's/.*"message":"\(\([^"\\]\|\\.\)*\)".*/\1/p' <<<"$line")
        tenth=$(awk -v p="${prog:-0}" 'BEGIN { printf "%d", p * 10 }')
        if [ "$step" != "$last" ] || { [ "$tenth" != "$prev" ] && [ "$step" != verify ]; }; then
            note "$(awk -v p="${prog:-0}" 'BEGIN { printf "%3d %%", p * 100 }')  $msg"
            last=$step
            prev=$tenth
        fi
    done
}

engine_complete() {   # manifest of $SYSTEM_DATA lists every component this script installs
    local have c
    have=" $(py manifest "$SYSTEM_DATA/manifest.json" | head -n 1) "
    for c in $ENGINE_COMPONENTS; do
        case "$have" in *" $c "*) ;; *) return 1 ;; esac
    done
}
engine_summary() {   # "components (N files, size)"
    local comps nfiles
    { read -r comps; read -r nfiles; } < <(py manifest "$SYSTEM_DATA/manifest.json")
    printf '%s (%s files, %s)' "$comps" "$nfiles" "$(du -sh "$SYSTEM_DATA" 2>/dev/null | cut -f1)"
}

# Level meter beside the microphone test: its own capture of the same input
# (PipeWire mixes it to both); it only draws, the test records separately.
METER_PID=""
start_meter() {   # seconds outfile
    local tool argv=()
    if command -v pw-record >/dev/null 2>&1; then
        argv=(pw-record --raw --rate 16000 --channels 1 --format s16)
        [ -n "$INPUT_DEVICE" ] && argv+=(--target "$INPUT_DEVICE")
        argv+=(-)
    elif command -v parec >/dev/null 2>&1; then
        argv=(parec --raw --rate=16000 --channels=1 --format=s16le)
        [ -n "$INPUT_DEVICE" ] && argv+=("--device=$INPUT_DEVICE")
    else
        return 1
    fi
    tool=${argv[0]}
    ( "${argv[@]}" 2>/dev/null | py meter "$1" "$TTY_OUT" > "$2" 2>/dev/null ) &
    METER_PID=$!
    note "(level meter: $tool)"
}
stop_meter() {
    [ -n "$METER_PID" ] || return 0
    wait_until 3 pids_gone "$METER_PID" || pkill -P "$METER_PID" 2>/dev/null
    wait "$METER_PID" 2>/dev/null
    METER_PID=""
}

# ===========================================================================
[ "$(id -u)" -ne 0 ] || die "Run this as your normal user, not with sudo or as root: it fixes your own assistant and session (it asks for sudo itself where needed)."

LOG=""
if mkdir -p "$STATE_DIR" 2>/dev/null && : 2>/dev/null > "$STATE_DIR/last-run.log"; then
    LOG=$STATE_DIR/last-run.log
    # Terminal: as is (colours, prompts without a newline). Log: plain text.
    exec > >(tee >(sed -u 's/\x1b\[[0-9;]*m//g' >> "$LOG")) 2>&1
    TEE_PID=$!
fi
trap close_log EXIT
TMPD=$(mktemp -d "${TMPDIR:-/tmp}/shadow-fix-voice.XXXXXX") || die "cannot create a temporary directory"
printf 'Shadow OS: fix the assistant voice (%s)\n' "$(date)"
[ "$YES" = 1 ] && printf '(--yes: no questions; safe defaults)\n'
[ "$YES" = 0 ] && [ "$INTERACTIVE" = 0 ] && printf '(no terminal for questions: safe defaults, as with --yes)\n'

# ---------------------------------------------------------------------------
step "0/7  Where this runs"
[ -f "$REPO/applications/assistant/voice/shadow-voice-setup" ] \
    || die "$SCRIPT is not inside a Shadow OS checkout with the new voice engine (no applications/assistant/voice/)."
for c in sudo dpkg-query apt-get readlink sed awk grep tr setsid timeout install; do
    command -v "$c" >/dev/null 2>&1 || die "The command '$c' is missing."
done
if [ -z "${XDG_RUNTIME_DIR:-}" ] || [ ! -d "$XDG_RUNTIME_DIR" ]; then
    die "XDG_RUNTIME_DIR is not set. Run this from a terminal inside your Shadow desktop (not over SSH or on a text console)."
fi
SESSION=""
if [ -n "${WAYLAND_DISPLAY:-}" ]; then
    case "$WAYLAND_DISPLAY" in /*) wl=$WAYLAND_DISPLAY ;; *) wl=$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY ;; esac
    [ -S "$wl" ] && SESSION="Wayland ($WAYLAND_DISPLAY)"
fi
if [ -z "$SESSION" ] && [ -n "${DISPLAY:-}" ]; then SESSION="X11 ($DISPLAY)"; fi
[ -n "$SESSION" ] || die "No graphical session here (WAYLAND_DISPLAY/DISPLAY). Run this from a terminal inside your Shadow desktop: the assistant is started in it at the end."
ok "user $(id -un), graphical session: $SESSION"
ok "checkout: $REPO"
if [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ] && [ -S "$XDG_RUNTIME_DIR/bus" ]; then
    export DBUS_SESSION_BUS_ADDRESS="unix:path=$XDG_RUNTIME_DIR/bus"
fi
[ -n "${DBUS_SESSION_BUS_ADDRESS:-}" ] || warn "no D-Bus session bus: a second assistant window could not hand over to the first"

# ---------------------------------------------------------------------------
step "1/7  Build and install the assistant from this checkout"
BUILD_WHY=""
if [ "$DO_BUILD" = 0 ]; then
    note "skipped (--no-build)"
    check SKIP "build and install from the checkout" ""
elif ! BUILD=$(find_build_dir); then
    if [ -n "$BUILD_ARG" ]; then
        fail "$BUILD_WHY"
        check FAIL "build and install from the checkout" "pass the build directory you configured for $REPO: --build-dir DIR"
    else
        warn "no configured build directory of this checkout (looked for $REPO/build and $REPO/*/CMakeCache.txt)"
        note "configure one once:  cmake -S $REPO -B $REPO/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo"
        check WARN "build and install from the checkout" "configure $REPO/build (see above) and run this script again, or keep the installed assistant (--no-build)"
    fi
else
    PREFIX=$(sed -n 's/^CMAKE_INSTALL_PREFIX:[A-Z]*=//p' "$BUILD/CMakeCache.txt")
    PREFIX=${PREFIX%/}
    [ -n "$PREFIX" ] || PREFIX=/usr/local
    ok "build directory $BUILD (installs to $PREFIX)"
    if [ "$PREFIX" = /usr ] && pkg_installed shadow-assistant; then
        note "(this replaces files of the installed shadow-assistant package; its next update puts them back)"
    fi
    : > "$STATE_DIR/build.log"
    rc=0
    if ! command -v cmake >/dev/null 2>&1; then
        echo "cmake: command not found (sudo apt install cmake ninja-build)" >> "$STATE_DIR/build.log"
        rc=127
    else
        note "building shadow-assistant with $(nproc) jobs (full output: $STATE_DIR/build.log)"
        cmake --build "$BUILD" --target shadow-assistant -j"$(nproc)" < /dev/null 2>&1 | build_progress
        rc=${PIPESTATUS[0]}
    fi
    if [ "$rc" -ne 0 ]; then
        fail "the build failed (exit $rc)"
        grep -E 'error|Error|FAILED|denied|not found' "$STATE_DIR/build.log" | head -n 12 | while IFS= read -r l; do note "$l"; done
        hint="fix the error above (full output: $STATE_DIR/build.log) and run this script again; --no-build keeps the installed assistant"
        if grep -qs 'Permission denied' "$STATE_DIR/build.log"; then
            hint="files in $BUILD belong to root (an earlier sudo build?): sudo chown -R $(id -un): $BUILD, then run this script again"
        fi
        check FAIL "build the assistant" "$hint"
    else
        ok "built $BUILD/bin/shadow-assistant"
        stop_assistant
        # The install log is written by this shell (as you), not by root.
        # shellcheck disable=SC2024
        if ! need_sudo; then
            fail "sudo is needed to install into $PREFIX"
            check FAIL "install the assistant" "run in a terminal: sudo cmake --install $BUILD --component shadow-assistant"
        elif sudo cmake --install "$BUILD" --component shadow-assistant < /dev/null > "$STATE_DIR/install.log" 2>&1; then
            n_new=$(grep -c '^-- Installing:' "$STATE_DIR/install.log")
            n_same=$(grep -c '^-- Up-to-date:' "$STATE_DIR/install.log")
            ok "installed into $PREFIX ($n_new files new or changed, $n_same up to date)"
            missing=""
            for f in bin/shadow-assistant lib/shadow-assistant/voice/shadow-voiced bin/shadow-voice-setup; do
                [ -x "$PREFIX/$f" ] || missing="$missing $PREFIX/$f"
            done
            if [ -n "$missing" ]; then
                fail "missing after the install:$missing"
                check FAIL "install the assistant" "this checkout predates the new voice engine; update it (git pull) and run this script again"
            else
                check PASS "build and install the assistant ($PREFIX)" ""
            fi
        else
            fail "sudo cmake --install failed (see $STATE_DIR/install.log)"
            tail -n 5 "$STATE_DIR/install.log" | while IFS= read -r l; do note "$l"; done
            check FAIL "install the assistant" "see $STATE_DIR/install.log"
        fi
    fi
fi

# Which copy runs: the one just installed, else the one on PATH.
if [ -n "$PREFIX" ] && [ -x "$PREFIX/bin/shadow-assistant" ]; then
    BIN=$PREFIX/bin/shadow-assistant
else
    BIN=$(command -v shadow-assistant 2>/dev/null || true)
    [ -z "$BIN" ] || PREFIX=$(dirname -- "$(dirname -- "$BIN")")
fi
if [ -n "$BIN" ]; then
    onpath=$(command -v shadow-assistant 2>/dev/null || true)
    if [ -n "$onpath" ] && [ "$(readlink -f -- "$onpath")" != "$(readlink -f -- "$BIN")" ]; then
        warn "the launcher starts $onpath, not $BIN (PATH order); remove the older copy or fix PATH"
    fi
    if [ -x "$PREFIX/lib/shadow-assistant/voice/shadow-voiced" ]; then
        HELPER=$PREFIX/lib/shadow-assistant/voice/shadow-voiced
    fi
fi
for c in "$PREFIX/bin/shadow-voice-setup" /usr/bin/shadow-voice-setup /usr/local/bin/shadow-voice-setup \
         "$REPO/applications/assistant/voice/shadow-voice-setup"; do
    if [ -n "$c" ] && [ -x "$c" ]; then SETUP=$c; break; fi
done

# The wake word at login: the install put org.shadow.assistant-listen.desktop
# into <sysconfdir>/xdg/autostart. With the default prefix /usr/local that is
# /usr/local/etc/xdg/autostart, which the session does not read (XDG_CONFIG_DIRS
# is /etc/xdg); a copy in ~/.config/autostart, with the full path of this
# binary, makes it start at login.
if [ -n "$BIN" ] && [ -n "$PREFIX" ]; then
    installed_entry=""
    if [ -n "${BUILD:-}" ] && [ -f "$BUILD/install_manifest_shadow-assistant.txt" ]; then
        installed_entry=$(grep "/xdg/autostart/$LISTEN_DESKTOP\$" "$BUILD/install_manifest_shadow-assistant.txt" | head -n 1)
    fi
    for f in "$PREFIX/etc/xdg/autostart/$LISTEN_DESKTOP" "/etc/xdg/autostart/$LISTEN_DESKTOP"; do
        if [ -z "$installed_entry" ] && [ -f "$f" ]; then installed_entry=$f; fi
    done
    read_by_session=0
    if [ -n "$installed_entry" ]; then
        while IFS= read -r d; do
            [ "$d" != "$CONF_HOME/autostart" ] && [ "${installed_entry%/*}" = "$d" ] && read_by_session=1
        done < <(autostart_dirs)
    fi
    if [ -e "$USER_AUTOSTART" ] && ! { [ -f "$USER_AUTOSTART" ] && [ "$(head -n 1 "$USER_AUTOSTART")" = "$MARK" ]; }; then
        warn "$USER_AUTOSTART is your own file and replaces the installed one; left as it is"
        check WARN "wake word at login" "check that $USER_AUTOSTART starts $BIN --background"
    elif [ "$read_by_session" = 1 ]; then
        # The session reads the installed entry; a copy from an earlier run
        # (other prefix) would only shadow it.
        rm -f "$USER_AUTOSTART"
        ok "wake word at login: $installed_entry"
        check PASS "wake word at login" ""
    else
        mkdir -p "${USER_AUTOSTART%/*}"
        {
            printf '%s\n' "$MARK" \
                "# ${installed_entry:-The installed autostart entry} is in a directory the session does not" \
                "# read (XDG_CONFIG_DIRS); this copy starts $BIN at login."
            if [ -n "$installed_entry" ] && [ -f "$installed_entry" ]; then
                sed "s|^Exec=.*|Exec=$BIN --background|" "$installed_entry"
            else
                printf '%s\n' '[Desktop Entry]' 'Type=Application' 'Name=Shadow Assistant (wake word)' \
                    'Comment=Listens for "Shadow" so the assistant answers without opening it first' \
                    "Exec=$BIN --background" 'Icon=org.shadow.assistant' 'Terminal=false' 'NoDisplay=true' \
                    'X-GNOME-Autostart-enabled=true' 'X-KDE-autostart-phase=2'
            fi
        } > "$USER_AUTOSTART.tmp" && mv -f "$USER_AUTOSTART.tmp" "$USER_AUTOSTART"
        if grep -qx "Exec=$BIN --background" "$USER_AUTOSTART" 2>/dev/null; then
            ok "wake word at login: $USER_AUTOSTART (${installed_entry:+$installed_entry is not read by the session})"
            check PASS "wake word at login" ""
            systemctl --user daemon-reload >/dev/null 2>&1 || true
        else
            fail "could not write $USER_AUTOSTART"
            check FAIL "wake word at login" "copy $installed_entry to ~/.config/autostart/"
        fi
    fi
fi

# ---------------------------------------------------------------------------
step "2/7  Debian packages the voice runs on"
missing=""
for p in $PACKAGES; do pkg_installed "$p" || missing="${missing:+$missing }$p"; done
SOF=$(sof_device || true)
sof_pkgs=""
if [ -n "$SOF" ]; then
    note "this computer's audio runs on Intel's Sound Open Firmware (SOF, $SOF): laptop"
    note "microphones there are a digital array that needs firmware-sof-signed and alsa-ucm-conf"
    if ! pkg_installed firmware-sof-signed; then
        if pkg_available firmware-sof-signed; then
            missing="${missing:+$missing }firmware-sof-signed"
            sof_pkgs=firmware-sof-signed
        else
            warn "firmware-sof-signed is not available: enable Debian's non-free-firmware component"
            check WARN "SOF microphone firmware" "add non-free-firmware to /etc/apt/sources.list.d/debian.sources, then: sudo apt install firmware-sof-signed and reboot"
        fi
    fi
fi
had_missing=$missing
if [ -z "$missing" ]; then
    ok "present: $PACKAGES${SOF:+ firmware-sof-signed}"
    check PASS "Debian packages" ""
else
    note "missing: $missing"
    if ask "Install them now (apt, without recommended extras; nothing is removed)?" Y; then
        if need_sudo; then
            # --no-remove: apt stops instead of removing anything.
            # shellcheck disable=SC2086
            if ! apt_get install -y --no-install-recommends --no-remove $missing > "$STATE_DIR/apt.log" 2>&1; then
                note "refreshing the package lists and trying once more..."
                apt_get update >> "$STATE_DIR/apt.log" 2>&1 || true
                # shellcheck disable=SC2086
                apt_get install -y --no-install-recommends --no-remove $missing >> "$STATE_DIR/apt.log" 2>&1 || true
            fi
        else
            fail "sudo is needed to install packages"
        fi
    fi
    missing=""
    for p in $had_missing; do pkg_installed "$p" || missing="${missing:+$missing }$p"; done
    if [ -z "$missing" ]; then
        ok "installed: $had_missing"
        check PASS "Debian packages" ""
    else
        fail "still missing: $missing (apt output: $STATE_DIR/apt.log)"
        tail -n 4 "$STATE_DIR/apt.log" 2>/dev/null | while IFS= read -r l; do note "$l"; done
        check FAIL "Debian packages" "sudo apt install --no-install-recommends $missing"
    fi
    # New ALSA use-case profiles are read by WirePlumber when it starts.
    case " $had_missing " in
        *" alsa-ucm-conf "*)
            if pkg_installed alsa-ucm-conf && systemctl --user is-active --quiet wireplumber.service 2>/dev/null; then
                note "restarting WirePlumber so it reads the new ALSA profiles (sound stops for a moment)"
                systemctl --user restart wireplumber.service >/dev/null 2>&1 || warn "could not restart WirePlumber"
                sleep 2
            fi ;;
    esac
    if [ -n "$sof_pkgs" ] && pkg_installed firmware-sof-signed; then
        warn "firmware-sof-signed was just installed: the microphone appears after a reboot"
        check WARN "SOF microphone firmware" "reboot, then run this script again"
    fi
fi
command -v python3 >/dev/null 2>&1 || die "python3 is missing and could not be installed: sudo apt install python3, then run this script again."

# ---------------------------------------------------------------------------
step "3/7  Voice engine data ($SYSTEM_DATA)"
if [ -z "$SETUP" ]; then
    fail "shadow-voice-setup not found"
    check FAIL "voice engine data" "install the assistant from this checkout (step 1)"
else
    engine_ok=0
    if [ -f "$SYSTEM_DATA/manifest.json" ]; then
        if timeout 600 "$SETUP" --system --check < /dev/null > "$TMPD/check.out" 2>&1; then
            if engine_complete; then
                engine_ok=1
                ok "installed, every file intact: $(engine_summary)"
            else
                note "installed, but not every part ($(py manifest "$SYSTEM_DATA/manifest.json" | head -n 1)); adding the rest"
            fi
        else
            warn "the installed engine data is damaged: $(tail -n 1 "$TMPD/check.out")"
        fi
    else
        note "not installed yet: about 700 MB to download once (every file sha256-checked), ~700 MB on disk"
    fi
    if [ "$engine_ok" = 1 ]; then
        check PASS "voice engine data" ""
    elif ! need_sudo; then
        fail "sudo is needed to install into $SYSTEM_DATA"
        check FAIL "voice engine data" "sudo $SETUP --system"
    else
        : > "$STATE_DIR/engine-setup.log"
        note "installing (a re-run resumes an interrupted download; log: $STATE_DIR/engine-setup.log)"
        sudo "$SETUP" --system --stt both --yes --progress-json < /dev/null 2>>"$STATE_DIR/engine-setup.log" | setup_progress
        rc=${PIPESTATUS[0]}
        if [ "$rc" = 0 ]; then
            engine_ok=1
            ok "installed: $(engine_summary); its self-test passed"
            check PASS "voice engine data" ""
        else
            why=$(grep -h 'error:' "$STATE_DIR/engine-setup.log" | tail -n 1 | sed 's/^shadow-voice-setup: error: //')
            case "$rc" in
                4) next="check the network and run this script again (finished downloads are kept and resumed)" ;;
                6) next="free disk space (~1.4 GB while installing), then run this script again" ;;
                12) next="another shadow-voice-setup is running (the assistant's Install button?); wait for it, then run this script again" ;;
                *) next="see $STATE_DIR/engine-setup.log; then: sudo $SETUP --system" ;;
            esac
            fail "shadow-voice-setup failed (exit $rc): ${why:-see the log}"
            check FAIL "voice engine data" "$next"
        fi
    fi
    # Which installation the engine will really use (a per-user copy wins).
    if [ -n "$HELPER" ]; then
        timeout 30 "$HELPER" --probe < /dev/null > "$TMPD/probe.json" 2>/dev/null
        prc=$?
        { read -r pdata; read -r preason; read -r pstt; } < <(py field "$TMPD/probe.json" data reason stt 2>/dev/null)
        if [ "$prc" = 0 ]; then
            ok "the engine finds its data in ${pdata:-?} (speech recognition: ${pstt:-?})"
            [ "$pdata" = "$SYSTEM_DATA" ] || note "(a per-user copy in ${pdata:-?} is used before $SYSTEM_DATA)"
        elif [ "$prc" = 5 ]; then
            warn "the engine has its data but no recorder (pw-record/parec/arecord): ${preason:-}"
        else
            fail "shadow-voiced --probe: ${preason:-exit $prc}"
            [ "$engine_ok" = 1 ] && check FAIL "engine finds its data" "${preason:-shadow-voiced --probe failed}"
        fi
    fi
fi

# ---------------------------------------------------------------------------
step "4/7  Assistant settings (${CONF/#$HOME/\~})"
stop_assistant
if [ ! -e "$CONF" ]; then
    ok "no settings file yet: the defaults apply (the assistant's Setup page shows how to add your API key)"
    check PASS "assistant settings" ""
elif [ ! -O "$CONF" ] || [ ! -w "$CONF" ]; then
    fail "$CONF does not belong to you or is not writable (left by a command run with sudo?)"
    check FAIL "assistant settings" "sudo chown $(id -un): $CONF && chmod 600 $CONF"
else
    if ! py conf-repair "$CONF" --check > "$TMPD/repair.json"; then
        fail "could not read $CONF (the error is above)"
        check FAIL "assistant settings" "check that $CONF is a readable text file, then run this script again"
    else
        nchg=$(python3 -I -c 'import json,sys; print(len(json.load(open(sys.argv[1]))["changes"]))' "$TMPD/repair.json")
        nsec=$(python3 -I -c 'import json,sys; print(json.load(open(sys.argv[1]))["secret_lines"])' "$TMPD/repair.json")
        mode=$(stat -c %a "$CONF")
        if [ "$nchg" = 0 ]; then
            ok "no comments on value lines; every value reads as written"
        elif backup_conf_once; then
            note "Qt reads a '# comment' after a value as part of the value (\"muted=false  # ...\""
            note "meant muted) and a '#' line as a setting. Changes (lines holding a key are never"
            note "shown or changed; $nsec such line(s) left exactly as they are):"
            python3 -I -c '
import json, sys
for c in json.load(open(sys.argv[1]))["changes"][:40]:
    sec = "[%s]" % c["section"] if c["section"] else ""
    print("           %4d %-11s- %s" % (c["line"], sec, c["old"]))
    print("           %4s %-11s+ %s" % ("", "", c["new"]))
' "$TMPD/repair.json"
            [ "$nchg" -gt 40 ] && note "... and $((nchg - 40)) more (all in the backup)"
            if py conf-repair "$CONF" --write > /dev/null; then
                ok "repaired $nchg line(s); the previous file is in $BACKUP/assistant.conf"
            else
                fail "could not write $CONF (the previous file is in $BACKUP/assistant.conf)"
            fi
        else
            fail "no backup could be made, so $CONF was not changed"
        fi
        if [ "${mode: -2}" != 00 ]; then
            chmod 600 "$CONF" && ok "the file holds your API key and was readable by others (mode $mode); now only by you (600)"
        fi
        # What the assistant itself reads from it now (its own parser, never the key).
        if [ -n "$BIN" ] && timeout 60 "$BIN" --dump config < /dev/null > "$TMPD/config.json" 2>/dev/null; then
            { read -r keyok; read -r wake; read -r muted; read -r voice; read -r stt; read -r indev; } < <(py field "$TMPD/config.json" \
                apiKeyConfigured voice.wakeEnabled voice.micMuted voice.voice voice.sttModel voice.inputDevice)
            ok "the assistant reads: voice ${voice:-?}, speech recognition ${stt:-?}, wake word $([ "$wake" = true ] && echo on || echo off), microphone $([ "$muted" = true ] && echo MUTED || echo not muted)${indev:+, input $indev}"
            if [ "$keyok" = true ]; then ok "an API key is configured (not shown)"; else warn "no API key configured: the assistant hears you but cannot answer until you add one in its Settings"; fi
            INPUT_DEVICE=$indev
            # Your own switches in the assistant: changed only when you say so
            # (never with --yes); the setting file keeps what you chose.
            if [ "$muted" = true ]; then
                warn "the assistant's own microphone switch is off (its privacy mute): it does not listen"
                if ask "Switch the assistant's microphone on?" Y N; then
                    backup_conf_once && py conf-set "$CONF" voice muted false > /dev/null && ok "microphone on (muted=false)"
                fi
            fi
            if [ "$wake" != true ]; then
                warn "the wake word is switched off in the assistant's settings"
                if ask "Switch the wake word on (saying \"Shadow\" wakes the assistant)?" Y N; then
                    backup_conf_once && py conf-set "$CONF" voice wake_word_enabled true > /dev/null \
                        && py conf-set "$CONF" voice wake_word_chosen true > /dev/null && ok "wake word on"
                fi
            fi
        else
            INPUT_DEVICE=$(py conf-get "$CONF" voice input_device "")
        fi
        check PASS "assistant settings" ""
    fi
fi

# ---------------------------------------------------------------------------
step "5/7  Sound system and microphone"
pactl_ok() { timeout 10 pactl info > "$TMPD/pactl-info" 2>&1; }
if ! pactl_ok && command -v systemctl >/dev/null 2>&1; then
    note "the sound server does not answer; starting PipeWire"
    systemctl --user start pipewire.socket pipewire.service wireplumber.service \
        pipewire-pulse.socket pipewire-pulse.service >/dev/null 2>&1
    wait_until 10 pactl_ok
fi
if ! pactl_ok; then
    fail "the sound server does not answer (pactl info: $(head -n 1 "$TMPD/pactl-info"))"
    check FAIL "PipeWire sound server" "systemctl --user restart pipewire pipewire-pulse wireplumber (or log out and in), then run this script again"
else
    server=$(sed -n 's/^Server Name: //p' "$TMPD/pactl-info")
    case "$server" in
        *PipeWire*) ok "sound server: $server" ;;
        *) warn "sound server: ${server:-unknown} (not PipeWire; the engine uses parec/pacat then)" ;;
    esac
    if pgrep -u "$(id -u)" -x wireplumber >/dev/null 2>&1 || ! pgrep -u "$(id -u)" -x pipewire >/dev/null 2>&1; then
        check PASS "PipeWire sound server" ""
    else
        warn "WirePlumber (the session manager) is not running: devices are not set up"
        check WARN "PipeWire sound server" "systemctl --user restart wireplumber"
    fi

    # The default input must be a microphone, not the monitor of an output
    # (which "hears" what the speakers play) or PulseAudio's dummy auto_null.
    sources() { timeout 10 pactl -f json list sources 2>/dev/null | py devices sources; }
    src=$(timeout 10 pactl get-default-source 2>/dev/null)
    real=$(sources | awk -F'\t' '$2 == 0 && $1 !~ /^auto_null/')
    line=$(printf '%s\n' "$real" | awk -F'\t' -v s="$src" '$1 == s' | head -n 1)
    if [ -z "$line" ]; then
        if [ -z "$real" ]; then
            fail "no microphone: the default input is '${src:-none}' and there is no other"
            if [ -n "$SOF" ]; then
                note "This laptop's microphone runs through the SOF DSP: it needs firmware-sof-signed and"
                note "alsa-ucm-conf (step 2) and a reboot; then the input 'Digital Microphone' appears."
                check FAIL "default input is a microphone" "reboot (firmware-sof-signed and alsa-ucm-conf must be installed), then run this script again"
            else
                check FAIL "default input is a microphone" "connect a microphone (or check that it is enabled in the firmware setup), then run this script again"
            fi
        else
            best=$(printf '%s\n' "$real" | head -n 1)
            warn "the default input is '${src:-none}' (a monitor of an output, not a microphone)"
            if ask "Use '$(cut -f5 <<<"$best")' as the default microphone?" Y; then
                timeout 10 pactl set-default-source "$(cut -f1 <<<"$best")" && sleep 0.5
            fi
            src=$(timeout 10 pactl get-default-source 2>/dev/null)
            line=$(sources | awk -F'\t' -v s="$src" '$1 == s && $2 == 0' | head -n 1)
            if [ -n "$line" ]; then ok "default input is now: $(cut -f5 <<<"$line") ($src)"
            else check FAIL "default input is a microphone" "choose your microphone as the input in the sound settings (wpctl status; wpctl set-default ID)"; fi
        fi
    else
        ok "default input: $(cut -f5 <<<"$line") ($src)"
    fi
    if [ -n "$line" ]; then
        case "$(cut -f1,5 <<<"$line" | tr '[:upper:]' '[:lower:]')" in
            *dmic*|*digital*) [ -n "$SOF" ] && ok "it is the SOF digital microphone array" ;;
        esac
        [ -n "$INPUT_DEVICE" ] && [ "$INPUT_DEVICE" != "$src" ] \
            && note "(the assistant is set to use the input '$INPUT_DEVICE' instead; input_device= in assistant.conf)"
        mic_ok=1
        if [ "$(cut -f3 <<<"$line")" = 1 ]; then
            warn "the microphone is muted in the sound system"
            if ask "Unmute it?" Y; then
                timeout 10 wpctl set-mute @DEFAULT_AUDIO_SOURCE@ 0 >/dev/null 2>&1 \
                    || timeout 10 pactl set-source-mute "$src" 0
                sleep 0.3
            fi
            if [ "$(sources | awk -F'\t' -v s="$src" '$1 == s { print $3 }')" = 1 ]; then
                mic_ok=0
                check FAIL "microphone not muted" "unmute it: wpctl set-mute @DEFAULT_AUDIO_SOURCE@ 0"
            else
                ok "unmuted"
            fi
        else
            ok "not muted"
        fi
        vol=$(sources | awk -F'\t' -v s="$src" '$1 == s { print $4 }')
        if [ -n "$vol" ] && [ "$vol" -ge 0 ] && [ "$vol" -lt 30 ]; then
            warn "its input level is very low ($vol %): your voice would barely register"
            if ask "Set it to 80 %?" Y; then
                timeout 10 wpctl set-volume @DEFAULT_AUDIO_SOURCE@ 0.8 >/dev/null 2>&1 \
                    || timeout 10 pactl set-source-volume "$src" 80%
                sleep 0.3
                vol=$(sources | awk -F'\t' -v s="$src" '$1 == s { print $4 }')
            fi
            if [ "${vol:-0}" -lt 30 ]; then
                mic_ok=0
                check FAIL "microphone level" "raise it in the sound settings: wpctl set-volume @DEFAULT_AUDIO_SOURCE@ 0.8"
            else
                ok "input level now $vol %"
            fi
        elif [ -n "$vol" ] && [ "$vol" -ge 0 ]; then
            ok "input level $vol %"
        fi
        [ "$mic_ok" = 1 ] && check PASS "a microphone, unmuted, with a usable level" ""
    fi
    # The speaker side, for the greeting below (only reported).
    sink=$(timeout 10 pactl get-default-sink 2>/dev/null)
    sline=$(timeout 10 pactl -f json list sinks 2>/dev/null | py devices sinks | awk -F'\t' -v s="$sink" '$1 == s' | head -n 1)
    if [ -z "$sline" ]; then
        warn "no default output (speakers/headphones) found"
    elif [ "$(cut -f3 <<<"$sline")" = 1 ] || [ "$(cut -f4 <<<"$sline")" = 0 ]; then
        warn "the output '$(cut -f5 <<<"$sline")' is muted or at 0 %: you will not hear the assistant"
        note "unmute: wpctl set-mute @DEFAULT_AUDIO_SINK@ 0; wpctl set-volume @DEFAULT_AUDIO_SINK@ 0.5"
    else
        ok "default output: $(cut -f5 <<<"$sline") ($(cut -f4 <<<"$sline") %)"
    fi
fi

# ---------------------------------------------------------------------------
step "6/7  Tests: the engine, the speaker, your microphone"
if [ -z "$HELPER" ]; then
    fail "shadow-voiced not found (the assistant from this checkout is not installed)"
    check FAIL "engine self-test" "run this script without --no-build, from a checkout with a configured build directory"
else
    note "engine self-test: it speaks a sentence and must recognise it again (no audio devices, ~10 s)"
    timeout 600 "$HELPER" --selftest < /dev/null > "$TMPD/selftest.json" 2> "$TMPD/selftest.err"
    rc=$?
    { read -r st_ok; read -r st_first; read -r st_load; } < <(py field "$TMPD/selftest.json" ok first_audio_ms load_s 2>/dev/null)
    if [ "$rc" = 0 ] && [ "$st_ok" = true ]; then
        ok "self-test passed: models loaded in ${st_load:-?} s, first audio of a reply after ${st_first:-?} ms"
        check PASS "engine self-test" ""
    else
        fail "the engine self-test failed (exit $rc)"
        python3 -I -c '
import json, sys
try:
    o = json.loads(open(sys.argv[1]).read().strip().splitlines()[-1])
except Exception:
    o = {}
for c in o.get("checks", []):
    if not c.get("ok"):
        print("         failed: %s: %s" % (c.get("name"), c.get("detail")))
if o.get("error"):
    print("         " + str(o["error"]))
' "$TMPD/selftest.json"
        tail -n 3 "$TMPD/selftest.err" | while IFS= read -r l; do note "$l"; done
        check FAIL "engine self-test" "$HELPER --selftest; details in $VOICE_LOG"
    fi

    # Speaker: the greeting in the voice and speed the assistant uses.
    voice=$(py conf-get "$CONF" voice voice af_heart)
    rate=$(py conf-get "$CONF" voice speech_rate 1.0)
    case "$voice" in [a-z]*_[a-z0-9]*) ;; *) voice=af_heart ;; esac
    case "$rate" in ''|*[!0-9.]*) rate=1.0 ;; esac
    if timeout 180 "$HELPER" --say "$GREETING" --out "$TMPD/greeting.wav" --voice "$voice" --speed "$rate" \
            < /dev/null > "$TMPD/say.json" 2>> "$TMPD/say.err"; then
        player=""
        for t in pw-play paplay aplay; do command -v "$t" >/dev/null 2>&1 && { player=$t; break; }; done
        note "playing a greeting in the voice '$voice' with $player: \"$GREETING\""
        if [ -n "$player" ] && timeout 60 "$player" "$TMPD/greeting.wav" < /dev/null > "$TMPD/play.err" 2>&1; then
            if [ "$INTERACTIVE" = 1 ]; then
                if ask "Did you hear it?" Y; then
                    ok "you heard the assistant's voice"
                    check PASS "speaker (heard)" ""
                else
                    fail "the greeting played, but you did not hear it"
                    check FAIL "speaker" "check the output device and its volume (wpctl status; the sound settings); headphones plugged in?"
                fi
            else
                ok "the greeting played (not confirmed by ear: no questions asked)"
                check PASS "speaker (played, not confirmed by ear)" ""
            fi
        else
            fail "playing failed: $(head -n 2 "$TMPD/play.err" 2>/dev/null | tr '\n' ' ')"
            check FAIL "speaker" "check the output device: wpctl status"
        fi
    else
        fail "the voice could not synthesise the greeting: $(py field "$TMPD/say.json" error 2>/dev/null)"
        check FAIL "speaker" "see $VOICE_LOG"
    fi

    # Microphone and speech recognition. The assistant is stopped (step 4), so
    # no wake word acts on the test sentence.
    if [ "$DO_CALIBRATE" = 0 ]; then
        note "microphone sentence test skipped (--no-calibrate)"
        check SKIP "microphone test and speech recognition" ""
    else
        tries=0
        while :; do
            tries=$((tries + 1))
            printf '\n'
            note "Microphone test. From where you usually sit, say in your normal voice:"
            printf '\n             %s"%s"%s\n\n' "$C_STEP" "$SENTENCE" "$C_OFF"
            if [ "$INTERACTIVE" = 1 ]; then
                printf '         Press Enter, then speak (you have %s seconds; s + Enter skips): ' "$CAL_SECS"
                read -r a || a=s
                case "$a" in [Ss]*) note "skipped"; check SKIP "microphone test and speech recognition" ""; break ;; esac
            else
                for n in 3 2 1; do printf '         %s...\n' "$n"; sleep 1; done
            fi
            note "speak now"
            dev_args=()
            [ -n "$INPUT_DEVICE" ] && dev_args=(--input-device "$INPUT_DEVICE")
            start_meter "$CAL_SECS" "$TMPD/meter.out"
            timeout $((CAL_SECS + 300)) "$HELPER" --calibrate "$CAL_SECS" --expect "$SENTENCE" "${dev_args[@]}" \
                < /dev/null > "$TMPD/calib.json" 2> "$TMPD/calib.err"
            crc=$?
            stop_meter
            peak_meter=$(cat "$TMPD/meter.out" 2>/dev/null)
            # KEY='value' lines from the helper's JSON (values single-quoted by py calib).
            CAL_ERROR="" CAL_SOURCE="" CAL_SILENCE=0 CAL_CLIPPING=0 CAL_PEAK="" CAL_FLOOR="" CAL_SPEECH_S=0
            CAL_RECOMMENDED="" CAL_BEST_WER="" CAL_WAKE_HEARD=0 CAL_ADVICE="" CAL_MODELS=""
            eval "$(py calib "$TMPD/calib.json" 2>/dev/null)"
            [ -n "$peak_meter" ] && note "(level meter: loudest 0.1 s at ${peak_meter} dBFS RMS)"
            if [ -z "$CAL_PEAK" ]; then
                # No level statistics: nothing was recorded at all.
                cp -f "$TMPD/calib.err" "$STATE_DIR/microphone-test.log" 2>/dev/null
                fail "the microphone could not be opened: ${CAL_ERROR:-$(tail -n 1 "$TMPD/calib.err" 2>/dev/null) (exit $crc)}"
                check FAIL "microphone test: recording" "check the input in the sound settings (wpctl status); details: $STATE_DIR/microphone-test.log"
                break
            fi
            if [ "$CAL_SILENCE" = 1 ]; then
                fail "the microphone delivers digital silence (${CAL_SOURCE:-?}): it is muted, or the wrong input is selected"
                check FAIL "microphone test: recording" "select and unmute your microphone (wpctl status); laptops with digital microphones: firmware-sof-signed + alsa-ucm-conf + reboot"
                break
            fi
            ok "microphone works (${CAL_SOURCE:-?}): loudest ${CAL_PEAK} dBFS, background noise ${CAL_FLOOR} dBFS$([ "$CAL_CLIPPING" = 1 ] && echo ', CLIPPING')"
            if [ -z "$CAL_RECOMMENDED" ]; then
                warn "no speech was heard in the $CAL_SECS seconds"
                [ -n "$CAL_ADVICE" ] && note "$CAL_ADVICE"
                if [ "$INTERACTIVE" = 1 ] && [ "$tries" -lt 2 ] && ask "Try again?" Y; then continue; fi
                check WARN "microphone test: no speech heard" "run this script again and say the sentence while the meter moves"
                break
            fi
            check PASS "microphone test: your voice is recorded" ""
            note "speech heard: $CAL_SPEECH_S s; each speech model's transcript:"
            printf '%s\n' "$CAL_MODELS" | while IFS= read -r l; do note "  $l"; done
            [ -n "$CAL_ADVICE" ] && note "advice: $CAL_ADVICE"
            wer_pct=$(awk -v w="$CAL_BEST_WER" 'BEGIN { printf "%d", w * 100 + 0.5 }')
            if [ "$wer_pct" -le 34 ]; then
                ok "speech recognition: $CAL_RECOMMENDED understands you best ($wer_pct % of the words wrong)"
                check PASS "speech recognition ($CAL_RECOMMENDED)" ""
            else
                warn "speech recognition is poor for you: $CAL_RECOMMENDED got $wer_pct % of the words wrong"
                check WARN "speech recognition ($CAL_RECOMMENDED, $wer_pct % wrong)" "speak closer to the microphone, reduce background noise, run this script again"
            fi
            [ "$CAL_WAKE_HEARD" = 1 ] || note "(the wake word itself was not recognised; wake_sensitivity=high in assistant.conf makes it more lenient)"
            old=$(py conf-get "$CONF" voice stt_model auto)
            if [ "$old" = "$CAL_RECOMMENDED" ]; then
                ok "stt_model=$CAL_RECOMMENDED is already set"
            elif backup_conf_once && py conf-set "$CONF" voice stt_model "$CAL_RECOMMENDED" > /dev/null; then
                ok "saved stt_model=$CAL_RECOMMENDED in assistant.conf (was ${old:-auto})"
            else
                warn "could not save stt_model=$CAL_RECOMMENDED in $CONF"
            fi
            break
        done
    fi
fi

# ---------------------------------------------------------------------------
step "7/7  Start the assistant (listening for \"Shadow\")"
MIC_FAILED=0
for c in "${CHECKS[@]}"; do
    case "$c" in "FAIL|"*microphone*|"FAIL|default input"*) MIC_FAILED=1 ;; esac
done
stop_assistant
START_HOW=""
START_UNIT=""
if [ -z "$BIN" ]; then
    fail "shadow-assistant is not installed"
    check FAIL "assistant running" "build and install it (run this script from your checkout without --no-build)"
else
    timeout 60 "$BIN" --dump voice < /dev/null > "$TMPD/voice.json" 2> "$TMPD/voice.err"
    { read -r engine; read -r pexit; read -r preason; read -r wake; read -r muted; } < <(py field "$TMPD/voice.json" \
        engine probeExit probe.reason config.wakeEnabled config.micMuted 2>/dev/null)
    if [ "$engine" = neural ] && [ "$pexit" = 0 ]; then
        ok "$BIN --dump voice: engine \"neural\", probe ok"
        check PASS "assistant finds the voice engine" ""
    else
        fail "$BIN --dump voice: engine \"${engine:-?}\" (probe exit ${pexit:-?}${preason:+: $preason})"
        cp -f "$TMPD/voice.err" "$STATE_DIR/dump-voice.log" 2>/dev/null
        check FAIL "assistant finds the voice engine" "${preason:-see $STATE_DIR/dump-voice.log}; then run this script again"
    fi
    if [ "$wake" != true ] || [ "$muted" = true ]; then
        warn "not started in the background: $([ "$wake" != true ] && echo 'the wake word is off' || echo 'its microphone is muted')"
        note "open the assistant from the launcher; switch the wake word on in its settings to use \"Shadow\""
        check WARN "listening for \"Shadow\"" "switch the wake word on / the microphone on in the assistant's settings"
    elif [ "$engine" != neural ]; then
        check FAIL "listening for \"Shadow\"" "fix the engine (above) first"
    elif ! start_background; then
        fail "could not start $BIN --background"
        check FAIL "assistant running" "start it from the launcher; output: $STATE_DIR/assistant.log"
    else
        apid=""
        has_assistant() { [ -n "$(assistant_pids)" ]; }
        wait_until 10 has_assistant && apid=$(assistant_pids)
        apid=${apid%% *}
        armed=""
        armed_now() {   # the engine of $apid runs a recorder: the wake word listens
            local h c argv0
            for h in $(children_of "$apid"); do
                case "$(cmdline_of "$h")" in *shadow-voiced*--serve*) ;; *) continue ;; esac
                for c in $(children_of "$h"); do
                    # By name as started: pw-record, parec and arecord are
                    # symlinks to pw-cat, pacat and aplay (their exe).
                    argv0=$(cmdline_of "$c"); argv0=${argv0%% *}; argv0=${argv0##*/}
                    case "$argv0" in
                        pw-record|parec|arecord) armed="$h $c $argv0"; return 0 ;;
                    esac
                done
            done
            return 1
        }
        stopped_next() {   # the next step for an assistant that would not stay up, from its own words
            if start_output | grep -q 'module "org\.shadow\.\(ui\|style\)"'; then
                printf '%s' "the Shadow UI library it loads (package shadow-ui) is missing or too old: sudo apt install --reinstall shadow-ui"
            else
                printf '%s' "read its error above; start it from the launcher to see more"
            fi
        }
        if [ -z "$apid" ] || ! wait_until 60 armed_now; then
            if [ -n "$apid" ] && kill -0 "$apid" 2>/dev/null; then
                fail "the assistant runs (PID $apid) but its engine did not open the microphone within 60 s"
                grep -a "shadow-voiced" "$VOICE_LOG" 2>/dev/null | tail -n 4 | while IFS= read -r l; do note "$l"; done
                check FAIL "listening for \"Shadow\"" "see $VOICE_LOG"
            else
                fail "the assistant stopped right after starting; it said:"
                start_output | while IFS= read -r l; do note "  $l"; done
                check FAIL "assistant running" "$(stopped_next)"
            fi
        else
            read -r hpid rpid rexe <<<"$armed"
            ok "running in the background (PID $apid, $START_HOW)"
            if [ "$MIC_FAILED" = 1 ]; then
                # The engine records, but the checks above found no working microphone.
                warn "its engine records with ${rexe##*/} (PID $rpid), but from no working microphone (see above)"
                check WARN "assistant running; listening for \"Shadow\" needs a working microphone" "fix the microphone (above), then run this script again"
            else
                ok "listening for \"Shadow\": engine PID $hpid records with ${rexe##*/} (PID $rpid)"
                check PASS "assistant running and listening for \"Shadow\"" ""
            fi
        fi
    fi
fi

# ---------------------------------------------------------------------------
printf '\n%s== Summary%s\n' "$C_STEP" "$C_OFF"
for c in "${CHECKS[@]}"; do
    st=${c%%|*}; rest=${c#*|}; what=${rest%%|*}; next=${rest#*|}
    case "$st" in
        PASS) col=$C_OK ;; WARN) col=$C_WARN ;; FAIL) col=$C_FAIL ;; *) col="" ;;
    esac
    printf '  %s%-4s%s  %s\n' "$col" "$st" "$C_OFF" "$what"
    [ -n "$next" ] && [ "$st" != PASS ] && printf '        next: %s\n' "$next"
done
nfail=0 nwarn=0
for c in "${CHECKS[@]}"; do
    case "${c%%|*}" in FAIL) nfail=$((nfail + 1)) ;; WARN) nwarn=$((nwarn + 1)) ;; esac
done
printf '\n'
if [ "$nfail" -eq 0 ] && [ "$FAILS" -eq 0 ]; then
    printf '%s\n' 'PASS: the assistant'"'"'s voice works.' \
        '' \
        "  Say \"Shadow\" and then what you want, e.g. \"Shadow, what time is it?\"" \
        '  It answers aloud. Your voice is processed on this computer only; just the' \
        '  words of your request go to Claude, as text.'
    [ "$nwarn" -gt 0 ] && printf '  (%s WARN line(s) in the summary: worth a look.)\n' "$nwarn"
else
    printf '%sFAIL%s: %s problem(s); each FAIL above says what to do next. Then run this script again.\n' \
        "$C_FAIL" "$C_OFF" "$((nfail > FAILS ? nfail : FAILS))"
fi
[ -n "$BACKUP" ] && printf '\nBackup of your settings: %s\n' "$BACKUP"
[ -n "$LOG" ] && printf 'Log of this run: %s\n' "$LOG"

# Exit status 0 = PASS, 1 = FAIL; close_log (EXIT trap) flushes the log.
[ "$nfail" -eq 0 ] && [ "$FAILS" -eq 0 ]
