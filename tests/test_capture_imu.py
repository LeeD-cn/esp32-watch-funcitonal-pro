"""Protocol/data integrity tests; no physical serial port required."""
import json
from pathlib import Path
import sys
import unittest
import tempfile
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from capture_imu import check_sample, csv_row, parse_message, summarize, record


class CaptureTests(unittest.TestCase):
    def setUp(self):
        self.rows = [[i, 100000 + i * 10000, (16777000 + i * 256) % 2**24,
                      8192, -8192, 0, 16384, -16384, 0] for i in range(100)]
        self.done = {"count": 100, "errors": 0, "result": 0, "restore": 0, "cancelled": False}

    def test_noise_and_framing(self):
        self.assertIsNone(parse_message(b"ESP boot log\n"))
        self.assertEqual(parse_message(b'log {"imu":"recording"}\n')["imu"], "recording")
        with self.assertRaises(json.JSONDecodeError):
            parse_message(b'{"imu":"sample" BROKEN\n')

    def test_sequence_timestamp_and_range(self):
        previous = None
        for row in self.rows:
            previous = check_sample(row, previous)
        for index, value in [(0, 2), (1, 100000), (2, 2**24), (3, 32768), (3, 1.5)]:
            bad = self.rows[1].copy()
            bad[index] = value
            with self.assertRaises(ValueError):
                check_sample(bad, self.rows[0])

    def test_units(self):
        row = csv_row(self.rows[0], "left", 1)
        self.assertEqual(row[-6:], [1000, -1000, 0, 500, -500, 0])

    def test_complete_and_quality(self):
        quality = summarize(self.rows, self.done, 1)
        self.assertAlmostEqual(quality["effective_hz"], 100)
        self.assertFalse(quality["warnings"])
        delayed = [r.copy() for r in self.rows]
        for row in delayed[50:]:
            row[1] += 40000
        self.assertTrue(summarize(delayed, self.done, 1)["warnings"])

    def test_incomplete_cancel_restore(self):
        for key, value in [("count", 101), ("errors", 1), ("cancelled", True),
                           ("restore", 1), ("result", 1)]:
            with self.assertRaises(ValueError):
                summarize(self.rows, dict(self.done, **{key: value}), 1)

    def test_record_files_and_lost_frame(self):
        for lost in (False, True):
            frames = [{"imu": "preparing", "version": 1, "odr_hz": 100,
                       "accel_range_g": 4, "gyro_range_dps": 1000},
                      {"imu": "recording"}, {"imu": "exporting"}]
            frames += [{"imu": "sample", "v": r} for r in self.rows if not (lost and r[0] == 50)]
            frames.append({"imu": "done", **self.done})
            iterator = iter((json.dumps(f) + "\n").encode() for f in frames)
            port = SimpleNamespace(reset_input_buffer=lambda: None, write=lambda data: len(data),
                                   read_until=lambda *a, **kw: next(iterator))
            with tempfile.TemporaryDirectory() as folder, patch("capture_imu.beep"), patch("builtins.print"):
                args = SimpleNamespace(output=Path(folder), label="still", seconds=1, delay=0)
                if lost:
                    with self.assertRaises(ValueError):
                        record(port, args, 1)
                else:
                    self.assertTrue(record(port, args, 1))
                metadata = json.loads(next(Path(folder).glob("*.json")).read_text(encoding="utf-8"))
                self.assertEqual(metadata["status"], "incomplete" if lost else "complete")
                csv_file = next(Path(folder).glob("*.csv"))
                self.assertEqual(csv_file.name.endswith(".partial.csv"), lost)


if __name__ == "__main__":
    unittest.main()
