/* narm_start.S - entry of WBFit's nc/tests/narm_run under qemu-aarch64_be (Linux user mode, no
   libc): the stack Linux gives, main(), exit; sys3() for write/mprotect/exit; the module as data.
   (pjdt's, the module's name changed.)
   SPDX-License-Identifier: GPL-2.0-only */
	.text
	.global _start
_start:
	mov	x29, #0
	bl	main
	mov	x8, #93
	svc	#0
	.global sys3
sys3:
	mov	x8, x0
	mov	x0, x1
	mov	x1, x2
	mov	x2, x3
	svc	#0
	ret
	.section .rodata
	.balign 16
	.global narm_image, narm_image_end
narm_image:
	.incbin NARM_FILE_S
narm_image_end:
