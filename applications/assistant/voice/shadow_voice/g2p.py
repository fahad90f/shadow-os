"""English text -> Kokoro phoneme tokens (for sherpa-onnx >= 2.0).

Why this exists: sherpa-onnx 2.x removed espeak-ng from its Kokoro front end and
only looks words up in the lexicon shipped with the model; anything missing
("opened", "Debian", "82", "PipeWire", "CPU") is silently skipped, so replies
lose words. We keep the same lexicon (misaki-derived, lexicon-us-en.txt /
lexicon-gb-en.txt) and add what misaki does around it:

  * text normalisation (textnorm.py): numbers, units, paths, acronyms, emoji;
  * misaki's function-word rules ("a" -> ɐ, "the" -> ðə/ði, "to" -> tə/tʊ),
    which the bare lexicon gets wrong ("ay screenshot", "thee terminal");
  * letter-by-letter spelling for acronyms ("U·S·B");
  * -s / -'s / -ed / -ing morphology on lexicon stems;
  * espeak-ng (Debian package) for every remaining word, mapped to misaki's
    phoneme set with misaki's own E2M table (Apache-2.0, hexgrad/misaki).

The result is handed to sherpa-onnx as GenerationConfig.tokens.
"""
import logging
import re
import shutil
import subprocess
import threading

from .textnorm import SPELL_SEP, normalize, split_sentences, strip_accents

log = logging.getLogger("shadow-voiced")

# misaki/espeak.py EspeakFallback.E2M, longest first.
_E2M = sorted({
    "ʔˌn̩": "ʔn", "ʔn̩": "ʔn", "a^ɪ": "I", "a^ʊ": "W", "d^ʒ": "ʤ", "e^ɪ": "A", "e": "A",
    "t^ʃ": "ʧ", "ɔ^ɪ": "Y", "ə^l": "ᵊl", "ʲo": "jo", "ʲə": "jə", "ʲ": "", "ɚ": "əɹ", "r": "ɹ",
    "x": "k", "ç": "k", "ɐ": "ə", "ɬ": "l", "̃": "",
}.items(), key=lambda kv: -len(kv[0]))

_VOICELESS = set("ptkfθ")
_SIBILANT = set("szʃʒʧʤ")
_VOWELS = set("AIOQWYaiuæɑɒɔəɛɜɪʊʌᵻ")          # misaki: what makes ctx.future_vowel true
_STRESS = "ˈˌ"


class KokoroG2P:
    def __init__(self, lexicon_path, tokens_path, british=False, espeak="espeak-ng"):
        self.british = british
        self.vocab = set()
        with open(tokens_path, encoding="utf-8") as f:
            for line in f:
                line = line.rstrip("\n")
                if not line:
                    continue
                tok = line.rsplit(" ", 1)[0]
                self.vocab.add(tok if tok else " ")
        self.lex = {}
        with open(lexicon_path, encoding="utf-8") as f:
            for line in f:
                word, _, phones = line.rstrip("\n").partition(" ")
                if word and phones:
                    self.lex[word] = phones.replace(" ", "")
        self.espeak = shutil.which(espeak) if espeak else None
        self.voice = "en-gb" if british else "en-us"
        self._cache = {}
        self._cache_lock = threading.Lock()
        self._warned_no_espeak = False
        self.oov_log = []                    # recent words that needed espeak (diagnostics/tests)

    # ---------------------------------------------------------------- lookup
    def known(self, word):
        return word.lower() in self.lex

    def _lookup(self, w):
        lw = w.lower()
        if lw in self.lex:
            return self.lex[lw]
        plain = strip_accents(lw)
        if plain != lw and plain in self.lex:
            return self.lex[plain]
        return None

    def _suffix_s(self, base):
        last = base.rstrip(_STRESS + "ː")[-1:]
        if last in _SIBILANT:
            return base + ("ɪz" if self.british else "ᵻz")
        return base + ("s" if last in _VOICELESS else "z")

    def _suffix_ed(self, base):
        last = base.rstrip(_STRESS + "ː")[-1:]
        if last in "td":
            return base + ("ɪd" if self.british else "ᵻd")
        return base + ("t" if last in "pkfθʃsʧ" else "d")

    def _morph(self, w):
        lw = strip_accents(w.lower())
        if lw.endswith("'s") and (b := self._lookup(lw[:-2])):
            return self._suffix_s(b)
        if lw.endswith("s") and len(lw) > 3:
            stems = ([lw[:-3] + "y"] if lw.endswith("ies") else []) + ([lw[:-2]] if lw.endswith("es") else []) + [lw[:-1]]
            for stem in stems:
                if (b := self._lookup(stem)):
                    return self._suffix_s(b)
        if lw.endswith("ed") and len(lw) > 4:
            cands = [lw[:-2], lw[:-1]]
            if lw.endswith("ied"):
                cands.insert(0, lw[:-3] + "y")
            if len(lw) > 5 and lw[-3] == lw[-4]:
                cands.append(lw[:-3])
            for stem in cands:
                if (b := self._lookup(stem)):
                    return self._suffix_ed(b)
        if lw.endswith("ing") and len(lw) > 5:
            cands = [lw[:-3], lw[:-3] + "e"]
            if lw[-4] == lw[-5]:
                cands.append(lw[:-4])
            if lw.endswith("ying"):
                cands.append(lw[:-4] + "ie")
            for stem in cands:
                if (b := self._lookup(stem)):
                    return b + "ɪŋ"
        if lw.endswith("ly") and len(lw) > 5 and (b := self._lookup(lw[:-2])):
            return b + "li"
        return None

    def _letter(self, ch):
        if ch.upper() == "A":
            return "ˈA"
        return self.lex.get(ch.lower(), "")

    def _spelled(self, token):
        """'C·P·U' / 'C·P·Us' / 'K·' -> letter names (+ plural suffix)."""
        plural = token.endswith("s")                 # letters are upper case, the plural "s" is not
        core = token[:-1] if plural else token
        letters = [c for c in core.split(SPELL_SEP) if c]
        ps = [self._letter(c) for c in letters if c.isalpha()]
        ps = [p for p in ps if p]
        if plural and ps:
            ps[-1] = self._suffix_s(ps[-1])
        return ps

    # ---------------------------------------------------------------- espeak-ng
    def _espeak_many(self, words):
        with self._cache_lock:
            todo = [w for w in dict.fromkeys(words) if w not in self._cache]
        if todo and not self.espeak:
            if not self._warned_no_espeak:
                log.warning("espeak-ng is not installed: words outside the lexicon are spelled out")
                self._warned_no_espeak = True
        if todo and self.espeak:
            got = self._run_espeak(todo)
            if len(got) != len(todo):
                got = [(self._run_espeak([w]) or [""])[0] for w in todo]
            with self._cache_lock:
                if len(self._cache) > 5000:
                    self._cache.clear()
                for w, ps in zip(todo, got):
                    self._cache[w] = self._e2m(ps) if ps else ""
        with self._cache_lock:
            return [self._cache.get(w, "") for w in words]

    def _run_espeak(self, words):
        try:
            r = subprocess.run([self.espeak, "-q", "--ipa", "--tie=^", "-v", self.voice],
                               input="\n".join(words) + "\n", capture_output=True, text=True, timeout=10)
        except (OSError, subprocess.SubprocessError) as e:
            log.warning("espeak-ng failed: %s", e)
            return []
        if r.returncode != 0:
            log.warning("espeak-ng exited %s: %s", r.returncode, r.stderr.strip()[:200])
            return []
        return [ln.strip() for ln in r.stdout.splitlines() if ln.strip()]

    def _e2m(self, ps):
        ps = ps.replace(" ", "")
        for old, new in _E2M:
            ps = ps.replace(old, new)
        ps = re.sub(r"(\S)̩", r"ᵊ\1", ps).replace(chr(809), "")
        if self.british:
            ps = ps.replace("e^ə", "ɛː").replace("iə", "ɪə").replace("ə^ʊ", "Q")
        else:
            ps = (ps.replace("o^ʊ", "O").replace("ɜːɹ", "ɜɹ").replace("ɜː", "ɜɹ")
                  .replace("ɪə", "iə").replace("ː", ""))
        ps = ps.replace("ɾ", "T").replace("ʔ", "t")
        return ps.replace("^", "")

    # ---------------------------------------------------------------- public
    _TOKEN = re.compile(r"[A-Z](?:" + SPELL_SEP + r"[A-Z]?)+s?|[A-Za-zÀ-ÖØ-öø-ÿ][A-Za-zÀ-ÖØ-öø-ÿ']*|[.,!?;:—…\"()]")

    def sentences(self, text, first_short=False):
        """Reply text -> list of normalised, speakable sentences."""
        return split_sentences(normalize(text, self.known), first_short=first_short)

    def phonemize(self, sentence):
        """One NORMALISED sentence -> (token list for sherpa-onnx, words that needed espeak)."""
        items = []            # [kind, phonemes, source]; kind: "w" word, "e" needs espeak, "p" punctuation
        for m in self._TOKEN.finditer(sentence.replace("’", "'")):
            tok = m.group(0)
            if SPELL_SEP in tok:
                items.extend(["w", p, "#spelled"] for p in self._spelled(tok))
                continue
            if not tok[0].isalpha():
                items.append(["p", tok, tok])
                continue
            tok = tok.strip("'")
            for part in (p for p in re.split(r"-+", tok) if p):
                if len(part) == 1 and part.lower() not in ("a", "i"):
                    items.append(["w", self._letter(part), "#letter"])
                    continue
                ps = self._lookup(part) or self._morph(part)
                items.append(["w", ps, part] if ps else ["e", "", part])
        need = [src for kind, _, src in items if kind == "e"]
        if need:
            fb = dict(zip(need, self._espeak_many(need)))
            self.oov_log = (self.oov_log + need)[-50:]
            for it in items:
                if it[0] == "e":
                    ps = fb.get(it[2], "")
                    if not ps and not self.espeak and len(it[2]) <= 4:
                        ps = "".join(self._letter(c) for c in it[2] if c.isalpha())
                    it[0], it[1] = "w", ps
        self._function_words(items)
        toks = []
        for kind, ps, _src in items:
            if kind == "p":
                while toks and toks[-1] == " ":
                    toks.pop()
                if ps in self.vocab:
                    toks.append(ps)
                toks.append(" ")
                continue
            if not ps:
                continue
            toks.extend(c for c in ps if c in self.vocab)
            toks.append(" ")
        while toks and toks[-1] == " ":
            toks.pop()
        return toks, need

    @staticmethod
    def _function_words(items):
        """misaki's special cases for a / an / the / to, which depend on the next word:
        "a" -> ɐ (the lexicon says ˈA, the letter), "the" -> ðə / ði, "to" -> tə / tʊ."""
        for i, (kind, ps, src) in enumerate(items):
            if kind != "w" or src.startswith("#"):
                continue
            low = src.lower()
            if low not in ("a", "an", "the", "to"):
                continue
            nxt = items[i + 1] if i + 1 < len(items) else None
            if nxt is None or nxt[0] != "w" or not nxt[1]:
                if low == "a":
                    items[i][1] = "ˈA"            # "plan A." -> the letter
                continue
            core = nxt[1].lstrip(_STRESS)
            vowel_next = bool(core) and core[0] in _VOWELS
            if low == "a":
                items[i][1] = "ɐ"
            elif low == "an":
                items[i][1] = "ən"
            elif low == "the":
                items[i][1] = "ði" if vowel_next else "ðə"
            else:
                items[i][1] = "tʊ" if vowel_next else "tə"
