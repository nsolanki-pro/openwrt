# The vendor source has many global functions without prototypes.
ccflags-y += -Wno-missing-prototypes -Wno-missing-declarations

# SF_ARCH=a28fullmask: platform.c only builds the task load/start/stop
# functions with this.
ccflags-y += -DCONFIG_SF16A18_LMAC_USE_M_SFDSP

obj-$(CONFIG_SF16A18_WIFI_MAC_HOST_OFFLOAD)		+= startcore.o
startcore-y				 = start_core.o platform.o
