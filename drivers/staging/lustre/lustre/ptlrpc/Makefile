# SPDX-License-Identifier: GPL-2.0
subdir-ccflags-y += -I$(srctree)/drivers/staging/lustre/include
subdir-ccflags-y += -I$(srctree)/drivers/staging/lustre/lustre/include

obj-$(CONFIG_LUSTRE_FS) += ptlrpc.o

ldlm_objs := l_lock.o ldlm_lock.o
ldlm_objs += ldlm_resource.o ldlm_lib.o
ldlm_objs += ldlm_plain.o ldlm_extent.o
ldlm_objs += ldlm_request.o ldlm_lockd.o
ldlm_objs += ldlm_flock.o ldlm_inodebits.o
ldlm_objs += ldlm_pool.o

ptlrpc_objs := client.o recover.o connection.o niobuf.o pack_generic.o
ptlrpc_objs += events.o ptlrpc_module.o service.o pinger.o
ptlrpc_objs += llog_net.o llog_client.o import.o ptlrpcd.o
ptlrpc_objs += pers.o lproc_ptlrpc.o wiretest.o layout.o
ptlrpc_objs += sec.o sec_bulk.o sec_gc.o sec_config.o
ptlrpc_objs += sec_null.o sec_plain.o nrs.o nrs_fifo.o

ptlrpc-y := $(ldlm_objs) $(ptlrpc_objs) sec_lproc.o
ptlrpc-$(CONFIG_LUSTRE_TRANSLATE_ERRNOS) += errno.o
