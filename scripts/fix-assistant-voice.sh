#!/usr/bin/env bash
# fix-assistant-voice.sh - make the Shadow Assistant hear "Shadow" and answer
# aloud on this computer, in one go, and prove that it does.
#
#     bash scripts/fix-assistant-voice.sh [--yes] [--no-build] [--no-calibrate]
#                                         [--build-dir DIR]
#
# Run it as your normal user (not with sudo) from a terminal inside the Shadow
# desktop, from your checkout of the Shadow OS sources (it finds the checkout
# from its own path). It asks for your password (sudo) only to install: build
# tools, the assistant, Debian packages and the voice engine data. You speak
# twice: a test sentence, then "Shadow, what time is it?" to the restarted
# assistant. It ends with a summary and one verdict:
#   PASS          every link was tested: your voice reached the engine, "Shadow"
#                 woke it, the restarted assistant answered aloud, and your
#                 session starts it at login (exit 0)
#   FAIL          something does not work; each FAIL says what to do (exit 1)
#   NOT VERIFIED  nothing failed, but a test was skipped, or a switch you set
#                 (the assistant's mute, its wake word) keeps it from
#                 listening; the summary says which (exit 3)
# Running it again is safe: it rebuilds and reinstalls only what changed (sudo
# may ask again), repeats the tests and restarts the assistant.
#
#   --yes           no questions: you speak after a countdown, and the safe
#                   defaults are taken (install what is missing, unmute the
#                   system microphone and speakers and raise a very low level,
#                   use a real microphone instead of a monitor). Your switches in
#                   the assistant (its mute, the wake word) are left as the
#                   assistant reads them, and your API key is never copied
#                   anywhere. Without --yes, Enter answers every question with
#                   what makes the voice work, except "Did you hear it?".
#   --no-build      use the installed assistant (it must have the new engine)
#   --no-calibrate  skip both tests where you speak (verdict: NOT VERIFIED)
#   --build-dir DIR the CMake build directory to use (default: build/cmake in
#                   the checkout or another configured one found in it; when
#                   there is none, build/cmake is created)
#
# Why the old voice failed, and what each step does about it:
#  1. The voice ran on pocketsphinx, whose Debian package has no acoustic model
#     without a "recommended" extra (Shadow installs without recommends), and
#     on speech-dispatcher, which hung ~30 s per reply. The new engine
#     (shadow-voiced: Silero VAD, a "Shadow" keyword spotter, Moonshine or
#     Whisper speech recognition, the Kokoro voice) runs offline from
#     /var/lib/shadow-voice.
#     -> Step 1 builds the assistant from this checkout (installing the build
#        tools and creating build/cmake when needed) and installs it into
#        /usr/local; steps 2-3 install the Debian packages it runs on and the
#        engine data (~700 MB, every file sha256-checked).
#  2. The settings template wrote "value  # comment" lines, which Qt read as
#     part of the value ("muted=false  # ..." meant MUTED).
#     -> Step 4 removes such comments (backup first). Lines holding your API
#        key or a password are never changed, shown or logged.
#  3. Laptop microphones are often a digital array behind the audio DSP (Intel
#     SOF): without firmware-sof-signed and alsa-ucm-conf there is no
#     microphone at all; a muted or near-silent input, or a "monitor" chosen as
#     the default input, looks like a deaf assistant.
#     -> Step 5 checks PipeWire, the default input and output, mute and level.
#  4. Nothing ever proved that the voice worked.
#     -> Step 6: the engine's self-test, a spoken greeting, and a test sentence
#        that you say: it must be understood, and "Shadow" must wake the
#        engine exactly as it will in use.
#     -> Step 7 starts the new assistant in the background (the API key must
#        reach it), you say "Shadow, what time is it?", and it must answer
#        aloud. Then the wake word is set up for every login.
#
# Everything is written to ~/.local/state/shadow-fix-assistant-voice/ (only you
# can read it): this run's log (with what the speech models heard you say), the
# build output, a backup of assistant.conf before any change. No audio is
# kept: the microphone tests record into memory only.

# Functions called only through wait_until or a trap look unreachable to shellcheck.
# shellcheck disable=SC2317
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
LISTEN_UNIT='app-org.shadow.assistant\x2dlisten@autostart.service'
USER_AUTOSTART=$CONF_HOME/autostart/$LISTEN_DESKTOP
SYSTEM_DATA=/var/lib/shadow-voice
# First line of every file this script writes.
MARK='# Written by Shadow OS scripts/fix-assistant-voice.sh.'
# Spoken by you in the microphone test: the wake word, then plain words that
# are no instruction (should an assistant listen anyway, it gets nothing to do).
SENTENCE='Shadow, the quick brown fox jumps over the lazy dog.'
CAL_SECS=7
WER_MAX=34          # % of the command words a model may get wrong
QUIET_DBFS=-30      # loudest sample of the test below this: the input is too quiet
ROUNDTRIP='Shadow, what time is it?'
GREETING="Hello, I'm Shadow. If you can hear me, my voice works."
# What the engine and this script run on. curl and bzip2: the engine download.
# pulseaudio-utils: pactl (the checks below) and the parec/pacat fallback.
PACKAGES="python3 python3-numpy espeak-ng pipewire-bin pipewire-pulse wireplumber pulseaudio-utils alsa-ucm-conf curl bzip2"
# What building the assistant needs on a Shadow OS (or Debian trixie) desktop:
# qt6-base-dev and the Qt tools come with qt6-declarative-dev. Measured on the
# 0.2.0 image (2026-11-21): 67 packages (4 of them a libc point update that
# libc6-dev pins), 57 MB to download, 261 MB on disk.
BUILD_PACKAGES="cmake ninja-build g++ qt6-declarative-dev qt6-svg-dev"
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
        -h|--help) sed -n '2,36p' "$SCRIPT"; exit 0 ;;
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
CHECKS=()        # "STATUS|what|next step|chain or side" for the summary
BACKUP=""
TEE_PID=""
TMPD=""
LOG=""
SUDO_STATE=""    # "", ok, fail
PREFIX=""        # install prefix of the assistant that runs
BIN=""           # the shadow-assistant binary that runs
HELPER=""        # its shadow-voiced
SETUP=""         # its shadow-voice-setup
BUILD=""         # the CMake build directory
BUILD_WHY=""
INPUT_DEVICE=""
INSTALLED_NEW=0  # this run installed the assistant from the checkout
STOPPED_CMDS=()  # command lines of the assistants this script stopped (restarted if needed)
STARTED_BG=0     # step 7 started the assistant in the background
WINDOW_CLOSED=0
START_HOW=""
START_UNIT=""
KEY_FORWARD=""   # name of the variable that holds the key, passed by name to the background assistant
LOGIN_KEY=""     # ok | missing (the assistant started at login has no API key)
MIC_OK=0         # the microphone test heard and understood you
SPEAK_REPLIES=true
WAKE_ON=""
MUTED=""
SENS=normal
ROUNDTRIP_OK=0
SPEAKERS_OK=0    # 1: the greeting was heard (or, with --yes, measured on a real output); 2: not confirmed
src=""           # the default input (PipeWire/PulseAudio source name)

LAST_FAIL=0      # the last line printed was a [FAIL]
step() { printf '\n%s== %s%s\n' "$C_STEP" "$*" "$C_OFF"; LAST_FAIL=0; }
ok()   { printf '  %s[ OK ]%s %s\n' "$C_OK" "$C_OFF" "$*"; LAST_FAIL=0; }
warn() { printf '  %s[WARN]%s %s\n' "$C_WARN" "$C_OFF" "$*"; LAST_FAIL=0; }
fail() { printf '  %s[FAIL]%s %s\n' "$C_FAIL" "$C_OFF" "$*"; FAILS=$((FAILS + 1)); LAST_FAIL=1; }
note() { printf '         %s\n' "$*"; }
die()  { printf '\n%sERROR:%s %s\n' "$C_FAIL" "$C_OFF" "$*"; exit 1; }
# One line of the final summary: PASS, WARN, FAIL, SKIP or OFF (switched off by
# you), what, next step. Every check is a link of the voice chain unless it is
# recorded with side(): a gap in the chain (WARN, SKIP, OFF) means NOT VERIFIED.
check() { CHECKS+=("$1|$2|${3:-}|chain"); }
side()  { CHECKS+=("$1|$2|${3:-}|side"); }

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

ask() {   # question default(Y|N|-) [answer without questions] -> 0 = yes; "-": y or n must be typed
    local q=$1 def=$2 auto=${3:-$2} a
    if [ "$INTERACTIVE" != 1 ]; then
        if [ "$auto" = Y ]; then note "$q -> yes (no questions asked)"; return 0; fi
        note "$q -> no (no questions asked)"
        return 1
    fi
    while :; do
        case "$def" in
            Y) printf '         %s [Y/n] ' "$q" ;;
            N) printf '         %s [y/N] ' "$q" ;;
            *) printf '         %s [y/n] ' "$q" ;;
        esac
        read -r a || a=""
        case "$a" in
            [Yy]*) return 0 ;;
            [Nn]*) return 1 ;;
            "") case "$def" in Y) return 0 ;; N) return 1 ;; esac ;;
            *) return 1 ;;
        esac
        printf '         (please answer y or n)\n'
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
# Root commands with umask 022 whatever yours is: files installed for every
# user (and the directories made for them) must be readable by everyone.
sudo_022() { sudo /bin/sh -c 'umask 022 && exec "$@"' sh "$@"; }

# apt without questions: no debconf dialogs, changed configuration files are
# kept, nothing is removed, and nothing reads the terminal.
apt_get() {
    sudo_022 env DEBIAN_FRONTEND=noninteractive apt-get -o DPkg::Lock::Timeout=120 \
        -o Dpkg::Options::=--force-confdef -o Dpkg::Options::=--force-confold "$@" < /dev/null
}
pkg_installed() { [ "$(dpkg-query -W -f='${db:Status-Abbrev}' "$1" 2>/dev/null)" = "ii " ]; }
pkg_available() { LC_ALL=C apt-cache policy "$1" 2>/dev/null | grep -q 'Candidate: [^(]'; }
apt_install() {   # log packages... : install, refreshing the package lists once if needed
    local logf=$1; shift
    if ! apt_get install -y --no-install-recommends --no-remove "$@" > "$logf" 2>&1; then
        note "refreshing the package lists and trying once more..."
        apt_get update >> "$logf" 2>&1 || true
        apt_get install -y --no-install-recommends --no-remove "$@" >> "$logf" 2>&1 || true
    fi
    local p
    for p in "$@"; do pkg_installed "$p" || return 1; done
}
free_mb() {   # MB free on the filesystem holding $1 (nearest existing parent)
    local d=$1
    while [ ! -d "$d" ]; do d=$(dirname -- "$d"); done
    df -P -m -- "$d" | awk 'NR == 2 { print $4 }'
}
same_fs() { [ "$(stat -c %d -- "$1")" = "$(stat -c %d -- "$2")" ]; }

backup_dir() {
    if [ -z "$BACKUP" ]; then
        BACKUP=$STATE_DIR/backup-$(date +%Y%m%d-%H%M%S)
        (umask 077 && mkdir -p "$BACKUP") || { fail "cannot create $BACKUP"; BACKUP=""; return 1; }
    fi
}
backup_conf_once() {   # copy assistant.conf (it holds the API key: mode 0600) before the first change
    [ -f "$CONF" ] || return 0
    [ -n "$BACKUP" ] && [ -f "$BACKUP/assistant.conf" ] && return 0
    # An earlier run's backup of exactly this file is kept, not copied again.
    local last="" b
    for b in "$STATE_DIR"/backup-*; do [ -d "$b" ] && last=$b; done
    if [ -n "$last" ] && cmp -s -- "$CONF" "$last/assistant.conf"; then
        BACKUP=$last
        return 0
    fi
    backup_dir || return 1
    install -m 600 -- "$CONF" "$BACKUP/assistant.conf"
}

# Python does the careful text work (INI with a secret in it, JSON, the level
# meter, the engine's log). It is the engine's own interpreter.
PYHELP=$(cat <<'PY'
import ast, json, math, os, re, sys, tempfile, time

# A line that holds (or held) a secret: never changed, never printed. Keys,
# tokens, passwords, and URLs with a user:password@ part.
SECRET_KEY = re.compile(r'^\s*[#;]?\s*[\w.-]*(key|token|secret|passw(or)?d|auth|credential)[\w.-]*\s*=', re.I)
SECRET_VALUE = re.compile(r'sk-ant-|\bsk-[A-Za-z0-9_-]{20,}|://[^/\s@]*@')
# The assistant's own rule (AssistantConfig::sanitizeValue): a comment starts
# at '#' or ';' at the start of the value or after a blank.
INLINE = re.compile(r'(?:^|\s)[#;].*$', re.S)


def secret(line):
    return bool(SECRET_KEY.search(line) or SECRET_VALUE.search(line))


def read_text(path):
    with open(path, 'rb') as f:
        return f.read().decode('utf-8', 'surrogateescape')


def replace_file(path, text):
    """Write atomically, mode 0600 (the file holds the API key). A symlink (dotfile
    managers) is kept: its target is what gets replaced."""
    path = os.path.realpath(path)
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


def cmd_conf_has(path, section, key):
    """exit 0 when a line sets KEY in SECTION to a non-empty value (the value is never printed)."""
    try:
        vals = parse(read_text(path))
    except FileNotFoundError:
        sys.exit(1)
    sys.exit(0 if vals.get((section, key)) else 1)


def set_line(text, section, key, value):
    """TEXT with KEY in [SECTION] set to VALUE; every other line (and the key line) kept as it is."""
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
        if cur == section and s and s[0] not in '#;' and '=' in s and (key == 'key' or not secret(line)):
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
    return ''.join(lines), old


def cmd_conf_set(path, section, key, value):
    """Set one value in [section], keeping every other line (and the key line) as it is."""
    if secret(key + '='):
        sys.exit('refusing to write a secret this way')
    try:
        text = read_text(path)
    except FileNotFoundError:
        text = ''
    new, old = set_line(text, section, key, value)
    replace_file(path, new)
    print('(not set)' if old is None else old)


def cmd_conf_key_from_env(path, var):
    """Add key=<value of $VAR> to [api] when the file has no key yet. Never prints the key;
    an existing key line is never changed."""
    value = os.environ.get(var, '').strip()
    if not value or '\n' in value:
        sys.exit('no usable value in ' + var)
    try:
        text = read_text(path)
    except FileNotFoundError:
        text = ''
    if parse(text).get(('api', 'key')):
        print('exists')
        return
    new, _old = set_line(text, 'api', 'key', value)
    replace_file(path, new)
    print('added')


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
    try:
        obj = load_json(path)
    except OSError:
        obj = None
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


def q(v):
    return "'" + str(v).replace("'", "'\\''") + "'"


def cmd_calib(path):
    """The microphone test result as KEY='value' lines for the shell."""
    o = load_json(path) or {}
    mic = o.get('mic') or {}
    models = o.get('models') or {}
    rec = o.get('recommended')
    wake = o.get('wake') or {}
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
    print('CAL_WAKE_OK=' + ('1' if wake.get('ok') else '0'))
    print('CAL_WAKE_HIGH=' + ('1' if wake.get('needs_high') else '0'))
    print('CAL_WAKE_HEARD=' + q(wake.get('heard') or ''))
    print('CAL_WAKE_VIA=' + q(' '.join(v for v in (wake.get('via') or []) if v)))
    print('CAL_WAKE_STT=' + q(wake.get('stt') or ''))
    print('CAL_ADVICE=' + q(' '.join(o.get('advice') or [])))
    lines = []
    for name in sorted(models, key=lambda k: (k != 'moonshine', k)):
        r = models[name]
        lines.append('%-9s heard: "%s"  (%s of %s command words wrong, %s ms)' % (
            name, r.get('text', ''), r.get('errors', '?'), r.get('words', '?'), r.get('stt_ms', '?')))
    print('CAL_MODELS=' + q('\n'.join(lines)))


def cmd_meter(seconds, tty):
    """Live input level from raw s16le mono 16 kHz on stdin, drawn on the terminal; prints the loudest 0.1 s."""
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


def cmd_voicelog(path, start, pid):
    """What the engine with PID logged after line START of voice.log, as KEY='value' lines."""
    tag = '[shadow-voiced/%s]' % pid
    ev = {'RT_WAKE_KWS': 0, 'RT_WAKE_STT': 0, 'RT_FINAL': 0, 'RT_REJECTED': 0, 'RT_TIMEOUT': 0, 'RT_SPEAK_ID': '',
          'RT_SPEAK_END': 0, 'RT_INTERRUPTED': 0, 'RT_MIC_STATUS': '', 'RT_MIC_MSG': '', 'RT_ERROR': '',
          'RT_FIRST_AUDIO_S': ''}
    t_final = None
    try:
        lines = read_text(path).splitlines()
    except (OSError, ValueError):
        lines = []
    # voice.log is rotated at 1 MB: a shorter file than START lines is a new one.
    lines = lines[int(start):] if int(start) <= len(lines) else lines
    for line in lines:
        if tag not in line:
            continue
        m = re.match(r'(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)', line)
        ts = time.mktime(time.strptime(m.group(1)[:19], '%Y-%m-%d %H:%M:%S')) + float('0' + m.group(1)[19:]) if m else None
        body = line.split(tag, 1)[1]
        mm = re.search(r': (\w+) (\{.*\})\s*$', body)
        fields = {}
        if mm:
            try:
                fields = ast.literal_eval(mm.group(2))
            except (ValueError, SyntaxError):
                fields = {}
        if ' final id=wake' in body:
            ev['RT_FINAL'] = 1
            t_final = ts
        elif mm and mm.group(1) == 'wake':
            ev['RT_WAKE_STT' if fields.get('via') == 'stt' else 'RT_WAKE_KWS'] = 1
        elif mm and mm.group(1) == 'wake_rejected':
            ev['RT_REJECTED'] += 1
        elif mm and mm.group(1) == 'timeout' and fields.get('id') == 'wake':
            ev['RT_TIMEOUT'] = 1
        elif mm and mm.group(1) == 'speak_start' and not ev['RT_SPEAK_ID'] and ev['RT_FINAL']:
            ev['RT_SPEAK_ID'] = str(fields.get('id', ''))
            if t_final is not None and ts is not None:
                ev['RT_FIRST_AUDIO_S'] = '%.1f' % (ts - t_final)
        elif mm and mm.group(1) == 'speak_end' and ev['RT_SPEAK_ID'] and fields.get('id') == ev['RT_SPEAK_ID']:
            ev['RT_SPEAK_END'] = 1
            ev['RT_INTERRUPTED'] = 1 if fields.get('interrupted') else 0
        elif mm and mm.group(1) == 'mic':
            ev['RT_MIC_STATUS'] = str(fields.get('status', ''))
            ev['RT_MIC_MSG'] = str(fields.get('message', ''))
        elif mm and mm.group(1) == 'error':
            ev['RT_ERROR'] = str(fields.get('message', ''))
    for k, v in ev.items():
        print(k + '=' + q(v))


cmd, args = sys.argv[1], sys.argv[2:]
{'conf-repair': cmd_conf_repair, 'conf-get': cmd_conf_get, 'conf-has': cmd_conf_has, 'conf-set': cmd_conf_set,
 'conf-key-from-env': cmd_conf_key_from_env, 'field': cmd_field, 'manifest': cmd_manifest,
 'devices': cmd_devices, 'calib': cmd_calib, 'meter': cmd_meter, 'voicelog': cmd_voicelog}[cmd](*args)
PY
)
py() { python3 -I -B -c "$PYHELP" "$@"; }

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
# The voice processes of an assistant: its engine and the recorders / speech
# tools of the old voice. Anything else it started (a copy, a scan, a capture
# you asked for) is left running.
voice_child() {
    local argv0
    argv0=$(cmdline_of "$1"); argv0=${argv0%% *}; argv0=${argv0##*/}
    case "$argv0" in
        python3*|python) case "$(cmdline_of "$1")" in *shadow-voiced*) return 0 ;; esac; return 1 ;;
        shadow-voiced|pocketsphinx*|parecord|parec|pw-record|pw-cat|arecord|spd-say|espeak-ng|pw-play|pacat|paplay|aplay) return 0 ;;
    esac
    return 1
}

# Stop every running copy of the assistant (and so its engine). Called before
# files are replaced, before the microphone tests (no copy may hear the test
# sentences) and before the new one is started; each call looks again.
stop_assistant() {   # why
    local pids hp kids="" others="" p c cmd args
    pids=$(assistant_pids)
    hp=$(helper_pids)
    [ -z "$pids" ] && [ -z "$hp" ] && return 0
    for p in $pids; do
        cmd=$(cmdline_of "$p")
        [ -n "$cmd" ] || continue
        # "EXE ARGS" (cmdline_of ends with a blank: the arguments without the program name)
        args=${cmd#* }
        STOPPED_CMDS+=("$(readlink "/proc/$p/exe" 2>/dev/null | sed 's/ (deleted)$//') ${args% }")
        case " $cmd " in *" --background "*) ;; *) WINDOW_CLOSED=1 ;; esac
        for c in $(children_of "$p"); do
            if voice_child "$c"; then kids="$kids $c"; else others="$others $(cmdline_of "$c" | cut -c1-60);"; fi
        done
    done
    note "stopping the running assistant (PID ${pids:-none}) ${1:-}"
    [ -z "$others" ] || note "left running (started by the assistant for you):${others% ;}"
    if [ -n "$pids" ]; then
        # shellcheck disable=SC2086
        kill -TERM $pids 2>/dev/null
        # shellcheck disable=SC2086
        if ! wait_until 8 pids_gone $pids; then
            # shellcheck disable=SC2086
            kill -KILL $pids 2>/dev/null
            # shellcheck disable=SC2086
            wait_until 3 pids_gone $pids || warn "could not stop shadow-assistant (PID $pids)"
        fi
    fi
    # The engine leaves when its input closes and old voice children usually
    # end with their parent; any that do not are stopped.
    hp="$(helper_pids) ${kids# }"
    hp=${hp# }
    # shellcheck disable=SC2086
    if [ -n "$hp" ] && ! wait_until 5 pids_gone $hp; then
        # shellcheck disable=SC2086
        kill -TERM $hp 2>/dev/null
        # shellcheck disable=SC2086
        wait_until 5 pids_gone $hp || kill -KILL $hp 2>/dev/null
    fi
    # A transient unit of an earlier run is gone with its process; nothing to clean.
    return 0
}

SD_USER=0
if command -v systemd-run >/dev/null 2>&1 && systemctl --user show-environment >/dev/null 2>&1; then SD_USER=1; fi
# The display variables the background assistant needs (a unit started by the
# user manager has the session's, but this terminal's are the ones that work).
display_envs() {
    local v
    DISPLAY_ENVS=()
    for v in WAYLAND_DISPLAY DISPLAY XAUTHORITY XDG_CURRENT_DESKTOP XDG_SESSION_TYPE XDG_SESSION_DESKTOP \
             XDG_MENU_PREFIX QT_QPA_PLATFORM QT_QPA_PLATFORMTHEME DBUS_SESSION_BUS_ADDRESS; do
        if [ -n "${!v:-}" ]; then DISPLAY_ENVS+=("--setenv=$v=${!v}"); fi
    done
    # The key, when it must be passed: by NAME only (systemd-run takes the value
    # from its own environment), so it never appears on a command line.
    if [ -n "$KEY_FORWARD" ]; then DISPLAY_ENVS+=("--setenv=$KEY_FORWARD"); fi
}
# What an assistant started the way step 7 starts it reads from its settings
# and environment (only whether a key is configured, never the key).
bg_dump_config() {   # outfile
    if [ "$SD_USER" = 1 ]; then
        display_envs
        timeout 90 systemd-run --user --wait --pipe --quiet --collect "${DISPLAY_ENVS[@]}" \
            "$BIN" --dump config < /dev/null > "$1" 2>/dev/null
    else
        timeout 60 "$BIN" --dump config < /dev/null > "$1" 2>/dev/null
    fi
}
# Start the assistant detached, listening for the wake word. A transient user
# service (systemd-run) survives closing this terminal and logs to the
# journal; without a systemd user manager it is detached with setsid.
start_background() {
    local unit
    if [ "$SD_USER" = 1 ]; then
        display_envs
        unit=shadow-assistant-wake-$(date +%s)
        if systemd-run --user --quiet --collect --unit="$unit" \
                --description="Shadow Assistant listening for the wake word (started by fix-assistant-voice.sh)" \
                "${DISPLAY_ENVS[@]}" "$BIN" --background < /dev/null >> "$STATE_DIR/assistant.log" 2>&1; then
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
start_detached() {   # command... : survives this terminal
    if [ "$SD_USER" = 1 ]; then
        display_envs
        systemd-run --user --quiet --collect "${DISPLAY_ENVS[@]}" "$@" < /dev/null > /dev/null 2>&1 && return 0
    fi
    setsid -f "$@" < /dev/null >> "$STATE_DIR/assistant.log" 2>&1
}

# On every exit (also Ctrl+C or a closed terminal): an assistant this script
# stopped is started again unless the new one runs in the background now.
restore_assistants() {
    [ "${#STOPPED_CMDS[@]}" -gt 0 ] || return 0
    if [ "$STARTED_BG" = 1 ]; then
        [ "$WINDOW_CLOSED" = 1 ] && printf '\nThe assistant window you had open was closed: open it again from the launcher (or say "Shadow").\n'
        return 0
    fi
    local c exe args again="" quiet=0
    for c in "${STOPPED_CMDS[@]}"; do
        exe=${c%% *}
        args=${c#"$exe"}; args=${args# }
        # A background copy exits at once while the wake word is off or the assistant muted.
        case " $args " in
            *" --background "*) if [ "$WAKE_ON" = false ] || [ "$MUTED" = true ]; then quiet=1; continue; fi ;;
        esac
        # The copy installed by this run replaces the one that ran before.
        if [ "$INSTALLED_NEW" = 1 ] && [ -x "$BIN" ]; then exe=$BIN; fi
        [ -x "$exe" ] || continue
        # shellcheck disable=SC2086
        start_detached "$exe" $args && again="$again $exe${args:+ $args};"
    done
    if [ -n "$again" ]; then
        printf '\nThe assistant this script had stopped was started again:%s\n' "${again%;}"
    elif [ "$quiet" = 1 ]; then
        printf '\nThe assistant is not running in the background: its wake word is off or it is muted (see above).\n'
    else
        printf '\nThe assistant was stopped by this script: start it from the launcher, or log out and in.\n'
    fi
}
on_exit() {
    local rc=$?
    trap - EXIT INT TERM HUP
    trap '' PIPE
    restore_assistants
    [ "$SUDO_STATE" = ok ] && sudo -k 2>/dev/null
    [ -z "$TMPD" ] || rm -rf -- "$TMPD"
    if [ -n "$TEE_PID" ]; then
        # Hand the rest of the log to the terminal; never wait for more than two seconds.
        exec >&- 2>&-
        local n=40
        while [ "$n" -gt 0 ] && kill -0 "$TEE_PID" 2>/dev/null; do sleep 0.05; n=$((n - 1)); done
    fi
    exit "$rc"
}
on_signal() { trap '' PIPE; printf '\n(interrupted)\n'; exit 130; }

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

# ---- the build directory ---------------------------------------------------------
tree_configured() { [ -f "$1/build.ninja" ] || [ -f "$1/Makefile" ]; }
# The configured CMake build directory of this checkout -> BUILD (or BUILD_WHY).
find_build_dir() {
    local c home cands=() ignored=""
    BUILD="" BUILD_WHY=""
    if [ -n "$BUILD_ARG" ]; then
        cands=("$BUILD_ARG")
    else
        cands=("$REPO/build/cmake" "$REPO/build")
        for c in "$REPO"/*/CMakeCache.txt "$REPO"/build/*/CMakeCache.txt; do
            [ -f "$c" ] || continue
            c=${c%/CMakeCache.txt}
            case " ${cands[*]} " in *" $c "*) ;; *) cands+=("$c") ;; esac
        done
    fi
    for c in "${cands[@]}"; do
        if [ ! -f "$c/CMakeCache.txt" ]; then
            [ -n "$BUILD_ARG" ] && BUILD_WHY="$c has no CMakeCache.txt (not a configured build directory)"
            continue
        fi
        home=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$c/CMakeCache.txt")
        if [ "$(readlink -f -- "$home" 2>/dev/null)" != "$REPO" ]; then
            [ -n "$BUILD_ARG" ] && BUILD_WHY="$c was configured for $home, not for $REPO"
            continue
        fi
        if ! tree_configured "$c"; then
            # e.g. an interrupted or failed "cmake -S . -B ...": nothing to build with
            [ -n "$BUILD_ARG" ] && BUILD_WHY="$c: its configuration never finished (no build.ninja or Makefile)"
            ignored="$ignored $c"
            continue
        fi
        BUILD=$c
        return 0
    done
    [ -z "$ignored" ] || note "ignored (configuration never finished, no build.ninja or Makefile):$ignored"
    return 1
}

# cmake --build output: everything to build.log, on the terminal one line per
# 10 % and the error lines.
build_progress() {
    local line pct shown=-1 n t
    while IFS= read -r line; do
        printf '%s\n' "$line" >> "$STATE_DIR/build.log"
        pct="" n=""
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

# The summary, the verdict, and the end of the run (exit status 0/1/3).
finish() {
    printf '\n%s== Summary%s\n' "$C_STEP" "$C_OFF"
    local c st rest what next kind col nfail=0 gaps=() off=0 nside=0
    for c in "${CHECKS[@]}"; do
        st=${c%%|*}; rest=${c#*|}; what=${rest%%|*}; rest=${rest#*|}; next=${rest%%|*}; kind=${rest##*|}
        case "$st" in
            PASS) col=$C_OK ;; WARN|OFF|SKIP) col=$C_WARN ;; FAIL) col=$C_FAIL ;; *) col="" ;;
        esac
        printf '  %s%-4s%s  %s\n' "$col" "$st" "$C_OFF" "$what"
        [ -n "$next" ] && [ "$st" != PASS ] && printf '        next: %s\n' "$next"
        case "$st" in
            FAIL) nfail=$((nfail + 1)) ;;
            WARN|SKIP|OFF)
                if [ "$kind" = chain ]; then gaps+=("$what"); [ "$st" = OFF ] && off=1; else nside=$((nside + 1)); fi ;;
        esac
    done
    printf '\n'
    local rc
    if [ "$nfail" -gt 0 ] || [ "$FAILS" -gt 0 ]; then
        printf '%sFAIL%s: %s problem(s); each FAIL above says what to do next. Then run this script again.\n' \
            "$C_FAIL" "$C_OFF" "$((nfail > FAILS ? nfail : FAILS))"
        rc=1
    elif [ "${#gaps[@]}" -gt 0 ] || [ "$ROUNDTRIP_OK" != 1 ]; then
        [ "$ROUNDTRIP_OK" = 1 ] || [ "${#gaps[@]}" -gt 0 ] || gaps+=("\"$ROUNDTRIP\" to the background assistant was not tested")
        if [ "$off" = 1 ]; then
            printf '%sNOT VERIFIED%s: nothing failed, but the assistant is not listening for "Shadow":\n' "$C_WARN" "$C_OFF"
        else
            printf '%sNOT VERIFIED%s: nothing failed, but not every part was tested:\n' "$C_WARN" "$C_OFF"
        fi
        for c in "${gaps[@]}"; do printf '  - %s\n' "$c"; done
        printf '  Each line above says what to do; then run this script again.\n'
        rc=3
    else
        printf '%sPASS%s: the assistant heard you and answered aloud.\n\n' "$C_OK" "$C_OFF"
        printf '%s\n' "  Say \"Shadow\" and then what you want, e.g. \"$ROUNDTRIP\"" \
            '  It listens for "Shadow" from login on. Your voice is processed on this' \
            '  computer only; just the words of your request go to Claude, as text. To' \
            "  stop it listening, switch \"Listen for Shadow\" off in the assistant's settings."
        [ "$nside" -gt 0 ] && printf '  (%s WARN line(s) in the summary: worth a look.)\n' "$nside"
        rc=0
    fi
    if [ "$INSTALLED_NEW" = 1 ] && [ "$PREFIX" = /usr ]; then
        printf '\n'
        printf '%s\n' "The assistant built from your checkout replaced the files of the shadow-assistant" \
            "package in /usr; its next package update puts the packaged ones back."
    elif [ "$INSTALLED_NEW" = 1 ]; then
        printf '\n'
        printf '%s\n' "The assistant runs from $PREFIX, built from your checkout: it takes precedence over" \
            "the shadow-assistant package, also after package updates. Back to the package:" \
            "  sudo xargs -d '\\n' rm -f < $STATE_DIR/install_manifest_shadow-assistant.txt; rm -f $USER_AUTOSTART"
    fi
    [ -n "$BACKUP" ] && printf '\nBackup of your settings: %s\n' "$BACKUP"
    [ -n "$LOG" ] && printf 'Log of this run: %s\n' "$LOG"
    exit "$rc"
}
# Stop here: a link everything after it depends on is missing.
abort_run() {   # what next
    [ "$LAST_FAIL" = 1 ] || printf '  %s[FAIL]%s %s\n' "$C_FAIL" "$C_OFF" "$1"
    check FAIL "$1" "$2"
    note "(the steps after this one need it: stopping here)"
    finish
}

# ===========================================================================
[ "$(id -u)" -ne 0 ] || die "Run this as your normal user, not with sudo or as root: it fixes your own assistant and session (it asks for sudo itself where needed)."

if (umask 077 && mkdir -p "$STATE_DIR" && chmod 700 "$STATE_DIR" && : > "$STATE_DIR/last-run.log") 2>/dev/null; then
    LOG=$STATE_DIR/last-run.log
    chmod 600 "$LOG"
    # Terminal: as is (colours, prompts without a newline). Log: plain text. The copy
    # ignores Ctrl+C (it reaches the whole terminal's process group), so what this
    # script says after an interruption still reaches you and the log.
    exec > >(trap '' INT TERM; exec tee >(sed -u 's/\x1b\[[0-9;]*m//g' >> "$LOG")) 2>&1
    TEE_PID=$!
fi
trap on_exit EXIT
trap on_signal INT TERM HUP
TMPD=$(mktemp -d "${TMPDIR:-/tmp}/shadow-fix-voice.XXXXXX") || die "cannot create a temporary directory"
printf 'Shadow OS: fix the assistant voice (%s)\n' "$(date)"
[ "$YES" = 1 ] && printf '(--yes: no questions; safe defaults)\n'
[ "$YES" = 0 ] && [ "$INTERACTIVE" = 0 ] && printf '(no terminal for questions: safe defaults, as with --yes)\n'

# ---------------------------------------------------------------------------
step "0/7  Where this runs"
[ -f "$REPO/applications/assistant/voice/shadow-voice-setup" ] \
    || die "$SCRIPT is not inside a Shadow OS checkout with the new voice engine (no applications/assistant/voice/)."
for c in sudo dpkg-query apt-get readlink sed awk grep tr setsid timeout install cmp df stat; do
    command -v "$c" >/dev/null 2>&1 || die "The command '$c' is missing."
done
command -v python3 >/dev/null 2>&1 || die "python3 is missing: sudo apt install python3, then run this script again."
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
if [ "$SD_USER" = 1 ]; then ok "systemd user manager: yes (the assistant is started as a user service)"
else note "no systemd user manager: the assistant is started as a detached process"; fi
case "$(uname -m)" in
    x86_64|amd64) ;;
    *) die "This is a $(uname -m) computer: the offline voice engine exists for x86-64 (amd64) only." ;;
esac

# What an assistant binary is: the new one (with the offline engine), and where.
voice_dump() {   # bin outfile -> 0 when it is the new assistant (it reports its engine helper)
    timeout 60 "$1" --dump voice < /dev/null > "$2" 2> "$2.err" || return 1
    [ -n "$(py field "$2" helper 2>/dev/null)" ]
}

# ---------------------------------------------------------------------------
step "1/7  The assistant: build it from this checkout and install it"
if [ "$DO_BUILD" = 0 ]; then
    note "not building (--no-build): using the installed assistant"
    # The first installed copy that is the new assistant (an old one may come first on PATH).
    BIN="" old=""
    for c in "$(command -v shadow-assistant 2>/dev/null)" /usr/local/bin/shadow-assistant /usr/bin/shadow-assistant; do
        if [ -z "$c" ] || [ ! -x "$c" ] || [ -n "$BIN" ]; then continue; fi
        if voice_dump "$c" "$TMPD/voice.json"; then BIN=$c; else old=${old:-$c}; fi
    done
    if [ -z "$BIN" ] && [ -n "$old" ]; then
        abort_run "the installed assistant ($old) is an old one, without the offline voice engine" \
            "run this script without --no-build: it builds the new one from this checkout and installs it"
    fi
    [ -n "$BIN" ] || abort_run "an installed assistant" "run this script without --no-build: it builds and installs it"
    PREFIX=$(dirname -- "$(dirname -- "$BIN")")
    ok "installed: $BIN (it has the offline voice engine)"
    check PASS "the assistant with the offline voice engine ($BIN)" ""
else
    NEW_TREE=0
    if ! find_build_dir; then
        if [ -n "$BUILD_ARG" ]; then
            abort_run "build directory: ${BUILD_WHY:-$BUILD_ARG is not usable}" \
                "pass the build directory you configured for $REPO, or leave --build-dir out: the script then creates build/cmake"
        fi
        BUILD=$REPO/build/cmake
        NEW_TREE=1
        note "this checkout has no configured build directory yet: creating $BUILD"
        note "(only the assistant: -DSHADOW_ONLY=assistant, RelWithDebInfo, installs into /usr/local)"
    else
        ok "build directory: $BUILD"
    fi
    # The tools: everything for a new tree; cmake, its generator and a compiler for an existing one.
    need=$BUILD_PACKAGES
    if [ "$NEW_TREE" = 0 ]; then
        need="cmake g++"
        grep -qs '^CMAKE_GENERATOR:INTERNAL=Ninja' "$BUILD/CMakeCache.txt" && need="$need ninja-build"
        grep -qs '^CMAKE_GENERATOR:INTERNAL=Unix Makefiles' "$BUILD/CMakeCache.txt" && need="$need make"
    fi
    missing=""
    for p in $need; do pkg_installed "$p" || missing="${missing:+$missing }$p"; done
    if [ -n "$missing" ]; then
        note "to build it, these Debian packages are missing: $missing"
        [ "$NEW_TREE" = 1 ] && note "(with what they pull in, about 70 packages: ~60 MB to download, ~300 MB on disk)"
        usr_free=$(free_mb /usr) repo_free=$(free_mb "$REPO")
        if same_fs /usr "$REPO"; then want_usr=750; want_repo=0; else want_usr=400; want_repo=350; fi
        if [ "$usr_free" -lt "$want_usr" ] || [ "$repo_free" -lt "$want_repo" ]; then
            abort_run "disk space to build the assistant (free: $usr_free MB for /usr, $repo_free MB for $REPO)" \
                "free about 750 MB (plus about 1.4 GB for the voice engine), then run this script again"
        fi
        if ! ask "Install them now (apt, without recommended extras; nothing is removed)?" Y; then
            abort_run "build tools not installed (you said no)" "sudo apt install --no-install-recommends $missing; then run this script again"
        fi
        need_sudo || abort_run "build tools: sudo is needed to install them" "run this script again from a terminal where you can enter your password"
        t0=$(date +%s)
        # shellcheck disable=SC2086
        if apt_install "$STATE_DIR/apt-build.log" $missing; then
            ok "installed in $(( $(date +%s) - t0 )) s: $missing"
        else
            tail -n 4 "$STATE_DIR/apt-build.log" | while IFS= read -r l; do note "$l"; done
            abort_run "build tools: apt could not install them (log: $STATE_DIR/apt-build.log)" \
                "sudo apt update && sudo apt install --no-install-recommends $missing; then run this script again"
        fi
    fi
    # Files of an earlier "sudo cmake ..." in the build directory belong to root:
    # the build (as you) could not write them.
    if [ -d "$BUILD" ] && [ -n "$(find "$BUILD" ! -user "$(id -u)" -print -quit 2>/dev/null)" ]; then
        warn "some files in $BUILD belong to another user (root, from an earlier sudo build?)"
        if ask "Give them back to you (sudo chown -R $(id -un): $BUILD)?" Y \
                && need_sudo && sudo chown -R "$(id -u):$(id -g)" -- "$BUILD"; then
            ok "$BUILD belongs to you again"
        else
            abort_run "build directory not writable by you" "sudo chown -R $(id -un): $BUILD; then run this script again"
        fi
    fi
    if [ "$NEW_TREE" = 1 ]; then
        gen="Unix Makefiles"; command -v ninja >/dev/null 2>&1 && gen=Ninja
        note "configuring ($gen; output: $STATE_DIR/configure.log)"
        t0=$(date +%s)
        if ! cmake -S "$REPO" -B "$BUILD" -G "$gen" -DSHADOW_ONLY=assistant -DCMAKE_BUILD_TYPE=RelWithDebInfo \
                -DCMAKE_INSTALL_PREFIX=/usr/local < /dev/null > "$STATE_DIR/configure.log" 2>&1; then
            grep -E -A3 'CMake Error|Could NOT find|Failed to find' "$STATE_DIR/configure.log" | head -n 12 | while IFS= read -r l; do note "$l"; done
            abort_run "configuring the build (cmake) failed" "see $STATE_DIR/configure.log"
        fi
        ok "configured $BUILD ($(( $(date +%s) - t0 )) s)"
    fi
    PREFIX=$(sed -n 's/^CMAKE_INSTALL_PREFIX:[A-Z]*=//p' "$BUILD/CMakeCache.txt")
    PREFIX=${PREFIX%/}
    [ -n "$PREFIX" ] || PREFIX=/usr/local
    jobs=$(nproc 2>/dev/null || echo 2)
    mem=$(awk '/^MemAvailable:/ { print int($2 / 1024) }' /proc/meminfo 2>/dev/null)
    maxjobs=$(( ${mem:-2000} / 900 )); [ "$maxjobs" -ge 1 ] || maxjobs=1
    [ "$jobs" -le "$maxjobs" ] || jobs=$maxjobs
    : > "$STATE_DIR/build.log"
    note "building shadow-assistant with $jobs jobs (a few minutes the first time; full output: $STATE_DIR/build.log)"
    t0=$(date +%s)
    cmake --build "$BUILD" --target shadow-assistant -j"$jobs" < /dev/null 2>&1 | build_progress
    rc=${PIPESTATUS[0]}
    if [ "$rc" -ne 0 ]; then
        fail "the build failed (exit $rc)"
        grep -E 'error|Error|FAILED|denied|not found' "$STATE_DIR/build.log" | head -n 12 | while IFS= read -r l; do note "$l"; done
        abort_run "building the assistant" "fix the error above (full output: $STATE_DIR/build.log) and run this script again"
    fi
    ok "built $BUILD/bin/shadow-assistant ($(( $(date +%s) - t0 )) s)"
    # Install only what differs: a staged install (as you) is compared with the installed
    # files. Its file list is also the list of what gets installed (kept for removing it);
    # CMake's own install manifest in the build directory is put back afterwards.
    m=$BUILD/install_manifest_shadow-assistant.txt
    rm -rf "$TMPD/stage" "$TMPD/manifest.saved"
    [ -f "$m" ] && cp -p -- "$m" "$TMPD/manifest.saved"
    changed=0 nstaged=0
    : > "$TMPD/staged.list"
    if DESTDIR=$TMPD/stage cmake --install "$BUILD" --component shadow-assistant < /dev/null > "$STATE_DIR/stage.log" 2>&1; then
        while IFS= read -r -d '' f; do
            nstaged=$((nstaged + 1))
            printf '%s\n' "${f#"$TMPD/stage"}" >> "$TMPD/staged.list"
            cmp -s -- "$f" "${f#"$TMPD/stage"}" || changed=$((changed + 1))
        done < <(find "$TMPD/stage" \( -type f -o -type l \) -print0)
        [ "$nstaged" -gt 0 ] || changed=1
    else
        changed=1
    fi
    rm -rf "$TMPD/stage"
    if [ -f "$TMPD/manifest.saved" ]; then cp -p -- "$TMPD/manifest.saved" "$m" 2>/dev/null; else rm -f -- "$m" 2>/dev/null; fi
    if [ "$changed" = 0 ]; then
        ok "already installed in $PREFIX and up to date ($nstaged files): nothing to install"
        INSTALLED_NEW=1
    else
        if [ "$PREFIX" != /usr ] && [ ! -e "$PREFIX/bin/shadow-assistant" ]; then
            note "it is installed into $PREFIX: there it takes precedence over the shadow-assistant"
            note "package, also after package updates (the summary says how to undo that)"
            if ! ask "Install it into $PREFIX?" Y; then
                abort_run "the new assistant was not installed (you said no)" "run this script again and answer yes"
            fi
        fi
        need_sudo || abort_run "install the assistant: sudo is needed for $PREFIX" "run this script again from a terminal where you can enter your password"
        stop_assistant "(it is started again at the end)"
        if sudo_022 cmake --install "$BUILD" --component shadow-assistant < /dev/null > "$STATE_DIR/install.log" 2>&1; then
            n_new=$(grep -c '^-- Installing:' "$STATE_DIR/install.log")
            n_same=$(grep -c '^-- Up-to-date:' "$STATE_DIR/install.log")
            ok "installed into $PREFIX ($n_new files new or changed, $n_same up to date)"
            INSTALLED_NEW=1
        else
            tail -n 5 "$STATE_DIR/install.log" | while IFS= read -r l; do note "$l"; done
            abort_run "installing the assistant (sudo cmake --install) failed" "see $STATE_DIR/install.log"
        fi
    fi
    # CMake's install manifest (written by root) back to you; the list of the installed
    # files kept for removing them later (the summary says how).
    if [ -f "$m" ] && [ ! -O "$m" ]; then sudo -n chown "$(id -u):$(id -g)" -- "$m" 2>/dev/null; fi
    [ -s "$TMPD/staged.list" ] && cp -f -- "$TMPD/staged.list" "$STATE_DIR/install_manifest_shadow-assistant.txt"
    BIN=$PREFIX/bin/shadow-assistant
    [ -x "$BIN" ] || abort_run "the assistant is not at $BIN after the install" "see $STATE_DIR/install.log"
    if ! voice_dump "$BIN" "$TMPD/voice.json"; then
        sed -n '1,5p' "$TMPD/voice.json.err" 2>/dev/null | while IFS= read -r l; do note "$l"; done
        if grep -qs 'module "org\.shadow\.\(ui\|style\)"\|libshadow' "$TMPD/voice.json.err"; then
            abort_run "the new assistant does not start: the Shadow UI library it needs (package shadow-ui) is missing or too old" \
                "sudo apt update && sudo apt install --only-upgrade shadow-ui; then run this script again"
        fi
        abort_run "the new assistant ($BIN) does not report its voice engine" \
            "this checkout predates the new voice engine: git pull, then run this script again"
    fi
    check PASS "build and install the assistant ($PREFIX)" ""
fi
HELPER=$(py field "$TMPD/voice.json" helper)
SETUP=""
for c in "$PREFIX/bin/shadow-voice-setup" "$(py field "$TMPD/voice.json" setupTool)"; do
    if [ -z "$SETUP" ] && [ -n "$c" ] && [ -x "$c" ] && [ "$(stat -c %u -- "$c")" = 0 ]; then SETUP=$c; fi
done
if [ -z "$HELPER" ] || [ ! -x "$HELPER" ]; then
    abort_run "the assistant's voice engine (shadow-voiced) was not found" "see $TMPD/voice.json"
fi
ok "voice engine: $HELPER"
if [ "$INSTALLED_NEW" = 1 ] && [ "$PREFIX" != /usr ] && pkg_installed shadow-assistant; then
    pkgver=$(dpkg-query -W -f='${Version}' shadow-assistant 2>/dev/null)
    ourver=$(sed -n 's/^SHADOW_VERSION="\{0,1\}\([0-9.]*\)"\{0,1\}.*/\1/p' "$REPO/distro/base/shadow.conf" 2>/dev/null)
    if [ -n "$pkgver" ] && [ -n "$ourver" ] && dpkg --compare-versions "${pkgver%%.build*}" gt "$ourver"; then
        warn "the installed shadow-assistant package ($pkgver) is newer than this checkout ($ourver)"
        side WARN "the shadow-assistant package ($pkgver) is newer than this checkout ($ourver)" \
            "git pull and run this script again, or go back to the package (the command is below)"
    fi
fi
onpath=$(command -v shadow-assistant 2>/dev/null || true)
if [ -n "$onpath" ] && [ "$(readlink -f -- "$onpath")" != "$(readlink -f -- "$BIN")" ]; then
    warn "the launcher starts $onpath, not $BIN (it comes first on your PATH)"
    if dpkg -S "$onpath" >/dev/null 2>&1 || dpkg -S "$(readlink -f -- "$onpath")" >/dev/null 2>&1; then
        # a package's file: renaming it would break the package; the PATH decides
        fixit="put $(dirname -- "$BIN") before $(dirname -- "$onpath") on your PATH (in ~/.profile), then log out and in"
    else
        fixit="rename it (mv $onpath $onpath.old) or put $(dirname -- "$BIN") first on your PATH"
    fi
    side WARN "the launcher starts another copy: $onpath" "it runs whenever no assistant runs in the background: $fixit"
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
            side WARN "SOF microphone firmware" "add non-free-firmware to /etc/apt/sources.list.d/debian.sources, then: sudo apt install firmware-sof-signed and reboot"
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
            # shellcheck disable=SC2086
            apt_install "$STATE_DIR/apt.log" $missing
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
        check WARN "SOF microphone firmware: just installed" "reboot, then run this script again"
    fi
fi

# ---------------------------------------------------------------------------
step "3/7  Voice engine data ($SYSTEM_DATA)"
if [ -z "$SETUP" ]; then
    fail "the installed shadow-voice-setup was not found (next to $BIN, owned by root)"
    check FAIL "voice engine data" "install the assistant from this checkout (run this script without --no-build)"
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
            note "(only the damaged parts are downloaded again)"
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
        # --no-selftest: the engine is tested in step 6, as you, not as root.
        sudo_022 "$SETUP" --system --stt both --yes --no-selftest --progress-json < /dev/null 2>>"$STATE_DIR/engine-setup.log" | setup_progress
        rc=${PIPESTATUS[0]}
        if [ "$rc" = 0 ]; then
            engine_ok=1
            ok "installed: $(engine_summary)"
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
    timeout 30 "$HELPER" --probe < /dev/null > "$TMPD/probe.json" 2>/dev/null
    prc=$?
    { read -r pdata; read -r preason; read -r pstt; } < <(py field "$TMPD/probe.json" data reason stt 2>/dev/null)
    if [ "$prc" = 0 ]; then
        ok "the engine finds its data in ${pdata:-?} (speech recognition: ${pstt:-?})"
        [ "$pdata" = "$SYSTEM_DATA" ] || note "(a per-user copy in ${pdata:-?} is used before $SYSTEM_DATA)"
    elif [ "$prc" = 5 ]; then
        fail "the engine has its data but no recorder (pw-record/parec/arecord): ${preason:-}"
        check FAIL "a recorder for the microphone" "sudo apt install pipewire-bin"
    else
        fail "shadow-voiced --probe: ${preason:-exit $prc}"
        [ "$engine_ok" = 1 ] && check FAIL "engine finds its data" "${preason:-shadow-voiced --probe failed}"
    fi
fi

# ---------------------------------------------------------------------------
step "4/7  Assistant settings (${CONF/#$HOME/\~})"
conf_target=$CONF
[ -L "$CONF" ] && conf_target=$(readlink -f -- "$CONF")
[ -L "$CONF" ] && note "(it is a link to $conf_target: that file is the one checked and changed)"
if [ ! -e "$CONF" ]; then
    ok "no settings file yet: the defaults apply"
    check PASS "assistant settings" ""
elif [ ! -O "$conf_target" ] || [ ! -w "$conf_target" ] || [ ! -w "$(dirname -- "$conf_target")" ]; then
    fail "$conf_target does not belong to you or is not writable (left by a command run with sudo?)"
    check FAIL "assistant settings" "sudo chown $(id -un): $conf_target && chmod 600 $conf_target"
elif ! py conf-repair "$CONF" --check > "$TMPD/repair.json"; then
    fail "could not read $CONF (the error is above)"
    check FAIL "assistant settings" "check that $CONF is a readable text file, then run this script again"
else
    nchg=$(python3 -I -c 'import json,sys; print(len(json.load(open(sys.argv[1]))["changes"]))' "$TMPD/repair.json")
    nsec=$(python3 -I -c 'import json,sys; print(json.load(open(sys.argv[1]))["secret_lines"])' "$TMPD/repair.json")
    mode=$(stat -L -c %a "$CONF")
    settings_ok=1
    if [ "$nchg" = 0 ]; then
        ok "no comments on value lines; every value reads as written"
    elif backup_conf_once; then
        # The assistant must not save its settings over the repaired file.
        stop_assistant "(it is started again at the end)"
        note "Qt reads a '# comment' after a value as part of the value (\"muted=false  # ...\""
        note "meant muted) and a '#' line as a setting. Changes (lines holding a key, token,"
        note "password or a URL with a password are never shown or changed; $nsec such line(s)):"
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
            settings_ok=0
        fi
    else
        fail "no backup could be made, so $CONF was not changed"
        settings_ok=0
    fi
    if [ "${mode: -2}" != 00 ]; then
        if chmod 600 -- "$conf_target"; then
            ok "the file holds your API key and was readable by others (mode $mode); now only by you (600)"
        else
            fail "could not make $conf_target private (mode $mode)"
            settings_ok=0
        fi
    fi
    if [ "$settings_ok" = 1 ]; then
        check PASS "assistant settings" ""
    else
        check FAIL "assistant settings" "see the error above; the backup is in ${BACKUP:-$STATE_DIR}; then run this script again"
    fi
fi
# What the assistant itself reads (its own parser; never the key).
if timeout 60 "$BIN" --dump config < /dev/null > "$TMPD/config.json" 2>/dev/null; then
    { read -r WAKE_ON; read -r MUTED; read -r voice; read -r stt; read -r indev; read -r SENS; read -r SPEAK_REPLIES; } \
        < <(py field "$TMPD/config.json" voice.wakeEnabled voice.micMuted voice.voice voice.sttModel voice.inputDevice \
            voice.wakeSensitivity voice.speakReplies)
    INPUT_DEVICE=$indev
    # Your own switches in the assistant (its mute, the wake word). The voice needs
    # both on, so the questions default to on; with --yes (or without a terminal)
    # they are left exactly as the assistant reads them, and nothing is written.
    if [ "$MUTED" = true ]; then
        warn "the assistant's own microphone switch is off (muted=true, its privacy mute): it never"
        note "opens the microphone, so it cannot hear \"Shadow\""
        if ask "Switch the assistant's microphone on?" Y N; then
            backup_conf_once && stop_assistant "(it is started again at the end)" \
                && py conf-set "$CONF" voice muted false > /dev/null && ok "microphone on (muted=false)" && MUTED=false
        fi
    fi
    # The assistant listens for "Shadow" unless you switched that off in it
    # (wake_word_chosen=true). Older versions wrote wake_word_enabled=false into
    # every saved file without asking; this assistant ignores such a line and
    # listens. Nothing is written then, unless you say you want it off.
    if [ "$WAKE_ON" = true ] && [ -f "$CONF" ] && [ "$(py conf-get "$CONF" voice wake_word_enabled "")" = false ]; then
        note "your settings file says wake_word_enabled=false: the old assistant wrote that without"
        note "asking you, so the new one ignores it and listens for \"Shadow\" (on this computer only)"
        if ! ask "Keep listening for \"Shadow\"? (n switches the wake word off)" Y Y; then
            backup_conf_once && stop_assistant "(it is started again at the end)" \
                && py conf-set "$CONF" voice wake_word_enabled false > /dev/null \
                && py conf-set "$CONF" voice wake_word_chosen true > /dev/null && WAKE_ON=false \
                && ok "recorded: wake word off (wake_word_chosen=true)"
        fi
    elif [ "$WAKE_ON" != true ]; then
        warn "the wake word is switched off in the assistant (you switched it off there): it does not listen for \"Shadow\""
        if ask "Switch it on? The microphone then listens for \"Shadow\" whenever you are logged in (on this computer only)." Y N; then
            backup_conf_once && stop_assistant "(it is started again at the end)" \
                && py conf-set "$CONF" voice wake_word_enabled true > /dev/null \
                && py conf-set "$CONF" voice wake_word_chosen true > /dev/null && ok "wake word on" && WAKE_ON=true
        fi
    fi
    # What it reads now, after any change above (its own parser again).
    if timeout 60 "$BIN" --dump config < /dev/null > "$TMPD/config.json" 2>/dev/null; then
        { read -r WAKE_ON; read -r MUTED; read -r voice; read -r stt; read -r indev; read -r SENS; read -r SPEAK_REPLIES; } \
            < <(py field "$TMPD/config.json" voice.wakeEnabled voice.micMuted voice.voice voice.sttModel voice.inputDevice \
                voice.wakeSensitivity voice.speakReplies)
        INPUT_DEVICE=$indev
    fi
    ok "the assistant reads: voice ${voice:-?}, speech recognition ${stt:-?}, wake word $([ "$WAKE_ON" = true ] && echo on || echo off) (sensitivity ${SENS:-normal}), its microphone switch $([ "$MUTED" = true ] && echo 'OFF (muted)' || echo on)${indev:+, input $indev}"
else
    fail "$BIN --dump config failed"
    check FAIL "the assistant reads its settings" "start it from a terminal to see its error: $BIN"
    INPUT_DEVICE=$(py conf-get "$CONF" voice input_device "")
fi

# ---------------------------------------------------------------------------
step "5/7  Sound system, microphone and speakers"
sources() { timeout 10 pactl -f json list sources 2>/dev/null | py devices sources; }
sinks() { timeout 10 pactl -f json list sinks 2>/dev/null | py devices sinks; }
pactl_ok() { timeout 10 pactl info > "$TMPD/pactl-info" 2>&1; }
if ! pactl_ok && command -v systemctl >/dev/null 2>&1; then
    note "the sound server does not answer; starting PipeWire"
    systemctl --user start pipewire.socket pipewire.service wireplumber.service \
        pipewire-pulse.socket pipewire-pulse.service >/dev/null 2>&1
    wait_until 10 pactl_ok
fi
SINK="" SINK_DESC=""
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
        warn "WirePlumber (the session manager) is not running: devices may not be set up"
        side WARN "WirePlumber is not running" "systemctl --user restart wireplumber"
    fi

    # The default input must be a microphone, not the monitor of an output
    # (which "hears" what the speakers play) or PulseAudio's dummy auto_null.
    src=$(timeout 10 pactl get-default-source 2>/dev/null)
    real=$(sources | awk -F'\t' '$2 == 0 && $1 !~ /^auto_null/')
    line=$(printf '%s\n' "$real" | awk -F'\t' -v s="$src" '$1 == s' | head -n 1)
    if [ -z "$line" ]; then
        if [ -z "$real" ]; then
            fail "no microphone: the default input is '${src:-none}' and there is no other"
            if [ -n "$SOF" ]; then
                note "This laptop's microphone runs through the SOF DSP: it needs firmware-sof-signed and"
                note "alsa-ucm-conf (step 2) and a reboot; then the input 'Digital Microphone' appears."
                check FAIL "a microphone" "reboot (firmware-sof-signed and alsa-ucm-conf must be installed), then run this script again"
            else
                check FAIL "a microphone" "connect a microphone (or check that it is enabled in the firmware setup), then run this script again"
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
            else check FAIL "a microphone as the default input" "choose your microphone as the input in the sound settings (wpctl status; wpctl set-default ID)"; fi
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
        mic_dev_ok=1
        if [ "$(cut -f3 <<<"$line")" = 1 ]; then
            warn "the microphone is muted in the sound system"
            if ask "Unmute it?" Y; then
                timeout 10 wpctl set-mute @DEFAULT_AUDIO_SOURCE@ 0 >/dev/null 2>&1 \
                    || timeout 10 pactl set-source-mute "$src" 0
                sleep 0.3
            fi
            if [ "$(sources | awk -F'\t' -v s="$src" '$1 == s { print $3 }')" = 1 ]; then
                mic_dev_ok=0
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
                mic_dev_ok=0
                check FAIL "microphone level" "raise it in the sound settings: wpctl set-volume @DEFAULT_AUDIO_SOURCE@ 0.8"
            else
                note "input level now $vol %"
            fi
        elif [ -n "$vol" ] && [ "$vol" -ge 0 ]; then
            note "input level $vol % (whether that is loud enough shows the test in step 6)"
        fi
        [ "$mic_dev_ok" = 1 ] && check PASS "a microphone, unmuted, as the default input" ""
    fi

    # The speakers: a real output device (not PipeWire's "Dummy Output"), not muted.
    SINK=$(timeout 10 pactl get-default-sink 2>/dev/null)
    sline=$(sinks | awk -F'\t' -v s="$SINK" '$1 == s' | head -n 1)
    case "$SINK" in auto_null*) sline="" ;; esac
    SINK_DESC=$(cut -f5 <<<"$sline")
    if [ -z "$sline" ]; then
        fail "no speakers: the default output is '${SINK:-none}' (PipeWire's Dummy Output: no sound card is set up)"
        if [ -n "$SOF" ]; then
            check FAIL "speakers" "this laptop's sound card needs firmware-sof-signed and alsa-ucm-conf (step 2) and a reboot"
        else
            check FAIL "speakers" "check that the sound card is enabled (wpctl status; sudo dmesg | grep -i snd), connect speakers or headphones"
        fi
        SINK=""
    else
        if [ "$(cut -f3 <<<"$sline")" = 1 ] || [ "$(cut -f4 <<<"$sline")" = 0 ]; then
            warn "the output '$SINK_DESC' is muted or at 0 %: you would not hear the assistant"
            if ask "Unmute it (and set it to 50 % if it is at 0 %)?" Y; then
                timeout 10 wpctl set-mute @DEFAULT_AUDIO_SINK@ 0 >/dev/null 2>&1 || timeout 10 pactl set-sink-mute "$SINK" 0
                [ "$(cut -f4 <<<"$sline")" = 0 ] && { timeout 10 wpctl set-volume @DEFAULT_AUDIO_SINK@ 0.5 >/dev/null 2>&1 \
                    || timeout 10 pactl set-sink-volume "$SINK" 50%; }
                sleep 0.3
                sline=$(sinks | awk -F'\t' -v s="$SINK" '$1 == s' | head -n 1)
            fi
        fi
        if [ "$(cut -f3 <<<"$sline")" = 1 ] || [ "$(cut -f4 <<<"$sline")" = 0 ]; then
            fail "the output '$SINK_DESC' is still muted or at 0 %"
            check FAIL "speakers not muted" "unmute them: wpctl set-mute @DEFAULT_AUDIO_SINK@ 0; wpctl set-volume @DEFAULT_AUDIO_SINK@ 0.5"
            SINK=""
        else
            ok "default output: $SINK_DESC ($(cut -f4 <<<"$sline") %, not muted)"
        fi
    fi
fi

# ---------------------------------------------------------------------------
step "6/7  Tests: the engine, the speakers, your voice"
note "engine self-test: it speaks a sentence and must recognise it again (no audio devices, ~10 s)"
timeout 600 "$HELPER" --selftest < /dev/null > "$TMPD/selftest.json" 2> "$TMPD/selftest.err"
rc=$?
{ read -r st_ok; read -r st_first; read -r st_load; } < <(py field "$TMPD/selftest.json" ok first_audio_ms load_s 2>/dev/null)
ENGINE_OK=0
if [ "$rc" = 0 ] && [ "$st_ok" = true ]; then
    ENGINE_OK=1
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

# Speakers: the greeting in the voice and speed the assistant uses. Its sound is
# measured on the output's monitor while it plays.
if [ -n "$SINK" ] && [ "$ENGINE_OK" = 1 ]; then
    voice=$(py conf-get "$CONF" voice voice af_heart)
    rate=$(py conf-get "$CONF" voice speech_rate 1.0)
    case "$voice" in [a-z]*_[a-z0-9]*) ;; *) voice=af_heart ;; esac
    case "$rate" in ''|*[!0-9.]*) rate=1.0 ;; esac
    if timeout 180 "$HELPER" --say "$GREETING" --out "$TMPD/greeting.wav" --voice "$voice" --speed "$rate" \
            < /dev/null > "$TMPD/say.json" 2>> "$TMPD/say.err"; then
        player=""
        for t in pw-play paplay aplay; do command -v "$t" >/dev/null 2>&1 && { player=$t; break; }; done
        secs=$(py field "$TMPD/say.json" seconds)
        secs=${secs%.*}; case "$secs" in ''|*[!0-9]*) secs=5 ;; esac
        mon_pid=""
        if command -v parec >/dev/null 2>&1; then
            ( timeout $((secs + 4)) parec -d "$SINK.monitor" --raw --rate=16000 --channels=1 --format=s16le 2>/dev/null \
                | py meter $((secs + 3)) "" > "$TMPD/monitor.out" 2>/dev/null ) &
            mon_pid=$!
            sleep 0.5
        fi
        note "playing a greeting in the voice '$voice' with $player: \"$GREETING\""
        if [ -n "$player" ] && timeout 60 "$player" "$TMPD/greeting.wav" < /dev/null > "$TMPD/play.err" 2>&1; then
            [ -n "$mon_pid" ] && wait "$mon_pid" 2>/dev/null
            mon=$(cat "$TMPD/monitor.out" 2>/dev/null)
            measured=0
            if [ -n "$mon" ] && awk -v m="$mon" 'BEGIN { exit !(m > -50) }'; then
                measured=1
                note "(its sound reached '$SINK_DESC': loudest 0.1 s at $mon dBFS on the output's monitor)"
            elif [ -n "$mon" ]; then
                warn "nothing of it reached '$SINK_DESC' (its monitor stayed at $mon dBFS)"
            fi
            if [ "$INTERACTIVE" = 1 ]; then
                if ask "Did you hear it?" -; then
                    ok "you heard the assistant's voice"
                    check PASS "speakers (you heard the greeting)" ""
                    SPEAKERS_OK=1
                else
                    fail "the greeting played on '$SINK_DESC', but you did not hear it"
                    check FAIL "speakers" "is '$SINK_DESC' the device you listen to? (wpctl status: choose the right output; headphones plugged in?) Then run this script again"
                fi
            elif [ "$measured" = 1 ]; then
                ok "the greeting played on '$SINK_DESC' (its sound measured; not confirmed by ear: no questions asked)"
                check PASS "speakers (the greeting played on '$SINK_DESC'; not confirmed by ear)" ""
                SPEAKERS_OK=1
            else
                warn "the greeting played, but its sound could not be measured"
                check WARN "speakers: not confirmed" "run this script without --yes: it asks whether you heard the greeting"
                SPEAKERS_OK=2
            fi
        else
            [ -n "$mon_pid" ] && kill "$mon_pid" 2>/dev/null
            fail "playing failed: $(head -n 2 "$TMPD/play.err" 2>/dev/null | tr '\n' ' ')"
            check FAIL "speakers" "check the output device: wpctl status"
        fi
    else
        fail "the voice could not synthesise the greeting: $(py field "$TMPD/say.json" error 2>/dev/null)"
        check FAIL "speakers" "see $VOICE_LOG"
    fi
fi

# Your voice: the test sentence must be understood, and "Shadow" must wake the
# engine exactly as it will in use. No assistant may listen meanwhile.
mic_level_raise() {   # raise the default input to 100 % (or the assistant's input)
    timeout 10 wpctl set-volume @DEFAULT_AUDIO_SOURCE@ 1.0 >/dev/null 2>&1 \
        || timeout 10 pactl set-source-volume "${src:-@DEFAULT_SOURCE@}" 100%
    sleep 0.3
}
if [ "$DO_CALIBRATE" = 0 ]; then
    note "the tests where you speak are skipped (--no-calibrate)"
    check SKIP "your voice: microphone test and wake word" "run this script without --no-calibrate"
elif [ "$ENGINE_OK" != 1 ]; then
    note "(the microphone test needs a working engine)"
else
    tries=0 raised=0 switched=0 tried=""
    while :; do
        tries=$((tries + 1))
        printf '\n'
        note "Microphone test. From where you usually sit, say in your normal voice:"
        printf '\n             %s"%s"%s\n\n' "$C_STEP" "$SENTENCE" "$C_OFF"
        if [ "$INTERACTIVE" = 1 ]; then
            printf '         Press Enter, then speak (you have %s seconds; s + Enter skips): ' "$CAL_SECS"
            read -r a || a=s
            case "$a" in [Ss]*) note "skipped"; check SKIP "your voice: microphone test and wake word" "run this script again and say the sentence"; break ;; esac
        else
            for n in 3 2 1; do printf '         %s...\n' "$n"; sleep 1; done
        fi
        # Looked for right before recording: an assistant opened meanwhile would hear the sentence.
        stop_assistant "(no assistant may react to the test sentence; it is started again at the end)"
        if [ -n "$(assistant_pids)" ]; then
            warn "an assistant is still running (PID $(assistant_pids)): it would react to the test sentence"
            check SKIP "your voice: microphone test" "close the assistant, then run this script again"
            break
        fi
        note "speak now"
        dev_args=()
        [ -n "$INPUT_DEVICE" ] && dev_args=(--input-device "$INPUT_DEVICE")
        start_meter "$CAL_SECS" "$TMPD/meter.out"
        timeout $((CAL_SECS + 300)) "$HELPER" --calibrate "$CAL_SECS" --expect "$SENTENCE" \
            --wake-sensitivity "${SENS:-normal}" "${dev_args[@]}" < /dev/null > "$TMPD/calib.json" 2> "$TMPD/calib.err"
        crc=$?
        stop_meter
        peak_meter=$(cat "$TMPD/meter.out" 2>/dev/null)
        CAL_ERROR="" CAL_SOURCE="" CAL_SILENCE=0 CAL_CLIPPING=0 CAL_PEAK="" CAL_FLOOR="" CAL_SPEECH_S=0
        CAL_RECOMMENDED="" CAL_BEST_WER="" CAL_WAKE_OK=0 CAL_WAKE_HIGH=0 CAL_WAKE_HEARD=""
        CAL_WAKE_VIA="" CAL_WAKE_STT="" CAL_ADVICE="" CAL_MODELS=""
        eval "$(py calib "$TMPD/calib.json" 2>/dev/null)"
        [ -n "$peak_meter" ] && note "(level meter: loudest 0.1 s at ${peak_meter} dBFS RMS)"
        if [ -z "$CAL_PEAK" ]; then
            # No level statistics: nothing was recorded at all.
            cp -f "$TMPD/calib.err" "$STATE_DIR/microphone-test.log" 2>/dev/null
            fail "the microphone could not be opened: ${CAL_ERROR:-$(tail -n 1 "$TMPD/calib.err" 2>/dev/null) (exit $crc)}"
            check FAIL "your voice: the microphone could not be opened" "check the input in the sound settings (wpctl status); details: $STATE_DIR/microphone-test.log"
            break
        fi
        if [ "$CAL_SILENCE" = 1 ]; then
            fail "the microphone delivers digital silence (${CAL_SOURCE:-?}): it is muted, or the wrong input is selected"
            check FAIL "your voice: the microphone is silent" "select and unmute your microphone (wpctl status); laptops with digital microphones: firmware-sof-signed + alsa-ucm-conf + reboot"
            break
        fi
        note "recorded with ${CAL_SOURCE:-?}: loudest sample $CAL_PEAK dBFS, background noise $CAL_FLOOR dBFS$([ "$CAL_CLIPPING" = 1 ] && echo ', CLIPPING')"
        quiet=0
        awk -v p="$CAL_PEAK" -v q="$QUIET_DBFS" 'BEGIN { exit !(p < q) }' && quiet=1
        vol=$(sources | awk -F'\t' -v s="$src" '$1 == s { print $4 }')
        if [ "$quiet" = 1 ] && [ "$raised" = 0 ] && [ -n "$vol" ] && [ "$vol" -lt 100 ]; then
            if [ -n "$CAL_RECOMMENDED" ]; then
                warn "your voice arrives very quietly (loudest sample $CAL_PEAK dBFS; input level $vol %)"
            else
                warn "the microphone picks up almost nothing (loudest sample $CAL_PEAK dBFS; input level $vol %)"
            fi
            if ask "Raise the microphone level to 100 % and test again?" Y; then
                mic_level_raise; raised=1
                continue
            fi
        fi
        if [ -z "$CAL_RECOMMENDED" ]; then
            warn "no speech was heard in the $CAL_SECS seconds (only background sound)"
            [ -n "$CAL_ADVICE" ] && note "$CAL_ADVICE"
            # Another microphone may be the one that hears you (e.g. an empty headset
            # jack is the default, the built-in array is not): try it.
            tried="$tried $src "
            other=""
            if [ -z "$INPUT_DEVICE" ]; then
                other=$(sources | awk -F'\t' -v t="$tried" '$2 == 0 && $1 !~ /^auto_null/ && index(t, " " $1 " ") == 0' | head -n 1)
            fi
            if [ -n "$other" ] && [ "$switched" -lt 2 ] \
                    && ask "Use the input '$(cut -f5 <<<"$other")' instead and test again?" Y; then
                if timeout 10 pactl set-default-source "$(cut -f1 <<<"$other")"; then
                    sleep 0.5
                    src=$(timeout 10 pactl get-default-source 2>/dev/null)
                    switched=$((switched + 1)) raised=0
                    ok "default input is now: $(cut -f5 <<<"$other") ($src)"
                    continue
                fi
            fi
            if [ "$INTERACTIVE" = 1 ] && [ "$tries" -lt 3 ] && ask "Try again?" Y; then continue; fi
            fail "the microphone delivers sound, but no speech"
            check FAIL "your voice: no speech heard" "is the right microphone selected (wpctl status)? Speak from closer while the meter moves, then run this script again"
            break
        fi
        wer_pct=$(awk -v w="$CAL_BEST_WER" 'BEGIN { printf "%d", w * 100 + 0.5 }')
        note "speech heard: $CAL_SPEECH_S s; what each speech model understood:"
        printf '%s\n' "$CAL_MODELS" | while IFS= read -r l; do note "  $l"; done
        [ -n "$CAL_ADVICE" ] && note "advice: $CAL_ADVICE"
        if [ "$wer_pct" -gt "$WER_MAX" ]; then
            warn "speech recognition is poor for you: the best model ($CAL_RECOMMENDED) got $wer_pct % of the words wrong"
            if [ "$INTERACTIVE" = 1 ] && [ "$tries" -lt 3 ] && ask "Try again?" Y; then continue; fi
            fail "your voice is not understood well enough ($wer_pct % of the words wrong)"
            next="speak closer to the microphone, reduce background noise"
            [ "$quiet" = 1 ] && next="raise the microphone level (wpctl set-volume @DEFAULT_AUDIO_SOURCE@ 1.0), speak closer"
            check FAIL "your voice: speech recognition ($wer_pct % of the words wrong)" "$next; then run this script again"
            break
        fi
        ok "your voice is understood: $CAL_RECOMMENDED got $wer_pct % of the words wrong"
        if [ "$CAL_WAKE_OK" != 1 ]; then
            warn "\"Shadow\" did not wake the engine: it heard \"$CAL_WAKE_HEARD\""
            if [ "$INTERACTIVE" = 1 ] && [ "$tries" -lt 3 ] && ask "Try again (say \"Shadow\" clearly, as the first word)?" Y; then continue; fi
            fail "the wake word \"Shadow\" is not recognised in your voice (heard: \"$CAL_WAKE_HEARD\")"
            check FAIL "your voice: the wake word \"Shadow\" was not recognised" "say \"Shadow\" clearly as the first word (\"SHA-doh\"), a little slower; then run this script again"
            break
        fi
        if [ "$CAL_WAKE_HIGH" = 1 ]; then
            warn "\"Shadow\" only wakes the engine with wake_sensitivity=high (heard: \"$CAL_WAKE_HEARD\")"
            if ask "Make the wake word more lenient (wake_sensitivity=high; it may then also react to similar words)?" Y; then
                if backup_conf_once && py conf-set "$CONF" voice wake_sensitivity high > /dev/null; then
                    SENS=high
                    ok "saved wake_sensitivity=high"
                fi
            fi
            if [ "$SENS" != high ]; then
                fail "the wake word works for your voice only with wake_sensitivity=high"
                check FAIL "your voice: the wake word" "set wake_sensitivity=high in [voice] of assistant.conf (or run this script again and answer yes)"
                break
            fi
        fi
        ok "\"Shadow\" woke the engine (${CAL_WAKE_VIA// /, }; with $CAL_WAKE_STT) and it took the rest as the command"
        check PASS "your voice: heard, understood ($CAL_RECOMMENDED, $wer_pct % wrong), \"Shadow\" recognised" ""
        MIC_OK=1
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

# ---------------------------------------------------------------------------
step "7/7  The assistant in the background: \"$ROUNDTRIP\""
timeout 60 "$BIN" --dump voice < /dev/null > "$TMPD/voice.json" 2> "$TMPD/voice.err"
{ read -r engine; read -r pexit; read -r preason; } < <(py field "$TMPD/voice.json" engine probeExit probe.reason 2>/dev/null)
if [ "$engine" = neural ] && [ "$pexit" = 0 ]; then
    ok "$BIN --dump voice: engine \"neural\", probe ok"
    check PASS "the assistant finds the voice engine" ""
else
    fail "$BIN --dump voice: engine \"${engine:-?}\" (probe exit ${pexit:-?}${preason:+: $preason})"
    cp -f "$TMPD/voice.err" "$STATE_DIR/dump-voice.log" 2>/dev/null
    check FAIL "the assistant finds the voice engine" "${preason:-see $STATE_DIR/dump-voice.log}; then run this script again"
fi

# The API key, as the assistant started in the background (and at login) sees it.
timeout 60 "$BIN" --dump config < /dev/null > "$TMPD/config.json" 2>/dev/null
{ read -r term_key; read -r term_src; } < <(py field "$TMPD/config.json" apiKeyConfigured apiKeySource)
bg_dump_config "$TMPD/bgconfig.json"
{ read -r bg_key; read -r bg_src; } < <(py field "$TMPD/bgconfig.json" apiKeyConfigured apiKeySource)
where=$([ "$SD_USER" = 1 ] && echo "started by your user manager, as at login" || echo "started from here")
if [ -z "$bg_key" ]; then
    warn "could not ask an assistant started in the background what it reads; going by this terminal"
    bg_key=$term_key bg_src=$term_src
fi
KEY_OK=0
if [ "$bg_key" = true ]; then
    KEY_OK=1 LOGIN_KEY=ok
    ok "an API key reaches the assistant $where (from ${bg_src:-local mode}; not shown)"
    [ "$term_key" = true ] && [ "$term_src" != "$bg_src" ] \
        && note "(this terminal has another one, from $term_src: the background assistant uses ${bg_src})"
    check PASS "API key: the background assistant has one" ""
    if [ "$SD_USER" = 0 ] && [ "${bg_src#*environment}" != "$bg_src" ]; then
        side WARN "API key: only in your environment (${bg_src%% *})" \
            "whether the assistant started at login has it depends on your login scripts; key=... in [api] of ${CONF/#$HOME/\~} (chmod 600) always works"
    fi
elif [ "$term_key" = true ] && [ "$SD_USER" = 1 ] && [ "${term_src#*environment}" != "$term_src" ]; then
    var=${term_src%% *}
    warn "your API key is only in this terminal's environment ($var, e.g. from ~/.profile or ~/.bashrc):"
    note "the assistant started in the background and at login cannot see it"
    if ! py conf-has "$CONF" api key \
            && ask "Save it in ${CONF/#$HOME/\~} (readable only by you), so that it answers there too?" Y N \
            && backup_conf_once && [ "$(py conf-key-from-env "$CONF" "$var")" = added ]; then
        chmod 600 -- "$(readlink -f -- "$CONF")"
        bg_dump_config "$TMPD/bgconfig.json"
        if [ "$(py field "$TMPD/bgconfig.json" apiKeyConfigured)" = true ]; then
            KEY_OK=1 LOGIN_KEY=ok
            ok "saved in [api] of assistant.conf (mode 600; not shown): the background assistant has it now"
            check PASS "API key: saved where the background assistant reads it" ""
        fi
    fi
    if [ "$KEY_OK" = 0 ]; then
        # This run: passed to the background assistant by name. At login it is missing.
        KEY_FORWARD=$var KEY_OK=1 LOGIN_KEY=missing
        note "(for now it is passed to the background assistant by name; after the next login it is missing)"
    fi
elif [ "$term_key" = true ]; then
    fail "this terminal's assistant has an API key (from $term_src), but the one started in the background does not"
    check FAIL "API key: not seen by the background assistant" "put key=... in [api] of ${CONF/#$HOME/\~} (chmod 600), then run this script again"
else
    fail "no API key is configured: the assistant hears you, but cannot answer"
    check FAIL "API key" "put your Claude API key as key=... in [api] of ${CONF/#$HOME/\~} (chmod 600; the assistant never stores it itself), then run this script again"
fi

ROUNDTRIP_OK=0
if [ "$MUTED" = true ]; then
    warn "not started in the background: the assistant's own microphone switch is off (privacy mute)"
    check OFF "listening for \"Shadow\": the assistant's microphone switch is off (your setting)" \
        "switch \"Mute the microphone\" off in the assistant's settings, or run this script again and answer yes"
elif [ "$WAKE_ON" != true ]; then
    warn "not started in the background: the wake word is switched off (your setting)"
    check OFF "listening for \"Shadow\": the wake word is off (your setting)" \
        "switch \"Listen for Shadow\" on in the assistant's settings, or run this script again and answer yes"
elif [ "$engine" != neural ]; then
    note "(not started: the assistant does not find its voice engine)"
else
    stop_assistant "(the new one is started now)"
    if ! start_background; then
        fail "could not start $BIN --background"
        check FAIL "the assistant in the background" "start it from the launcher; output: $STATE_DIR/assistant.log"
    else
        STARTED_BG=1
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
        if [ -z "$apid" ] || ! wait_until 60 armed_now; then
            if [ -n "$apid" ] && kill -0 "$apid" 2>/dev/null; then
                fail "the assistant runs (PID $apid) but its engine did not open the microphone within 60 s"
                grep -a "shadow-voiced" "$VOICE_LOG" 2>/dev/null | tail -n 4 | while IFS= read -r l; do note "$l"; done
                check FAIL "the assistant in the background: listening" "see $VOICE_LOG"
            else
                STARTED_BG=0
                fail "the assistant stopped right after starting; it said:"
                start_output | while IFS= read -r l; do note "  $l"; done
                if start_output | grep -q 'module "org\.shadow\.\(ui\|style\)"'; then
                    check FAIL "the assistant in the background" "the Shadow UI library it loads (package shadow-ui) is missing or too old: sudo apt install --reinstall shadow-ui"
                else
                    check FAIL "the assistant in the background" "read its error above; start it from the launcher to see more"
                fi
            fi
        else
            read -r hpid _ rexe <<<"$armed"
            sleep 1
            RT_MIC_STATUS="" RT_MIC_MSG=""
            eval "$(py voicelog "$VOICE_LOG" 0 "$hpid")"
            if [ "$RT_MIC_STATUS" = error ]; then
                fail "its engine reports a microphone problem: $RT_MIC_MSG"
                check FAIL "the assistant in the background: microphone" "$RT_MIC_MSG"
            else
                ok "running in the background (PID $apid, $START_HOW); its engine (PID $hpid) records with ${rexe##*/}"
                if [ "$MIC_OK" = 1 ]; then
                    check PASS "the assistant runs in the background, its microphone open" ""
                else
                    warn "it records, but from no microphone that works (the microphone test above did not pass)"
                    check WARN "the assistant runs in the background, but cannot hear you yet" "fix the microphone (above), then run this script again"
                fi
                # The whole way, as you will use it: your voice -> wake word -> Claude -> its voice.
                if [ "$DO_CALIBRATE" = 0 ]; then
                    check SKIP "\"$ROUNDTRIP\": not tested (--no-calibrate)" "run this script without --no-calibrate"
                elif [ "$MIC_OK" != 1 ]; then
                    note "(\"$ROUNDTRIP\" is not tried: the microphone test above did not pass)"
                elif [ "$KEY_OK" != 1 ]; then
                    note "(\"$ROUNDTRIP\" is not tried: no API key)"
                else
                    rt_try=0
                    while :; do
                        rt_try=$((rt_try + 1))
                        printf '\n'
                        note "Last test: talk to the assistant now running in the background."
                        if [ "$INTERACTIVE" = 1 ]; then
                            printf '         Press Enter, then say "%s" (s + Enter skips): ' "$ROUNDTRIP"
                            read -r a || a=s
                            case "$a" in [Ss]*) note "skipped"; check SKIP "\"$ROUNDTRIP\": not tested (you skipped it)" "run this script again and say it"; break ;; esac
                        else
                            note "in 3 seconds, say: \"$ROUNDTRIP\""
                            for n in 3 2 1; do printf '         %s...\n' "$n"; sleep 1; done
                        fi
                        mark=$(wc -l < "$VOICE_LOG" 2>/dev/null || echo 0)
                        RT_FINAL=0 RT_REJECTED=0 RT_SPEAK_ID="" RT_SPEAK_END=0 RT_INTERRUPTED=0 RT_ERROR=""
                        RT_FIRST_AUDIO_S=""
                        printf '         Say now: "%s"\n' "$ROUNDTRIP"
                        # Heard within 20 s; then the answer (Claude, then its voice) within 90 s.
                        heard_by=$(( $(date +%s) + 20 )) answer_by=0
                        while :; do
                            sleep 0.5
                            eval "$(py voicelog "$VOICE_LOG" "$mark" "$hpid")"
                            now=$(date +%s)
                            if [ "$RT_FINAL" = 1 ] && [ "$answer_by" = 0 ]; then
                                answer_by=$((now + 90))
                                note "it heard you; waiting for the answer..."
                            fi
                            if [ -n "$RT_SPEAK_ID" ] && { [ "$RT_SPEAK_END" = 1 ] || [ "${RT_SPEAK_ID#r}" = "$RT_SPEAK_ID" ]; }; then break; fi
                            [ "$RT_FINAL" = 0 ] && [ "$now" -ge "$heard_by" ] && break
                            [ "$answer_by" != 0 ] && [ "$now" -ge "$answer_by" ] && break
                        done
                        # What it said first: e... is "Sorry, that didn't work" (the request
                        # failed); r... its reply, c... a question to confirm a step: answers.
                        spoke=""
                        case "$RT_SPEAK_ID" in "") ;; e*) spoke=error ;; *) spoke=answer ;; esac
                        how=""; [ "${RT_SPEAK_ID:0:1}" = c ] && how=" (asking you to confirm a step)"
                        if [ "$spoke" = answer ] && [ "$RT_INTERRUPTED" = 0 ] && [ "$SPEAKERS_OK" = 2 ]; then
                            ok "it heard \"$ROUNDTRIP\" and answered$how (whether you can hear it was not confirmed: see the speakers above)"
                            check WARN "\"$ROUNDTRIP\": heard and answered; audible not confirmed" "run this script without --yes: it asks whether you heard the greeting"
                        elif [ "$spoke" = answer ] && [ "$RT_INTERRUPTED" = 0 ] && [ "$SPEAKERS_OK" != 1 ]; then
                            warn "it heard \"$ROUNDTRIP\" and answered$how, but its voice went to no speakers that work (above)"
                            check WARN "\"$ROUNDTRIP\": heard and answered, but not audible" "fix the speakers (above), then run this script again"
                        elif [ "$spoke" = answer ] && [ "$RT_INTERRUPTED" = 0 ]; then
                            ok "it heard \"$ROUNDTRIP\" and answered aloud$how (its voice started ${RT_FIRST_AUDIO_S:-?} s after you stopped speaking)"
                            check PASS "\"$ROUNDTRIP\": heard, and answered aloud" ""
                            ROUNDTRIP_OK=1
                        elif [ "$spoke" = error ]; then
                            fail "it heard you, but the request to Claude failed (it said \"Sorry, that didn't work\")"
                            check FAIL "\"$ROUNDTRIP\": heard, but the request to Claude failed" "open the assistant: its window shows the error (API key not valid, network?). The background assistant uses the key from ${bg_src:-$term_src}"
                        elif [ "$RT_FINAL" = 1 ] && [ -n "$RT_SPEAK_ID" ]; then
                            fail "it heard you and started to answer, but playing the answer failed${RT_ERROR:+: $RT_ERROR}"
                            check FAIL "\"$ROUNDTRIP\": the answer could not be played" "check the output device (wpctl status); details in $VOICE_LOG"
                        elif [ "$RT_FINAL" = 1 ]; then
                            if [ "$SPEAK_REPLIES" = false ]; then
                                ok "it heard \"$ROUNDTRIP\" and took it as a request (spoken replies are off: it answers on screen)"
                                check OFF "spoken replies are off (your setting): it answers on screen only" "switch \"Speak replies aloud\" on in the assistant's settings"
                                ROUNDTRIP_OK=1
                            else
                                fail "it heard you, but no answer came within 90 s"
                                check FAIL "\"$ROUNDTRIP\": heard, but no answer" "open the assistant: its window shows what happened (network? API key?)"
                            fi
                        else
                            if [ "$RT_REJECTED" -gt 0 ]; then
                                warn "it heard someone speak, but not \"Shadow\" at the start"
                            elif [ "$RT_MIC_STATUS" = error ]; then
                                warn "its microphone failed: $RT_MIC_MSG"
                            else
                                warn "it heard nothing"
                            fi
                            if [ "$INTERACTIVE" = 1 ] && [ "$rt_try" -lt 2 ] && ask "Try again?" Y; then continue; fi
                            fail "the assistant in the background did not take \"$ROUNDTRIP\""
                            check FAIL "\"$ROUNDTRIP\": the background assistant did not react" "say it clearly, \"Shadow\" first; if the microphone test above passed, see $VOICE_LOG"
                        fi
                        break
                    done
                fi
            fi
        fi
    fi
fi

# The wake word from login on: only for the new assistant, verified above.
if [ "$WAKE_ON" = true ] && [ "$MUTED" != true ] && [ "$engine" = neural ]; then
    installed_entry=""
    for f in "$PREFIX/etc/xdg/autostart/$LISTEN_DESKTOP" "/etc/xdg/autostart/$LISTEN_DESKTOP"; do
        if [ -z "$installed_entry" ] && [ -f "$f" ]; then installed_entry=$f; fi
    done
    read_by_session=0
    if [ -n "$installed_entry" ]; then
        exec_bin=$(sed -n 's/^Exec=\([^ ]*\).*/\1/p' "$installed_entry" | head -n 1)
        case "$exec_bin" in /*) ;; *) exec_bin=$(command -v "$exec_bin" 2>/dev/null || true) ;; esac
        while IFS= read -r d; do
            [ "$d" != "$CONF_HOME/autostart" ] && [ "${installed_entry%/*}" = "$d" ] \
                && [ "$(readlink -f -- "$exec_bin")" = "$(readlink -f -- "$BIN")" ] && read_by_session=1
        done < <(autostart_dirs)
    fi
    login_ok=0     # 1: an entry that starts $BIN is in place ($login_src)
    login_src=""
    if [ -e "$USER_AUTOSTART" ] && ! { [ -f "$USER_AUTOSTART" ] && [ "$(head -n 1 "$USER_AUTOSTART")" = "$MARK" ]; }; then
        own=$(sed -n 's/^Exec=//p' "$USER_AUTOSTART" | head -n 1)
        warn "$USER_AUTOSTART is your own file (Exec=$own); left as it is"
        check WARN "\"Shadow\" at login: your own autostart entry" "check that $USER_AUTOSTART starts $BIN --background"
    elif [ "$read_by_session" = 1 ]; then
        # The session reads the installed entry, which starts this assistant.
        rm -f "$USER_AUTOSTART"
        login_src=$installed_entry login_ok=1
    else
        mkdir -p "${USER_AUTOSTART%/*}"
        {
            printf '%s\n' "$MARK" "# Starts $BIN at login, listening for \"Shadow\"" \
                "# (${installed_entry:-the installed entry} is not read by the session or starts another copy)."
            printf '%s\n' '[Desktop Entry]' 'Type=Application' 'Name=Shadow Assistant (wake word)' \
                'Comment=Listens for "Shadow" so the assistant answers without opening it first' \
                "Exec=$BIN --background" 'Icon=org.shadow.assistant' 'Terminal=false' 'NoDisplay=true' \
                'X-GNOME-Autostart-enabled=true' 'X-KDE-autostart-phase=2'
        } > "$USER_AUTOSTART.tmp" && mv -f "$USER_AUTOSTART.tmp" "$USER_AUTOSTART"
        if grep -qx "Exec=$BIN --background" "$USER_AUTOSTART" 2>/dev/null; then
            login_src=$USER_AUTOSTART login_ok=1
        else
            fail "could not write $USER_AUTOSTART"
            check FAIL "\"Shadow\" at login" "copy ${installed_entry:-applications/assistant/$LISTEN_DESKTOP} to ~/.config/autostart/ with Exec=$BIN --background"
        fi
    fi
    if [ "$login_ok" = 1 ]; then
        # What really starts at login: the unit systemd's autostart generator makes of
        # the entries the session reads (the user's directory first).
        login_confirmed=0 login_exec=""
        if [ "$SD_USER" = 1 ]; then
            systemctl --user daemon-reload >/dev/null 2>&1
            login_exec=$(systemctl --user cat "$LISTEN_UNIT" 2>/dev/null | sed -n 's/^ExecStart=[-:@+!]*//p' | head -n 1)
            if [ -n "$login_exec" ] && [ "$(readlink -f -- "${login_exec%% *}")" = "$(readlink -f -- "$BIN")" ] \
                    && [ "${login_exec#* }" = --background ]; then
                login_confirmed=1
            fi
        fi
        if [ "$login_confirmed" = 1 ]; then
            ok "at login: $login_src (your session turns it into $LISTEN_UNIT, which starts $BIN --background)"
        elif [ "$SD_USER" = 1 ]; then
            if [ -n "$login_exec" ]; then why="starts \"$login_exec\", not $BIN --background"; else why="does not exist"; fi
            warn "at login: $login_src is in place, but your user manager's $LISTEN_UNIT $why"
        else
            warn "at login: $login_src starts $BIN (not confirmed: no systemd user manager here)"
        fi
        if [ "$LOGIN_KEY" = missing ]; then
            fail "at login the assistant starts and listens, but has no API key (it is only in your shell's $KEY_FORWARD)"
            check FAIL "\"Shadow\" at login: no API key there" "put key=... in [api] of ${CONF/#$HOME/\~} (chmod 600), or $KEY_FORWARD=... in a file in ~/.config/environment.d/; log out and in"
        elif [ "$login_confirmed" = 1 ]; then
            check PASS "autostart at login: $BIN --background" ""
        else
            check WARN "autostart at login: not confirmed" \
                "log out and in, then say \"Shadow\"; if it does not answer: systemctl --user status '$LISTEN_UNIT'"
        fi
    fi
fi

finish
