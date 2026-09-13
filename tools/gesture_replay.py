"""Replay captured CSV files through the stage-9 gesture state machine."""
import argparse
import csv
from dataclasses import dataclass
from pathlib import Path
import math

READY_MIN_S = 0.15
ARM_GRACE_S = 0.45
EVENT_START_DPS = 100.0
EVENT_END_DPS = 60.0
EVENT_END_S = 0.08
EVENT_MAX_S = 0.90
MIN_PEAK_DPS = 240.0
MIN_EXCESS_ANGLE_DEG = 12.0


@dataclass
class Event:
    direction: str
    timestamp_us: int
    peak_dps: float
    excess_angle_deg: float


def load_csv(path):
    with path.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    return [{k: (row[k] if k in ("label",) else float(row[k])) for k in row} for row in rows]


def ready_pose(row):
    ax, ay, az = row["ax_mg"], row["ay_mg"], row["az_mg"]
    norm = math.sqrt(ax * ax + ay * ay + az * az)
    gyro = math.sqrt(row["gx_dps"] ** 2 + row["gy_dps"] ** 2 + row["gz_dps"] ** 2)
    return (750 <= norm <= 1250 and 500 <= ax <= 1250 and
            -700 <= ay <= 100 and -400 <= az <= 850 and gyro <= 55)


def replay(rows):
    if len(rows) < 2:
        return []
    state = "waiting"
    stable_s = 0.0
    since_ready_s = 99.0
    direction = 0
    peak = area = event_s = quiet_s = 0.0
    events = []
    previous_us = int(rows[0]["timestamp_us"])

    for row in rows[1:]:
        now_us = int(row["timestamp_us"])
        dt = min(max((now_us - previous_us) / 1_000_000, 0), 0.03)
        previous_us = now_us
        pose = ready_pose(row)

        if state in ("waiting", "armed"):
            if pose:
                stable_s += dt
                since_ready_s = 0.0
                if stable_s >= READY_MIN_S:
                    state = "armed"
            else:
                stable_s = 0.0
                since_ready_s += dt

            gy = row["gy_dps"]
            if state == "armed" and since_ready_s <= ARM_GRACE_S and abs(gy) >= EVENT_START_DPS:
                state = "tracking"
                direction = 1 if gy > 0 else -1
                peak = abs(gy)
                area = max(abs(gy) - EVENT_START_DPS, 0) * dt
                event_s = dt
                quiet_s = 0.0
            elif state == "armed" and since_ready_s > ARM_GRACE_S:
                state = "waiting"

        elif state == "tracking":
            signed = direction * row["gy_dps"]
            event_s += dt
            peak = max(peak, signed)
            area += max(signed - EVENT_START_DPS, 0) * dt
            quiet_s = quiet_s + dt if signed < EVENT_END_DPS else 0.0
            if peak >= MIN_PEAK_DPS and area >= MIN_EXCESS_ANGLE_DEG:
                events.append(Event("right" if direction > 0 else "left", now_us, peak, area))
                state = "locked"
                stable_s = 0.0
            elif quiet_s >= EVENT_END_S or event_s >= EVENT_MAX_S:
                state = "waiting"
                stable_s = 0.0

        else:  # locked: require leaving the chest pose before a fresh preparation.
            if not pose:
                stable_s += dt
                if stable_s >= 0.50:
                    state = "waiting"
                    stable_s = 0.0
            else:
                stable_s = 0.0
    return events


def main():
    parser = argparse.ArgumentParser(description="离线回放阶段 9 手势状态机")
    parser.add_argument("paths", nargs="*", type=Path)
    parser.add_argument("--captures", type=Path,
                        default=Path(__file__).resolve().parents[1] / "captures")
    args = parser.parse_args()
    paths = args.paths or sorted(args.captures.glob("*.csv"))
    errors = 0
    for path in paths:
        rows = load_csv(path)
        events = replay(rows)
        label = rows[0]["label"] if rows else "empty"
        expected = [label] if label in ("left", "right") else []
        actual = [event.direction for event in events]
        ok = actual == expected
        errors += not ok
        detail = ", ".join(f"{e.direction}:{e.peak_dps:.0f}dps/{e.excess_angle_deg:.1f}deg"
                           for e in events) or "none"
        print(f"{'PASS' if ok else 'FAIL'} {path.name}: {detail}")
    print(f"files={len(paths)} mismatches={errors}")
    raise SystemExit(1 if errors else 0)


if __name__ == "__main__":
    main()
