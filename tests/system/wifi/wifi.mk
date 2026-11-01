# wifi.mk: the Linux AP guest of the Wi-Fi suite (tests/system/wifi), built
# by `make wifi-guest` alone and by the suite that boots it, never by `all`,
# since its packages are a download of some 160 MiB. The relay is always
# built for the x86-64 Linux guest, whatever ARCH selects for Rach2.

include tests/system/wifi/hwsimrelay.mk

WIFI_GUEST_DIR = build/wifi/guest
WIFI_MKCPIO = build/tools/wifi/mkcpio
WIFI_GUEST_INPUTS = tests/system/wifi/mkguest.sh tests/system/wifi/packages.txt \
    tests/system/wifi/modules.txt $(wildcard tests/system/wifi/certs/*) \
    tests/system/wifi/root/init tests/system/wifi/root/sbin/wifiap \
    $(wildcard tests/system/wifi/root/etc/* tests/system/wifi/root/etc/wifiap/*)

$(WIFI_MKCPIO): tests/system/wifi/mkcpio.c
	@mkdir -p $(dir $@)
	$(HOST_LINK)

$(WIFI_GUEST_DIR)/initrd: $(WIFI_GUEST_INPUTS) $(WIFI_MKCPIO) $(SHA256TOOL) $(WIFI_HWSIMRELAY)
	MKCPIO=$(WIFI_MKCPIO) SHA256=$(SHA256TOOL) HWSIMRELAY=$(WIFI_HWSIMRELAY) \
	    sh tests/system/wifi/mkguest.sh $(WIFI_GUEST_DIR)

$(WIFI_GUEST_DIR)/vmlinuz: $(WIFI_GUEST_DIR)/initrd

.PHONY: wifi-guest
wifi-guest: $(WIFI_GUEST_DIR)/initrd $(WIFI_GUEST_DIR)/vmlinuz
