// rusthello — the first Ring 3 application written in Rust for Mectov OS.
//
// Proves the .mct format is language-agnostic: a freestanding no_std Rust
// binary linked at 0x08000000 with entry `_start`, flattened with objcopy and
// wrapped in the MCT1 header, runs exactly like a C app. The kernel loader
// maps code + zeroes BSS, and thread_create provides the user stack — so the
// Rust runtime obligations are just: no_std, no_main, a panic handler, and
// syscalls via `int $0x80` (same ABI the C apps use).
#![no_std]
#![no_main]

use core::arch::asm;

const SYS_PRINT: u32 = 1; // EBX=str_ptr, ECX=color
const SYS_EXIT: u32 = 10; // EBX=exit_code

// Invoke a syscall via the kernel's int $0x80 ABI (registers_t layout).
unsafe fn syscall(num: u32, arg1: u32, arg2: u32, arg3: u32) -> u32 {
    let ret: u32;
    asm!(
        "int $0x80",
        inlateout("eax") num => ret,
        in("ebx") arg1,
        in("ecx") arg2,
        in("edx") arg3,
        options(nostack)
    );
    ret
}

fn sys_print(msg: &[u8]) {
    unsafe {
        // SYS_PRINT needs a NUL-terminated string; the kernel reads it via the
        // pointer and color comes from ECX.
        syscall(SYS_PRINT, msg.as_ptr() as u32, 0x0F, 0);
    }
}

fn sys_exit(code: u32) -> ! {
    unsafe {
        syscall(SYS_EXIT, code, 0, 0);
    }
    // Never returns — mirror the C wrapper's safety loop.
    loop {}
}

#[no_mangle]
pub extern "C" fn _start() -> ! {
    sys_print(b"rusthello: Hello from Rust on Mectov OS!\n");
    sys_print(b"rusthello: no_std, no_main, linked at 0x08000000\n");
    sys_print(b"rusthello: ALL RUST TESTS PASSED\n");
    sys_exit(0)
}

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    sys_print(b"rusthello: PANIC (app would exit 1)\n");
    sys_exit(1)
}
