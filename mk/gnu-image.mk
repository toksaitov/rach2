# The GNU system's root (vendor/gnu/Readme.md): the os-real system of
# krffs/manifests/os-real-gnu.manifest with the GNU layer installed over it
# by vendor/gnu/install_root.sh, as an ext2 volume the kernel boots with
# rach2.root=ext2. It is built where the GNU layer is, on a Linux host, and
# is not part of `all`. 4096 MiB: the system takes 1.2 GiB of it, and the
# rest is room for what the machine compiles and fetches. The prerequisites
# are those of os-real's ext2 image, less Dropbear, with the overlay, the
# list with the CA bundle's digest and the five stamps of the GNU layer.
OS_REAL_GNU_EXT2 = $(IMAGES_DIR)/os-real-gnu.ext2
GNU_IMAGE_STAMPS = $(GNU_PORTS_STAMP) $(GNU_NATIVE_STAMP) $(GNU_GDB_STAMP) \
                   $(GNU_JS_STAMP) $(GNU_CHROMIUM_STAMP)

.PHONY: os-real-gnu-ext2
os-real-gnu-ext2: $(OS_REAL_GNU_EXT2)

$(OS_REAL_GNU_EXT2): $(DEMO_CREDS) krffs/manifests/os-real-gnu.manifest $(MANIFEST_DEPS_os_real_gnu) \
                     tools/image/mkext2.sh vendor/gnu/install_root.sh vendor/gnu/fetch.sh \
                     vendor/gnu/sources-extra.txt $(GNU_IMAGE_STAMPS) $(IMAGE_TOOLS) $(MAN_ALL) \
                     | preflight krffs rach2 utils check-busybox check-tcc check-pdpmake check-setid check-e2fsck ports/doom
	RACH2_ARCH=$(ARCH) sh tools/image/mkext2.sh --overlay vendor/gnu/install_root.sh \
	    krffs/manifests/os-real-gnu.manifest $@ 4096
