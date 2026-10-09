#!/usr/bin/env python3
"""Send WLED DRGB realtime packets to the ESP32 LED bar client.

Usage:
  python tools/drgb_test.py <esp-ip> solid <r> <g> <b> [leds]
  python tools/drgb_test.py <esp-ip> rainbow [seconds] [leds]
  python tools/drgb_test.py <esp-ip> soak [seconds] [leds]
  python tools/drgb_test.py <esp-ip> malformed

[leds] is the number of pixels per packet (default 8, clamped to 1-24).
"""
import colorsys
import socket
import sys
import time

PORT = 21324
DRGB = 2
TIMEOUT_SEC = 2
DEFAULT_LEDS = 8
MAX_LEDS = 24


def clamp_leds(n):
    """Clamp a requested pixel count to 1..MAX_LEDS, noting any change."""
    c = max(1, min(MAX_LEDS, n))
    if c != n:
        print(f"note: {n} LEDs is outside 1-{MAX_LEDS}, using {c}")
    return c


def packet(pixels):
    buf = bytearray(2 + len(pixels) * 3)
    buf[0] = DRGB
    buf[1] = TIMEOUT_SEC
    for i, (r, g, b) in enumerate(pixels):
        buf[2 + i * 3] = r & 0xFF
        buf[3 + i * 3] = g & 0xFF
        buf[4 + i * 3] = b & 0xFF
    return bytes(buf)


def send(sock, host, data):
    sock.sendto(data, (host, PORT))


def solid(sock, host, r, g, b, leds=DEFAULT_LEDS, seconds=5):
    data = packet([(r, g, b)] * leds)
    end = time.time() + seconds
    while time.time() < end:
        send(sock, host, data)
        time.sleep(1 / 30)


def rainbow(sock, host, seconds=10, leds=DEFAULT_LEDS):
    end = time.time() + seconds
    phase = 0.0
    while time.time() < end:
        px = []
        for i in range(leds):
            h = (phase + i / leds) % 1.0
            r, g, b = colorsys.hsv_to_rgb(h, 1.0, 1.0)
            px.append((int(r * 255), int(g * 255), int(b * 255)))
        send(sock, host, packet(px))
        phase = (phase + 0.01) % 1.0
        time.sleep(1 / 30)


def soak(sock, host, seconds=60, leds=DEFAULT_LEDS):
    end = time.time() + seconds
    frames = 0
    while time.time() < end:
        v = frames % 256
        send(sock, host, packet([(v, 255 - v, 128)] * leds))
        frames += 1
        time.sleep(1 / 30)
    print(f"sent {frames} frames in {seconds}s")


def malformed(sock, host):
    cases = [
        ("empty", b""),
        ("one byte", bytes([2])),
        ("header only keepalive", bytes([2, 2])),
        ("wrong protocol WARLS", bytes([1, 2, 255, 0, 0])),
        ("wrong protocol DRGBW", bytes([3, 2, 255, 0, 0])),
        ("wrong protocol DNRGB", bytes([4, 2, 0, 0, 255, 0, 0])),
        ("oversized", bytes([2, 2]) + bytes(25 * 3)),
        ("partial pixel", bytes([2, 2, 9, 9, 9, 7, 7])),
        ("zero timeout", bytes([2, 0, 0, 255, 0])),
    ]
    for name, data in cases:
        print(f"sending {name} ({len(data)} bytes)")
        send(sock, host, data)
        time.sleep(0.5)
    print("strip must still be responsive; check droppedPackets via "
          "GET /api/led/config")


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    host = sys.argv[1]
    mode = sys.argv[2]
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    if mode == "solid":
        r, g, b = (int(x) for x in sys.argv[3:6])
        leds = clamp_leds(int(sys.argv[6])) if len(sys.argv) > 6 else DEFAULT_LEDS
        solid(sock, host, r, g, b, leds)
    elif mode == "rainbow":
        secs = int(sys.argv[3]) if len(sys.argv) > 3 else 10
        leds = clamp_leds(int(sys.argv[4])) if len(sys.argv) > 4 else DEFAULT_LEDS
        rainbow(sock, host, secs, leds)
    elif mode == "soak":
        secs = int(sys.argv[3]) if len(sys.argv) > 3 else 60
        leds = clamp_leds(int(sys.argv[4])) if len(sys.argv) > 4 else DEFAULT_LEDS
        soak(sock, host, secs, leds)
    elif mode == "malformed":
        malformed(sock, host)
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
