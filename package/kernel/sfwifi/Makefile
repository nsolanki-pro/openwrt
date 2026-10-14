# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=sfwifi
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define KernelPackage/sfwifi
  SUBMENU:=Wireless Drivers
  TITLE:=Siflower SF19A2890 WiFi driver
  DEPENDS:=@TARGET_siflower_sf19a2890 +kmod-cfg80211 +sfwifi-firmware \
	+@DRIVER_11AC_SUPPORT
  FILES:= \
	$(PKG_BUILD_DIR)/rf/sf16a18_rf.ko \
	$(PKG_BUILD_DIR)/startcore/startcore.ko \
	$(PKG_BUILD_DIR)/fmac/sf16a18_fmac.ko
  # No AUTOLOAD: the chain must load once per boot, in order, with
  # memory compacted before the fmac's large DMA pools (a reload wedges
  # the LMAC). /etc/init.d/sfwifi in the target's base-files does that.
endef

define KernelPackage/sfwifi/description
  Siflower SF19A2890 dual-band FullMAC WiFi driver (vendor sf_smac:
  sf16a18_rf, startcore, sf16a18_fmac). Depends on sfwifi-firmware for
  the LMAC firmware and RF configuration under /lib/firmware.
endef

NOSTDINC_FLAGS := \
	$(KERNEL_NOSTDINC_FLAGS) \
	-I$(PKG_BUILD_DIR) \
	-I$(STAGING_DIR)/usr/include/mac80211-backport/uapi \
	-I$(STAGING_DIR)/usr/include/mac80211-backport \
	-I$(STAGING_DIR)/usr/include/mac80211/uapi \
	-I$(STAGING_DIR)/usr/include/mac80211 \
	-include backport/autoconf.h \
	-include backport/backport.h

SFWIFI_MAKE_OPTS := \
	CONFIG_SF16A18_WIFI_RF=m \
	CONFIG_SF16A18_WIFI_MAC_HOST_OFFLOAD=m \
	CONFIG_SF16A18_WIFI_FULL_MAC=m

define Build/Compile
	+$(KERNEL_MAKE) $(PKG_JOBS) M="$(PKG_BUILD_DIR)" \
		NOSTDINC_FLAGS="$(NOSTDINC_FLAGS)" \
		$(SFWIFI_MAKE_OPTS) modules
endef

$(eval $(call KernelPackage,sfwifi))
