.set MAGIC,    0x1BADB002
.set FLAGS,    (1<<0)|(1<<1)|(1<<2)
.set CHECKSUM, -(MAGIC + FLAGS)
.section .multiboot
.align 4
.long MAGIC
.long FLAGS
.long CHECKSUM
.long 0
.long 0
.long 0
.long 0
.long 0
.long 0       /* mode_type: 0 = linear framebuffer */
.long 1024    /* width  */
.long 768     /* height */
.long 32      /* depth  */
.section .bss
.align 16
stack_bottom:
.skip 16384
stack_top:
.section .text
.global _start
.type _start, @function
_start:
	mov $stack_top, %esp
	push %ebx
	call kstart
	cli
1:	hlt
	jmp 1b
