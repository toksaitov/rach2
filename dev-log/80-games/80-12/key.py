#!/usr/bin/env python3
"""key.py: press a key, type a line or close Crysis's window on $DISPLAY.

Usage: key.py KEYSYM | --text LINE | --close | --list

80-6's helper for Xvfb, for Xwayland: it lists the root's named children and
sends through XTEST, which reaches the window that has the X focus, so it
leaves the focus to Weston's window manager. --text types LINE and Return.
"""
import ctypes as c
import os
import sys
import time

x = c.CDLL('libX11.so.6')
t = c.CDLL('libXtst.so.6')
x.XOpenDisplay.argtypes = [c.c_char_p]
x.XOpenDisplay.restype = c.c_void_p
x.XDefaultRootWindow.argtypes = [c.c_void_p]
x.XDefaultRootWindow.restype = c.c_ulong
x.XQueryTree.argtypes = [c.c_void_p, c.c_ulong, c.POINTER(c.c_ulong),
                         c.POINTER(c.c_ulong),
                         c.POINTER(c.POINTER(c.c_ulong)), c.POINTER(c.c_uint)]
x.XFetchName.argtypes = [c.c_void_p, c.c_ulong, c.POINTER(c.c_char_p)]
x.XFlush.argtypes = [c.c_void_p]
x.XStringToKeysym.argtypes = [c.c_char_p]
x.XStringToKeysym.restype = c.c_ulong
x.XKeysymToKeycode.argtypes = [c.c_void_p, c.c_ulong]
x.XKeysymToKeycode.restype = c.c_uint
x.XkbKeycodeToKeysym.argtypes = [c.c_void_p, c.c_uint, c.c_int, c.c_int]
x.XkbKeycodeToKeysym.restype = c.c_ulong
x.XInternAtom.argtypes = [c.c_void_p, c.c_char_p, c.c_int]
x.XInternAtom.restype = c.c_ulong
x.XSendEvent.argtypes = [c.c_void_p, c.c_ulong, c.c_int, c.c_long, c.c_void_p]
x.XGetInputFocus.argtypes = [c.c_void_p, c.POINTER(c.c_ulong),
                             c.POINTER(c.c_int)]
t.XTestFakeKeyEvent.argtypes = [c.c_void_p, c.c_uint, c.c_int, c.c_ulong]


class Data(c.Union):
    """A client message's payload."""
    _fields_ = [('l', c.c_long * 5), ('b', c.c_char * 20)]


class Message(c.Structure):
    """Xlib's XClientMessageEvent."""
    _fields_ = [('type', c.c_int), ('serial', c.c_ulong),
                ('send_event', c.c_int), ('display', c.c_void_p),
                ('window', c.c_ulong), ('message_type', c.c_ulong),
                ('format', c.c_int), ('data', Data)]


class Event(c.Union):
    """Xlib's XEvent, of which only the client message is used."""
    _fields_ = [('message', Message), ('pad', c.c_long * 24)]


def windows(d, parent, depth, found):
    """Print the named windows under PARENT and collect Crysis's in FOUND."""
    root = c.c_ulong()
    up = c.c_ulong()
    children = c.POINTER(c.c_ulong)()
    count = c.c_uint()
    if not x.XQueryTree(d, parent, c.byref(root), c.byref(up),
                        c.byref(children), c.byref(count)):
        return
    for i in range(count.value):
        name = c.c_char_p()
        if x.XFetchName(d, children[i], c.byref(name)) and name.value:
            text = name.value.decode(errors='replace')
            print('key: window', hex(children[i]), text, flush=True)
            if 'crysis' in text.lower():
                found.append(children[i])
        if depth < 3:
            windows(d, children[i], depth + 1, found)


def press(d, key):
    """Press and release the key code KEY."""
    t.XTestFakeKeyEvent(d, key, 1, 0)
    x.XFlush(d)
    time.sleep(.05)
    t.XTestFakeKeyEvent(d, key, 0, 0)
    x.XFlush(d)


def main():
    """Do what the first argument names; exit 1 without Crysis's window."""
    d = x.XOpenDisplay(os.environ['DISPLAY'].encode())
    assert d
    found = []
    windows(d, x.XDefaultRootWindow(d), 0, found)
    focus = c.c_ulong()
    revert = c.c_int()
    x.XGetInputFocus(d, c.byref(focus), c.byref(revert))
    print('key: focus', hex(focus.value), flush=True)
    what = sys.argv[1]
    if what == '--list':
        return 0 if found else 1
    if what == '--close':
        if not found:
            return 1
        e = Event()
        e.message.type = 33
        e.message.display = d
        e.message.window = found[0]
        e.message.message_type = x.XInternAtom(d, b'WM_PROTOCOLS', 0)
        e.message.format = 32
        e.message.data.l[0] = x.XInternAtom(d, b'WM_DELETE_WINDOW', 0)
        print('key: closing window', hex(found[0]), flush=True)
        assert x.XSendEvent(d, found[0], 0, 0, c.byref(e))
        x.XFlush(d)
        return 0
    if what == '--text':
        shift = x.XKeysymToKeycode(d, x.XStringToKeysym(b'Shift_L'))
        for ch in sys.argv[2]:
            key = x.XKeysymToKeycode(d, ord(ch))
            assert key, ch
            shifted = x.XkbKeycodeToKeysym(d, key, 0, 0) != ord(ch)
            if shifted:
                t.XTestFakeKeyEvent(d, shift, 1, 0)
            press(d, key)
            if shifted:
                t.XTestFakeKeyEvent(d, shift, 0, 0)
            x.XFlush(d)
            time.sleep(.1)
        what = 'Return'
    key = x.XKeysymToKeycode(d, x.XStringToKeysym(what.encode()))
    assert key, what
    print('key: pressing', what, key, flush=True)
    press(d, key)
    return 0


if __name__ == '__main__':
    sys.exit(main())
