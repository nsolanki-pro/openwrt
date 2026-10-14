KERNEL_LOADADDR := 0x80100000

define Device/Default
  PROFILES := Default
  BLOCKSIZE := 64k
  FILESYSTEMS := squashfs
  DEVICE_DTS_DIR := ../dts
  KERNEL := kernel-bin | append-dtb | lzma | uImage lzma
  KERNEL_INITRAMFS := kernel-bin | append-dtb | lzma | uImage lzma
  IMAGES := sysupgrade.bin
  IMAGE/sysupgrade.bin = append-kernel | pad-to $$$$(BLOCKSIZE) | \
	append-rootfs | pad-rootfs | append-metadata
endef

define Device/siflower_sf19a2890-evb
  DEVICE_VENDOR := Siflower
  DEVICE_MODEL := SF19A2890 EVB
  BOARD_NAME := siflower,sf19a2890-evb
  DEVICE_DTS := sf19a2890_evb
  DEVICE_PACKAGES := kmod-switch-rtl8367b swconfig
endef
TARGET_DEVICES += siflower_sf19a2890-evb

define Device/tenda_ac10-v5
  DEVICE_VENDOR := Tenda
  DEVICE_MODEL := AC10
  DEVICE_VARIANT := V5
  DEVICE_DTS := sf19a2890_tenda_ac10-v5
  # the flash runs with 4K erase blocks, so rootfs_data starts on 4K
  BLOCKSIZE := 4k
  IMAGE_SIZE := 7488k
  DEVICE_PACKAGES := kmod-dsa-mt7530 kmod-sfwifi sfwifi-firmware \
	zram-swap
  IMAGES += factory.bin
  IMAGE/factory.bin = append-kernel | pad-to $$$$(BLOCKSIZE) | \
	append-rootfs | pad-rootfs | check-size
endef
TARGET_DEVICES += tenda_ac10-v5
