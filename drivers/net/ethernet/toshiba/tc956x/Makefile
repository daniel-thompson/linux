ccflags-y += -DTC956X
ccflags-y += -DCONFIG_TC956X_PLATFORM_SUPPORT

ifdef CONFIG_TC956X_DEBUG
ccflags-y += -DDEBUG
endif

ifdef CONFIG_TC965X_SPIOV_VF
ccflags-y += -DTC956X_SRIOV_VF

obj-$(CONFIG_TC956X_NET) := tc956x_vf_pcie_eth.o
tc956x_vf_pcie_eth-y := tc956xmac_main.o tc956xmac_ethtool.o \
	      mmc_core.o tc956xmac_hwtstamp.o tc956xmac_ptp.o \
	      hwif.o  tc956xmac_tc.o dwxgmac2_core.o \
	      dwxgmac2_descs.o dwxgmac2_dma.o tc956x_pci.o \
	      tc956x_vf_mbx_wrapper.o tc956x_msigen.o \
	      tc956x_vf_rsc_mng.o tc956x_vf_mbx.o \
	      tc956x_pcie_logstat.o tc956x_qcom.o

else

ccflags-y +=-DTC956X_SRIOV_PF

obj-$(CONFIG_TC956X_NET) := tc956x_pcie_eth.o
tc956x_pcie_eth-y := tc956xmac_main.o tc956xmac_ethtool.o tc956xmac_mdio.o \
	      mmc_core.o tc956xmac_hwtstamp.o tc956xmac_ptp.o tc956x_xpcs.o tc956x_pma.o \
	      hwif.o  tc956xmac_tc.o dwxgmac2_core.o \
	      dwxgmac2_descs.o dwxgmac2_dma.o tc956x_pci.o \
	      tc956x_msigen.o \
	      tc956x_pcie_logstat.o tc956x_pf_rsc_mng.o \
	      tc956x_qcom.o

ifeq ($(pf), 1)
	tc956x_pcie_eth-y += tc956x_pf_mbx_wrapper.o tc956x_pf_rsc_mng.o tc956x_pf_mbx.o
else ifeq ($(cpe), 1)
ccflags-y += -DTC956X_CPE_CONFIG
else ifeq ($(port_bridge), 1)
ccflags-y += -DTC956X_ENABLE_MAC2MAC_BRIDGE
else
ccflags-y += -DTC956X_AUTOMOTIVE_CONFIG
endif

ifeq ($(DMA_OFFLOAD), 1)
	tc956x_pcie_eth-y += tc956x_ipa_intf.o
endif

ifdef CONFIG_TC956X_MAC_SELFTESTS
	tc956x_pcie_eth-y += tc956xmac_selftests.o
endif
#TC956X_Host_Driver-industrial_limited_tested_20241022_V_04-00-01-QPSSW-210.patch
ccflags-y += -Wmissing-prototypes

endif
