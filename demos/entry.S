/* Shared Ring-3 entry for all M4 demos (demos/entry.S).
 *
 * Every demo defines `demo_main` (no name clashes: separate binaries) and
 * links entry.o FIRST, so _start lands at image offset 0 and the kernel can
 * enter at BASE+0 with no symbol lookup. _thread_exit sits at the FIXED
 * offset 0x40 (DEMO_THREAD_EXIT_OFF): the kernel pushes it as the return
 * address of CLONE children, so a worker's `ret` exits the thread. Keep
 * _start under 64 bytes (assembler errors if .org goes backwards).
 */
    .code64
    .global _start
    .text
_start:
    call demo_main
    mov %eax, %edi          /* exit(demo_main's return, normally 0) */
    mov $10, %eax           /* SYS64_EXIT */
    int $0x80
1:  hlt
    jmp 1b

    .org 0x40, 0x90
    .global _thread_exit
_thread_exit:
    mov $10, %eax           /* SYS64_EXIT */
    int $0x80
1:  hlt
    jmp 1b

    /* M7.2 argv entry at fixed 0x80: stack holds argc, argv (see spawn_args
     * layout); call args_main(argc, argv), then exit with its return.
     * The default args_main below is WEAK: demos that take argv (argdemo)
     * override it with a strong definition; the rest link fine. */
    .org 0x80, 0x90
    .global _start_args
_start_args:
    pop %rdi                /* argc */
    pop %rsi                /* argv */
    call args_main
    mov %eax, %edi
    mov $10, %eax           /* SYS64_EXIT */
    int $0x80
1:  hlt
    jmp 1b

    .weak args_main
args_main:
    xor %edi, %edi
    mov $10, %eax
    int $0x80
1:  hlt
    jmp 1b

    .section .note.GNU-stack,"",@progbits
