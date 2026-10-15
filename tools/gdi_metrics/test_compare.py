"""compare.py holds a capture against Windows' with the documented tolerances."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("compare", Path(__file__).with_name("compare.py"))
compare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compare)


def row(**over):
    base = {"face": "Arial", "height": -12, "weight": 400, "italic": 0, "realized": "Arial",
            "tm": [15, 12, 3, 3, 0, 5, 24, 400, 0, 96, 96, 30, 255, 31, 32, 0, 0, 0, 39, 0],
            "widths": [4] * 224, "extents": [[20, 15]] * 20}
    base.update(over)
    return base


class CompareTests(unittest.TestCase):
    def test_identical_is_clean(self):
        self.assertEqual(compare.compare_case(row(), row())[0], [])

    def test_line_metrics_are_exact_and_widths_have_a_pixel(self):
        tm = list(row()["tm"])
        tm[1] = 13
        self.assertTrue(compare.compare_case(row(), row(tm=tm))[0])
        tm = list(row()["tm"])
        tm[5] = 6  # average width, within one
        bad, notes = compare.compare_case(row(), row(tm=tm))
        self.assertEqual(bad, [])
        self.assertTrue(notes)

    def test_character_widths(self):
        few = [4] * 224
        few[10] = 5  # one off by one: within
        self.assertEqual(compare.compare_case(row(), row(widths=few))[0], [])
        far = [4] * 224
        far[10] = 6
        self.assertTrue(compare.compare_case(row(), row(widths=far))[0])
        many = [5] * 10 + [4] * 214  # 10 of 224 off by one: more than 2 %
        self.assertTrue(compare.compare_case(row(), row(widths=many))[0])

    def test_extents_and_face(self):
        ext = [[20, 15]] * 19 + [[21, 15]]
        self.assertEqual(compare.compare_case(row(), row(extents=ext))[0], [])
        ext = [[20, 16]] + [[20, 15]] * 19
        self.assertTrue(compare.compare_case(row(), row(extents=ext))[0])
        self.assertTrue(compare.compare_case(row(), row(realized="Open Sans"))[0])

    def test_only_and_skipped_characters(self):
        tm = list(row()["tm"])
        tm[6] = 30  # maximum width: another version of the face
        far = [4] * 224
        far[0x7f - 0x20] = 9
        self.assertTrue(compare.compare_case(row(), row(tm=tm, widths=far))[0])
        self.assertEqual(compare.compare_case(row(), row(tm=tm, widths=far),
                                              only={"height", "ascent", "widths"},
                                              skip_chars={0x7f})[0], [])
        tm[1] = 13
        self.assertTrue(compare.compare_case(row(), row(tm=tm), only={"ascent"})[0])

    def test_files_round_trip(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "a.jsonl"
            path.write_text(json.dumps(row()) + "\n" + json.dumps(row(weight=700)) + "\n")
            self.assertEqual(len(compare.load(path)), 2)


if __name__ == "__main__":
    unittest.main()
