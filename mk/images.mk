# mk/images.mk: the products that are not RAM images.
#
# The root Makefile includes this file after it has named the kernels and the
# RAM images, and before the test include: every rule here makes something the
# machine or a stick carries, so it is the build's and not a suite's, and a
# suite that needs one of these names it as a prerequisite. tools/verify/check.sh
# refuses a rule under tests/ that makes a product.
#
# The three tools the stick scripts run: the MBR writer, which krffs/mkdisk.sh
# builds on demand as well; the stick image's judge, linked with the kernel's
# own MBR reader so the rules it applies are the kernel's; and the exFAT
# builder and reader, which the exfat suite uses too and which the sticks'
# volume is written with.
MKMBR           = build/tools/image/mkmbr
STICKCHECK      = build/tools/image/stickcheck
MKEXFATFIXTURE  = build/tests/host/mkexfatfixture
$(MKMBR): tools/image/mkmbr.c
	@mkdir -p $(dir $@)
	$(HOST_LINK)
$(STICKCHECK): tools/image/stickcheck.c rach2/sys/block/mbr.c rach2/sys/block/mbr.h
	@mkdir -p $(dir $@)
	$(HOST_LINK) -I rach2/sys -I rach2/sys/lib
$(MKEXFATFIXTURE): tests/host/mkexfatfixture.c tests/host/exfat_upcase.inc
	@mkdir -p $(dir $@)
	$(HOST_LINK)

# The bootable ISOs of the metal and recovery images (tools/image/mkiso.sh):
# each embeds its kernel and RAM image as GRUB modules under the pinned GRUB
# artifacts. The recovery ISO takes the recovery image's name, with the key
# tag when this tree has a key.
build/images/rach2-metal.iso: $(METAL_KERNEL) $(METAL_IMAGE) tools/image/mkiso.sh
	sh tools/image/mkiso.sh rach2-metal
$(RECOVERY_ISO): $(RECOVERY_KERNEL) $(RECOVERY_IMAGE) tools/image/mkiso.sh tools/image/keyname.sh
	@sh tools/image/keyname.sh $@
	sh tools/image/mkiso.sh $(RECOVERY_NAME)

# The stick image (tools/image/mkstick.sh): the metal hybrid ISO followed by
# an exFAT volume, judged whole by stickcheck and walked by mkexfatfixture.
STICK_IMAGE = $(IMAGES_DIR)/rach2-stick.img
rach2-stick-image: $(STICK_IMAGE)
$(STICK_IMAGE): build/images/rach2-metal.iso tools/image/mkstick.sh $(MKEXFATFIXTURE) $(MKMBR) $(STICKCHECK)
	sh tools/image/mkstick.sh build/images/rach2-metal.iso $@ $(MKEXFATFIXTURE) $(MKMBR) $(STICKCHECK)

# The install stick (tools/image/mkinstallstick.sh): the recovery ISO followed
# by an exFAT volume carrying the sealed disk image and the manifest naming its
# length and hash. It is what a person boots to install this system. The stick
# takes the recovery ISO's name with it: a stick whose recovery answers this
# tree's key is not the stick anybody else runs.
INSTALL_STICK = $(IMAGES_DIR)/rach2-install-stick$(RECOVERY_KEY_TAG).img
rach2-install-stick: $(INSTALL_STICK)
$(INSTALL_STICK): $(RECOVERY_ISO) $(SEALED_DISK) tools/image/mkinstallstick.sh \
                  $(MKEXFATFIXTURE) $(MKMBR) $(STICKCHECK)
	@sh tools/image/keyname.sh $@
	sh tools/image/mkinstallstick.sh $(RECOVERY_ISO) $(SEALED_DISK) $@ \
	    $(MKEXFATFIXTURE) $(MKMBR) $(STICKCHECK)
.PHONY: rach2-stick-image rach2-install-stick
