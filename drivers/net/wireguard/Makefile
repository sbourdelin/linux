ccflags-y := -O3
ccflags-y += -D'pr_fmt(fmt)=KBUILD_MODNAME ": " fmt'
wireguard-y := main.o noise.o device.o peer.o timers.o queueing.o send.o receive.o socket.o hashtables.o allowedips.o ratelimiter.o cookie.o netlink.o
obj-$(CONFIG_WIREGUARD) := wireguard.o
