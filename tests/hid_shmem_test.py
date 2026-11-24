"""Konsistenztest HID-Shared-Memory (Azahar hid.h <-> runtime/kernel/hle.c).

Prueft die Offsets/Groessen der vier Ringpuffer gegen die aus Azahar
abgeleiteten Werte. Laeuft ohne Build:
    python tests/hid_shmem_test.py
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HLE = (ROOT / "runtime" / "kernel" / "hle.c").read_text(encoding="utf-8")
HDR = (ROOT / "runtime" / "platform" / "hid_ring.h").read_text(encoding="utf-8")

# Azahar core/hle/service/hid/hid.h:
#   pad:       ticks 0x00/0x08, index 0x10, slider 0x18, current 0x1C,
#              entries[8] je 16 Byte ab 0x28
#   touch:     ticks 0x0A8 (ASSERT_REG_POSITION 0x2A), index 0xB8
#              entries[8] je 8 Byte ab 0xC8
#   accel:     ticks 0x108, index 0x118, entries[8] je 6 Byte ab 0x128
#   gyro:      ticks 0x158, index 0x168, entries[32] je 6 Byte ab 0x178
EXPECTED = {
    "HID_PAD_TICKS": 0x000,
    "HID_PAD_TICKS_PREV": 0x008,
    "HID_PAD_INDEX": 0x010,
    "HID_PAD_SLIDER": 0x018,
    "HID_PAD_CURRENT": 0x01C,
    "HID_PAD_ENTRIES_OFF": 0x028,
    "HID_TOUCH_TICKS": 0x0A8,
    "HID_TOUCH_TICKS_PREV": 0x0B0,
    "HID_TOUCH_INDEX": 0x0B8,
    "HID_TOUCH_RAW": 0x0C0,
    "HID_TOUCH_ENTRIES_OFF": 0x0C8,
    "HID_ACC_TICKS": 0x108,
    "HID_ACC_TICKS_PREV": 0x110,
    "HID_ACC_INDEX": 0x118,
    "HID_ACC_RAW": 0x120,
    "HID_ACC_ENTRIES_OFF": 0x128,
    "HID_GYRO_TICKS": 0x158,
    "HID_GYRO_TICKS_PREV": 0x160,
    "HID_GYRO_INDEX": 0x168,
    "HID_GYRO_RAW": 0x170,
    "HID_GYRO_ENTRIES_OFF": 0x178,
}
COUNTS = {
    "HID_PAD_ENTRIES": 8,
    "HID_TOUCH_ENTRIES": 8,
    "HID_ACCEL_ENTRIES": 8,
    "HID_GYRO_ENTRIES": 32,
}
RINGS = [
    ("pad", "HID_PAD_ENTRIES_OFF", 8 * 16, "HID_TOUCH_TICKS"),
    ("touch", "HID_TOUCH_ENTRIES_OFF", 8 * 8, "HID_ACC_TICKS"),
    ("accel", "HID_ACC_ENTRIES_OFF", 8 * 6, "HID_GYRO_TICKS"),
    ("gyro", "HID_GYRO_ENTRIES_OFF", 32 * 6, None),
]
SYSCLOCK = 268111856


def macro_hex(name):
    m = re.search(r"#define\s+%s\s+0x([0-9A-Fa-f]+)u" % re.escape(name), HDR)
    if not m:
        raise AssertionError("Makro %s fehlt in hid_ring.h" % name)
    return int(m.group(1), 16)


def macro_int(name):
    m = re.search(r"#define\s+%s\s+(\d+)u" % re.escape(name), HDR)
    if not m:
        raise AssertionError("Makro %s fehlt in hid_ring.h" % name)
    return int(m.group(1))


def main():
    ok = True
    for name, want in EXPECTED.items():
        got = macro_hex(name)
        if got != want:
            ok = False
            print("FAIL %-24s hle.c=0x%03X erwartet=0x%03X" % (name, got, want))
    for name, want in COUNTS.items():
        got = macro_int(name)
        if got != want:
            ok = False
            print("FAIL %-24s hle.c=%d erwartet=%d" % (name, got, want))

    for name, start_name, size, next_name in RINGS:
        start = EXPECTED[start_name]
        end = start + size
        if end > 0x1000:
            ok = False
            print("FAIL %s-Ende 0x%X > 0x1000" % (name, end))
        if next_name is not None and end != EXPECTED[next_name]:
            ok = False
            print("FAIL %s endet bei 0x%X, %s beginnt 0x%X"
                  % (name, end, next_name, EXPECTED[next_name]))

    for name, div in (("HID_PAD_TICKS_PERIOD", 234),
                      ("HID_ACC_TICKS_PERIOD", 104),
                      ("HID_GYRO_TICKS_PERIOD", 101)):
        m = re.search(r"#define\s+%s\s+\(SYSCLOCK_ARM11\s*/\s*(\d+)u\)" % name, HLE)
        if not m or int(m.group(1)) != div:
            ok = False
            print("FAIL %s: SYSCLOCK_ARM11/%d fehlt" % (name, div))

    if ok:
        print("OK HID-Shared-Memory-Layout (Azahar hid.h)")
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
