# SPDX-License-Identifier: GPL-2.0
#
# Makefile for native language support
#

nls_base-y := nls_core.o nls_default.o
obj-$(CONFIG_NLS)		+= nls_base.o

obj-$(CONFIG_NLS_CODEPAGE_437)	+= nls_cp437.o
obj-$(CONFIG_NLS_CODEPAGE_737)	+= nls_cp737.o
obj-$(CONFIG_NLS_CODEPAGE_775)	+= nls_cp775.o
obj-$(CONFIG_NLS_CODEPAGE_850)	+= nls_cp850.o
obj-$(CONFIG_NLS_CODEPAGE_852)	+= nls_cp852.o
obj-$(CONFIG_NLS_CODEPAGE_855)	+= nls_cp855.o
obj-$(CONFIG_NLS_CODEPAGE_857)	+= nls_cp857.o
obj-$(CONFIG_NLS_CODEPAGE_860)	+= nls_cp860.o
obj-$(CONFIG_NLS_CODEPAGE_861)	+= nls_cp861.o
obj-$(CONFIG_NLS_CODEPAGE_862)	+= nls_cp862.o
obj-$(CONFIG_NLS_CODEPAGE_863)	+= nls_cp863.o
obj-$(CONFIG_NLS_CODEPAGE_864)	+= nls_cp864.o
obj-$(CONFIG_NLS_CODEPAGE_865)	+= nls_cp865.o
obj-$(CONFIG_NLS_CODEPAGE_866)	+= nls_cp866.o
obj-$(CONFIG_NLS_CODEPAGE_869)	+= nls_cp869.o
obj-$(CONFIG_NLS_CODEPAGE_874)	+= nls_cp874.o
obj-$(CONFIG_NLS_CODEPAGE_932)	+= nls_cp932.o nls_euc-jp.o
obj-$(CONFIG_NLS_CODEPAGE_936)	+= nls_cp936.o
obj-$(CONFIG_NLS_CODEPAGE_949)	+= nls_cp949.o
obj-$(CONFIG_NLS_CODEPAGE_950)	+= nls_cp950.o
obj-$(CONFIG_NLS_CODEPAGE_1250) += nls_cp1250.o
obj-$(CONFIG_NLS_CODEPAGE_1251)	+= nls_cp1251.o
obj-$(CONFIG_NLS_ASCII)		+= nls_ascii.o
obj-$(CONFIG_NLS_ISO8859_1)	+= nls_iso8859-1.o
obj-$(CONFIG_NLS_ISO8859_2)	+= nls_iso8859-2.o
obj-$(CONFIG_NLS_ISO8859_3)	+= nls_iso8859-3.o
obj-$(CONFIG_NLS_ISO8859_4)	+= nls_iso8859-4.o
obj-$(CONFIG_NLS_ISO8859_5)	+= nls_iso8859-5.o
obj-$(CONFIG_NLS_ISO8859_6)	+= nls_iso8859-6.o
obj-$(CONFIG_NLS_ISO8859_7)	+= nls_iso8859-7.o
obj-$(CONFIG_NLS_ISO8859_8)	+= nls_cp1255.o
obj-$(CONFIG_NLS_ISO8859_9)	+= nls_iso8859-9.o
obj-$(CONFIG_NLS_ISO8859_13)	+= nls_iso8859-13.o
obj-$(CONFIG_NLS_ISO8859_14)	+= nls_iso8859-14.o
obj-$(CONFIG_NLS_ISO8859_15)	+= nls_iso8859-15.o
obj-$(CONFIG_NLS_KOI8_R)	+= nls_koi8-r.o
obj-$(CONFIG_NLS_KOI8_U)	+= nls_koi8-u.o nls_koi8-ru.o
obj-$(CONFIG_NLS_UTF8)		+= nls_utf8.o
obj-$(CONFIG_NLS_MAC_CELTIC)    += mac-celtic.o
obj-$(CONFIG_NLS_MAC_CENTEURO)  += mac-centeuro.o
obj-$(CONFIG_NLS_MAC_CROATIAN)  += mac-croatian.o
obj-$(CONFIG_NLS_MAC_CYRILLIC)  += mac-cyrillic.o
obj-$(CONFIG_NLS_MAC_GAELIC)    += mac-gaelic.o
obj-$(CONFIG_NLS_MAC_GREEK)     += mac-greek.o
obj-$(CONFIG_NLS_MAC_ICELAND)   += mac-iceland.o
obj-$(CONFIG_NLS_MAC_INUIT)     += mac-inuit.o
obj-$(CONFIG_NLS_MAC_ROMANIAN)  += mac-romanian.o
obj-$(CONFIG_NLS_MAC_ROMAN)     += mac-roman.o
obj-$(CONFIG_NLS_MAC_TURKISH)   += mac-turkish.o

nls_utf8n-y += nls_utf8n-norm.o nls_utf8n-core.o
obj-$(CONFIG_NLS_UTF8_NORMALIZATION) += nls_utf8n.o

$(obj)/nls_utf8n-norm.o: $(obj)/utf8data.h
$(obj)/utf8data.h: $(srctree)/$(src)/ucd/*.txt $(objtree)/scripts/mkutf8data FORCE
	$(call cmd,mkutf8data)
quiet_cmd_mkutf8data = MKUTF8DATA $@
      cmd_mkutf8data = $(objtree)/scripts/mkutf8data \
		-a $(srctree)/$(src)/ucd/DerivedAge-10.0.0.txt \
		-c $(srctree)/$(src)/ucd/DerivedCombiningClass-10.0.0.txt \
		-p $(srctree)/$(src)/ucd/DerivedCoreProperties-10.0.0.txt \
		-d $(srctree)/$(src)/ucd/UnicodeData-10.0.0.txt \
		-f $(srctree)/$(src)/ucd/CaseFolding-10.0.0.txt \
		-n $(srctree)/$(src)/ucd/NormalizationCorrections-10.0.0.txt \
		-t $(srctree)/$(src)/ucd/NormalizationTest-10.0.0.txt \
		-o $@
