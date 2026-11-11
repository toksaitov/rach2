# Raise and focus Crysis's window on the guest's X display :99 and press a key
# in it, type a line and Return (--text LINE), or close the window (--close).
# Usage: 2653-key.py KEYSYM | --text LINE | --close
import ctypes as c, sys, time
x=c.CDLL('libX11.so.6'); t=c.CDLL('libXtst.so.6')
x.XOpenDisplay.argtypes=[c.c_char_p]; x.XOpenDisplay.restype=c.c_void_p
x.XDefaultRootWindow.argtypes=[c.c_void_p]; x.XDefaultRootWindow.restype=c.c_ulong
x.XQueryTree.argtypes=[c.c_void_p,c.c_ulong,c.POINTER(c.c_ulong),c.POINTER(c.c_ulong),c.POINTER(c.POINTER(c.c_ulong)),c.POINTER(c.c_uint)]
x.XFetchName.argtypes=[c.c_void_p,c.c_ulong,c.POINTER(c.c_char_p)]
x.XSetInputFocus.argtypes=[c.c_void_p,c.c_ulong,c.c_int,c.c_ulong]
x.XRaiseWindow.argtypes=[c.c_void_p,c.c_ulong]
x.XFlush.argtypes=[c.c_void_p]
x.XStringToKeysym.argtypes=[c.c_char_p]; x.XStringToKeysym.restype=c.c_ulong
x.XKeysymToKeycode.argtypes=[c.c_void_p,c.c_ulong]; x.XKeysymToKeycode.restype=c.c_uint
t.XTestFakeKeyEvent.argtypes=[c.c_void_p,c.c_uint,c.c_int,c.c_ulong]
d=x.XOpenDisplay(b':99'); assert d
target=0
r=c.c_ulong(); p=c.c_ulong(); children=c.POINTER(c.c_ulong)(); count=c.c_uint()
assert x.XQueryTree(d,x.XDefaultRootWindow(d),c.byref(r),c.byref(p),c.byref(children),c.byref(count))
for i in range(count.value):
    name=c.c_char_p()
    if x.XFetchName(d,children[i],c.byref(name)) and name.value:
        print('key-probe: window',hex(children[i]),name.value.decode(errors='replace'),flush=True)
        if b'crysis' in name.value.lower():
            target=children[i]
            x.XRaiseWindow(d,children[i]); x.XSetInputFocus(d,children[i],1,0)
if sys.argv[1] == '--close':
    class Data(c.Union):
        _fields_=[('l',c.c_long*5),('b',c.c_char*20)]
    class Message(c.Structure):
        _fields_=[('type',c.c_int),('serial',c.c_ulong),('send_event',c.c_int),('display',c.c_void_p),('window',c.c_ulong),('message_type',c.c_ulong),('format',c.c_int),('data',Data)]
    class Event(c.Union):
        _fields_=[('message',Message),('pad',c.c_long*24)]
    x.XInternAtom.argtypes=[c.c_void_p,c.c_char_p,c.c_int];x.XInternAtom.restype=c.c_ulong
    x.XSendEvent.argtypes=[c.c_void_p,c.c_ulong,c.c_int,c.c_long,c.c_void_p]
    assert target
    e=Event();e.message.type=33;e.message.display=d;e.message.window=target
    e.message.message_type=x.XInternAtom(d,b'WM_PROTOCOLS',0);e.message.format=32
    e.message.data.l[0]=x.XInternAtom(d,b'WM_DELETE_WINDOW',0)
    print('key-probe: closing window',hex(target),flush=True)
    assert x.XSendEvent(d,target,0,0,c.byref(e))
    x.XFlush(d)
    sys.exit(0)
if sys.argv[1] == '--text':
    x.XkbKeycodeToKeysym.argtypes=[c.c_void_p,c.c_uint,c.c_int,c.c_int]
    x.XkbKeycodeToKeysym.restype=c.c_ulong
    shift=x.XKeysymToKeycode(d,x.XStringToKeysym(b'Shift_L'))
    for ch in sys.argv[2]:
        symbol=ord(ch); key=x.XKeysymToKeycode(d,symbol); assert key, ch
        shifted=x.XkbKeycodeToKeysym(d,key,0,0)!=symbol
        if shifted:t.XTestFakeKeyEvent(d,shift,1,0)
        t.XTestFakeKeyEvent(d,key,1,0);t.XTestFakeKeyEvent(d,key,0,0)
        if shifted:t.XTestFakeKeyEvent(d,shift,0,0)
        x.XFlush(d);time.sleep(.1)
    key=x.XKeysymToKeycode(d,x.XStringToKeysym(b'Return'))
else:
    key=x.XKeysymToKeycode(d,x.XStringToKeysym(sys.argv[1].encode())); assert key
print('key-probe: pressing',sys.argv[1:],key,flush=True)
t.XTestFakeKeyEvent(d,key,1,0);x.XFlush(d);time.sleep(.05)
t.XTestFakeKeyEvent(d,key,0,0);x.XFlush(d)
