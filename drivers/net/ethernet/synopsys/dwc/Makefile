obj-$(CONFIG_DWC_XLGMAC_PCI) += snps-dwc-xlgmac.o

snps-dwc-xlgmac-objs := dwc-xlgmac-pci.o dwc-xlgmac.o

snps-dwc-xlgmac-$(CONFIG_DWC_ETH) += dwc-eth-net.o dwc-eth-desc.o \
				     dwc-eth-hw.o dwc-eth-mdio.o \
				     dwc-eth-ethtool.o dwc-eth-ptp.o
snps-dwc-xlgmac-$(CONFIG_DWC_ETH_DCB) += dwc-eth-dcb.o
snps-dwc-xlgmac-$(CONFIG_DEBUG_FS) += dwc-eth-debugfs.o
