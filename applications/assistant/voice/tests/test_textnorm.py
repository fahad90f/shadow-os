"""Text normalisation for speech (pure Python) and the g2p front end (needs the Kokoro lexicon)."""
import os
import re
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from shadow_voice.textnorm import normalize, num2words, ordinal, split_sentences, version_words, year_words  # noqa

from harness import DATA, need_data  # noqa: E402


def norm(text):
    return normalize(text)


class Numbers(unittest.TestCase):
    def test_words(self):
        self.assertEqual(num2words(0), "zero")
        self.assertEqual(num2words(82), "eighty-two")
        self.assertEqual(num2words(1024), "one thousand twenty-four")
        self.assertEqual(num2words(2_400_000), "two million four hundred thousand")
        self.assertEqual(ordinal(21), "twenty-first")
        self.assertEqual(ordinal(12), "twelfth")
        self.assertEqual(year_words(2026), "twenty twenty-six")
        self.assertEqual(year_words(2005), "two thousand five")
        self.assertEqual(year_words(1900), "nineteen hundred")
        self.assertEqual(version_words("2.0.2"), "two point oh point two")
        self.assertEqual(version_words("3.13"), "three point thirteen")

    def test_in_text(self):
        cases = {
            "Your disk is 82% full.": "eighty-two percent",
            "Downloads uses 14 GB": "fourteen gigabytes",
            "1 GB left": "one gigabyte",
            "It costs $5.99": "five dollars and ninety-nine cents",
            "€1": "one euro",
            "1,024 files": "one thousand twenty-four files",
            "at 10:30 AM": "ten thirty A·M",
            "at 7:05": "seven oh five",
            "at 5pm": "five P·M",
            "on 2026-11-21": "November twenty-first, twenty twenty-six",
            "the 21st": "twenty-first",
            "in 2026": "twenty twenty-six",
            "the 1990s": "nineteen nineties",
            "2.4 MB": "two point four megabytes",
            "-5 degrees": "minus five degrees",
            "1/2 cup": "one half",
            "10-20%": "ten to twenty percent",
            "3.5 GHz": "three point five gigahertz",
            "25 MB/s": "twenty-five megabytes per second",
            "65°C": "sixty-five degrees Celsius",
            "40 ms": "forty milliseconds",
            "1080p": "ten eighty P·",
            "4K": "four K·",
            "~20 files": "about twenty files",
            "#3": "number three",
            "007": "zero zero seven",
        }
        for text, want in cases.items():
            with self.subTest(text=text):
                self.assertIn(want, norm(text))
                self.assertFalse(re.search(r"\d", norm(text)), f"digits left in {norm(text)!r}")

    def test_versions(self):
        self.assertIn("Python three point thirteen", norm("Python 3.13"))
        self.assertIn("two point oh point two", norm("sherpa-onnx 2.0.2"))
        self.assertIn("version one point two point three", norm("v1.2.3"))
        self.assertIn("six point eighteen point forty-four", norm("kernel 6.18.44"))
        self.assertIn("three point one four", norm("pi is 3.14"))


class Words(unittest.TestCase):
    def test_acronyms(self):
        self.assertIn("C·P·U", norm("The CPU is busy"))
        self.assertIn("U·S·B", norm("Plug in the USB stick"))
        self.assertIn("U·R·L", norm("Copy the URL"))
        self.assertIn("ram", norm("16 GB RAM"))
        self.assertIn("G·P·Us", norm("two GPUs"))
        self.assertIn("I·T", norm("Ask your IT team"))
        self.assertIn("M·P three", norm("an MP3 file"))
        self.assertIn("jay son", norm("as JSON"))
        self.assertEqual(norm("DO NOT STOP NOW."), "do not stop now.")

    def test_camel_case(self):
        self.assertIn("pipe wire", norm("PipeWire"))
        self.assertIn("wire plumber", norm("WirePlumber"))
        self.assertIn("git hub", norm("GitHub"))
        self.assertIn("You Tube", norm("YouTube"))
        self.assertIn("cute", norm("Qt"))

    def test_paths_and_links(self):
        self.assertEqual(norm("Saved to ~/Pictures/shot.png."), "Saved to home slash Pictures slash shot dot P·N·G.")
        self.assertIn("slash E·T·C slash fstab", norm("Edit /etc/fstab"))
        self.assertIn("report dot P·D·F", norm("Open report.pdf"))
        self.assertIn("notes dot text", norm("see notes.txt"))
        self.assertIn("git hub dot com", norm("Go to https://github.com/k2-fsa/sherpa-onnx/releases"))
        self.assertIn("anna at example dot com", norm("Mail anna@example.com"))
        self.assertIn("files or folders", norm("files/folders"))

    def test_symbols_markdown_emoji(self):
        self.assertEqual(norm("**Done!** 🎉 `ls -la` works ✅"), "Done! ls -la works")
        self.assertEqual(norm("# Title\nSome *text* here"), "Title. Some text here")
        self.assertEqual(norm("A [link](https://x.org) here"), "A link here")
        self.assertEqual(norm("😀👍🏽"), "")
        self.assertEqual(norm("​zero‍ width﻿"), "zero width")
        self.assertEqual(norm("Café — naïve"), "Café, naïve")
        self.assertEqual(norm("你好 Привет hi"), "hi")
        self.assertIn("C plus plus", norm("C++ and Rust"))
        self.assertIn("and", norm("R&D"))

    def test_sentences(self):
        text = norm("Okay. I took a screenshot of your whole screen and saved it in your Pictures folder as a "
                    "PNG file. It is 2.4 MB.")
        parts = split_sentences(text, first_short=True)
        self.assertEqual(parts[0], "Okay.")
        self.assertTrue(all(len(p.split()) <= 40 for p in parts))
        self.assertEqual(" ".join(parts), text)
        long = " ".join(["word"] * 100) + "."
        self.assertTrue(all(len(p.split()) <= 40 for p in split_sentences(long)))
        self.assertEqual(split_sentences(""), [])
        self.assertEqual(split_sentences("..."), [])


@need_data
class G2P(unittest.TestCase):
    """Every word of a sentence must become phonemes (sherpa-onnx alone drops unknown words)."""

    @classmethod
    def setUpClass(cls):
        from shadow_voice.g2p import KokoroG2P
        d = os.path.join(DATA, "models/tts/kokoro")
        cls.g = KokoroG2P(os.path.join(d, "lexicon-us-en.txt"), os.path.join(d, "tokens.txt"))
        cls.gb = KokoroG2P(os.path.join(d, "lexicon-gb-en.txt"), os.path.join(d, "tokens.txt"), british=True)

    def words_spoken(self, text, g=None):
        g = g or self.g
        out = []
        for s in g.sentences(text):
            toks, need = g.phonemize(s)
            out.append(("".join(toks), need))
        return out

    def test_nothing_dropped(self):
        if not self.g.espeak:
            self.skipTest("espeak-ng not installed")
        text = ("PipeWire on Debian uses 82% of 16 GB RAM; the CPU, GPU and USB ports are fine. "
                "Python 3.13 saved ~/Pictures/shot.png at 10:30. Kokoro opened WirePlumber's config.")
        for s in self.g.sentences(text):
            toks, need = self.g.phonemize(s)
            words = [w for w in re.findall(r"[A-Za-z·']+", s)]
            spaces = "".join(toks).count(" ")
            self.assertGreaterEqual(spaces + 1, len(words) - s.count("·") // 3,
                                    f"words dropped in {s!r} -> {''.join(toks)!r}")
            for w in need:
                self.assertTrue(self.g._espeak_many([w])[0], f"espeak-ng gave nothing for {w!r}")

    def test_function_words(self):
        (ps, _), = self.words_spoken("Take a screenshot of the terminal.")
        self.assertIn(" ɐ ", ps, "the article 'a' must be the reduced vowel, not the letter A")
        self.assertIn("ðə tˈɜɹmənəl", ps)
        (ps, _), = self.words_spoken("Open the app to install it.")
        self.assertIn("ði ˈæp", ps)
        self.assertIn("tʊ ɪnstˈɔl", ps.replace("ˌ", ""))
        (ps, _), = self.words_spoken("This is plan A.")
        self.assertTrue(ps.rstrip(".").endswith("ˈA"), ps)

    def test_spelled_and_morphology(self):
        (ps, need), = self.words_spoken("The CPUs opened.")
        self.assertIn("sˈi pˈi jˈuz", ps)
        self.assertIn("ˈOpᵊnd", ps)
        self.assertEqual(need, [])

    def test_shouting_is_not_spelled(self):
        self.assertEqual(self.g.sentences("SHADOW IS READY. DO NOT TURN OFF."), ["shadow is ready.", "do not turn off."])
        self.assertIn("U·S·B", self.g.sentences("PLUG THE USB STICK IN")[0])

    def test_british_voice_lexicon(self):
        (ps, _), = self.words_spoken("Shadow can schedule tomatoes.", self.gb)
        self.assertTrue(ps)

    def test_odd_unicode_never_raises(self):
        for text in ["🙂🙂", "™®©", "\x00\x07 ctrl", "ﬁnance ﬂow", "Ｆｕｌｌｗｉｄｔｈ", "a" * 2000, "Ünïcödé çafé"]:
            with self.subTest(text=text[:20]):
                for s in self.g.sentences(text):
                    toks, _ = self.g.phonemize(s)
                    self.assertTrue(all(t in self.g.vocab for t in toks))


if __name__ == "__main__":
    unittest.main()
