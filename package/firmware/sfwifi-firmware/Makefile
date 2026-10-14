# SPDX-License-Identifier: GPL-2.0-only
#
# Firmware and runtime configuration for the Siflower SF19A2890 WiFi
# driver (vendor sf_smac, packaged as kmod-sfwifi).
#
# The LMAC firmware (sf1688_lb_fmac.bin, sf1688_hb_fmac.bin) and the AGC
# tables (agcram.bin, agcram_24g.bin) are from the Tenda AC10 V5's
# original firmware, which the driver is built for. They carry no
# licence. The other files are from Siflower's MIT-licensed 1806 SDK,
# https://github.com/BPI-SINOVOIP/BPI-WiFi5-Siflower (commit
# d5c6ffd0a992), openwrt-18.06/package/kernel/sf_smac/config/ (the
# a28fullmask variant). sf_rf_expa_config.ini holds the Tenda AC10 V5's
# external PA control values, from its original firmware.
#
# The driver requests the .bin blobs as "siflower/<name>" and the .ini
# configs relative to /lib/firmware, so the two install locations
# below must not be merged.

include $(TOPDIR)/rules.mk

PKG_NAME:=sfwifi-firmware
# Firmware vintage: Siflower 1806 SDK (openwrt-18.06 based) and the
# Tenda AC10 V5 original firmware
PKG_VERSION:=2018.06
PKG_RELEASE:=1

PKG_FLAGS:=nonshared

include $(INCLUDE_DIR)/package.mk

define Package/sfwifi-firmware
  SECTION:=firmware
  CATEGORY:=Firmware
  TITLE:=Siflower SF19A2890 WiFi firmware
  DEPENDS:=@TARGET_siflower_sf19a2890
endef

define Package/sfwifi-firmware/description
  LMAC (SFDSP) firmware, AGC/LDPC RAM images, RF firmware and runtime
  configuration files for the Siflower SF19A2890 FullMAC WiFi driver
  (sfwifi / kmod-sfwifi).
endef

define Build/Compile
endef

define Package/sfwifi-firmware/install
	$(INSTALL_DIR) $(1)/lib/firmware/siflower
	$(CP) ./files/*.ini $(1)/lib/firmware/
	$(CP) ./files/siflower/*.bin $(1)/lib/firmware/siflower/
	# sf_smac requests the blobs by bare name: link them at the top level
	for f in ./files/siflower/*.bin; do \
		$(LN) siflower/$$$${f##*/} $(1)/lib/firmware/$$$${f##*/}; \
	done
endef

$(eval $(call BuildPackage,sfwifi-firmware))
