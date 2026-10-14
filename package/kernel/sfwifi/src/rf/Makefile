top_dir = $(src)

# SoC build options, as the vendor OpenWrt package passes them for the
# SF19A2890 (SF_ARCH=a28fullmask, no CSD). Dual-antenna calibration and
# the external PA bands are board properties, read from the DT at runtime
# (dual-antenna-calibrate, siflower,lb-ext-pa, siflower,hb-ext-pa).
CONFIG_DUAL_ANTENNA_CALIBRATE := y
CONFIG_UPDATE_GAIN_TABLE := y
# RF gain compensation against temperature, as in the vendor build.
CONFIG_SF16A18_COOLING_TEMP := y

ifeq ($(CONFIG_DEBUG_FS), y)
CONFIG_SF16A18_RF_SYSFS_DEBUG=y
CONFIG_SF16A18_RF_RAW_CMD_DEBUG=y
else
CONFIG_SF16A18_RF_SYSFS_DEBUG=n
CONFIG_SF16A18_RF_RAW_CMD_DEBUG=n
endif

#Header dir list
ccflags-y += -I$(top_dir)

# The vendor source has many global functions without prototypes.
ccflags-y += -Wno-missing-prototypes -Wno-missing-declarations

#Define macro
#ccflags-y += -Wno-unused-function
#ccflags-y += -DBOOT_FINISH_WITH_IRQ
#ccflags-y += -DENHANCE_LOWRATE_POWER
ccflags-y += -DENABLE_HK
#ccflags-y += -DUNSTANDARD_CH_ENABLE
ccflags-$(CONFIG_UPDATE_GAIN_TABLE) += -DUPDATE_GAIN_TABLE
#ccflags-y += -DFORCE_UPDATE_GAIN_TABLE

ccflags-y += -DENABLE_DIGGAIN_TABLE

# ate_aetset (used by the fmac's ioctl code) is built only with this
# define.
ccflags-y += -DCONFIG_SF16A18_WIFI_ATE_TOOLS

ccflags-$(CONFIG_SF16A18_COOLING_TEMP) += -DCOOLING_TEMP
ccflags-$(CONFIG_DUAL_ANTENNA_CALIBRATE) += -DCFG_DUAL_ANTENNA_CALIBRATE
ccflags-$(CONFIG_DUAL_ANTENNA_CALIBRATE_CSD) += -DCFG_DUAL_ANTENNA_CALIBRATE_CSD
ccflags-$(CONFIG_SIWIFI_TEMPERATURE_CONTROL) += -DCONFIG_SIWIFI_TEMPERATURE_CONTROL

#source files list
sf16a18_rf-$(CONFIG_SF16A18_WIFI_IQ_TESTENGINE_ENABLE) += iq_te_core.o
sf16a18_rf-y += rf_entry.o
sf16a18_rf-y += rf_access.o
sf16a18_rf-$(CONFIG_SF16A18_COOLING_TEMP) += sfax8_thermal.o
sf16a18_rf-$(CONFIG_SF16A18_RF_SYSFS_DEBUG) += rf_sysfs.o
ccflags-$(CONFIG_SF16A18_RF_SYSFS_DEBUG) += -DCONFIG_SF16A18_RF_SYSFS_DEBUG

obj-$(CONFIG_SF16A18_WIFI_RF) += sf16a18_rf.o
