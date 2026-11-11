# Makefile: build vgpu-backend in the VM against Debian's virglrenderer,
# libdrm, Vulkan's loader and the transport's header in rach2/sys/virtio.
CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra
# vgpushm.h and device.c read and write the shared region through pointers
# cast to wider types.
CFLAGS += -std=c99 -fno-strict-aliasing
CPPFLAGS += -I../../../rach2/sys/virtio -I../module $(shell pkg-config --cflags virglrenderer libdrm)
LDLIBS += $(shell pkg-config --libs virglrenderer libdrm vulkan)
OBJS = main.o device.o virgl.o kms.o vkprobe.o

vgpu-backend: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(OBJS): backend.h vgpudev.h ../../../rach2/sys/virtio/vgpushm.h ../module/vgpushm_uapi.h

clean:
	rm -f vgpu-backend $(OBJS)

.PHONY: clean
