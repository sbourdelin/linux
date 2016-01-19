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
 * The last two parameters are both input and output and must be initialized.
 *
 * @cmd: [IN] Message Cmd
 * @in_ebx: [IN] Message Len, through EBX
 * @in_si: [IN] Input argument through SI, set to 0 if not used
 * @in_di: [IN] Input argument through DI, set ot 0 if not used
 * @port_num: [IN] port number + [channel id]
 * @magic: [IN] hypervisor magic value
 * @eax: [OUT] value of EAX register
 * @ebx: [OUT] e.g. status from an HB message status command
 * @ecx: [OUT] e.g. status from a non-HB message status command
 * @edx: [OUT] e.g. channel id
 * @si:  [OUT]
 * @di:  [OUT]
 */
#define VMW_PORT(cmd, in_ebx, in_si, in_di,	\
		 port_num, magic,		\
		 eax, ebx, ecx, edx, si, di)	\
({						\
	asm volatile ("inl %%dx, %%eax;" :	\
		"=a"(eax),			\
		"=b"(ebx),			\
		"=c"(ecx),			\
		"=d"(edx),			\
		"=S"(si),			\
		"=D"(di) :			\
		"a"(magic),			\
		"b"(in_ebx),			\
		"c"(cmd),			\
		"d"(port_num),			\
		"S"(in_si),			\
		"D"(in_di) :			\
		"memory");			\
})


/**
 * Hypervisor-specific bi-directional communication channel.  Should never
 * execute on bare metal hardware.  The caller must make sure to check for
 * supported hypervisor before using these macros.
 *
 * The last 3 parameters are both input and output and must be initialized.
 *
 * @cmd: [IN] Message Cmd
 * @in_ecx: [IN] Message Len, through ECX
 * @in_si: [IN] Input argument through SI, set to 0 if not used
 * @in_di: [IN] Input argument through DI, set to 0 if not used
 * @port_num: [IN] port number + [channel id]
 * @magic: [IN] hypervisor magic value
 * @eax: [OUT] value of EAX register
 * @ebx: [OUT] e.g. status from an HB message status command
 * @ecx: [OUT] e.g. status from a non-HB message status command
 * @edx: [OUT] e.g. channel id
 * @si:  [OUT]
 * @di:  [OUT]
 * @bp:  [INOUT] set to 0 if not used
 */
#define VMW_PORT_HB_OUT(cmd, in_ecx, in_si, in_di,	\
			port_num, magic,		\
			eax, ebx, ecx, edx, si, di, bp)	\
({							\
	asm volatile ("push %%rbp;"			\
		"xchgq %6, %%rbp;"			\
		"rep outsb;"				\
		"xchgq %%rbp, %6;"			\
		"pop %%rbp;" :				\
		"=a"(eax),				\
		"=b"(ebx),				\
		"=c"(ecx),				\
		"=d"(edx),				\
		"=S"(si),				\
		"=D"(di),				\
		"+r"(bp) :				\
		"a"(magic),				\
		"b"(cmd),				\
		"c"(in_ecx),				\
		"d"(port_num),				\
		"S"(in_si),				\
		"D"(in_di) :				\
		"memory", "cc");			\
})


#define VMW_PORT_HB_IN(cmd, in_ecx, in_si, in_di,	\
		       port_num, magic,			\
		       eax, ebx, ecx, edx, si, di, bp)	\
({							\
	asm volatile ("push %%rbp;"			\
		"xchgq %6, %%rbp;"			\
		"rep insb;"				\
		"xchgq %%rbp, %6;"			\
		"pop %%rbp" :				\
		"=a"(eax),				\
		"=b"(ebx),				\
		"=c"(ecx),				\
		"=d"(edx),				\
		"=S"(si),				\
		"=D"(di),				\
		"+r"(bp) :				\
		"a"(magic),				\
		"b"(cmd),				\
		"c"(in_ecx),				\
		"d"(port_num),				\
		"S"(in_si),				\
		"D"(in_di) :				\
		"memory", "cc");			\
})


#endif

