	.file	"terminal.c"
	.text
	.type	syscall, @function
syscall:
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
# 102 "src/include/syscall.h" 1
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
	.size	syscall, .-syscall
	.type	syscall5, @function
syscall5:
.LFB1:
	.cfi_startproc
	pushl	%ebp
	.cfi_def_cfa_offset 8
	.cfi_offset 5, -8
	movl	%esp, %ebp
	.cfi_def_cfa_register 5
	pushl	%edi
	pushl	%esi
	pushl	%ebx
	subl	$16, %esp
	.cfi_offset 7, -12
	.cfi_offset 6, -16
	.cfi_offset 3, -20
	movl	8(%ebp), %eax
	movl	12(%ebp), %ebx
	movl	16(%ebp), %ecx
	movl	20(%ebp), %edx
	movl	24(%ebp), %esi
	movl	28(%ebp), %edi
#APP
# 113 "src/include/syscall.h" 1
	int $0x80
# 0 "" 2
#NO_APP
	movl	%eax, -16(%ebp)
	movl	-16(%ebp), %eax
	addl	$16, %esp
	popl	%ebx
	.cfi_restore 3
	popl	%esi
	.cfi_restore 6
	popl	%edi
	.cfi_restore 7
	popl	%ebp
	.cfi_restore 5
	.cfi_def_cfa 4, 4
	ret
	.cfi_endproc
.LFE1:
	.size	syscall5, .-syscall5
	.type	sys_yield, @function
sys_yield:
.LFB11:
	.cfi_startproc
	pushl	%ebp
	.cfi_def_cfa_offset 8
	.cfi_offset 5, -8
	movl	%esp, %ebp
	.cfi_def_cfa_register 5
	pushl	$0
	pushl	$0
	pushl	$0
	pushl	$9
	call	syscall
	addl	$16, %esp
	nop
	leave
	.cfi_restore 5
	.cfi_def_cfa 4, 4
	ret
	.cfi_endproc
.LFE11:
	.size	sys_yield, .-sys_yield
	.type	sys_exit, @function
sys_exit:
.LFB12:
	.cfi_startproc
	pushl	%ebp
	.cfi_def_cfa_offset 8
	.cfi_offset 5, -8
	movl	%esp, %ebp
	.cfi_def_cfa_register 5
	pushl	$0
	pushl	$0
	pushl	$0
	pushl	$10
	call	syscall
	addl	$16, %esp
.L7:
	nop
	jmp	.L7
	.cfi_endproc
.LFE12:
	.size	sys_exit, .-sys_exit
	.type	sys_draw_rect, @function
sys_draw_rect:
.LFB13:
	.cfi_startproc
	pushl	%ebp
	.cfi_def_cfa_offset 8
	.cfi_offset 5, -8
	movl	%esp, %ebp
	.cfi_def_cfa_register 5
	subl	$16, %esp
	movl	20(%ebp), %eax
	sall	$16, %eax
	movl	%eax, %edx
	movl	24(%ebp), %eax
	movzwl	%ax, %eax
	orl	%edx, %eax
	movl	%eax, -4(%ebp)
	movl	28(%ebp), %eax
	pushl	%eax
	pushl	-4(%ebp)
	pushl	16(%ebp)
	pushl	12(%ebp)
	pushl	8(%ebp)
	pushl	$11
	call	syscall5
	addl	$24, %esp
	nop
	leave
	.cfi_restore 5
	.cfi_def_cfa 4, 4
	ret
	.cfi_endproc
.LFE13:
	.size	sys_draw_rect, .-sys_draw_rect
	.type	sys_draw_text, @function
sys_draw_text:
.LFB14:
	.cfi_startproc
	pushl	%ebp
	.cfi_def_cfa_offset 8
	.cfi_offset 5, -8
	movl	%esp, %ebp
	.cfi_def_cfa_register 5
	movl	24(%ebp), %edx
	movl	20(%ebp), %eax
	pushl	%edx
	pushl	%eax
	pushl	16(%ebp)
	pushl	12(%ebp)
	pushl	8(%ebp)
	pushl	$12
	call	syscall5
	addl	$24, %esp
	nop
	leave
	.cfi_restore 5
	.cfi_def_cfa 4, 4
	ret
	.cfi_endproc
.LFE14:
	.size	sys_draw_text, .-sys_draw_text
	.type	sys_create_window, @function
sys_create_window:
.LFB16:
	.cfi_startproc
	pushl	%ebp
	.cfi_def_cfa_offset 8
	.cfi_offset 5, -8
	movl	%esp, %ebp
	.cfi_def_cfa_register 5
	movl	24(%ebp), %eax
	pushl	%eax
	pushl	20(%ebp)
	pushl	16(%ebp)
	pushl	12(%ebp)
	pushl	8(%ebp)
	pushl	$15
	call	syscall5
	addl	$24, %esp
	leave
	.cfi_restore 5
	.cfi_def_cfa 4, 4
	ret
	.cfi_endproc
.LFE16:
	.size	sys_create_window, .-sys_create_window
	.type	sys_get_event, @function
sys_get_event:
.LFB17:
	.cfi_startproc
	pushl	%ebp
	.cfi_def_cfa_offset 8
	.cfi_offset 5, -8
	movl	%esp, %ebp
	.cfi_def_cfa_register 5
	movl	12(%ebp), %eax
	pushl	$0
	pushl	%eax
	pushl	8(%ebp)
	pushl	$16
	call	syscall
	addl	$16, %esp
	leave
	.cfi_restore 5
	.cfi_def_cfa 4, 4
	ret
	.cfi_endproc
.LFE17:
	.size	sys_get_event, .-sys_get_event
	.type	sys_update_window, @function
sys_update_window:
.LFB18:
	.cfi_startproc
	pushl	%ebp
	.cfi_def_cfa_offset 8
	.cfi_offset 5, -8
	movl	%esp, %ebp
	.cfi_def_cfa_register 5
	pushl	$0
	pushl	$0
	pushl	8(%ebp)
	pushl	$17
	call	syscall
	addl	$16, %esp
	nop
	leave
	.cfi_restore 5
	.cfi_def_cfa 4, 4
	ret
	.cfi_endproc
.LFE18:
	.size	sys_update_window, .-sys_update_window
	.type	sys_set_stdout_ipc, @function
sys_set_stdout_ipc:
.LFB31:
	.cfi_startproc
	pushl	%ebp
	.cfi_def_cfa_offset 8
	.cfi_offset 5, -8
	movl	%esp, %ebp
	.cfi_def_cfa_register 5
	pushl	%ebx
	.cfi_offset 3, -12
	movl	$44, %eax
	movl	8(%ebp), %edx
	movl	%edx, %ebx
#APP
# 271 "src/include/syscall.h" 1
	int $0x80
# 0 "" 2
#NO_APP
	nop
	movl	-4(%ebp), %ebx
	leave
	.cfi_restore 5
	.cfi_restore 3
	.cfi_def_cfa 4, 4
	ret
	.cfi_endproc
.LFE31:
	.size	sys_set_stdout_ipc, .-sys_set_stdout_ipc
	.type	sys_exec_cmd, @function
sys_exec_cmd:
.LFB32:
	.cfi_startproc
	pushl	%ebp
	.cfi_def_cfa_offset 8
	.cfi_offset 5, -8
	movl	%esp, %ebp
	.cfi_def_cfa_register 5
	pushl	%ebx
	subl	$16, %esp
	.cfi_offset 3, -12
	movl	$45, %eax
	movl	8(%ebp), %edx
	movl	%edx, %ebx
#APP
# 275 "src/include/syscall.h" 1
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
.LFE32:
	.size	sys_exec_cmd, .-sys_exec_cmd
	.type	sys_ipc_create, @function
sys_ipc_create:
.LFB33:
	.cfi_startproc
	pushl	%ebp
	.cfi_def_cfa_offset 8
	.cfi_offset 5, -8
	movl	%esp, %ebp
	.cfi_def_cfa_register 5
	pushl	%ebx
	subl	$16, %esp
	.cfi_offset 3, -12
	movl	$23, %eax
	movl	8(%ebp), %edx
	movl	%edx, %ebx
#APP
# 282 "src/include/syscall.h" 1
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
.LFE33:
	.size	sys_ipc_create, .-sys_ipc_create
	.type	sys_ipc_try_recv, @function
sys_ipc_try_recv:
.LFB34:
	.cfi_startproc
	pushl	%ebp
	.cfi_def_cfa_offset 8
	.cfi_offset 5, -8
	movl	%esp, %ebp
	.cfi_def_cfa_register 5
	pushl	%esi
	pushl	%ebx
	subl	$16, %esp
	.cfi_offset 6, -12
	.cfi_offset 3, -16
	movl	16(%ebp), %eax
	movl	%eax, -16(%ebp)
	movl	$28, %eax
	movl	8(%ebp), %ebx
	movl	$0, %ecx
	movl	12(%ebp), %edx
	leal	-16(%ebp), %esi
#APP
# 288 "src/include/syscall.h" 1
	int $0x80
# 0 "" 2
#NO_APP
	movl	%eax, -12(%ebp)
	cmpl	$0, -12(%ebp)
	jne	.L21
	movl	-16(%ebp), %eax
	jmp	.L23
.L21:
	movl	-12(%ebp), %eax
.L23:
	addl	$16, %esp
	popl	%ebx
	.cfi_restore 3
	popl	%esi
	.cfi_restore 6
	popl	%ebp
	.cfi_restore 5
	.cfi_def_cfa 4, 4
	ret
	.cfi_endproc
.LFE34:
	.size	sys_ipc_try_recv, .-sys_ipc_try_recv
	.local	buf
	.comm	buf,1776,32
	.local	col
	.comm	col,1776,32
	.local	cx
	.comm	cx,4,4
	.local	cy
	.comm	cy,4,4
	.local	cmd
	.comm	cmd,256,32
	.local	cmd_len
	.comm	cmd_len,4,4
	.local	ipc_qid
	.comm	ipc_qid,4,4
	.type	term_scroll, @function
term_scroll:
.LFB35:
	.cfi_startproc
	pushl	%ebp
	.cfi_def_cfa_offset 8
	.cfi_offset 5, -8
	movl	%esp, %ebp
	.cfi_def_cfa_register 5
	subl	$16, %esp
	movl	$0, -4(%ebp)
	jmp	.L25
.L28:
	movl	$0, -8(%ebp)
	jmp	.L26
.L27:
	movl	-4(%ebp), %eax
	addl	$1, %eax
	imull	$74, %eax, %edx
	movl	-8(%ebp), %eax
	addl	%edx, %eax
	addl	$buf, %eax
	movzbl	(%eax), %eax
	movl	-4(%ebp), %edx
	imull	$74, %edx, %ecx
	movl	-8(%ebp), %edx
	addl	%ecx, %edx
	addl	$buf, %edx
	movb	%al, (%edx)
	movl	-4(%ebp), %eax
	addl	$1, %eax
	imull	$74, %eax, %edx
	movl	-8(%ebp), %eax
	addl	%edx, %eax
	addl	$col, %eax
	movzbl	(%eax), %eax
	movl	-4(%ebp), %edx
	imull	$74, %edx, %ecx
	movl	-8(%ebp), %edx
	addl	%ecx, %edx
	addl	$col, %edx
	movb	%al, (%edx)
	addl	$1, -8(%ebp)
.L26:
	cmpl	$73, -8(%ebp)
	jle	.L27
	addl	$1, -4(%ebp)
.L25:
	cmpl	$22, -4(%ebp)
	jle	.L28
	movl	$0, -12(%ebp)
	jmp	.L29
.L30:
	movl	-12(%ebp), %eax
	addl	$buf+1702, %eax
	movb	$32, (%eax)
	movl	-12(%ebp), %eax
	addl	$col+1702, %eax
	movb	$0, (%eax)
	addl	$1, -12(%ebp)
.L29:
	cmpl	$73, -12(%ebp)
	jle	.L30
	movl	$23, cy
	nop
	leave
	.cfi_restore 5
	.cfi_def_cfa 4, 4
	ret
	.cfi_endproc
.LFE35:
	.size	term_scroll, .-term_scroll
	.type	term_putchar, @function
term_putchar:
.LFB36:
	.cfi_startproc
	pushl	%ebp
	.cfi_def_cfa_offset 8
	.cfi_offset 5, -8
	movl	%esp, %ebp
	.cfi_def_cfa_register 5
	subl	$8, %esp
	movl	8(%ebp), %edx
	movl	12(%ebp), %eax
	movb	%dl, -4(%ebp)
	movb	%al, -8(%ebp)
	cmpb	$10, -4(%ebp)
	jne	.L32
	movl	$0, cx
	movl	cy, %eax
	addl	$1, %eax
	movl	%eax, cy
	jmp	.L33
.L32:
	cmpb	$13, -4(%ebp)
	jne	.L34
	movl	$0, cx
	jmp	.L33
.L34:
	cmpb	$8, -4(%ebp)
	jne	.L35
	movl	cx, %eax
	testl	%eax, %eax
	jle	.L33
	movl	cx, %eax
	subl	$1, %eax
	movl	%eax, cx
	movl	cy, %edx
	movl	cx, %eax
	imull	$74, %edx, %edx
	addl	%edx, %eax
	addl	$buf, %eax
	movb	$32, (%eax)
	movl	cy, %edx
	movl	cx, %eax
	imull	$74, %edx, %edx
	addl	%edx, %eax
	addl	$col, %eax
	movb	$0, (%eax)
	jmp	.L33
.L35:
	movl	cx, %eax
	cmpl	$73, %eax
	jle	.L36
	movl	$0, cx
	movl	cy, %eax
	addl	$1, %eax
	movl	%eax, cy
.L36:
	movl	cy, %edx
	movl	cx, %eax
	imull	$74, %edx, %edx
	addl	%edx, %eax
	leal	buf(%eax), %edx
	movzbl	-4(%ebp), %eax
	movb	%al, (%edx)
	movl	cy, %edx
	movl	cx, %eax
	imull	$74, %edx, %edx
	addl	%edx, %eax
	leal	col(%eax), %edx
	movzbl	-8(%ebp), %eax
	movb	%al, (%edx)
	movl	cx, %eax
	addl	$1, %eax
	movl	%eax, cx
.L33:
	movl	cy, %eax
	cmpl	$23, %eax
	jle	.L38
	call	term_scroll
.L38:
	nop
	leave
	.cfi_restore 5
	.cfi_def_cfa 4, 4
	ret
	.cfi_endproc
.LFE36:
	.size	term_putchar, .-term_putchar
	.type	term_print, @function
term_print:
.LFB37:
	.cfi_startproc
	pushl	%ebp
	.cfi_def_cfa_offset 8
	.cfi_offset 5, -8
	movl	%esp, %ebp
	.cfi_def_cfa_register 5
	subl	$4, %esp
	movl	12(%ebp), %eax
	movb	%al, -4(%ebp)
	jmp	.L40
.L41:
	movzbl	-4(%ebp), %edx
	movl	8(%ebp), %eax
	leal	1(%eax), %ecx
	movl	%ecx, 8(%ebp)
	movzbl	(%eax), %eax
	movsbl	%al, %eax
	pushl	%edx
	pushl	%eax
	call	term_putchar
	addl	$8, %esp
.L40:
	movl	8(%ebp), %eax
	movzbl	(%eax), %eax
	testb	%al, %al
	jne	.L41
	nop
	nop
	leave
	.cfi_restore 5
	.cfi_def_cfa 4, 4
	ret
	.cfi_endproc
.LFE37:
	.size	term_print, .-term_print
	.type	vga_to_rgb, @function
vga_to_rgb:
.LFB38:
	.cfi_startproc
	pushl	%ebp
	.cfi_def_cfa_offset 8
	.cfi_offset 5, -8
	movl	%esp, %ebp
	.cfi_def_cfa_register 5
	subl	$4, %esp
	movl	8(%ebp), %eax
	movb	%al, -4(%ebp)
	movzbl	-4(%ebp), %eax
	cmpl	$15, %eax
	ja	.L43
	movl	.L45(,%eax,4), %eax
	jmp	*%eax
	.section	.rodata
	.align 4
	.align 4
.L45:
	.long	.L60
	.long	.L59
	.long	.L58
	.long	.L57
	.long	.L56
	.long	.L55
	.long	.L54
	.long	.L53
	.long	.L52
	.long	.L51
	.long	.L50
	.long	.L49
	.long	.L48
	.long	.L47
	.long	.L46
	.long	.L44
	.text
.L60:
	movl	$1118491, %eax
	jmp	.L61
.L59:
	movl	$9024762, %eax
	jmp	.L61
.L58:
	movl	$10937249, %eax
	jmp	.L61
.L57:
	movl	$9757397, %eax
	jmp	.L61
.L56:
	movl	$15961000, %eax
	jmp	.L61
.L55:
	movl	$13346551, %eax
	jmp	.L61
.L54:
	movl	$16376495, %eax
	jmp	.L61
.L53:
	movl	$12239582, %eax
	jmp	.L61
.L52:
	movl	$5790576, %eax
	jmp	.L61
.L51:
	movl	$9024762, %eax
	jmp	.L61
.L50:
	movl	$10937249, %eax
	jmp	.L61
.L49:
	movl	$9757397, %eax
	jmp	.L61
.L48:
	movl	$15961000, %eax
	jmp	.L61
.L47:
	movl	$16106215, %eax
	jmp	.L61
.L46:
	movl	$16376495, %eax
	jmp	.L61
.L44:
	movl	$13489908, %eax
	jmp	.L61
.L43:
	movl	$13489908, %eax
.L61:
	leave
	.cfi_restore 5
	.cfi_def_cfa 4, 4
	ret
	.cfi_endproc
.LFE38:
	.size	vga_to_rgb, .-vga_to_rgb
	.type	draw_terminal, @function
draw_terminal:
.LFB39:
	.cfi_startproc
	pushl	%ebp
	.cfi_def_cfa_offset 8
	.cfi_offset 5, -8
	movl	%esp, %ebp
	.cfi_def_cfa_register 5
	subl	$32, %esp
	movl	$600, -12(%ebp)
	movl	$400, -16(%ebp)
	pushl	$1118491
	pushl	-16(%ebp)
	pushl	-12(%ebp)
	pushl	$0
	pushl	$0
	pushl	8(%ebp)
	call	sys_draw_rect
	addl	$24, %esp
	movl	$0, -4(%ebp)
	jmp	.L63
.L67:
	movl	$0, -8(%ebp)
	jmp	.L64
.L66:
	movl	-4(%ebp), %eax
	imull	$74, %eax, %edx
	movl	-8(%ebp), %eax
	addl	%edx, %eax
	addl	$buf, %eax
	movzbl	(%eax), %eax
	movb	%al, -17(%ebp)
	movl	-4(%ebp), %eax
	imull	$74, %eax, %edx
	movl	-8(%ebp), %eax
	addl	%edx, %eax
	addl	$col, %eax
	movzbl	(%eax), %eax
	movb	%al, -18(%ebp)
	cmpb	$0, -17(%ebp)
	je	.L65
	cmpb	$0, -18(%ebp)
	je	.L65
	movzbl	-17(%ebp), %eax
	movb	%al, -20(%ebp)
	movb	$0, -19(%ebp)
	movzbl	-18(%ebp), %eax
	pushl	%eax
	call	vga_to_rgb
	addl	$4, %esp
	movl	-4(%ebp), %edx
	movl	%edx, %ecx
	sall	$4, %ecx
	movl	-8(%ebp), %edx
	sall	$3, %edx
	pushl	%eax
	leal	-20(%ebp), %eax
	pushl	%eax
	pushl	%ecx
	pushl	%edx
	pushl	8(%ebp)
	call	sys_draw_text
	addl	$20, %esp
.L65:
	addl	$1, -8(%ebp)
.L64:
	cmpl	$73, -8(%ebp)
	jle	.L66
	addl	$1, -4(%ebp)
.L63:
	cmpl	$23, -4(%ebp)
	jle	.L67
	movl	cy, %eax
	sall	$4, %eax
	leal	14(%eax), %edx
	movl	cx, %eax
	sall	$3, %eax
	pushl	$65416
	pushl	$2
	pushl	$8
	pushl	%edx
	pushl	%eax
	pushl	8(%ebp)
	call	sys_draw_rect
	addl	$24, %esp
	pushl	8(%ebp)
	call	sys_update_window
	addl	$4, %esp
	nop
	leave
	.cfi_restore 5
	.cfi_def_cfa 4, 4
	ret
	.cfi_endproc
.LFE39:
	.size	draw_terminal, .-draw_terminal
	.type	drain_ipc, @function
drain_ipc:
.LFB40:
	.cfi_startproc
	pushl	%ebp
	.cfi_def_cfa_offset 8
	.cfi_offset 5, -8
	movl	%esp, %ebp
	.cfi_def_cfa_register 5
	subl	$144, %esp
	movl	$0, -4(%ebp)
	jmp	.L69
.L74:
	movl	ipc_qid, %eax
	pushl	$128
	leal	-140(%ebp), %edx
	pushl	%edx
	pushl	%eax
	call	sys_ipc_try_recv
	addl	$12, %esp
	movl	%eax, -12(%ebp)
	cmpl	$0, -12(%ebp)
	jle	.L75
	movl	$0, -8(%ebp)
	jmp	.L72
.L73:
	movl	-8(%ebp), %eax
	addl	$1, %eax
	movzbl	-140(%ebp,%eax), %eax
	movzbl	%al, %edx
	leal	-140(%ebp), %ecx
	movl	-8(%ebp), %eax
	addl	%ecx, %eax
	movzbl	(%eax), %eax
	movsbl	%al, %eax
	pushl	%edx
	pushl	%eax
	call	term_putchar
	addl	$8, %esp
	addl	$2, -8(%ebp)
.L72:
	movl	-8(%ebp), %eax
	cmpl	-12(%ebp), %eax
	jl	.L73
	addl	$1, -4(%ebp)
.L69:
	cmpl	$4095, -4(%ebp)
	jle	.L74
	jmp	.L76
.L75:
	nop
.L76:
	nop
	leave
	.cfi_restore 5
	.cfi_def_cfa 4, 4
	ret
	.cfi_endproc
.LFE40:
	.size	drain_ipc, .-drain_ipc
	.section	.rodata
.LC0:
	.string	"root@mectov"
.LC1:
	.string	" ~$ "
	.text
	.type	print_prompt, @function
print_prompt:
.LFB41:
	.cfi_startproc
	pushl	%ebp
	.cfi_def_cfa_offset 8
	.cfi_offset 5, -8
	movl	%esp, %ebp
	.cfi_def_cfa_register 5
	pushl	$10
	pushl	$.LC0
	call	term_print
	addl	$8, %esp
	pushl	$15
	pushl	$.LC1
	call	term_print
	addl	$8, %esp
	nop
	leave
	.cfi_restore 5
	.cfi_def_cfa 4, 4
	ret
	.cfi_endproc
.LFE41:
	.size	print_prompt, .-print_prompt
	.section	.rodata
.LC2:
	.string	"Terminal (Ring 3)"
	.align 4
.LC3:
	.string	"Mectov OS v18.0 Terminal [Ring 3]\n"
	.align 4
.LC4:
	.string	"Welcome Bos Alif! System ready.\n\n"
	.text
	.globl	_start
	.type	_start, @function
_start:
.LFB42:
	.cfi_startproc
	pushl	%ebp
	.cfi_def_cfa_offset 8
	.cfi_offset 5, -8
	movl	%esp, %ebp
	.cfi_def_cfa_register 5
	subl	$48, %esp
	pushl	$.LC2
	pushl	$400
	pushl	$600
	pushl	$40
	pushl	$60
	call	sys_create_window
	addl	$20, %esp
	movl	%eax, -24(%ebp)
	cmpl	$0, -24(%ebp)
	jns	.L79
	call	sys_exit
.L79:
	pushl	$57005
	call	sys_ipc_create
	addl	$4, %esp
	movl	%eax, ipc_qid
	movl	ipc_qid, %eax
	testl	%eax, %eax
	jle	.L80
	movl	ipc_qid, %eax
	pushl	%eax
	call	sys_set_stdout_ipc
	addl	$4, %esp
.L80:
	movl	$0, -4(%ebp)
	jmp	.L81
.L84:
	movl	$0, -8(%ebp)
	jmp	.L82
.L83:
	movl	-4(%ebp), %eax
	imull	$74, %eax, %edx
	movl	-8(%ebp), %eax
	addl	%edx, %eax
	addl	$buf, %eax
	movb	$0, (%eax)
	movl	-4(%ebp), %eax
	imull	$74, %eax, %edx
	movl	-8(%ebp), %eax
	addl	%edx, %eax
	addl	$col, %eax
	movb	$0, (%eax)
	addl	$1, -8(%ebp)
.L82:
	cmpl	$73, -8(%ebp)
	jle	.L83
	addl	$1, -4(%ebp)
.L81:
	cmpl	$23, -4(%ebp)
	jle	.L84
	pushl	$11
	pushl	$.LC3
	call	term_print
	addl	$8, %esp
	pushl	$13
	pushl	$.LC4
	call	term_print
	addl	$8, %esp
	call	print_prompt
	movl	$0, cmd_len
	pushl	-24(%ebp)
	call	draw_terminal
	addl	$4, %esp
	movl	$0, -12(%ebp)
	jmp	.L85
.L97:
	movl	-48(%ebp), %eax
	cmpl	$1, %eax
	jne	.L86
	pushl	-24(%ebp)
	call	draw_terminal
	addl	$4, %esp
	jmp	.L85
.L86:
	movl	-48(%ebp), %eax
	cmpl	$2, %eax
	jne	.L85
	movl	-36(%ebp), %eax
	cmpl	$27, %eax
	jne	.L88
	pushl	$0
	call	sys_set_stdout_ipc
	addl	$4, %esp
	call	sys_exit
.L88:
	movl	-36(%ebp), %eax
	cmpl	$10, %eax
	jne	.L89
	pushl	$15
	pushl	$10
	call	term_putchar
	addl	$8, %esp
	movl	cmd_len, %eax
	movb	$0, cmd(%eax)
	movl	cmd_len, %eax
	testl	%eax, %eax
	jle	.L90
	movzbl	cmd, %eax
	cmpb	$99, %al
	jne	.L91
	movzbl	cmd+1, %eax
	cmpb	$108, %al
	jne	.L91
	movzbl	cmd+2, %eax
	cmpb	$101, %al
	jne	.L91
	movzbl	cmd+3, %eax
	cmpb	$97, %al
	jne	.L91
	movzbl	cmd+4, %eax
	cmpb	$114, %al
	jne	.L91
	movzbl	cmd+5, %eax
	testb	%al, %al
	jne	.L91
	movl	$0, -16(%ebp)
	jmp	.L92
.L95:
	movl	$0, -20(%ebp)
	jmp	.L93
.L94:
	movl	-16(%ebp), %eax
	imull	$74, %eax, %edx
	movl	-20(%ebp), %eax
	addl	%edx, %eax
	addl	$buf, %eax
	movb	$0, (%eax)
	movl	-16(%ebp), %eax
	imull	$74, %eax, %edx
	movl	-20(%ebp), %eax
	addl	%edx, %eax
	addl	$col, %eax
	movb	$0, (%eax)
	addl	$1, -20(%ebp)
.L93:
	cmpl	$73, -20(%ebp)
	jle	.L94
	addl	$1, -16(%ebp)
.L92:
	cmpl	$23, -16(%ebp)
	jle	.L95
	movl	$0, cx
	movl	$0, cy
	jmp	.L90
.L91:
	pushl	$cmd
	call	sys_exec_cmd
	addl	$4, %esp
	call	drain_ipc
.L90:
	call	print_prompt
	movl	$0, cmd_len
	pushl	-24(%ebp)
	call	draw_terminal
	addl	$4, %esp
	jmp	.L85
.L89:
	movl	-36(%ebp), %eax
	cmpl	$8, %eax
	jne	.L96
	movl	cmd_len, %eax
	testl	%eax, %eax
	jle	.L85
	movl	cmd_len, %eax
	subl	$1, %eax
	movl	%eax, cmd_len
	pushl	$15
	pushl	$8
	call	term_putchar
	addl	$8, %esp
	pushl	-24(%ebp)
	call	draw_terminal
	addl	$4, %esp
	jmp	.L85
.L96:
	movl	-36(%ebp), %eax
	cmpl	$31, %eax
	jle	.L85
	movl	-36(%ebp), %eax
	cmpl	$126, %eax
	jg	.L85
	movl	cmd_len, %eax
	cmpl	$254, %eax
	jg	.L85
	movl	-36(%ebp), %ecx
	movl	cmd_len, %eax
	leal	1(%eax), %edx
	movl	%edx, cmd_len
	movl	%ecx, %edx
	movb	%dl, cmd(%eax)
	movl	-36(%ebp), %eax
	movsbl	%al, %eax
	pushl	$10
	pushl	%eax
	call	term_putchar
	addl	$8, %esp
	pushl	-24(%ebp)
	call	draw_terminal
	addl	$4, %esp
.L85:
	leal	-48(%ebp), %eax
	pushl	%eax
	pushl	-24(%ebp)
	call	sys_get_event
	addl	$8, %esp
	testl	%eax, %eax
	jne	.L97
	addl	$1, -12(%ebp)
	movl	-12(%ebp), %ecx
	movl	$1374389535, %edx
	movl	%ecx, %eax
	imull	%edx
	movl	%edx, %eax
	sarl	$4, %eax
	movl	%ecx, %edx
	sarl	$31, %edx
	subl	%edx, %eax
	imull	$50, %eax, %edx
	movl	%ecx, %eax
	subl	%edx, %eax
	testl	%eax, %eax
	jne	.L98
	movl	cx, %eax
	movl	%eax, -28(%ebp)
	movl	cy, %eax
	movl	%eax, -32(%ebp)
	call	drain_ipc
	movl	cx, %eax
	cmpl	%eax, -28(%ebp)
	jne	.L99
	movl	cy, %eax
	cmpl	%eax, -32(%ebp)
	je	.L98
.L99:
	pushl	-24(%ebp)
	call	draw_terminal
	addl	$4, %esp
.L98:
	call	sys_yield
	jmp	.L85
	.cfi_endproc
.LFE42:
	.size	_start, .-_start
	.ident	"GCC: (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0"
	.section	.note.GNU-stack,"",@progbits
