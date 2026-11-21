"""Engine data lookup (models + runtime wheels) and the fast --probe.

Data roots are searched in this order (spec 2.1):

    --data DIR  >  $SHADOW_VOICE_DATA  >  ${XDG_DATA_HOME:-~/.local/share}/shadow/voice
                >  /var/lib/shadow-voice  >  /usr/share/shadow-voice

An explicit choice (--data or $SHADOW_VOICE_DATA) is authoritative: when it is
incomplete we say so instead of silently falling back to another install, so a
test or a user pointing at a directory gets an answer about THAT directory.

A root is usable when its manifest.json lists vad + one STT model + the TTS
model + the runtime, and every file the manifest lists exists with the recorded
size (sha256 is checked by `shadow-voice-setup --check`, not here: hashing
~700 MB would take seconds and --probe must stay under one second).

Nothing in this module imports numpy or sherpa_onnx at module level.
"""
import json
import os
import platform
import shutil
import sys
import sysconfig
import time

from . import VERSION

SYSTEM_ROOTS = ("/var/lib/shadow-voice", "/usr/share/shadow-voice")
MANIFEST_FORMAT = 1

# Files the helper loads, per component, relative to the data root (spec 2.1 layout).
COMPONENT_FILES = {
    "vad": ("models/vad/silero_vad.onnx",),
    "kws": ("models/kws/encoder.int8.onnx", "models/kws/decoder.int8.onnx", "models/kws/joiner.int8.onnx",
            "models/kws/tokens.txt", "models/kws/keywords.txt"),
    "stt-moonshine": ("models/stt/moonshine/encoder_model.ort", "models/stt/moonshine/decoder_model_merged.ort",
                      "models/stt/moonshine/tokens.txt"),
    "stt-whisper": ("models/stt/whisper/encoder.int8.onnx", "models/stt/whisper/decoder.int8.onnx",
                    "models/stt/whisper/tokens.txt"),
    "tts-kokoro": ("models/tts/kokoro/model.onnx", "models/tts/kokoro/voices.bin", "models/tts/kokoro/tokens.txt",
                   "models/tts/kokoro/lexicon-us-en.txt", "models/tts/kokoro/lexicon-gb-en.txt"),
    "runtime": (),          # checked through the Python import below
}
STT_MODELS = ("moonshine", "whisper")          # preference order for stt "auto"
CAPTURE_TOOLS = ("pw-record", "parec", "arecord")
PLAYBACK_TOOLS = ("pw-play", "pacat", "aplay")
# Debian packages that provide each tool (for messages that name the fix).
TOOL_PACKAGES = {"pw-record": "pipewire-bin", "pw-play": "pipewire-bin", "parec": "pulseaudio-utils",
                 "pacat": "pulseaudio-utils", "arecord": "alsa-utils", "aplay": "alsa-utils",
                 "espeak-ng": "espeak-ng"}

EXIT_OK, EXIT_NO_DATA, EXIT_NO_PYDEPS, EXIT_NO_CAPTURE = 0, 3, 4, 5
SETUP_HINT = "Run: shadow-voice-setup (or use Install in the assistant's voice settings)"


def user_data_root():
    base = os.environ.get("XDG_DATA_HOME") or os.path.join(os.path.expanduser("~"), ".local", "share")
    return os.path.join(base, "shadow", "voice")


def candidate_roots(explicit=None):
    """[(path, why)] in lookup order; an explicit choice is the only candidate."""
    if explicit:
        return [(os.path.abspath(explicit), "--data")]
    env = os.environ.get("SHADOW_VOICE_DATA")
    if env:
        return [(os.path.abspath(env), "$SHADOW_VOICE_DATA")]
    return [(user_data_root(), "user")] + [(r, "system") for r in SYSTEM_ROOTS]


class DataRoot:
    """One data root and what is (completely) installed in it."""

    def __init__(self, path):
        self.path = path
        self.manifest = None
        self.problems = []          # human-readable reasons this root is not usable
        self.components = set()     # complete components
        self._check()

    def file(self, rel):
        return os.path.join(self.path, rel)

    def _check(self):
        mpath = os.path.join(self.path, "manifest.json")
        if not os.path.isdir(self.path):
            self.problems.append(f"{self.path} does not exist")
            return
        try:
            with open(mpath, encoding="utf-8") as f:
                m = json.load(f)
        except FileNotFoundError:
            self.problems.append(f"{mpath} is missing (engine data not installed)")
            return
        except (OSError, ValueError) as e:
            self.problems.append(f"{mpath} is unreadable: {e}")
            return
        if not isinstance(m, dict) or m.get("format") != MANIFEST_FORMAT or not isinstance(m.get("installed"), dict):
            self.problems.append(f"{mpath} has an unknown format (re-run shadow-voice-setup)")
            return
        self.manifest = m
        for comp, info in m["installed"].items():
            missing = self._incomplete(comp, info if isinstance(info, dict) else {})
            if missing:
                self.problems.append(f"{comp}: {missing}")
            else:
                self.components.add(comp)

    def _incomplete(self, comp, info):
        """'' when every file of the component is present with the recorded size."""
        recorded = info.get("files") or {}
        for rel in COMPONENT_FILES.get(comp, ()):
            if not os.path.isfile(self.file(rel)):
                return f"{rel} is missing"
        for rel, meta in recorded.items():
            try:
                size = os.path.getsize(self.file(rel))
            except OSError:
                return f"{rel} is missing"
            if isinstance(meta, dict) and meta.get("size") is not None and size != meta["size"]:
                return f"{rel} has the wrong size ({size} != {meta['size']}; re-run shadow-voice-setup)"
        return ""

    @property
    def stt_models(self):
        return [s for s in STT_MODELS if f"stt-{s}" in self.components]

    @property
    def has_kws(self):
        return "kws" in self.components

    @property
    def usable(self):
        return ("vad" in self.components and "tts-kokoro" in self.components and "runtime" in self.components
                and bool(self.stt_models))

    @property
    def stt_default(self):
        d = (self.manifest or {}).get("stt_default")
        return d if d in self.stt_models else (self.stt_models[0] if self.stt_models else None)

    def missing_components(self):
        out = []
        for comp in ("runtime", "vad", "tts-kokoro"):
            if comp not in self.components:
                out.append(comp)
        if not self.stt_models:
            out.append("stt (moonshine or whisper)")
        return out


def find_data_root(explicit=None):
    """(DataRoot or None, [searched {path, why, problems}])."""
    searched = []
    for path, why in candidate_roots(explicit):
        root = DataRoot(path)
        if root.usable:
            return root, searched + [{"path": path, "why": why, "usable": True}]
        searched.append({"path": path, "why": why, "usable": False,
                         "problems": root.problems or [f"incomplete: missing {', '.join(root.missing_components())}"]})
    return None, searched


def ext_suffix():
    return sysconfig.get_config_var("EXT_SUFFIX") or ".so"


class RuntimeDepsError(Exception):
    """numpy or sherpa_onnx cannot be imported; .missing names what to install."""

    def __init__(self, message, missing):
        super().__init__(message)
        self.missing = missing


def activate_runtime(root):
    """Put <root>/runtime first on sys.path and import numpy + sherpa_onnx.

    Returns {"numpy": version, "sherpa_onnx": version, "import_s": seconds}.
    runtime/ goes FIRST so the pinned sherpa_onnx (and the numpy wheel, when the
    installer had to fall back to it) win over anything else on the system.
    """
    rt = os.path.join(root.path, "runtime")
    if rt not in sys.path:
        sys.path.insert(0, rt)
    t = time.monotonic()
    try:
        import numpy
        numpy_version = numpy.__version__      # an empty leftover numpy/ directory imports as a namespace
        numpy.zeros(1)                         # package and fails only here
    except Exception as e:  # ImportError, a broken binary (OSError), an incomplete package (AttributeError)
        raise RuntimeDepsError(f"numpy cannot be imported ({type(e).__name__}: {e}). "
                               "Install it: sudo apt install python3-numpy", ["python3-numpy"]) from e
    so_name = "_sherpa_onnx" + ext_suffix()
    if not os.path.exists(os.path.join(rt, "sherpa_onnx", "lib", so_name)):
        raise RuntimeDepsError(
            f"the installed sherpa-onnx runtime has no module for Python {platform.python_version()} "
            f"({so_name}). Re-run shadow-voice-setup to install it for this Python.",
            [f"sherpa-onnx runtime for Python {sys.version_info[0]}.{sys.version_info[1]}"])
    try:
        import sherpa_onnx
    except Exception as e:
        raise RuntimeDepsError(f"sherpa_onnx cannot be imported from {rt} ({e}). Re-run shadow-voice-setup.",
                               ["sherpa-onnx runtime"]) from e
    return {"numpy": numpy_version, "sherpa_onnx": getattr(sherpa_onnx, "__version__", "?"),
            "import_s": round(time.monotonic() - t, 3)}


def first_tool(names, path=None):
    for n in names:
        if shutil.which(n, path=path):
            return n
    return None


def probe(explicit=None):
    """The --probe answer (spec 2.1): (dict, exit code). Loads no model."""
    t0 = time.monotonic()
    out = {"ok": False, "version": VERSION, "data": None, "python": platform.python_version(),
           "missing": [], "warnings": [], "stt": [], "tts": [], "kws": False,
           "capture": first_tool(CAPTURE_TOOLS), "playback": first_tool(PLAYBACK_TOOLS), "reason": ""}
    root, searched = find_data_root(explicit)
    code = EXIT_OK
    if root is None:
        out["missing"].append("engine data")
        out["searched"] = searched
        detail = "; ".join(f"{s['path']}: {s['problems'][0]}" for s in searched if s.get("problems"))
        out["reason"] = f"Voice engine data is not installed ({detail}). {SETUP_HINT}"
        code = EXIT_NO_DATA
    else:
        out.update(data=root.path, stt=root.stt_models, tts=["kokoro"], kws=root.has_kws,
                   stt_default=root.stt_default)
        if not root.has_kws:
            out["warnings"].append("keyword spotter not installed: the wake word is confirmed by speech-to-text only")
        try:
            out.update(activate_runtime(root))
        except RuntimeDepsError as e:
            out["missing"].extend(e.missing)
            out["reason"] = str(e)
            code = EXIT_NO_PYDEPS
    if code == EXIT_OK and not out["capture"]:
        out["missing"].append("capture tool (pw-record, parec or arecord)")
        out["reason"] = ("No microphone capture tool found. Install one: sudo apt install pipewire-bin "
                         "(or pulseaudio-utils / alsa-utils)")
        code = EXIT_NO_CAPTURE
    if not out["playback"]:
        out["warnings"].append("no playback tool (pw-play, pacat or aplay): replies cannot be spoken. "
                               "Install: sudo apt install pipewire-bin")
    if not shutil.which("espeak-ng"):
        out["warnings"].append("espeak-ng is not installed: words outside the voice lexicon are spelled out. "
                               "Install: sudo apt install espeak-ng")
    out["ok"] = code == EXIT_OK
    if out["ok"]:
        out["reason"] = "ready"
    out["probe_s"] = round(time.monotonic() - t0, 3)
    return out, code
