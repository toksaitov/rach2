# hwsimrelay.mk: one x86-64 Linux binary, independent of the Rach2 ARCH.
WIFI_HWSIMRELAY = build/wifi/hwsimrelay
WIFI_RELAY_UAPI = build/wifi/uapi/libnl-3.12.0/include/linux-private
WIFI_RELAY_HEADERS = $(WIFI_RELAY_UAPI)/linux/netlink.h $(WIFI_RELAY_UAPI)/linux/genetlink.h

$(WIFI_RELAY_UAPI)/linux/netlink.h: vendor/libnl/libnl-3.12.0.tar.gz | $(SHA256TOOL)
	@test "$$($(SHA256TOOL) $< | cut -d ' ' -f 1)" = \
	    fc51ca7196f1a3f5fdf6ffd3864b50f4f9c02333be28be4eeca057e103c0dd18
	@mkdir -p build/wifi/uapi
	tar -xzf $< -C build/wifi/uapi libnl-3.12.0/include/linux-private

$(WIFI_RELAY_UAPI)/linux/genetlink.h: $(WIFI_RELAY_UAPI)/linux/netlink.h

# The wrapper always selects x86-64, even in a build of the i686 kernel.
# Re-enter the root make so its musl rule uses the same architecture.
ifneq ($(ARCH),x86_64)
build/x86_64/vendor-work/musl/.stamp:
	+$(MAKE) ARCH=x86_64 musl
endif

$(WIFI_HWSIMRELAY): tests/system/wifi/hwsimrelay.c tests/system/wifi/hwsimrelay.mk \
    rach2/sys/network/wireless/relay.h $(WIFI_RELAY_HEADERS) \
    build/x86_64/vendor-work/musl/.stamp vendor/musl/muslgcc mk/arch.sh
	@mkdir -p $(dir $@) build/tmp
	TMPDIR="$(CURDIR)/build/tmp" RACH2_ARCH=x86_64 vendor/musl/muslgcc \
	    -ansi -O2 -Wall -Wextra -Werror -I$(WIFI_RELAY_UAPI) $< -o $@
