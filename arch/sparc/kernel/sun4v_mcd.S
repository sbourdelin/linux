/* sun4v_mcd.S: Sun4v memory corruption detected precise exception handler
 *
 * Copyright (C) 2015 Bob Picco <bob.picco@oracle.com>
 * Copyright (C) 2015 Khalid Aziz <khalid.aziz@oracle.com>
 */
	.text
	.align 32

sun4v_mcd_detect_precise:
	ba,pt	%xcc, etrap
	rd	%pc, %g7
	or	%l4, %g0, %o1
	or 	%l5, %g0, %o2
	call	sun4v_mem_corrupt_detect_precise
	add	%sp, PTREGS_OFF, %o0
	ba,a,pt	%xcc, rtrap
