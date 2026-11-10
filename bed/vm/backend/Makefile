# Makefile: build vgpu-backend in the VM against Debian's virglrenderer,
# libdrm, Vulkan's loader and the transport's header in bed/shm.
CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra
# vgpushm.h copies blocks a 32-bit word at a time through cast pointers.
CFLAGS += -std=c99 -fno-strict-aliasing
CPPFLAGS += -I../../shm -I../module $(shell pkg-config --cflags virglrenderer libdrm)
LDLIBS += $(shell pkg-config --libs virglrenderer libdrm vulkan)
OBJS = main.o device.o virgl.o kms.o vkprobe.o

vgpu-backend: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(OBJS): backend.h vgpudev.h ../../shm/vgpushm.h ../module/vgpushm_uapi.h

clean:
	rm -f vgpu-backend $(OBJS)

.PHONY: clean
