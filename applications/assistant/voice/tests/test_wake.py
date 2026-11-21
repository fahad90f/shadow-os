"""Wake-word policy, transcript clean-up, yes/no and WER (pure Python, no data needed)."""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from shadow_voice.wake import clean_transcript, dedupe, match_wake, wer, yes_no  # noqa: E402


class StrictWake(unittest.TestCase):
    def test_start_of_utterance(self):
        for text, rest in [("Shadow, open the terminal.", "open the terminal"),
                           ("shadow open the terminal", "open the terminal"),
                           ("Hey Shadow, take a screenshot.", "take a screenshot"),
                           ("OK Shadow what time is it?", "what time is it"),
                           ("Okay, Shadow. Lock the screen", "Lock the screen"),
                           ("Hi shadow", ""),
                           ("Shadow.", ""),
                           ("SHADOW!", ""),
                           ("Shadow's the weather nice?", "the weather nice"),
                           ("  ...Shadow, play music", "play music")]:
            with self.subTest(text=text):
                woke, got, how = match_wake(text)
                self.assertTrue(woke)
                self.assertEqual(got, rest)
                self.assertEqual(how, "strict")

    def test_mentions_do_not_wake(self):
        for text in ["I saw my shadow on the wall this morning.", "The shadow of the tree is long.",
                     "Open the shadow settings", "What is a shadow?", "Is that your shadow?"]:
            with self.subTest(text=text):
                self.assertFalse(match_wake(text)[0])
                self.assertFalse(match_wake(text, kws_hit=True)[0], "a KWS hit must not make a mention wake")

    def test_plural_and_derived_words(self):
        for text in ["Shadows grow longer in winter.", "Shadowing a senior engineer is useful.",
                     "shadowed by clouds", "Shadowy figures"]:
            with self.subTest(text=text):
                self.assertFalse(match_wake(text)[0])
                self.assertFalse(match_wake(text, kws_hit=True)[0])
                self.assertFalse(match_wake(text, kws_hit=True, sensitivity="high")[0])

    def test_similar_words(self):
        for text in ["Shallow water is warm in summer.", "Show me the weather for tomorrow.",
                     "Shall we go now?", "The meadow is green after the rain.", "Sharon, open the door please.",
                     "Follow the arrow to the exit.", "Chateau wines are expensive."]:
            with self.subTest(text=text):
                self.assertFalse(match_wake(text)[0])
                self.assertFalse(match_wake(text, kws_hit=True)[0])
                self.assertFalse(match_wake(text, kws_hit=True, sensitivity="high")[0])


class FuzzyWake(unittest.TestCase):
    def test_near_miss_needs_kws(self):
        for text, rest in [("Shadda, open the terminal", "open the terminal"),
                           ("Chadow what time is it", "what time is it"),
                           ("Shado. Lock the screen.", "Lock the screen")]:
            with self.subTest(text=text):
                self.assertFalse(match_wake(text, kws_hit=False)[0], "near miss without KWS must not wake")
                woke, got, how = match_wake(text, kws_hit=True)
                self.assertTrue(woke)
                self.assertEqual(got, rest)
                self.assertEqual(how, "fuzzy")

    def test_misheard_hey(self):
        woke, rest, _ = match_wake("Pay shadow, open the browser", kws_hit=True)
        self.assertTrue(woke)
        self.assertEqual(rest, "open the browser")
        self.assertFalse(match_wake("Pay shadow, open the browser", kws_hit=True, sensitivity="low")[0])
        woke, rest, _ = match_wake("Pay shadda, open the browser", kws_hit=True)
        self.assertTrue(woke)

    def test_sensitivity(self):
        # one edit away, not in the curated list: normal + high only
        self.assertFalse(match_wake("Shadoq open it", kws_hit=True, sensitivity="low")[0])
        self.assertTrue(match_wake("Shadoq open it", kws_hit=True, sensitivity="normal")[0])
        # two edits: never (too close to names and real words: "Sharon")
        self.assertFalse(match_wake("Shtadaw open it", kws_hit=True, sensitivity="normal")[0])
        self.assertFalse(match_wake("Shtadaw open it", kws_hit=True, sensitivity="high")[0])
        # curated near miss without any KWS hit: high only
        self.assertFalse(match_wake("Shadda open it", kws_hit=False, sensitivity="normal")[0])
        self.assertTrue(match_wake("Shadda open it", kws_hit=False, sensitivity="high")[0])
        # unknown sensitivity falls back to normal
        self.assertTrue(match_wake("Shadoq open it", kws_hit=True, sensitivity="bogus")[0])


class Transcripts(unittest.TestCase):
    def test_whisper_labels(self):
        for raw in ["(wind blowing)", "[ Sound Effects ]", "( ( ( ( ( (", "[", "(static)", "[MUSIC]",
                    "♪ la la ♪", "*cough*", "", "   ", "you", "Thanks for watching!"]:
            with self.subTest(raw=raw):
                self.assertEqual(clean_transcript(raw), "")
        self.assertEqual(clean_transcript("[ Silence ] Shadow, open (laughs) the terminal."),
                         "Shadow, open the terminal.")
        self.assertEqual(clean_transcript("Thank you."), "Thank you.")
        self.assertEqual(clean_transcript("Set it to 50"), "Set it to 50")

    def test_dedupe(self):
        self.assertEqual(dedupe("on the window on the window on the window"), "on the window")
        self.assertEqual(dedupe("open open the terminal"), "open the terminal")


class YesNo(unittest.TestCase):
    def test_answers(self):
        for text, want in [("Yes.", "yes"), ("yeah, go ahead", "yes"), ("Sure!", "yes"), ("Do it.", "yes"),
                           ("Okay.", "yes"), ("Please do", "yes"), ("No.", "no"), ("Nope", "no"),
                           ("No, don't do it", "no"), ("Cancel that.", "no"), ("Don't.", "no"),
                           ("Not now", "no"), ("Okay, cancel", "no"), ("Yes, but wait", "yes"),
                           ("I'm not sure what you mean", "no"), ("What?", None), ("", None)]:
            with self.subTest(text=text):
                self.assertEqual(yes_no(text), want)


class Wer(unittest.TestCase):
    def test_wer(self):
        self.assertEqual(wer("open the terminal", "Open the terminal."), 0.0)
        self.assertEqual(wer("what's using my disk space", "What is using my disc space?"), 0.0)
        self.assertEqual(wer("set the volume to fifty percent", "Set the volume to 50%."), 0.0)
        self.assertAlmostEqual(wer("open the terminal", "open terminal"), 1 / 3)
        self.assertEqual(wer("open the terminal", ""), 1.0)


if __name__ == "__main__":
    unittest.main()
