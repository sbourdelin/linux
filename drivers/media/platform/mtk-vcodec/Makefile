obj-$(CONFIG_VIDEO_MEDIATEK_VCODEC) += mtk_vcodec_intr.o \
				       mtk_vcodec_util.o \
				       mtk_vcodec_enc_drv.o \
				       mtk_vcodec_enc.o \
				       mtk_vcodec_enc_pm.o

obj-$(CONFIG_VIDEO_MEDIATEK_VCODEC) += common/ vp8_enc/

ccflags-y += -I$(srctree)/drivers/media/platform/mtk-vcodec/include \
	     -I$(srctree)/drivers/media/platform/mtk-vcodec \
	     -I$(srctree)/drivers/media/platform/mtk-vpu

