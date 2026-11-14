"""ear.clean_text: Whisper's loops on noise are cut, real speech is kept."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from ear import clean_text  # noqa: E402


class CleanText(unittest.TestCase):
    def test_loops_are_cut(self):
        self.assertEqual(clean_text("ดีเลย แล้วมันจะได้" + "ตก" * 40), "ดีเลย แล้วมันจะได้ตก")
        self.assertEqual(clean_text("กậน" + "มันกัน" * 3 + "กัน" * 30), "กậนมันกันมันกันมันกัน")

    def test_only_a_loop_is_nothing(self):
        self.assertEqual(clean_text("มันมันมันมันมันมันมันมัน"), "")

    def test_speech_is_kept(self):
        t = "ก็เหลือสามแสนใช่ป่ะ แล้วสมมุติ"
        self.assertEqual(clean_text(t), t)
        self.assertEqual(clean_text("โอเค โอเค"), "โอเค โอเค")
        self.assertEqual(clean_text("ครับ"), "ครับ")


if __name__ == "__main__":
    unittest.main()
