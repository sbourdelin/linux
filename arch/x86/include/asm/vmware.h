/*
 * Copyright (C) 2015, VMware, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE, GOOD TITLE or
 * NON INFRINGEMENT.  See the GNU General Public License for more
 * details.
 *
 * Based on code from vmware.c and vmmouse.c.
 * Author:
 *   Sinclair Yeh <syeh@vmware.com>
 */
#ifndef _ASM_X86_VMWARE_H
#define _ASM_X86_VMWARE_H


/**
 * Hypervisor-specific bi-directional communication channel.  Should never
 * execute on bare metal hardware.  The caller must make sure to check for
 * supported hypervisor before using these macros.
 *
 * Several of the parameters are both input and output and must be initialized.
 *
 * @in1: [IN] Message Len or Message Cmd (HB)
 * @in2: [IN] Message Len (HB) or Message Cmd
 * @port_num: [IN] port number + [channel id]
 * @magic: [IN] hypervisor magic value
 * @eax: [OUT] value of EAX register
 * @ebx: [OUT] e.g. status from an HB message status command
 * @ecx: [OUT] e.g. status from a non-HB message status command
 * @edx: [OUT] e.g. channel id
 * @si: [INOUT] set to 0 if not used
 * @di: [INOUT] set to 0 if not used
 * @bp: [INOUT] set to 0 if not used
 */
#define VMW_PORT(in1, in2, port_num, magic, eax, ebx, ecx, edx, si, di) \
({                                                                      \
	__asm__ __volatile__ ("inl %%dx" :                              \
		"=a"(eax),                                              \
		"=b"(ebx),                                              \
		"=c"(ecx),                                              \
		"=d"(edx),                                              \
		"=S"(si),                                               \
		"=D"(di) :                                              \
		"a"(magic),                                             \
		"b"(in1),                                               \
		"c"(in2),                                               \
		"d"(port_num),                                          \
		"S"(si),                                                \
		"D"(di) :                                               \
		"memory");                                              \
})


#define VMW_PORT_HB_OUT(in1, in2, port_num, magic,      \
			eax, ebx, ecx, edx, si, di, bp) \
({                                                      \
	__asm__ __volatile__ ("movq %13, %%rbp;"        \
		"cld; rep outsb; "                      \
		"movq %%rbp, %6" :                      \
		"=a"(eax),                              \
		"=b"(ebx),                              \
		"=c"(ecx),                              \
		"=d"(edx),                              \
		"=S"(si),                               \
		"=D"(di),                               \
		"=r"(bp) :                              \
		"a"(magic),                             \
		"b"(in1),                               \
		"c"(in2),                               \
		"d"(port_num),                          \
		"S"(si),                                \
		"D"(di),                                \
		"r"(bp) :                               \
		"memory", "cc");                        \
})


#define VMW_PORT_HB_IN(in1, in2, port_num, magic,            \
		       eax, ebx, ecx, edx, si, di, bp)       \
({                                                           \
	__asm__ __volatile__ ("push %%rbp; movq %13, %%rbp;" \
		"cld; rep insb; "                            \
		"movq %%rbp, %6;pop %%rbp" :                 \
		"=a"(eax),                                   \
		"=b"(ebx),                                   \
		"=c"(ecx),                                   \
		"=d"(edx),                                   \
		"=S"(si),                                    \
		"=D"(di),                                    \
		"=r"(bp) :                                   \
		"a"(magic),                                  \
		"b"(in1),                                    \
		"c"(in2),                                    \
		"d"(port_num),                               \
		"S"(si),                                     \
		"D"(di),                                     \
		"r"(bp) :                                    \
		"memory", "cc");                             \
})


#endif

