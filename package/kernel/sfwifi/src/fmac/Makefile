DRV_PATH ?= $(src)/..
umac_dir=$(DRV_PATH)/fmac/bb_src/umac
lmac_dir=$(DRV_PATH)/fmac/bb_src/lmac
export umac_dir
export lmac_dir

obj-$(CONFIG_SF16A18_WIFI_FULL_MAC) += sf16a18_fmac.o


root_src_ref_dir = ./bb_src/umac
export root_src_ref_dir

# SF19A2890 (A28 full mask) and LMAC firmware build options, as the vendor
# OpenWrt package passes them for this SoC. Board differences (external
# PA, dual-antenna calibration) are read from the DT and the factory
# partition at runtime. Set before fullmac.mk is included: it and the
# files it includes branch on these.
CONFIG_A28 := y
CONFIG_SFFMAC_ENABLE_TOKEN := y
CONFIG_SIWIFI_ACS_INTERNAL := y
CONFIG_SF16A18_WIFI_ATE_TOOLS := y
# send temperature compensation steps from the RF driver to the LMAC
CONFIG_SF16A18_COOLING_TEMP := y
# The firmware runs with MM_FEAT_LITE_MEM_BIT set, and
# siwifi_set_hw_feature() refuses a UMAC built without it. Define only
# the macro: the Makefile variable CONFIG_WIFI_LITE_MEMORY would also
# drop testmode, cmdstr and error dump.
ccflags-y += -DCONFIG_WIFI_LITE_MEMORY
ccflags-y += -DCONFIG_SF16A18_LMAC_USE_M_SFDSP -DCONFIG_SFA28_FULLMASK
# As in the vendor package for non-CSD boards: with an external PA on
# the 5 GHz band, use default_hb_txpower_table_expa_low_power.ini. The
# full-power table drives the PA too hard for high MCS and TX rate
# control settles at VHT MCS3.
ccflags-y += -DCONFIG_UPDATE_GAIN_TABLE
# The source tests CONFIG_SF16A18_WIFI_ATE_TOOLS with #ifdef too; the
# vendor package passes it as a define.
ccflags-y += -DCONFIG_SF16A18_WIFI_ATE_TOOLS
# siwifi_main.c only fills station info with the NL80211_STA_INFO_* bits
# under CPTCFG_WIRELESS (a backports convention). Its other uses are
# behind pre-4.x kernel version checks.
ccflags-y += -DCPTCFG_WIRELESS=1

# The vendor source has many global functions without prototypes.
ccflags-y += -Wno-missing-prototypes -Wno-missing-declarations

# Build time shown in debugfs; reproducible when SOURCE_DATE_EPOCH is
# set.
SFWIFI_COMPILE_TIME := $(or $(SOURCE_DATE_EPOCH),$(shell date +%s))
ccflags-y += -DCONFIG_COMPILE_TIME=$(SFWIFI_COMPILE_TIME)

include $(umac_dir)/fullmac/fullmac.mk

ccflags-y += -I$(DRV_PATH)/rf/

sf16a18_fmac-y += $(SF16A18_FULLMAC_SRC_OBJS)
