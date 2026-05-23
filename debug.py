#!/usr/bin/env python3
import os
import sys
import time
import re
import subprocess
import glob

# Mectov-OS Live Debugger & Crash Symbolicator
# An automated, high-end debugger terminal assistant for x86 kernel development.

# Colors
C_RESET = "\033[0m"
C_BOLD = "\033[1m"
C_RED = "\033[31m"
C_GREEN = "\033[32m"
C_YELLOW = "\033[33m"
C_BLUE = "\033[34m"
C_MAGENTA = "\033[35m"
C_CYAN = "\033[36m"
C_WHITE = "\033[37m"

C_BG_RED = "\033[41m"
C_BG_YELLOW = "\033[43m"

# Logger banners
print(f"{C_BOLD}{C_CYAN}===================================================={C_RESET}")
print(f"{C_BOLD}{C_CYAN}  MECTOV-OS LIVE DEBUGGER & CRASH SYMBOLICATOR      {C_RESET}")
print(f"{C_BOLD}{C_CYAN}===================================================={C_RESET}")
print(f"[*] Monitoring serial_debug.log...")
print(f"[*] Automatic symbolication armed for kernel & apps.")
print(f"{C_CYAN}----------------------------------------------------{C_RESET}\n")

LOG_FILE = "serial_debug.log"
KERNEL_ELF = "myos.bin" if os.path.exists("myos.bin") else "myos.elf"

# Standard x86 Interrupt Numbers mapped to descriptive names
EXCEPTIONS = {
    0: "Divide-by-Zero Error",
    1: "Debug",
    2: "Non-maskable Interrupt",
    3: "Breakpoint",
    4: "Overflow",
    5: "Bound Range Exceeded",
    6: "Invalid Opcode (Invalid Instruction)",
    7: "Device Not Available",
    8: "Double Fault",
    9: "Coprocessor Segment Overrun",
    10: "Invalid TSS",
    11: "Segment Not Present",
    12: "Stack-Segment Fault",
    13: "General Protection Fault (GPF)",
    14: "Page Fault (PF)",
    16: "x87 Floating-Point Exception",
    17: "Alignment Check",
    18: "Machine Check",
    19: "SIMD Floating-Point Exception",
    20: "Virtualization Exception",
    30: "Security Exception",
    255: "Spurious Interrupt / Unregistered Exception (or Stack Corruption)"
}

def colorize_line(line):
    # Strip newline
    line = line.rstrip('\n')
    
    # Check for crash/exception markers
    if "[EXCEPTION]" in line or "[KERNEL PANIC]" in line:
        return f"{C_BOLD}{C_BG_RED}{C_WHITE} {line} {C_RESET}"
    if "[CRASH]" in line:
        return f"{C_BOLD}{C_RED}{line}{C_RESET}"
        
    # Standard tags coloring
    if line.startswith("[TASK]"):
        return f"{C_CYAN}{line}{C_RESET}"
    if line.startswith("[SYSCALL]"):
        return f"{C_MAGENTA}{line}{C_RESET}"
    if line.startswith("[*]"):
        return f"{C_GREEN}{line}{C_RESET}"
    if line.startswith("[!]"):
        return f"{C_YELLOW}{line}{C_RESET}"
    if line.startswith("[-]"):
        return f"{C_RED}{line}{C_RESET}"
        
    # Register dumps and exception details
    if any(k in line for k in ["EIP:", "ESP:", "EBP:", "EAX:", "EBX:", "ECX:", "EDX:", "CR3:", "PF addr:"]):
        return f"{C_YELLOW}{line}{C_RESET}"
        
    return line

def find_all_elfs():
    # Recursively find all .elf files in the current folder and subfolders
    elf_files = glob.glob("**/*.elf", recursive=True)
    # Filter out kernel.elf if it has a different name, keep myos.elf and app elfs
    return elf_files

def get_source_line(file_path, line_number):
    if not os.path.exists(file_path):
        return None
    try:
        with open(file_path, 'r', encoding='utf-8', errors='ignore') as f:
            lines = f.readlines()
            if 1 <= line_number <= len(lines):
                return lines[line_number - 1].strip()
    except Exception:
        pass
    return None

def symbolicate_address(eip_hex, is_user=False):
    eip_val = int(eip_hex, 16)
    
    # 1. Determine if it's Kernel space or User space
    # Kernel starts typically around 0x00100000 (below 0x08000000)
    # User space is loaded at 0x08000000 and above
    target_elf = None
    is_user_space = is_user or (eip_val >= 0x08000000)
    
    if is_user_space:
        # Check if the EIP is in the kernel heap/BSS or user stack range (below 0x08000000)
        # Executing here is definitely a stack execution crash!
        if eip_val < 0x08000000:
            return {
                "elf": "USER_STACK",
                "func": "None (Stack Execution)",
                "file": "User Stack Memory / Corrupt Return Pointer",
                "line": 0,
                "code": "CPU executed stack memory! Possible stack corruption or smashed return address.",
                "is_user": True,
                "is_stack_exec": True
            }
            
        # User space! Let's search all compiled app ELFs
        elf_files = find_all_elfs()
        
        # We try to run addr2line on each ELF file to see which one resolves the address
        for elf in elf_files:
            if elf == KERNEL_ELF:
                continue
            try:
                out = subprocess.check_output(["addr2line", "-e", elf, "-f", "-p", eip_hex]).decode().strip()
                # If it resolves to a valid function (does not start with ??)
                if not out.startswith("??"):
                    target_elf = elf
                    break
            except Exception:
                continue
    else:
        # Kernel space!
        if os.path.exists(KERNEL_ELF):
            target_elf = KERNEL_ELF

    if not target_elf:
        return None
        
    try:
        # Run addr2line
        # Format: FunctionName at /path/to/source.c:line
        res = subprocess.check_output(["addr2line", "-e", target_elf, "-f", "-p", eip_hex]).decode().strip()
        
        # Parse output: "func at /path/to/file.c:123" or similar (or with :? if line is unknown)
        match = re.match(r"(.*) at (.*):(\d+|\?)", res)
        if match:
            func = match.group(1)
            file_path = match.group(2)
            line_no_str = match.group(3)
            
            line_no = int(line_no_str) if line_no_str.isdigit() else 0
            
            # Clean up path to make it relative/short if possible
            short_path = os.path.relpath(file_path) if os.path.isabs(file_path) else file_path
            
            # Read actual code line
            code_line = get_source_line(file_path, line_no) if line_no > 0 else None
            
            return {
                "elf": target_elf,
                "func": func,
                "file": short_path,
                "line": line_no if line_no > 0 else "Unknown",
                "code": code_line,
                "is_user": is_user_space
            }
    except Exception as e:
        pass
        
    return None

def print_crash_report(crash_info):
    int_no = crash_info.get("int_no", None)
    int_name = EXCEPTIONS.get(int_no, "Unknown Exception")
    eip = crash_info.get("EIP", None)
    cr2 = crash_info.get("cr2", None)
    err_code = crash_info.get("err_code", None)
    tid = crash_info.get("tid", None)
    is_user = crash_info.get("is_user", False)
    
    symbol_info = symbolicate_address(eip, is_user) if eip else None
    
    print("\n" + f"{C_BOLD}{C_RED}┌──────────────────────────────────────────────────────────┐{C_RESET}")
    print(f"{C_BOLD}{C_RED}│                    SYSTEM CRASH REPORT                   │{C_RESET}")
    print(f"{C_BOLD}{C_RED}└──────────────────────────────────────────────────────────┘{C_RESET}")
    
    # Exception info
    print(f" {C_BOLD}Exception:{C_RESET} {C_RED}{C_BOLD}int_no {int_no if int_no is not None else '?'}{C_RESET} -> {C_YELLOW}{int_name}{C_RESET}")
    if tid is not None:
        print(f" {C_BOLD}Task Context:{C_RESET} Task ID {C_CYAN}{tid}{C_RESET} ({'User Mode - Ring 3' if is_user else 'Kernel Mode'})")
        
    if cr2:
        print(f" {C_BOLD}Page Fault Address (CR2):{C_RESET} {C_RED}{C_BOLD}{cr2}{C_RESET}")
        
    if err_code:
        # Decode page fault error code if it's a PF
        err_val = int(err_code, 16)
        p_flag = "Present" if (err_val & 1) else "Non-present page"
        w_flag = "Write access" if (err_val & 2) else "Read access"
        u_flag = "User mode" if (err_val & 4) else "Supervisor mode"
        r_flag = "Reserved bit violation" if (err_val & 8) else ""
        i_flag = "Instruction fetch" if (err_val & 16) else ""
        
        pf_details = f"{p_flag}, {w_flag}, {u_flag}"
        if r_flag: pf_details += f", {r_flag}"
        if i_flag: pf_details += f", {i_flag}"
        
        print(f" {C_BOLD}Error Code Details:{C_RESET} {C_RED}{err_code}{C_RESET} ({pf_details})")
        
    print(f" {C_BOLD}Instruction Pointer (EIP):{C_RESET} {C_GREEN}{C_BOLD}{eip}{C_RESET}")
    
    # Symbolication output
    if symbol_info:
        if symbol_info.get("is_stack_exec"):
            print(f"\n {C_RED}{C_BOLD}[🚨] CRITICAL STACK CRASH DETECTED:{C_RESET}")
            print(f"   • {C_BOLD}Alert:{C_RESET}      {C_BG_RED}{C_WHITE} STACK EXECUTION / Wild Jump {C_RESET}")
            print(f"   • {C_BOLD}Diagnosis:{C_RESET} The CPU jumped to instruction pointer {C_YELLOW}{eip}{C_RESET} which lies in User Stack Memory.")
            print(f"                 This indicates a serious stack corruption, buffer overflow, or smashed return address!")
            print(f"                 This was caused by duplicate application launch memory collisions under a double-click!")
        else:
            elf_type = "APP BINARY" if symbol_info["is_user"] else "KERNEL KERNEL"
            print(f"\n {C_GREEN}{C_BOLD}[✓] Symbolication Success:{C_RESET}")
            print(f"   • {C_BOLD}Binary:{C_RESET}   {C_MAGENTA}{symbol_info['elf']}{C_RESET} ({elf_type})")
            print(f"   • {C_BOLD}Function:{C_RESET} {C_CYAN}{C_BOLD}{symbol_info['func']}(){C_RESET}")
            print(f"   • {C_BOLD}Source:{C_RESET}   {C_WHITE}{symbol_info['file']}{C_RESET} : {C_YELLOW}{C_BOLD}Line {symbol_info['line']}{C_RESET}")
            
            if symbol_info["code"]:
                print(f"\n   {C_BOLD}Code Line {symbol_info['line']}:{C_RESET}")
                print(f"   {C_RED}--->{C_RESET}  {C_BG_YELLOW}{C_WHITE} {symbol_info['code']} {C_RESET}")
    else:
        print(f"\n {C_RED}[!] Symbolication Failed: No matching symbols found for EIP {eip}.{C_RESET}")
        print("   Make sure code is compiled with debugging info and ELFs/myos.elf are present.")

    # Other registers
    print(f"\n {C_BOLD}Registers Snapshot:{C_RESET}")
    print(f"   EAX: {crash_info.get('EAX', '?-')}   EBX: {crash_info.get('EBX', '?-')}   ECX: {crash_info.get('ECX', '?-')}   EDX: {crash_info.get('EDX', '?-')}")
    print(f"   ESP: {crash_info.get('ESP', '?-')}   EBP: {crash_info.get('EBP', '?-')}   CR3: {crash_info.get('CR3', '?-')}")
    print(f"{C_BOLD}{C_RED}────────────────────────────────────────────────────────────{C_RESET}\n")

def monitor_logs():
    import socket
    
    use_socket = False
    f = None
    
    # Try connecting to the TCP server first (with a short timeout of 1.5 seconds)
    print(f"[*] Attempting to connect to QEMU serial socket at 127.0.0.1:45454...")
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(1.5)
    try:
        s.connect(("127.0.0.1", 45454))
        # Disable timeout for active monitoring
        s.settimeout(None)
        print(f"{C_BOLD}{C_GREEN}[+] Connected to QEMU real-time serial socket at 127.0.0.1:45454!{C_RESET}")
        f = s.makefile("r", encoding="utf-8", errors="replace")
        use_socket = True
    except Exception as e:
        print(f"{C_BOLD}{C_YELLOW}[!] TCP connection failed ({e}).{C_RESET}")
        print(f"[*] Falling back to tailing file: {LOG_FILE}")
        
    if not use_socket:
        # Wait for the log file to be created
        while not os.path.exists(LOG_FILE):
            print(f"[*] Waiting for {LOG_FILE} to be created by QEMU...")
            time.sleep(1)
            
        print(f"[+] Found {LOG_FILE}. Active monitoring started!")
        f = open(LOG_FILE, "r", encoding="utf-8", errors="replace")
        # Go to the end of the file to monitor live logs
        f.seek(0, os.SEEK_END)
        
    crash_data = {}
    in_exception_block = False
    
    try:
        while True:
            if use_socket:
                try:
                    line = f.readline()
                    if not line:
                        print(f"{C_BOLD}{C_RED}[!] Socket closed by QEMU. Exiting...{C_RESET}")
                        break
                except socket.timeout:
                    continue
                except Exception as e:
                    print(f"{C_BOLD}{C_RED}[!] Socket read error: {e}. Exiting...{C_RESET}")
                    break
            else:
                pos = f.tell()
                line = f.readline()
                if not line:
                    # Check if file was truncated
                    f.seek(0, os.SEEK_END)
                    if f.tell() < pos:
                        # Truncated! Start reading from beginning
                        f.seek(0)
                        continue
                    # Not truncated, seek back to where we were
                    f.seek(pos)
                    time.sleep(0.05)
                    continue
            
            # Process line
            print(colorize_line(line))
            
            # Crash detection state machine
            if "[EXCEPTION]" in line:
                in_exception_block = True
                crash_data = {"is_user": False}
                
                # Parse int_no and CS
                match = re.search(r"int_no=(0x[0-9a-fA-F]+)", line)
                if match:
                    crash_data["int_no"] = int(match.group(1), 16)
                    
                match = re.search(r"CS=(0x[0-9a-fA-F]+)", line)
                if match:
                    cs_val = int(match.group(1), 16)
                    crash_data["is_user"] = (cs_val & 3) == 3
                    
            elif "[KERNEL PANIC] Unhandled Exception:" in line:
                in_exception_block = True
                crash_data = {"is_user": False}
                match = re.search(r"Exception:\s*(\d+|0x[0-9a-fA-F]+)", line)
                if match:
                    val = match.group(1)
                    crash_data["int_no"] = int(val, 16) if val.startswith("0x") else int(val)
                    
            elif in_exception_block:
                # Parse crash parameters
                if "Task ID:" in line:
                    match = re.search(r"Task ID:\s*(0x[0-9a-fA-F]+|\d+)", line)
                    if match:
                        crash_data["tid"] = match.group(1)
                        
                elif "PF addr:" in line:
                    match = re.search(r"PF addr:\s*(0x[0-9a-fA-F]+)", line)
                    if match:
                        crash_data["cr2"] = match.group(1)
                    match = re.search(r"err=(0x[0-9a-fA-F]+)", line)
                    if match:
                        crash_data["err_code"] = match.group(1)
                        
                elif "EIP:" in line:
                    for reg in ["EIP", "ESP", "EBP", "CS"]:
                        match = re.search(fr"{reg}:\s*(0x[0-9a-fA-F]+)", line)
                        if match:
                            crash_data[reg] = match.group(1)
                            
                elif "EAX:" in line:
                    for reg in ["EAX", "EBX", "ECX", "EDX"]:
                        match = re.search(fr"{reg}:\s*(0x[0-9a-fA-F]+)", line)
                        if match:
                            crash_data[reg] = match.group(1)
                            
                elif "CR3:" in line:
                    match = re.search(r"CR3:\s*(0x[0-9a-fA-F]+)", line)
                    if match:
                        crash_data["CR3"] = match.group(1)
                        
                    # CR3 is usually the last line printed in user crash
                    # Trigger the full report!
                    print_crash_report(crash_data)
                    in_exception_block = False
                    crash_data = {}
                    
                # For kernel panic, it halts on 'HLT', the last prints might be 'EIP=' and 'CS='
                elif "EIP=" in line and not crash_data.get("EIP"):
                    match = re.search(r"EIP=\s*(0x[0-9a-fA-F]+|\d+)", line)
                    if match:
                        val = match.group(1)
                        crash_data["EIP"] = val if val.startswith("0x") else f"0x{int(val):x}"
                    match = re.search(r"CS=\s*(0x[0-9a-fA-F]+|\d+)", line)
                    if match:
                        crash_data["CS"] = match.group(1)
                        
                    # For kernel panic, this is the end
                    print_crash_report(crash_data)
                    in_exception_block = False
                    crash_data = {}
    finally:
        if f:
            f.close()

if __name__ == "__main__":
    try:
        monitor_logs()
    except KeyboardInterrupt:
        print(f"\n{C_BOLD}{C_GREEN}[*] Live Debugger stopped. Happy hacking, bos alif!{C_RESET}")
        sys.exit(0)
