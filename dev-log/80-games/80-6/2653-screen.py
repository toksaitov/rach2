# Read the guest's X root window on :99, 640x480, into /tmp/task2653-screen.ppm.
# Usage: 2653-screen.py
import ctypes as c
x=c.CDLL('libX11.so.6')
x.XOpenDisplay.argtypes=[c.c_char_p];x.XOpenDisplay.restype=c.c_void_p
x.XDefaultRootWindow.argtypes=[c.c_void_p];x.XDefaultRootWindow.restype=c.c_ulong
x.XGetImage.argtypes=[c.c_void_p,c.c_ulong,c.c_int,c.c_int,c.c_uint,c.c_uint,c.c_ulong,c.c_int];x.XGetImage.restype=c.c_void_p
class XI(c.Structure):
 _fields_=[('width',c.c_int),('height',c.c_int),('xoffset',c.c_int),('format',c.c_int),('data',c.c_void_p),('byte_order',c.c_int),('bitmap_unit',c.c_int),('bitmap_bit_order',c.c_int),('bitmap_pad',c.c_int),('depth',c.c_int),('bytes_per_line',c.c_int),('bits_per_pixel',c.c_int),('red_mask',c.c_ulong),('green_mask',c.c_ulong),('blue_mask',c.c_ulong)]
d=x.XOpenDisplay(b':99');assert d
p=x.XGetImage(d,x.XDefaultRootWindow(d),0,0,640,480,0xffffffff,2);assert p
i=c.cast(p,c.POINTER(XI)).contents
assert i.bits_per_pixel==32 and i.byte_order==0
b=c.string_at(i.data,i.bytes_per_line*i.height);o=bytearray()
for row in range(i.height):
 for col in range(i.width):
  n=row*i.bytes_per_line+col*4;o.extend((b[n+2],b[n+1],b[n]))
open('/tmp/task2653-screen.ppm','wb').write(b'P6\n640 480\n255\n'+o)
