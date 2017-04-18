# SYNOPSYS Platform Support

obj-$(CONFIG_SND_DESIGNWARE_I2S) += designware_i2s.o

designware_i2s-objs := designware_i2s_main.o

ifdef CONFIG_SND_DESIGNWARE_PCM
designware_i2s-objs += designware_i2s_pcm.o
endif
