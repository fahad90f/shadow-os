"""Text normalisation for speech: what a reply LOOKS like -> what should be SAID.

Kokoro (through sherpa-onnx 2.x) pronounces only words found in its lexicon;
everything else is silently dropped. So "82%", "16 GB", "PipeWire", "CPU",
"~/Pictures/shot.png" or "Python 3.13" must be turned into plain words first.
This module does that (pure Python, unit-tested):

  * unicode clean-up: emoji, pictographs, zero-width and odd symbols removed,
    smart quotes/dashes/bullets mapped, non-Latin scripts dropped;
  * Markdown residue (the app strips Markdown; this is the safety net);
  * URLs -> their host ("github dot com"), e-mail addresses, file paths and
    file names ("home slash Pictures slash shot dot P·N·G");
  * numbers: integers, decimals, thousands separators, negatives, ordinals,
    years, ranges, fractions, currency, percent, times, ISO dates, versions
    ("Python 3.13" -> "three point thirteen", "2.0.2" -> "two point oh point two");
  * units (GB, MB/s, GHz, °C, ms, ...) with singular/plural;
  * CamelCase splitting and acronym spelling ("USB" -> "U·S·B"; "RAM" stays a word).

Spelled letters are joined with "·" so the phoneme front end (g2p.py) can tell
the letter "A" from the article "a".
"""
import re
import unicodedata

SPELL_SEP = "·"

_ONES = ("zero one two three four five six seven eight nine ten eleven twelve thirteen fourteen fifteen "
         "sixteen seventeen eighteen nineteen").split()
_TENS = "_ _ twenty thirty forty fifty sixty seventy eighty ninety".split()
_ORD = {"one": "first", "two": "second", "three": "third", "five": "fifth", "eight": "eighth",
        "nine": "ninth", "twelve": "twelfth"}
_SCALES = ((10 ** 15, "quadrillion"), (10 ** 12, "trillion"), (10 ** 9, "billion"), (10 ** 6, "million"),
           (1000, "thousand"))


def num2words(n):
    n = int(n)
    if n < 0:
        return "minus " + num2words(-n)
    if n < 20:
        return _ONES[n]
    if n < 100:
        return _TENS[n // 10] + ("" if n % 10 == 0 else "-" + _ONES[n % 10])
    if n < 1000:
        return _ONES[n // 100] + " hundred" + ("" if n % 100 == 0 else " " + num2words(n % 100))
    for div, name in _SCALES:
        if n >= div:
            if n // div >= 1000 and div == _SCALES[0][0]:
                return " ".join(_ONES[int(c)] for c in str(n))
            return num2words(n // div) + " " + name + ("" if n % div == 0 else " " + num2words(n % div))
    return str(n)


def ordinal(n):
    w = num2words(n)
    sep = "-" if "-" in w.split()[-1] else " "
    head, _, last = w.rpartition(sep)
    if last in _ORD:
        last = _ORD[last]
    elif last.endswith("y"):
        last = last[:-1] + "ieth"
    else:
        last += "th"
    return (head + sep + last) if head else last


def year_words(n):
    if 2000 <= n <= 2009 or n < 1000:
        return num2words(n)
    hi, lo = divmod(n, 100)
    if lo == 0:
        return num2words(hi) + " hundred"
    return num2words(hi) + " " + (("oh " + num2words(lo)) if lo < 10 else num2words(lo))


def _decade(n):
    """1990 -> 'nineteen nineties', 2000 -> 'two thousands'."""
    w = year_words(n)
    head, _, last = w.rpartition(" ")
    last = last[:-1] + "ies" if last.endswith("y") else last + "s"
    return (head + " " + last) if head else last


def digits_words(s):
    return " ".join(_ONES[int(c)] for c in s if c.isdigit())


def decimal_words(intpart, frac):
    return f"{num2words(int(intpart or '0'))} point {digits_words(frac)}"


def version_words(v):
    """'2.0.2' -> 'two point oh point two'; '3.13' -> 'three point thirteen'."""
    parts = v.split(".")
    out = []
    for p in parts:
        if not p.isdigit():
            out.append(p)
        elif p == "0":
            out.append("oh" if len(out) else "zero")
        elif len(p) > 1 and p.startswith("0"):
            out.append(digits_words(p))
        else:
            out.append(num2words(int(p)))
    return " point ".join(out)


_FRACTIONS = {"1/2": "one half", "1/3": "one third", "2/3": "two thirds", "1/4": "one quarter",
              "3/4": "three quarters", "1/8": "one eighth", "1/10": "one tenth"}

# unit -> (singular, plural); matched case-sensitively first, then case-insensitively.
_UNITS = {
    "B": ("byte", "bytes"), "KB": ("kilobyte", "kilobytes"), "kB": ("kilobyte", "kilobytes"),
    "MB": ("megabyte", "megabytes"), "GB": ("gigabyte", "gigabytes"), "TB": ("terabyte", "terabytes"),
    "PB": ("petabyte", "petabytes"), "KiB": ("kibibyte", "kibibytes"), "MiB": ("mebibyte", "mebibytes"),
    "GiB": ("gibibyte", "gibibytes"), "TiB": ("tebibyte", "tebibytes"),
    "Kb": ("kilobit", "kilobits"), "Mb": ("megabit", "megabits"), "Gb": ("gigabit", "gigabits"),
    "kbps": ("kilobit per second", "kilobits per second"), "Kbps": ("kilobit per second", "kilobits per second"),
    "Mbps": ("megabit per second", "megabits per second"), "Gbps": ("gigabit per second", "gigabits per second"),
    "KB/s": ("kilobyte per second", "kilobytes per second"), "MB/s": ("megabyte per second", "megabytes per second"),
    "GB/s": ("gigabyte per second", "gigabytes per second"),
    "Hz": ("hertz", "hertz"), "kHz": ("kilohertz", "kilohertz"), "KHz": ("kilohertz", "kilohertz"),
    "MHz": ("megahertz", "megahertz"), "GHz": ("gigahertz", "gigahertz"),
    "ms": ("millisecond", "milliseconds"), "µs": ("microsecond", "microseconds"), "us": None,
    "ns": ("nanosecond", "nanoseconds"), "sec": ("second", "seconds"), "secs": ("second", "seconds"),
    "s": ("second", "seconds"), "min": ("minute", "minutes"), "mins": ("minute", "minutes"),
    "h": ("hour", "hours"), "hr": ("hour", "hours"), "hrs": ("hour", "hours"),
    "°C": ("degree Celsius", "degrees Celsius"), "°F": ("degree Fahrenheit", "degrees Fahrenheit"),
    "°": ("degree", "degrees"), "K": None,
    "W": ("watt", "watts"), "kW": ("kilowatt", "kilowatts"), "Wh": ("watt hour", "watt hours"),
    "kWh": ("kilowatt hour", "kilowatt hours"), "mAh": ("milliamp hour", "milliamp hours"),
    "V": ("volt", "volts"), "mV": ("millivolt", "millivolts"), "A": None, "mA": ("milliamp", "milliamps"),
    "km": ("kilometer", "kilometers"), "m": ("meter", "meters"), "cm": ("centimeter", "centimeters"),
    "mm": ("millimeter", "millimeters"), "kg": ("kilogram", "kilograms"), "g": ("gram", "grams"),
    "mg": ("milligram", "milligrams"), "lb": ("pound", "pounds"), "lbs": ("pound", "pounds"),
    "km/h": ("kilometer per hour", "kilometers per hour"), "mph": ("mile per hour", "miles per hour"),
    "fps": ("frame per second", "frames per second"), "dpi": ("dot per inch", "dots per inch"),
    "px": ("pixel", "pixels"), "pt": ("point", "points"), "x": ("times", "times"),
    "cores": ("core", "cores"), "GT/s": ("gigatransfer per second", "gigatransfers per second"),
}
_UNIT_ALT = "|".join(sorted((re.escape(u) for u, v in _UNITS.items() if v), key=len, reverse=True))

_CURRENCY = {"$": ("dollar", "dollars", "cent", "cents"), "€": ("euro", "euros", "cent", "cents"),
             "£": ("pound", "pounds", "penny", "pence"), "¥": ("yen", "yen", "", "")}
_MONTHS = ("January February March April May June July August September October November December").split()

# Acronyms that are said as words, and short all-caps words that are just shouting.
WORD_ACRONYMS = frozenset("RAM ROM NASA GNOME LAN WAN SIM PIN GIF LASER RADAR NATO WIFI UNIX LINUX POSIX "
                          "CAPTCHA ASCII JPEG IKEA LEGO SWAT YAML TOML COVID OPEC UNICEF FIFA AIDS SCSI "
                          "SATA PCIE NVME QR OK".split())
_ACRONYM_SAY = {"JSON": "jay son", "SQL": "sequel", "WIFI": "wifi", "NVME": f"N{SPELL_SEP}V{SPELL_SEP}M{SPELL_SEP}E",
                "PCIE": f"P{SPELL_SEP}C{SPELL_SEP}I{SPELL_SEP}E", "QR": f"Q{SPELL_SEP}R", "OK": "okay",
                "SCSI": "scuzzy", "SATA": "sah tah", "GUI": "gooey", "WYSIWYG": "wizzy wig", "SUDO": "sue doo"}
SHOUTED_WORDS = frozenset("NO GO DO ON OFF YES AND THE FOR NOT ALL NEW NOW OUT BUT YOU ARE CAN GET SET RUN TOP "
                          "BIG HOT WIN STOP NOTE READ WARN WAIT DONE FAIL PASS SAVE OPEN HELP INFO TIP FREE "
                          "BE ME MY WE HE SHE HIS HER OUR ONE TWO SIX TEN UP AT AS IF OR SO TO OF BY AN".split())
# Words the lexicon gets wrong or does not have, written the way they are said.
SAY_AS = {"wi-fi": "wifi", "qt": "cute", "sudo": "sue doo", "systemd": "system D", "nginx": "engine x",
          "kwin": f"K{SPELL_SEP} win", "pipewire": "pipe wire", "wireplumber": "wire plumber",
          "github": "git hub", "gitlab": "git lab", "onnx": f"O{SPELL_SEP}N{SPELL_SEP}N{SPELL_SEP}X",
          "macos": f"mac O{SPELL_SEP}S", "ios": f"I{SPELL_SEP}O{SPELL_SEP}S",
          "e.g.": "for example", "i.e.": "that is", "etc.": "et cetera", "vs.": "versus", "vs": "versus",
          "approx.": "approximately", "dr.": "doctor", "mr.": "mister", "mrs.": "missus", "ms.": "miz",
          "no.": "number", "nr.": "number", "w/": "with", "w/o": "without", "a.m.": f"A{SPELL_SEP}M",
          "p.m.": f"P{SPELL_SEP}M"}
_VERSION_WORDS = ("version", "versions", "release", "python", "qt", "kde", "plasma", "linux", "kernel",
                  "debian", "ubuntu", "gnome", "firefox", "chrome", "chromium", "node", "java", "gcc", "cmake",
                  "sherpa-onnx", "pipewire", "kwin", "mesa", "android", "ios", "macos", "windows", "trixie",
                  "bookworm", "update", "upgrade", "build", "glibc", "openssl", "perl", "ruby", "go", "rust")
_EXTENSIONS_SAY = {"txt": "text", "md": "markdown", "conf": "conf", "cfg": "config", "jpeg": "jpeg", "zip": "zip",
                   "log": "log", "deb": "deb", "gz": f"G{SPELL_SEP}Z", "tar": "tar", "doc": "doc", "docx": "doc x",
                   "xlsx": "excel", "json": "jay son", "yaml": "yaml", "html": f"H{SPELL_SEP}T{SPELL_SEP}M{SPELL_SEP}L",
                   "desktop": "desktop", "service": "service", "py": f"P{SPELL_SEP}Y", "sh": f"S{SPELL_SEP}H",
                   "ini": "ini", "iso": f"I{SPELL_SEP}S{SPELL_SEP}O", "img": "image", "wav": "wave", "mp3": f"M{SPELL_SEP}P three",
                   "mp4": f"M{SPELL_SEP}P four", "ogg": "ogg", "flac": "flack", "png": f"P{SPELL_SEP}N{SPELL_SEP}G",
                   "jpg": f"J{SPELL_SEP}P{SPELL_SEP}G", "pdf": f"P{SPELL_SEP}D{SPELL_SEP}F", "svg": f"S{SPELL_SEP}V{SPELL_SEP}G",
                   "cpp": f"C{SPELL_SEP}P{SPELL_SEP}P", "h": "H", "c": "C", "js": f"J{SPELL_SEP}S", "ts": f"T{SPELL_SEP}S",
                   "qml": f"Q{SPELL_SEP}M{SPELL_SEP}L", "csv": f"C{SPELL_SEP}S{SPELL_SEP}V", "xml": f"X{SPELL_SEP}M{SPELL_SEP}L",
                   "rs": f"R{SPELL_SEP}S", "exe": f"E{SPELL_SEP}X{SPELL_SEP}E", "appimage": "app image", "rpm": f"R{SPELL_SEP}P{SPELL_SEP}M"}


def spell(word):
    """'CPU' -> 'C·P·U' (digits become words: 'MP3' -> 'M·P three')."""
    out, letters = [], []
    for ch in re.findall(r"[A-Za-z]|\d+", word):
        if ch.isdigit():
            if letters:
                out.append(SPELL_SEP.join(letters))
                letters = []
            out.append(num2words(int(ch)) if len(ch) < 5 else digits_words(ch))
        else:
            letters.append(ch.upper())
    if letters:
        out.append(SPELL_SEP.join(letters) + (SPELL_SEP if len(letters) == 1 else ""))
    return " ".join(out)


# --------------------------------------------------------------------------- unicode
_DROP_RANGES = ((0x1F000, 0x1FAFF), (0x2600, 0x27BF), (0x2B00, 0x2BFF), (0xFE00, 0xFE0F), (0x1F3FB, 0x1F3FF),
                (0xE0000, 0xE007F), (0x200B, 0x200F), (0x2060, 0x206F), (0xFEFF, 0xFEFF), (0x2300, 0x23FF),
                (0x25A0, 0x25FF), (0x2190, 0x21FF))
_CHAR_MAP = {"‘": "'", "’": "'", "‚": "'", "‛": "'", "′": "'", "“": '"', "”": '"', "„": '"', "″": '"',
             "«": '"', "»": '"', "‹": "'", "›": "'", "–": "–", "—": "—", "―": "—", "‐": "-", "‑": "-",
             "‒": "-", "−": "-", "…": "…", "•": ", ", "◦": ", ", "‣": ", ", "∙": ", ", "▪": ", ", "·": " ",
             "→": " to ", "⇒": " to ", "➜": " to ", "←": " from ", "×": " times ", "÷": " divided by ",
             "±": " plus or minus ", "≈": " about ", "≤": " at most ", "≥": " at least ", "≠": " not equal to ",
             "©": " copyright ", "®": "", "™": "", "§": " section ", "№": " number ", " ": " ",
             " ": " ", " ": " ", "\t": " ", "µ": "µ", "°": "°"}


def clean_unicode(text):
    out = []
    for ch in unicodedata.normalize("NFC", text):
        if ch in _CHAR_MAP:
            out.append(_CHAR_MAP[ch])
            continue
        cp = ord(ch)
        if any(lo <= cp <= hi for lo, hi in _DROP_RANGES):
            out.append(" ")
            continue
        cat = unicodedata.category(ch)
        if cat in ("So", "Sk", "Cs", "Co", "Cn", "Cf", "Me", "Mn") and ch not in "°µ":
            out.append(" " if cat in ("So", "Sk") else "")
            continue
        if cat.startswith("C") and ch not in "\n":
            out.append(" ")
            continue
        if cat.startswith("L") and not _latin(ch):
            nk = unicodedata.normalize("NFKC", ch)
            out.append(nk if nk.isascii() else " ")
            continue
        out.append(ch)
    return "".join(out)


def _latin(ch):
    if ch.isascii():
        return True
    try:
        return "LATIN" in unicodedata.name(ch)
    except ValueError:
        return False


def strip_accents(word):
    return "".join(c for c in unicodedata.normalize("NFKD", word) if not unicodedata.combining(c))


# --------------------------------------------------------------------------- markdown residue
def strip_markdown(t):
    t = re.sub(r"```.*?(```|$)", " (code omitted). ", t, flags=re.S)
    t = re.sub(r"`([^`\n]{1,80})`", r"\1", t)
    t = t.replace("`", " ")
    t = re.sub(r"!\[([^\]]*)\]\([^)]*\)", r"\1", t)
    t = re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", t)
    t = re.sub(r"(?m)^\s{0,3}#{1,6}\s+(.*)$", r"\1.", t)
    t = re.sub(r"(?m)^\s{0,3}>\s?", "", t)
    t = re.sub(r"(?m)^\s*(?:[-*+]|\d{1,2}[.)])\s+(?=\S)", ". ", t)           # list items -> own sentences
    t = re.sub(r"(?m)^\s*\|?(?:\s*:?-{3,}:?\s*\|)+\s*:?-*:?\s*$", " ", t)     # table separator rows
    t = re.sub(r"\s*\|\s*", ", ", t)
    t = re.sub(r"(\*\*|__|~~)(.+?)\1", r"\2", t)
    t = re.sub(r"(?<![\w*])\*(?!\s)([^*\n]+?)(?<!\s)\*(?![\w*])", r"\1", t)
    t = re.sub(r"(?<![\w_])_(?!\s)([^_\n]+?)(?<!\s)_(?![\w_])", r"\1", t)
    t = re.sub(r"</?[a-zA-Z][^>\n]{0,40}>", " ", t)
    return t


# --------------------------------------------------------------------------- links and paths
def _host_words(host):
    host = re.sub(r"^www\.", "", host.lower())
    return " dot ".join(p for p in host.split(".") if p)


def _url(m):
    return " " + _host_words(m.group(1)) + " "


def _email(m):
    user = re.sub(r"[._+-]", lambda c: {".": " dot ", "_": " underscore ", "+": " plus ", "-": " dash "}[c.group(0)],
                  m.group(1))
    return f" {user} at {_host_words(m.group(2))} "


def _filename(name):
    """'shot-2026.png' -> 'shot 2026 dot P·N·G'."""
    base, dot, ext = name.rpartition(".")
    if not dot or not base or not re.fullmatch(r"[A-Za-z0-9]{1,9}", ext) or ext.isdigit():
        return re.sub(r"[_\-]+", " ", name).replace(".", " dot ")
    e = ext.lower()
    say = _EXTENSIONS_SAY.get(e)
    if say is None:
        say = spell(ext) if (len(ext) <= 4 and not re.search(r"[aeiou]", e)) else ext
    base = re.sub(r"[_\-]+", " ", base).replace(".", " dot ")
    return f"{base} dot {say}"


# How the usual Unix directory names are said ("slash E·T·C slash fstab", "slash user slash bin").
_PATH_WORDS = {"etc": f"E{SPELL_SEP}T{SPELL_SEP}C", "usr": "user", "tmp": "temp", "bin": "bin", "sbin": "S bin",
               "lib": "lib", "var": "var", "dev": "dev", "opt": "opt", "proc": "proc", "sys": "sys",
               ".config": "dot config", ".local": "dot local", ".cache": "dot cache"}


def _path(m):
    p = m.group(0)
    tail = p[len(p.rstrip(".,;:!?")):]            # "...shot.png." -> the sentence's full stop
    p = p[:len(p) - len(tail)] if tail else p
    lead = ""
    if p.startswith("~/"):
        lead, p = "home slash ", p[2:]
    elif p.startswith("~"):
        lead, p = "home ", p[1:]
    elif p.startswith("../"):
        lead, p = "dot dot slash ", p[3:]
    elif p.startswith("./"):
        lead, p = "", p[2:]
    elif p.startswith("/"):
        lead, p = "slash ", p[1:]
    parts = [x for x in p.split("/") if x]
    said = [_PATH_WORDS.get(x, None) or (_filename(x) if "." in x.strip(".") else re.sub(r"[_\-]+", " ", x))
            for x in parts]
    return " " + lead + " slash ".join(said) + tail + " "


_PATH_RE = re.compile(r"(?<![\w/:.~])(?:~/|\.{1,2}/|/)(?:[\w.@%+\-]+/?)+(?![\w/])|"
                      r"(?<![\w/:.])[A-Za-z_][\w.\-]*(?:/[\w.\-]+){2,}/?(?![\w/])")
_FILE_RE = re.compile(r"(?<![\w/.@])([A-Za-z_][\w\-]*(?:\.[\w\-]+)*\.(?:" + "|".join(
    sorted(_EXTENSIONS_SAY, key=len, reverse=True)) + r"))(?![\w/])", re.I)


# --------------------------------------------------------------------------- numbers
def _currency(m):
    sym, whole, cents = m.group(1), m.group(2).replace(",", ""), m.group(3)
    one, many, sub1, subn = _CURRENCY[sym]
    w = int(whole)
    out = f"{num2words(w)} {one if w == 1 else many}"
    if cents and int(cents) and sub1:
        c = int(cents.ljust(2, "0")[:2])
        out += f" and {num2words(c)} {sub1 if c == 1 else subn}"
    return f" {out} "


def _time(m):
    h, mi, ampm = int(m.group(1)), m.group(2), (m.group(3) or "")
    if h > 24 or int(mi) > 59:
        return m.group(0)
    if mi == "00":
        words = num2words(h) + ("" if ampm else " o'clock") if h <= 12 or ampm else num2words(h) + " hundred"
    elif mi[0] == "0":
        words = f"{num2words(h)} oh {num2words(int(mi[1]))}"
    else:
        words = f"{num2words(h)} {num2words(int(mi))}"
    if ampm:
        words += " " + spell(ampm.replace(".", "").upper())
    return f" {words} "


def _iso_date(m):
    y, mo, d = int(m.group(1)), int(m.group(2)), int(m.group(3))
    if not (1 <= mo <= 12 and 1 <= d <= 31):
        return m.group(0)
    return f" {_MONTHS[mo - 1]} {ordinal(d)}, {year_words(y)} "


def _unit(m):
    num, unit = m.group(1), m.group(2)
    entry = _UNITS.get(unit)
    if entry is None:
        return m.group(0)
    one, many = entry
    plain = num.replace(",", "")
    try:
        is_one = float(plain) == 1.0 and "." not in plain
    except ValueError:
        is_one = False
    return f" {plain} {one if is_one else many} "


def _number(m):
    s = m.group(0).replace(",", "")
    neg = s.startswith("-")
    s = s.lstrip("-")
    if "." in s:
        i, f = s.split(".", 1)
        w = decimal_words(i, f)
    elif len(s) > 1 and s.startswith("0"):
        w = digits_words(s)                       # "007", zip codes, PINs
    elif len(s) > 15:
        w = digits_words(s)
    else:
        w = num2words(int(s))
    return f" {'minus ' if neg else ''}{w} "


def spell_numbers(text):
    """Light number expansion for comparing transcripts (no lexicon needed)."""
    t = re.sub(r"(\d+(?:\.\d+)?)\s?%", r"\1 percent", text)
    t = re.sub(r"\b(\d+)(st|nd|rd|th)\b", lambda m: ordinal(int(m.group(1))), t)
    t = re.sub(r"(?<![\w.])-?\d[\d,]*(?:\.\d+)?", _number, t)
    return re.sub(r"\s+", " ", t).strip()


def _versions(t):
    def kw(m):
        return f"{m.group(1)} {version_words(m.group(2))} "
    t = re.sub(r"(?<![\w.])v(\d+(?:\.\d+)+)\b", lambda m: f" version {version_words(m.group(1))} ", t)
    t = re.sub(r"(?i)\b(" + "|".join(re.escape(w) for w in _VERSION_WORDS) + r")\s?v?(\d+(?:\.\d+)+)\b", kw, t)
    t = re.sub(r"(?<![\w.])v(\d+(?:\.\d+)+)\b", lambda m: f" version {version_words(m.group(1))} ", t)
    t = re.sub(r"(?<![\w.])(\d+\.\d+\.\d+(?:\.\d+)*)(?![\w.]|\.\d)", lambda m: f" {version_words(m.group(1))} ", t)
    return t


def normalize_numbers(t):
    t = re.sub(r"(?<=\d),(?=\d{3}\b)", "", t)                                  # 1,024 -> 1024
    t = re.sub(r"\b(\d{4})-(\d{2})-(\d{2})\b", _iso_date, t)
    t = re.sub(r"\b(\d{1,2}):(\d{2})(?::\d{2})?\s?([AaPp]\.?[Mm]\.?)?(?![\w:])", _time, t)
    t = re.sub(r"\b(\d{1,2})\s?([AaPp]\.?[Mm]\.?)(?=[\s,.!?;:]|$)",
               lambda m: f" {num2words(int(m.group(1)))} {spell(m.group(2).replace('.', '').upper())} ", t)
    t = _versions(t)
    t = re.sub(r"([$€£¥])\s?(\d[\d,]*)(?:\.(\d{1,2}))?\b", _currency, t)
    t = re.sub(r"(\d+(?:\.\d+)?)\s?%", r"\1 percent", t)
    t = re.sub(r"\b(\d+)(st|nd|rd|th)\b", lambda m: f" {ordinal(int(m.group(1)))} ", t)
    t = re.sub(r"\b(1[1-9]|20)(\d)0'?s\b", lambda m: " " + _decade(int(m.group(1) + m.group(2) + "0")) + " ", t)
    t = re.sub(r"\b24/7\b", " twenty-four seven ", t)
    t = re.sub(r"\b(\d+)/(\d+)\b", lambda m: f" {_FRACTIONS.get(m.group(0), num2words(int(m.group(1))) + ' over ' + num2words(int(m.group(2))))} ", t)
    t = re.sub(r"\b(\d+(?:\.\d+)?)\s?(?:-|–|to)\s?(\d+(?:\.\d+)?)\s?(percent|" + _UNIT_ALT + r")?\b",
               lambda m: f" {m.group(1)} to {m.group(2)} {m.group(3) or ''} ", t)
    t = re.sub(r"(\d+(?:\.\d+)?)\s?(" + _UNIT_ALT + r")(?![\w/])", _unit, t)
    t = re.sub(r"\b(\d{3,4})p\b", lambda m: f" {year_words(int(m.group(1))) if len(m.group(1)) == 4 else num2words(int(m.group(1)))} P{SPELL_SEP} ", t)
    t = re.sub(r"\b(\d+)[kK]\b", lambda m: f" {num2words(int(m.group(1)))} K{SPELL_SEP} ", t)
    t = re.sub(r"(?<![\w.])(1[1-9]\d\d|20\d\d)(?![\w.]|\s*(?:percent|dollars|euros|pounds|" + "|".join(
        sorted({v[1].split()[0] for v in _UNITS.values() if v}, key=len, reverse=True)) + r"))",
        lambda m: f" {year_words(int(m.group(1)))} ", t)
    t = re.sub(r"(?<![\w.])-?\d+(?:\.\d+)?(?![\w])", _number, t)
    t = re.sub(r"(?<=[A-Za-z])(?=\d)|(?<=\d)(?=[A-Za-z])", " ", t)             # x86 -> x 86
    t = re.sub(r"\d+", lambda m: _number(m), t)
    return t


# --------------------------------------------------------------------------- words
def _camel(m):
    w = m.group(0)
    low = w.lower()
    if low in SAY_AS:
        return SAY_AS[low]
    if re.fullmatch(r"[A-Z]{2,}s", w):
        return w                                    # "CPUs": an acronym plural, see acronyms()
    return " ".join(re.findall(r"[A-Z]+(?=[A-Z][a-z])|[A-Z]?[a-z']+|[A-Z]+", w))


def acronyms(t, known_word=None):
    """Say all-caps tokens the way people do: USB -> U·S·B, CPUs -> U·S·Bs, RAM stays a
    word, and a run of capitalised ordinary words ("DO NOT TURN OFF") is shouting, not
    a list of acronyms. A lone short caps word that is also an English word ("IT",
    "US") is spelled: in a reply that is almost always the acronym."""
    toks = list(re.finditer(r"\b[A-Z][A-Z']*[A-Z]s?\b", t))

    def word_like(w):
        core = w[:-1] if (len(w) > 2 and w.endswith("s") and w[:-1].isupper()) else w
        return core in SHOUTED_WORDS or (known_word is not None and known_word(core.lower()))

    def shouting(i):
        """A neighbour (separated by spaces only) is an ordinary word in capitals too."""
        prev = i > 0 and t[toks[i - 1].end():toks[i].start()].isspace() and word_like(toks[i - 1].group(0))
        nxt = (i + 1 < len(toks) and t[toks[i].end():toks[i + 1].start()].isspace()
               and word_like(toks[i + 1].group(0)))
        return bool(prev or nxt)

    out, pos = [], 0
    for i, m in enumerate(toks):
        w = m.group(0)
        plural = len(w) > 2 and w.endswith("s") and w[:-1].isupper()
        core = w[:-1] if plural else w
        low = core.lower()
        if core in _ACRONYM_SAY:
            say = _ACRONYM_SAY[core]
        elif core in WORD_ACRONYMS:
            say = low
        elif core in SHOUTED_WORDS or (word_like(w) and (shouting(i) or len(core) >= 7)):
            say = low                               # shouting or a long capitalised word
        elif len(core) >= 7:
            say = low
        else:
            say = spell(core)
        if plural:
            say += "s"
        out.append(t[pos:m.start()])
        out.append(say)
        pos = m.end()
    out.append(t[pos:])
    return "".join(out)


def normalize(text, known_word=None):
    """Full normalisation of one reply (may contain several sentences)."""
    t = clean_unicode(text or "")
    t = strip_markdown(t)
    t = re.sub(r"(?i)\bhttps?://([^\s/:?#]+)[^\s]*", _url, t)
    t = re.sub(r"(?i)(?<![\w@.])www\.([a-z0-9-]+(?:\.[a-z0-9-]+)+)[^\s]*", lambda m: " " + _host_words(m.group(1)) + " ", t)
    t = re.sub(r"\b([\w.+-]+)@([A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)+)\b", _email, t)
    for k in ("e.g.", "i.e.", "etc.", "approx.", "a.m.", "p.m."):
        t = re.sub(r"(?i)(?<!\w)" + re.escape(k), " " + SAY_AS[k] + " ", t)
    t = re.sub(r"(?i)\b(vs|dr|mr|mrs|ms|no|nr)\.(?=\s+\S)", lambda m: " " + SAY_AS[m.group(1).lower() + "."] + " "
               if m.group(1).lower() != "no" or re.match(r"\s*\d", t[m.end():]) else m.group(0), t)
    t = _PATH_RE.sub(_path, t)
    t = _FILE_RE.sub(lambda m: " " + _filename(m.group(1)) + " ", t)
    t = re.sub(r"\bC\+\+", "C plus plus", t)
    t = re.sub(r"\bC#", "C sharp", t)
    t = re.sub(r"~\s?(?=\d)", " about ", t)
    t = re.sub(r"#\s?(?=\d)", " number ", t)
    t = normalize_numbers(t)
    t = re.sub(r"(?<=\d)\s*x\s*(?=\d)", " by ", t)
    t = t.replace("&", " and ").replace("@", " at ")
    t = re.sub(r"\s\+\s|(?<=\w)\+(?=\w)", " plus ", t)
    t = re.sub(r"\s=\s|(?<=\w)=(?=\w)", " equals ", t)
    t = re.sub(r"\s<\s", " less than ", t)
    t = re.sub(r"\s>\s", " greater than ", t)
    t = re.sub(r"(?<=[a-z])/(?=[a-z])", " or ", t)
    t = t.replace("/", " slash ")
    t = re.sub(r"\b[A-Za-z]+(?:[A-Z][a-z]+)+\b|\b[a-z]+[A-Z]\w*\b|\b[A-Z]{2,}[a-z]\w*\b", _camel, t)
    t = re.sub(r"(?i)\b(" + "|".join(re.escape(k) for k in SAY_AS if re.fullmatch(r"[a-z]+", k)) + r")\b",
               lambda m: SAY_AS[m.group(1).lower()] if not m.group(1).isupper() or m.group(1).lower() in ("qt",)
               else m.group(1), t)
    t = acronyms(t, known_word)
    t = re.sub(r"[*_#<>\[\]{}|\\^~`$€£¥%]", " ", t)
    t = re.sub(r"\s*[–—]\s*", ", ", t)
    t = re.sub(r"\s+([,.!?;:…])", r"\1", t)
    t = re.sub(r"([,;:])(?:\s*[,;:])+", r"\1", t)
    t = re.sub(r"(^|[.!?…]\s*)[,;:]\s*", r"\1", t)
    t = re.sub(r"\.(?:\s*\.)+", ".", t)
    t = re.sub(r"\s+", " ", t).strip(" ,;:")
    return t


# --------------------------------------------------------------------------- sentences
MAX_WORDS = 40
FIRST_WORDS = 10
FIRST_CLAUSE = 6       # a comma within the first 6 words ends the first chunk there
# Conjunctions a long sentence may be cut before (after a comma is preferred).
_CONJ = frozenset("and but or so because while which when where then although though unless until "
                  "since whereas".split())


def _natural_cut(words, lo, hi):
    """Last comma/semicolon/colon (cut after it) or conjunction (cut before it) in [lo, hi]."""
    hi = min(hi, len(words) - 3)
    commas = [i + 1 for i in range(lo - 1, hi) if words[i].endswith((",", ";", ":"))]
    if commas:
        return commas[-1]
    conj = [i for i in range(lo, hi + 1) if words[i].lower() in _CONJ]
    return conj[-1] if conj else None


def _cut(words, limit, first):
    lo = max(3, limit // 3)
    cut = _natural_cut(words, lo, limit)
    if cut is None:
        # nothing natural before the limit: a little later is better than mid-phrase,
        # except for the very first chunk, whose length is the reply's first-audio latency
        cut = _natural_cut(words, limit, int(limit * (1.4 if first else 2)))
    if cut is None and (first or len(words) > MAX_WORDS):
        cut = min(limit, len(words) - 3)
    return cut if cut and cut >= 3 else len(words)


def split_sentences(norm, first_short=False):
    """Normalised text -> speakable chunks (sentences; long ones cut at commas/conjunctions).

    first_short (start of a reply): the first chunk has at most ~10 words so the first
    audio comes quickly, and every following chunk at most about twice the words already
    queued: it is synthesised while those play (synthesis runs at ~0.3-0.5x real time
    on a 4-core laptop), so a short "Okay." followed by a 20-word sentence does not
    leave a silent gap. Later chunks may be up to 40 words (Kokoro's limit is 510
    phoneme tokens)."""
    parts = [p for p in re.split(r"(?<=[.!?…])\s+(?=\S)", norm) if re.search(r"[A-Za-z]", p)]
    out = []
    queued = 0
    for p in parts:
        words = p.split()
        while words:
            if first_short:
                limit = FIRST_WORDS if not out else max(6, min(MAX_WORDS, 2 * queued + 4))
            else:
                limit = MAX_WORDS
            cut = len(words) if len(words) <= limit else _cut(words, limit, first_short and not out)
            if first_short and not out and len(words) > FIRST_CLAUSE:
                # the very first audio: a leading clause ("Okay," / "Sure, ...;") is enough
                early = _natural_cut(words, 1, FIRST_CLAUSE)
                if early:
                    cut = min(cut, early)
            out.append(" ".join(words[:cut]))
            queued += cut
            words = words[cut:]
    return out
