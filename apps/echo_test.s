	.file	"echo_test.c"
	.text
	.type	syscall3, @function
syscall3:
.LFB0:
	.cfi_startproc
	pushl	%ebp
	.cfi_def_cfa_offset 8
	.cfi_offset 5, -8
	movl	%esp, %ebp
	.cfi_def_cfa_register 5
	pushl	%ebx
	subl	$16, %esp
	.cfi_offset 3, -12
	movl	8(%ebp), %eax
	movl	12(%ebp), %ebx
	movl	16(%ebp), %ecx
	movl	20(%ebp), %edx
#APP
# 6 "apps/echo_test.c" 1
	int $0x80
# 0 "" 2
#NO_APP
	movl	%eax, -8(%ebp)
	movl	-8(%ebp), %eax
	movl	-4(%ebp), %ebx
	leave
	.cfi_restore 5
	.cfi_restore 3
	.cfi_def_cfa 4, 4
	ret
	.cfi_endproc
.LFE0:
	.size	syscall3, .-syscall3
	.globl	_start
	.type	_start, @function
_start:
.LFB1:
	.cfi_startproc
	pushl	%ebp
	.cfi_def_cfa_offset 8
	.cfi_offset 5, -8
	movl	%esp, %ebp
	.cfi_def_cfa_register 5
	subl	$16, %esp
	movb	$62, -4(%ebp)
	movb	$32, -3(%ebp)
	movb	$0, -2(%ebp)
	leal	-4(%ebp), %eax
	pushl	$0
	pushl	$10
	pushl	%eax
	pushl	$1
	call	syscall3
	addl	$16, %esp
	movb	$0, -1(%ebp)
	jmp	.L4
.L7:
	pushl	$0
	pushl	$0
	pushl	$0
	pushl	$13
	call	syscall3
	addl	$16, %esp
	movb	%al, -1(%ebp)
	cmpb	$0, -1(%ebp)
	jne	.L9
	pushl	$0
	pushl	$0
	pushl	$0
	pushl	$9
	call	syscall3
	addl	$16, %esp
.L4:
	cmpb	$0, -1(%ebp)
	je	.L7
	jmp	.L6
.L9:
	nop
.L6:
	movzbl	-1(%ebp), %eax
	movb	%al, -7(%ebp)
	movb	$10, -6(%ebp)
	movb	$0, -5(%ebp)
	leal	-7(%ebp), %eax
	pushl	$0
	pushl	$15
	pushl	%eax
	pushl	$1
	call	syscall3
	addl	$16, %esp
	pushl	$0
	pushl	$0
	pushl	$0
	pushl	$10
	call	syscall3
	addl	$16, %esp
.L8:
	nop
	jmp	.L8
	.cfi_endproc
.LFE1:
	.size	_start, .-_start
	.ident	"GCC: (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0"
	.section	.note.GNU-stack,"",@progbits
