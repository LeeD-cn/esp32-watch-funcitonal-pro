#!/usr/bin/env python3
"""Read the watch's current hardware step counter over USB serial."""

import argparse
import json
import time


def choose_port(requested):
    from serial.tools import list_ports

    if requested:
        return requested
    ports = list(list_ports.comports())
    if len(ports) == 1:
        return ports[0].device
    if not ports:
        raise RuntimeError("未发现串口")
    names = ", ".join(port.device for port in ports)
    raise RuntimeError(f"发现多个串口（{names}），请使用 --port 指定")


def main():
    parser = argparse.ArgumentParser(description="查询手表今日步数")
    parser.add_argument("--port", help="串口，例如 COM3")
    args = parser.parse_args()

    try:
        import serial
    except ImportError as exc:
        raise SystemExit("缺少 pyserial，请先执行：python -m pip install pyserial") from exc

    try:
        port_name = choose_port(args.port)
        port = serial.Serial(port=None, baudrate=115200, timeout=0.25, write_timeout=2)
        port.dtr = False
        port.rts = False
        port.port = port_name
        port.open()
        port.reset_input_buffer()
        port.write(b'{"cmd":"get_steps"}\n')
        port.flush()

        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            line = port.readline().decode("utf-8", errors="replace").strip()
            start = line.find('{"steps":')
            if start < 0:
                continue
            reply = json.loads(line[start:])
            print(
                f"今日步数: {reply['steps']}  "
                f"传感器累计: {reply['sensor_steps']}  "
                f"日期: {reply['date']}  运行: {reply['running']}  "
                f"错误码: {reply['error']}"
            )
            return
        raise RuntimeError("5 秒内未收到步数回复")
    except (OSError, RuntimeError, serial.SerialException) as exc:
        raise SystemExit(f"查询失败：{exc}\n请先关闭 VS Code 串口监视器和上位机串口连接。") from exc
    finally:
        if "port" in locals() and port.is_open:
            port.close()


if __name__ == "__main__":
    main()
