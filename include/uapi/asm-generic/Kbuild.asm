#
# Headers that are optional in usr/include/asm/
#
opt-header += kvm.h
opt-header += kvm_para.h
opt-header += a.out.h

#
# Headers that are mandatory in usr/include/asm/
#
mandatory-y += auxvec.h
mandatory-y += bitsperlong.h
mandatory-y += byteorder.h
mandatory-y += errno.h
mandatory-y += fcntl.h
mandatory-y += ioctl.h
mandatory-y += ioctls.h
mandatory-y += ipcbuf.h
mandatory-y += mman.h
mandatory-y += msgbuf.h
mandatory-y += param.h
mandatory-y += poll.h
mandatory-y += posix_types.h
mandatory-y += ptrace.h
mandatory-y += resource.h
mandatory-y += sembuf.h
mandatory-y += setup.h
mandatory-y += shmbuf.h
mandatory-y += sigcontext.h
mandatory-y += siginfo.h
mandatory-y += signal.h
mandatory-y += socket.h
mandatory-y += sockios.h
mandatory-y += stat.h
mandatory-y += statfs.h
mandatory-y += swab.h
mandatory-y += termbits.h
mandatory-y += termios.h
mandatory-y += types.h
mandatory-y += unistd.h

mandatory-y += $(foreach hdr,$(opt-header), \
	      $(if \
		$(wildcard \
			$(srctree)/arch/$(SRCARCH)/include/uapi/asm/$(hdr) \
			$(srctree)/arch/$(SRCARCH)/include/asm/$(hdr) \
		), \
		$(hdr) \
		))
