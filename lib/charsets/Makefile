charsets-y += core.o

obj-$(CONFIG_CHARSETS) += charsets.o

obj-$(CONFIG_CHARSETS) += ascii.o

utf8-y += utf8_core.o utf8norm.o
obj-$(CONFIG_UTF8_NORMALIZATION) +=  utf8.o

$(obj)/utf8norm.o: $(obj)/utf8data.h
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

