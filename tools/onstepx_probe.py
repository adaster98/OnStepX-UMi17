#!/usr/bin/env python3
"""Minimal LX200 probe for a USB-connected OnStepX board.

Usage:  python3 onstepx_probe.py /dev/ttyUSB0 [baud]

Sends a handful of read-only OnStep commands and prints the replies.
Nothing here moves a motor.
"""
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB0"
BAUD = int(sys.argv[2]) if len(sys.argv) > 2 else 9600

# (command, description, expects a '#'-terminated reply)
PROBES = [
    (":GVP#", "product name (expect OnStepX)", True),
    (":GVN#", "firmware version", True),
    (":GVD#", "firmware date", True),
    (":GVT#", "firmware time", True),
    (":GVM#", "full version string", True),
    (":GU#", "general status flags", True),
    (":GC#", "local date (RTC)", True),
    (":GL#", "local time (RTC)", True),
    (":Gt#", "site latitude", True),
    (":Gg#", "site longitude", True),
    (":GR#", "current RA", True),
    (":GD#", "current Dec", True),
    (":GXE7#", "PEC steps per worm rotation (expect 64000)", True),
    (":GXE8#", "PEC buffer seconds", True),
    (":GXE6#", "steps per sidereal second", True),
]


def wait_ready(ser, timeout=25.0):
    """Opening the port resets the board; OnStepX needs ~10 s to bring up its
    command channels. Poll until it answers rather than guessing a delay."""
    end = time.time() + timeout
    while time.time() < end:
        ser.reset_input_buffer()
        ser.write(b":GVP#")
        ser.flush()
        time.sleep(0.4)
        reply = ser.read(64)
        if b"Step" in reply:
            return True
        time.sleep(0.4)
    return False

def ask(ser, cmd, terminated, timeout=2.0):
    ser.reset_input_buffer()
    ser.write(cmd.encode())
    ser.flush()
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        chunk = ser.read(64)
        if chunk:
            buf += chunk
            if terminated and buf.endswith(b"#"):
                break
        else:
            if buf:
                break
    return buf.decode(errors="replace")


def main():
    # dsrdtr/rtscts off, then drop DTR and RTS so the auto-reset circuit
    # does not hold the ESP32 in reset or bootloader mode.
    ser = serial.Serial()
    ser.port = PORT
    ser.baudrate = BAUD
    ser.timeout = 0.2
    ser.dsrdtr = False
    ser.rtscts = False
    ser.open()
    ser.dtr = False
    ser.rts = False

    print(f"Opened {PORT} at {BAUD} 8N1")
    if not wait_ready(ser):
        print("warning: board did not answer :GVP# - replies may be unreliable")
    ser.reset_input_buffer()
    print()

    replies = 0
    for cmd, desc, terminated in PROBES:
        reply = ask(ser, cmd, terminated)
        if reply:
            replies += 1
        print(f"{cmd:<10} {desc:<34} -> {reply!r}")

    print()
    if replies == 0:
        print("No replies at all. Wrong baud rate, wrong port, board held in "
              "reset, or the firmware is not running.")
    else:
        print(f"{replies}/{len(PROBES)} commands answered.")
    ser.close()


if __name__ == "__main__":
    main()
