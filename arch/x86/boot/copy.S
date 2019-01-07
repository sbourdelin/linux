/* ----------------------------------------------------------------------- *
 *
 *   Copyright (C) 1991, 1992 Linus Torvalds
 *   Copyright 2007 rPath, Inc. - All Rights Reserved
 *
 *   This file is part of the Linux kernel, and is made available under
 *   the terms of the GNU General Public License version 2.
 *
 * ----------------------------------------------------------------------- */

#include <linux/linkage.h>

/*
 * Memory copy routines
 */

	.code16
	.text

GLOBAL(memcpy)
	pushw	%si
	pushw	%di
	movw	%ax, %di
	movw	%dx, %si
	pushw	%cx
	shrw	$2, %cx
	rep; movsl
	popw	%cx
	andw	$3, %cx
	rep; movsb
	popw	%di
	popw	%si
	retl
ENDPROC(memcpy)

GLOBAL(copy_from_fs)
	pushw	%ds
	pushw	%fs
	popw	%ds
	calll	memcpy
	popw	%ds
	retl
ENDPROC(copy_from_fs)

GLOBAL(copy_to_fs)
	pushw	%es
	pushw	%fs
	popw	%es
	calll	memcpy
	popw	%es
	retl
ENDPROC(copy_to_fs)
