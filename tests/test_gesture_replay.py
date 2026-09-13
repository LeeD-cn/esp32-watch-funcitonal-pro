"""Pure state-machine tests independent of captured user data."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from gesture_replay import replay


def row(seq, gy=0, ax=900, ay=-300, az=200):
    return {"timestamp_us": seq * 10000, "ax_mg": ax, "ay_mg": ay, "az_mg": az,
            "gx_dps": 0, "gy_dps": gy, "gz_dps": 0}


class GestureReplayTests(unittest.TestCase):
    def test_valid_directions_after_ready_pose(self):
        for gy, expected in [(-280, "left"), (300, "right")]:
            rows = [row(i) for i in range(30)]
            rows += [row(i + 30, gy) for i in range(16)]
            events = replay(rows)
            self.assertEqual([e.direction for e in events], [expected])

    def test_requires_preparation(self):
        rows = [row(i, 300, ax=-900) for i in range(40)]
        self.assertEqual(replay(rows), [])

    def test_rejects_fast_short_and_slow_large(self):
        ready = [row(i) for i in range(30)]
        fast_short = ready + [row(i + 30, 300) for i in range(3)] + [row(i + 33) for i in range(20)]
        slow_large = ready + [row(i + 30, 60) for i in range(80)]
        self.assertEqual(replay(fast_short), [])
        self.assertEqual(replay(slow_large), [])


if __name__ == "__main__":
    unittest.main()
