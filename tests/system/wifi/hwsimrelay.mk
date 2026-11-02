# hwsimrelay.mk: one x86-64 Linux binary, independent of the Rach2 ARCH.
WIFI_HWSIMRELAY = build/wifi/hwsimrelay
WIFI_RELAY_UAPI = build/wifi/uapi/libnl-3.12.0/include/linux-private

# Re-enter the root make so its musl rule uses x86-64's paths and compiler.
ifneq ($(ARCH),x86_64)
build/x86_64/vendor-work/musl/.stamp: vendor/musl/build_libc.sh mk/arch.sh $(LIBC_TARBALL) $(LIBC_PATCHES)
	+$(MAKE) ARCH=x86_64 musl
_ := $(shell [ ! -f build/x86_64/vendor-work/musl/.stamp ] || \
    [ -s build/x86_64/vendor-work/musl/install/lib/libc.a ] || \
    rm -f build/x86_64/vendor-work/musl/.stamp)
endif

$(WIFI_HWSIMRELAY): tests/system/wifi/hwsimrelay.c tests/system/wifi/hwsimrelay.mk \
    rach2/sys/network/wireless/relay.h vendor/libnl/libnl-3.12.0.tar.gz \
    build/x86_64/vendor-work/musl/.stamp vendor/musl/muslgcc mk/arch.sh | $(SHA256TOOL)
	@test "$$($(SHA256TOOL) vendor/libnl/libnl-3.12.0.tar.gz | cut -d ' ' -f 1)" = \
	    fc51ca7196f1a3f5fdf6ffd3864b50f4f9c02333be28be4eeca057e103c0dd18
	@mkdir -p $(dir $@) build/wifi/uapi
	tar -xzf vendor/libnl/libnl-3.12.0.tar.gz -C build/wifi/uapi libnl-3.12.0/include/linux-private
	RACH2_ARCH=x86_64 vendor/musl/muslgcc \
	    -ansi -O2 -Wall -Wextra -Werror -I$(WIFI_RELAY_UAPI) $< -o $@
