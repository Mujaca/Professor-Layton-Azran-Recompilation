"""Haelt den Bildschirm an (Windows) bzw. weckt ihn auf.

  python tools/keep_awake.py           # dauerhaft an (laeuft im Hintergrund)
  python tools/keep_awake.py --wake    # einmal wecken
"""
import ctypes
import sys

ES_CONTINUOUS = 0x80000000
ES_SYSTEM_REQUIRED = 0x00000001
ES_DISPLAY_REQUIRED = 0x00000002

WM_SYSCOMMAND = 0x0112
SC_MONITORPOWER = 0xF170
HWND_BROADCAST = 0xFFFF


def wake():
    # Monitor einschalten (SC_MONITORPOWER mit -1)
    ctypes.windll.user32.SendMessageW(HWND_BROADCAST, WM_SYSCOMMAND, SC_MONITORPOWER, -1)
    # Mausbewegung um 0 Pixel fordert ebenfalls Aktivitaet an
    ctypes.windll.user32.mouse_event(0x0001, 0, 0, 0, 0)


def main():
    wake()
    if "--wake" in sys.argv:
        print("wake gesendet")
        return
    ctypes.windll.kernel32.SetThreadExecutionState(
        ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED)
    print("Display bleibt an (Strg+C beendet)")
    try:
        while True:
            import time
            time.sleep(30)
            wake()
    except KeyboardInterrupt:
        ctypes.windll.kernel32.SetThreadExecutionState(ES_CONTINUOUS)


if __name__ == "__main__":
    main()
