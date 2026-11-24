"""Clicks a screen point with a given button via SendInput, then puts the
cursor back. Usage: python -I click.py x y [middle|left|right] [ctrl]"""
import ctypes
import sys
import time
from ctypes import wintypes

user32 = ctypes.WinDLL("user32", use_last_error=True)
user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", wintypes.LONG), ("dy", wintypes.LONG),
                ("mouseData", wintypes.DWORD), ("dwFlags", wintypes.DWORD),
                ("time", wintypes.DWORD), ("dwExtraInfo", ctypes.c_size_t)]


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD),
                ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD),
                ("dwExtraInfo", ctypes.c_size_t)]


class _U(ctypes.Union):
    _fields_ = [("mi", MOUSEINPUT), ("ki", KEYBDINPUT),
                ("pad", ctypes.c_byte * 32)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", wintypes.DWORD), ("u", _U)]


def send(*inputs):
    arr = (INPUT * len(inputs))(*inputs)
    user32.SendInput(len(inputs), arr, ctypes.sizeof(INPUT))


def mouse(flags):
    i = INPUT(type=0)
    i.u.mi = MOUSEINPUT(0, 0, 0, flags, 0, 0)
    return i


def key(vk, up=False):
    i = INPUT(type=1)
    i.u.ki = KEYBDINPUT(vk, 0, 2 if up else 0, 0, 0)
    return i


DOWN = {"left": 0x2, "right": 0x8, "middle": 0x20}
UP = {"left": 0x4, "right": 0x10, "middle": 0x40}

x, y = int(sys.argv[1]), int(sys.argv[2])
btn = sys.argv[3] if len(sys.argv) > 3 else "middle"
ctrl = "ctrl" in sys.argv[4:]
old = wintypes.POINT()
user32.GetCursorPos(ctypes.byref(old))
user32.SetCursorPos(x, y)
time.sleep(0.25)
user32.SetCursorPos(x + 1, y)
time.sleep(0.25)
if ctrl:
    send(key(0x11))
    time.sleep(0.05)
send(mouse(DOWN[btn]))
time.sleep(0.08)
send(mouse(UP[btn]))
if ctrl:
    time.sleep(0.05)
    send(key(0x11, up=True))
time.sleep(0.4)
if btn != "middle":
    pass
else:
    user32.SetCursorPos(old.x, old.y)
print("clicked", btn, x, y, "ctrl" if ctrl else "")
