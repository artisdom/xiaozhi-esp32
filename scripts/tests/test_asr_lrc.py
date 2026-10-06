import importlib.util
import os
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "asr_lrc.py"
spec = importlib.util.spec_from_file_location("asr_lrc", SCRIPT)
asr_lrc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(asr_lrc)


def tokens(words, start=1.0, step=0.5):
    """Recognised words as asr_tokens() would return them: (key, start, end, shown)."""
    return [(w.lower(), start + i * step, start + (i + 1) * step - 0.1, w) for i, w in enumerate(words)]


class AsrLrcTest(unittest.TestCase):
    def test_tokenize_english_and_chinese(self):
        self.assertEqual(asr_lrc.tokenize("Red, blue green!", False), ["Red,", "blue", "green!"])
        self.assertEqual(asr_lrc.tokenize("红色，蓝色 ABC", True), ["红", "色，", "蓝", "色", "ABC"])

    def test_segment_lines_breaks_at_pauses_and_sentences(self):
        toks = tokens(["One", "two", "three."], 0) + tokens(["Four", "five"], 2.0) + tokens(["Six"], 5.0)
        lines = asr_lrc.segment_lines(toks, False)
        self.assertEqual([[t for t, _ in line] for line in lines],
                         [["One", "two", "three."], ["Four", "five"], ["Six"]])

    def test_align_reference_uses_reference_text_and_audio_times(self):
        ref = ["alpha beta gamma", "delta epsilon"]
        # The recording says "gama" and repeats the whole verse later.
        heard = tokens(["alpha", "beta", "gama", "delta", "epsilon"], 10.0) + \
            tokens(["alpha", "beta", "gamma", "delta", "epsilon"], 30.0)
        lines = asr_lrc.align_reference(ref, heard, False, 60.0)
        self.assertEqual(len(lines), 4)  # Two lines, sung twice.
        self.assertEqual([t for t, _ in lines[0]], ["alpha", "beta", "gamma"])
        self.assertAlmostEqual(lines[0][0][1], 10.0)
        self.assertAlmostEqual(lines[2][0][1], 30.0)
        for line in lines:  # Times never go backwards inside a line.
            self.assertEqual([t for _, t in line], sorted(t for _, t in line))

    def test_align_reference_rejects_a_different_song(self):
        heard = tokens(["completely", "different", "words", "here", "today", "again"])
        self.assertEqual(asr_lrc.align_reference(["alpha beta gamma delta"], heard, False, 30.0), [])

    def test_align_reference_drops_lines_that_are_not_heard(self):
        ref = ["alpha beta gamma", "never sung anywhere", "delta epsilon zeta"]
        heard = tokens(["alpha", "beta", "gamma", "delta", "epsilon", "zeta"])
        lines = asr_lrc.align_reference(ref, heard, False, 30.0)
        self.assertEqual([[t for t, _ in l] for l in lines],
                         [["alpha", "beta", "gamma"], ["delta", "epsilon", "zeta"]])

    def test_parse_sections_titles_and_credits(self):
        text = ("[First Song]\nline one\nline two\n\n2. Second\nsecond a\nWords: someone\n"
                "\nKnownTitle\n\nknown line a\nknown line b\n")
        with tempfile.TemporaryDirectory() as temp:
            path = os.path.join(temp, "t.txt")
            Path(path).write_text(text, encoding="utf-8")
            sections = asr_lrc.parse_sections(path, known={"knowntitle"})
        self.assertEqual(sections["firstsong"], ["line one", "line two"])
        self.assertEqual(sections["second"], ["second a"])  # Credit line dropped.
        self.assertEqual(sections["knowntitle"], ["known line a", "known line b"])

    def test_write_lrc_has_a_stamp_for_every_word(self):
        with tempfile.TemporaryDirectory() as temp:
            mp3 = os.path.join(temp, "song.mp3")
            count = asr_lrc.write_lrc(mp3, [[("Hello", 1.0), ("world", 1.5)], [("Bye", 5.25)]], False, "asr", "song")
            text = Path(os.path.join(temp, "song.lrc")).read_text(encoding="utf-8")
        self.assertEqual(count, 2)
        self.assertIn("[00:01.00]<00:01.00>Hello <00:01.50>world", text)
        self.assertIn("[00:05.25]<00:05.25>Bye", text)
        self.assertIn("[by:asr]", text)


if __name__ == "__main__":
    unittest.main()
