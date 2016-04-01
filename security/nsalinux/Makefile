#
# Makefile for building the NSALinux module as part of the kernel tree.
#

obj-$(CONFIG_SECURITY_NSALINUX) := nsalinux.o

nsalinux-y := avc.o hooks.o nsalinuxfs.o netlink.o nlmsgtab.o netif.o \
	     netnode.o netport.o exports.o \
	     ss/ebitmap.o ss/hashtab.o ss/symtab.o ss/sidtab.o ss/avtab.o \
	     ss/policydb.o ss/services.o ss/conditional.o ss/mls.o ss/status.o

nsalinux-$(CONFIG_SECURITY_NETWORK_XFRM) += xfrm.o

nsalinux-$(CONFIG_NETLABEL) += netlabel.o

ccflags-y := -I$(srctree)/security/nsalinux -I$(srctree)/security/nsalinux/include

$(addprefix $(obj)/,$(nsalinux-y)): $(obj)/flask.h

quiet_cmd_flask = GEN     $(obj)/flask.h $(obj)/av_permissions.h
      cmd_flask = scripts/nsalinux/genheaders/genheaders $(obj)/flask.h $(obj)/av_permissions.h

targets += flask.h av_permissions.h
$(obj)/flask.h: $(src)/include/classmap.h FORCE
	$(call if_changed,flask)
