"""Independent USB six-axis recorder. No Qt or watch-host protocol dependency."""
import argparse
import csv
from datetime import datetime
import hashlib
import json
from pathlib import Path
import statistics
import time
import uuid

LABELS = ("still", "left", "right", "slow_left", "slow_right",
          "short_left", "short_right", "raise", "lower", "rotate", "talk", "walk")
HEADER = ["label", "trial", "seq", "timestamp_us", "sensor_ticks",
          "ax_raw", "ay_raw", "az_raw", "gx_raw", "gy_raw", "gz_raw",
          "ax_mg", "ay_mg", "az_mg", "gx_dps", "gy_dps", "gz_dps"]


def parse_message(line):
    # ESP logs can precede a response on the same line. Never parse general
    # get_config replies, or save unrelated logs that might contain credentials.
    start = line.find(b'{"imu":')
    if start < 0:
        return None
    obj = json.loads(line[start:].decode("utf-8"))
    if not isinstance(obj, dict):
        raise ValueError("Invalid diagnostic frame")
    return obj


def check_sample(values, previous):
    if not isinstance(values, list) or len(values) != 9:
        raise ValueError("Invalid sample length")
    if any(type(v) is not int for v in values):
        raise ValueError("Sample must contain integers")
    expected = 0 if previous is None else previous[0] + 1
    if values[0] != expected:
        raise ValueError("Lost/duplicate/out-of-order sample; please recapture")
    if values[1] < 0 or (previous is not None and values[1] <= previous[1]):
        raise ValueError("Non-increasing timestamp; please recapture")
    if not 0 <= values[2] < 2**24:
        raise ValueError("Invalid sensor counter")
    if any(not -32768 <= v <= 32767 for v in values[3:]):
        raise ValueError("Raw value outside signed 16-bit range")
    return values


def csv_row(values, label, trial):
    return [label, trial, *values,
            *(round(v * 1000 / 8192, 6) for v in values[3:6]),
            *(round(v * 1000 / 32768, 6) for v in values[6:9])]


def summarize(rows, done, seconds):
    if done.get("cancelled") is not False or done.get("result") != 0 or done.get("restore") != 0:
        raise ValueError(f"Capture cancelled or sensor error: {done}")
    if done.get("errors") != 0 or done.get("count") != len(rows) or len(rows) < 2:
        raise ValueError("Incomplete or failed capture")
    intervals = [(b[1] - a[1]) / 1000 for a, b in zip(rows, rows[1:])]
    if min(intervals) <= 0:
        raise ValueError("Invalid sample timing")
    span = (rows[-1][1] - rows[0][1]) / 1e6
    hz = (len(rows) - 1) / span
    saturated = sum(any(abs(v) >= 32700 for v in r[3:]) for r in rows)
    warnings = []
    if hz < 80 or hz > 110:
        warnings.append("Effective rate is outside 80..110 Hz; inspect before tuning")
    if max(intervals) > 30:
        warnings.append("Sampling gap exceeds 30 ms; inspect movement interval")
    if span < seconds * 0.9:
        warnings.append("Recorded time span is too short")
    if saturated:
        warnings.append("Near-range-limit raw values detected; review sensor range")
    return {"count": len(rows), "span_s": span, "effective_hz": hz,
            "median_interval_ms": statistics.median(intervals),
            "max_interval_ms": max(intervals),
            "gaps_over_15ms": sum(x > 15 for x in intervals),
            "near_saturation_frames": saturated,
            "mean_accel_mg": [statistics.mean(r[3+i] for r in rows)*1000/8192 for i in range(3)],
            "mean_gyro_dps": [statistics.mean(r[6+i] for r in rows)*1000/32768 for i in range(3)],
            "warnings": warnings}


def beep():
    try:
        import winsound
        winsound.Beep(1000, 100)
    except (ImportError, RuntimeError):
        print("\a", end="", flush=True)


def record(port, args, trial):
    stem = f"{datetime.now():%Y%m%d-%H%M%S}_{args.label}_{trial:02d}_{uuid.uuid4().hex[:8]}"
    partial = args.output / (stem + ".partial.csv")
    final = args.output / (stem + ".csv")
    rows = []
    metadata = {"label": args.label, "trial": trial, "hand": "left", "face": "outward",
                "seconds_requested": args.seconds, "delay_ms": args.delay * 1000,
                "native_axes": True, "transport": "usb_ram_buffer", "status": "incomplete"}
    started = recording = exporting = False
    # Native USB: do not toggle RTS/DTR or reboot. Previous run must have finished.
    port.reset_input_buffer()
    request = {"cmd": "imu_capture", "seconds": args.seconds, "delay_ms": args.delay * 1000}
    try:
        with partial.open("x", newline="", encoding="utf-8") as out:
            writer = csv.writer(out)
            writer.writerow(HEADER)
            port.write((json.dumps(request) + "\n").encode("ascii"))
            deadline = time.monotonic() + args.delay + args.seconds + 30
            while time.monotonic() < deadline:
                line = port.read_until(b"\n", size=4096)
                if not line:
                    continue
                message = parse_message(line)
                if message is None:
                    if b'unknown cmd' in line:
                        raise ValueError("Watch firmware has no imu_capture command; build and flash 9A first")
                    continue
                kind = message.get("imu")
                if kind == "error":
                    raise ValueError(f"Watch rejected request: {message.get('reason')}")
                if kind == "preparing":
                    if started or message.get("version") != 1 or message.get("odr_hz") != 100 or \
                            message.get("accel_range_g") != 4 or message.get("gyro_range_dps") != 1000:
                        raise ValueError("Unexpected firmware protocol/configuration")
                    started = True
                    metadata["device_config"] = message
                    instruction = "全程保持静止" if args.label == "still" else "先停稳再完成一次动作"
                    print(f"准备 {args.delay} 秒；听到提示音后开始，{instruction}。", flush=True)
                elif kind == "recording":
                    if not started or recording:
                        raise ValueError("Unexpected recording state")
                    recording = True
                    print(f"正在记录 {args.seconds} 秒（包括收手和停稳）...", flush=True)
                    beep()
                elif kind == "exporting":
                    if not recording or exporting:
                        raise ValueError("Unexpected export state")
                    exporting = True
                    print("动作记录结束，正在保存。", flush=True)
                    beep()
                elif kind == "sample":
                    if not exporting or len(rows) >= args.seconds * 110 + 8:
                        raise ValueError("Unexpected/excess sample")
                    values = check_sample(message.get("v"), rows[-1] if rows else None)
                    writer.writerow(csv_row(values, args.label, trial))
                    rows.append(values)
                elif kind == "done":
                    metadata["device_result"] = message
                    metadata["quality"] = summarize(rows, message, args.seconds)
                    if not exporting:
                        raise ValueError("Missing acquisition/export state")
                    metadata["status"] = "complete"
                    break
                else:
                    raise ValueError(f"Unexpected IMU message: {kind}")
            else:
                raise TimeoutError("No complete capture: wake watch, check USB and firmware, then retry")
        # Only a count/sequence/status-validated capture gets a normal .csv name.
        partial.rename(final)
        metadata["sha256"] = hashlib.sha256(final.read_bytes()).hexdigest()
        q = metadata["quality"]
        print(f"保存 {final}\n有效采样 {q['effective_hz']:.1f} Hz，最大间隔 {q['max_interval_ms']:.1f} ms")
        for warning in q["warnings"]:
            print("注意：" + warning)
        return not q["warnings"]
    except BaseException as exc:
        metadata["status"] = "incomplete"
        metadata["error"] = str(exc) or type(exc).__name__
        raise
    finally:
        (args.output / (stem + ".json")).write_text(
            json.dumps(metadata, ensure_ascii=False, indent=2), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description="独立 BMI270 采集工具（左手、表盘朝外）")
    parser.add_argument("--port", help="例如 COM18；省略时列出串口并询问")
    parser.add_argument("--label", choices=LABELS)
    parser.add_argument("--seconds", type=int, default=6, choices=range(1, 16), metavar="1..15")
    parser.add_argument("--delay", type=int, default=3, choices=range(0, 11), metavar="0..10")
    parser.add_argument("--trials", type=int, default=1)
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parents[1] / "captures")
    args = parser.parse_args()
    if not 1 <= args.trials <= 100:
        parser.error("trials must be 1..100")
    import serial
    from serial.tools import list_ports
    if not args.port:
        for device in list_ports.comports():
            print(device.device, device.description)
        args.port = input("手表串口（如 COM18）：").strip()
    if not args.label:
        print("标签：" + ", ".join(LABELS))
        args.label = input("本批动作标签：").strip()
        if args.label not in LABELS:
            parser.error("Unknown label")
    args.output.mkdir(parents=True, exist_ok=True)
    print(f"保持 USB 线松弛；关闭串口监视器和上位机 USB 连接。本批标签：{args.label}。")
    print("采样不执行翻页。不要在采样中切换页面。KEY4 可取消采集并息屏。")
    port = serial.Serial(port=None, baudrate=115200, timeout=1, write_timeout=2)
    port.dtr = False
    port.rts = False
    port.port = args.port
    with port:
        for trial in range(1, args.trials + 1):
            input(f"[{trial}/{args.trials}] {args.label}：准备好后按 Enter，或 Ctrl+C 退出。")
            if not record(port, args, trial):
                print("此批暂停：先检查数据质量，再继续采其他动作。")
                break


if __name__ == "__main__":
    try:
        main()
    except (KeyboardInterrupt, EOFError):
        print("\n已停止电脑采集。手表会在本次时限结束后恢复；也可按 KEY4 取消。")
    except Exception as exc:
        print(f"采集失败：{exc}\n不完整数据保留为 .partial.csv，不能作为合格样本。")
        raise SystemExit(1)
