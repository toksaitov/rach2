# Makefile: build vgpushm.ko against the running kernel's headers, or those
# of KDIR, as Debian's linux-headers package installs them.
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all clean
