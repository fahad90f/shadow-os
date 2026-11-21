"""Wake-word policy and transcript helpers (pure Python, unit-tested).

Policy (measured in the voice lab, see README): an utterance is addressed to
Shadow when its transcript STARTS with the wake word ("Shadow, ...", "Hey
Shadow ...", "OK Shadow's ..."). The plural "shadows", "shadowing" and a mention
later in the sentence ("I saw my shadow") never wake. A near-miss spelling of
the first word ("Shadda", "Chadow", "Pay shadow" = misheard "Hey") is accepted
only when the acoustic keyword spotter also fired, because on its own it is far
too common in ordinary speech.

wake_sensitivity tunes both halves:

  low     KWS threshold 0.30; near-miss only from a short curated list, first word only
  normal  KWS threshold 0.20; curated list or one edit away, first word or after one
          short lead word ("Pay shadow")
  high    KWS threshold 0.12 (the spotter fires more easily); as normal, and the
          curated near-misses are accepted even WITHOUT a keyword-spotter hit
          (first word only)
Words that merely start with "shadow" ("shadows", "shadowy", "shadowing") never wake.
"""
import re

WAKE_WORD = "shadow"

# Strict match at the start of the transcript. Group 1 = the rest (the command).
_STRICT = re.compile(r"^\W*(?:(?:hey|hi|ok|okay|a|yo)\W+)?shadow(?:'s)?(?![\w'])\W*(.*)$", re.I | re.S)
_WORD = re.compile(r"[A-Za-z']+")

# Spellings speech-to-text produced for "Shadow" in the voice-lab test set (the prototype's
# measured list: 0 false wakes on the negative set when combined with a KWS hit).
NEAR_MISSES = frozenset({"shadda", "shada", "shado", "chado", "shadoe", "shatter", "shutter", "shout", "shaddow",
                         "chadow", "shad", "shaddu", "shaddo", "shadoh", "shadu"})
# How speech-to-text mishears the "Hey" / "OK" in front of the wake word ("Pay shadow").
# Only these may precede a near miss: "The shadow of the tree" must never wake.
HEY_LIKE = frozenset({"hey", "hi", "ok", "okay", "a", "yo", "pay", "say", "hay", "they", "eh", "ay", "hello", "hej"})
_CURATED_LOW = frozenset({"shadda", "shado", "shadoe", "shaddow", "chadow", "shaddo", "shadoh"})

SENSITIVITY = {
    #          kws threshold, max edits, allow one lead word, curated list w/o kws
    "low": dict(kws_threshold=0.30, max_edits=0, lead_word=False, fuzzy_without_kws=False),
    "normal": dict(kws_threshold=0.20, max_edits=1, lead_word=True, fuzzy_without_kws=False),
    "high": dict(kws_threshold=0.12, max_edits=1, lead_word=True, fuzzy_without_kws=True),
}


def levenshtein(a, b):
    d = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        prev, d[0] = d[0], i
        for j, cb in enumerate(b, 1):
            prev, d[j] = d[j], min(d[j] + 1, d[j - 1] + 1, prev + (ca != cb))
    return d[len(b)]


def _near_miss(word, sensitivity, curated_only):
    w = word.lower().strip("'")
    if w.startswith("shadow") or w in ("shallow", "shall", "show"):
        return False            # real words ("shadows", "shadowy"); "shadow" itself is handled separately
    curated = _CURATED_LOW if sensitivity == "low" else NEAR_MISSES
    if w in curated:
        return True
    if curated_only:
        return False
    edits = SENSITIVITY[sensitivity]["max_edits"]
    # The wake word starts with /ʃ/: an edit-distance match must keep an s/c onset, so
    # "meadow" or "window" never pass even at high sensitivity.
    return edits > 0 and len(w) >= 4 and w[0] in "sc" and levenshtein(w, WAKE_WORD) <= edits


def match_wake(text, kws_hit=False, sensitivity="normal"):
    """(woke, rest, how). how is "strict", "fuzzy" or "" (not addressed)."""
    if sensitivity not in SENSITIVITY:
        sensitivity = "normal"
    m = _STRICT.match(text or "")
    if m:
        return True, m.group(1).strip(" ,.!?;:-"), "strict"
    pol = SENSITIVITY[sensitivity]
    words = list(_WORD.finditer(text or ""))
    if not words:
        return False, (text or "").strip(), ""
    candidates = [0]
    if pol["lead_word"] and len(words) > 1 and words[0].group(0).lower() in HEY_LIKE:
        candidates.append(1)
    for i in candidates:
        w = words[i].group(0)
        if i == 1 and kws_hit and w.lower() in ("shadow", "shadow's"):
            # "Pay shadow" / "Say Shadow": a misheard "Hey" in front of the real wake word
            return True, text[words[i].end():].strip(" ,.!?;:-"), "fuzzy"
        if kws_hit and _near_miss(w, sensitivity, curated_only=False):
            return True, text[words[i].end():].strip(" ,.!?;:-"), "fuzzy"
        if not kws_hit and i == 0 and pol["fuzzy_without_kws"] and _near_miss(w, sensitivity, curated_only=True):
            return True, text[words[i].end():].strip(" ,.!?;:-"), "fuzzy"
    return False, text.strip(), ""


def kws_threshold(sensitivity):
    return SENSITIVITY.get(sensitivity, SENSITIVITY["normal"])["kws_threshold"]


# --------------------------------------------------------------------------- transcripts
_NONSPEECH = re.compile(r"\[[^\]]*\]|\([^)]*\)|\*[^*]*\*|♪[^♪]*♪|[\[\]()*♪]")


def clean_transcript(text):
    """Whisper labels non-speech instead of returning nothing: "(wind blowing)",
    "[ Sound Effects ]", "( ( ( (", "♪". Drop such annotations; a result without
    letters or digits is no speech at all ("")."""
    t = _NONSPEECH.sub(" ", text or "")
    t = " ".join(t.split())
    t = re.sub(r"\s+([,.!?;:])", r"\1", t)
    if re.fullmatch(r"[\W_]*(?:you|thanks for watching)[\W_]*", t, re.I):
        # Whisper's classic hallucinations on noise; never a real request on their own.
        return ""
    return t if re.search(r"[A-Za-z0-9]", t) else ""


def dedupe(text):
    """Partial hypotheses on cut-off audio can loop ("on the window on the window"): drop repeats."""
    w = text.split()
    for n in range(1, 6):
        i = 0
        while i + 2 * n <= len(w):
            a = [x.lower().strip(",.?!") for x in w[i:i + n]]
            b = [x.lower().strip(",.?!") for x in w[i + n:i + 2 * n]]
            if a == b:
                del w[i + n:i + 2 * n]
            else:
                i += 1
    return " ".join(w)


# --------------------------------------------------------------------------- yes / no
_YES_STRONG = ("yes", "yeah", "yep", "yup", "yea", "sure", "affirmative", "absolutely", "definitely", "confirm",
               "confirmed", "go ahead", "do it", "please do", "of course", "run it", "go for it", "proceed")
_NO_STRONG = ("no", "nope", "nah", "negative", "cancel", "stop", "don't", "do not", "abort", "never mind",
              "nevermind", "deny", "not now", "not", "wait", "hold on")
_YES_WEAK = ("okay", "ok", "alright", "all right", "fine", "right", "correct", "go")


def yes_no(text):
    """"yes" / "no" / None for an answer to a spoken confirmation.

    The first strong word decides ("no, don't do it" -> no, "yes please" -> yes);
    weak agreement ("okay", "fine") counts only when nothing strong was said."""
    t = " " + re.sub(r"[^a-z' ]+", " ", (text or "").lower()) + " "
    t = re.sub(r"\s+", " ", t)
    best = None
    for words, label in ((_YES_STRONG, "yes"), (_NO_STRONG, "no")):
        for w in words:
            i = t.find(" " + w + " ")
            if i >= 0 and (best is None or i < best[0]):
                best = (i, label)
    if best:
        return best[1]
    for w in _YES_WEAK:
        if (" " + w + " ") in t:
            return "yes"
    return None


# --------------------------------------------------------------------------- word error rate
_CONTRACTIONS = {"what's": "what is", "it's": "it is", "that's": "that is", "there's": "there is",
                 "i'm": "i am", "you're": "you are", "we're": "we are", "they're": "they are",
                 "don't": "do not", "doesn't": "does not", "can't": "cannot", "won't": "will not",
                 "isn't": "is not", "aren't": "are not", "let's": "let us", "i'll": "i will",
                 "you'll": "you will", "i've": "i have", "i'd": "i would", "here's": "here is",
                 "how's": "how is", "where's": "where is", "who's": "who is", "didn't": "did not"}


def normalize_words(text):
    from .textnorm import spell_numbers   # local import: keeps this module dependency-free
    t = (text or "").lower().replace("’", "'")
    t = spell_numbers(t)
    t = re.sub(r"[^a-z0-9' ]+", " ", t)
    out = []
    for w in t.split():
        w = w.strip("'")
        if not w:
            continue
        out.extend(_CONTRACTIONS.get(w, w).split())
    return [{"ok": "okay", "disc": "disk", "alright": "all right"}.get(w, w) for w in out]


def word_errors(ref, hyp):
    """(errors, reference word count) — Levenshtein distance over normalised words."""
    r, h = normalize_words(ref), normalize_words(hyp)
    d = list(range(len(h) + 1))
    for i, rw in enumerate(r, 1):
        prev, d[0] = d[0], i
        for j, hw in enumerate(h, 1):
            prev, d[j] = d[j], min(d[j] + 1, d[j - 1] + 1, prev + (rw != hw))
    return d[len(h)], len(r)


def wer(ref, hyp):
    e, n = word_errors(ref, hyp)
    return e / n if n else (0.0 if not normalize_words(hyp) else 1.0)
