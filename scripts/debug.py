#!/usr/bin/env python3
# =============================================================================
#  MECTOV-OS LIVE DEBUGGER v3.0 — Smart Crash Symbolicator & OS Analyzer
#  Tracks: processes, windows, VMM, memory, syscalls, network, crashes
# =============================================================================
import os, sys, time, re, subprocess, glob, socket, collections, threading

# ── ANSI Colors ───────────────────────────────────────────────────────────────
R   = "\033[0m"
B   = "\033[1m"
DIM = "\033[2m"
RED = "\033[31m"; GRN = "\033[32m"; YLW = "\033[33m"
BLU = "\033[34m"; MAG = "\033[35m"; CYN = "\033[36m"; WHT = "\033[37m"
BGR = "\033[41m"; BGY = "\033[43m"; BGG = "\033[42m"; BGC = "\033[46m"

def clr(*parts): return "".join(parts) + R

# ── Config ────────────────────────────────────────────────────────────────────
LOG_FILE   = "serial_debug.log"
KERNEL_ELF = "myos.bin" if os.path.exists("myos.bin") else "myos.elf"
QEMU_HOST  = "127.0.0.1"
QEMU_PORT  = 45454

# ── x86 Exception Table ───────────────────────────────────────────────────────
EXCEPTIONS = {
    0:  ("Divide-by-Zero",            "Division by zero in integer arithmetic.",
         "Check for zero divisors before div/idiv instructions."),
    1:  ("Debug",                     "Single-step or hardware breakpoint.",
         "May be intentional; check if a debugger is attached."),
    2:  ("NMI",                       "Non-maskable interrupt.",
         "Hardware error or watchdog timeout."),
    3:  ("Breakpoint",                "INT3 software breakpoint.",
         "Leftover debug INT3 in production code."),
    4:  ("Overflow",                  "INTO detected overflow.",
         "Arithmetic overflow in signed operations."),
    5:  ("Bound Range",               "BOUND instruction check failed.",
         "Array index out of bounds."),
    6:  ("Invalid Opcode",            "CPU encountered an invalid instruction.",
         "Code corruption, wrong EIP, or executing data as code."),
    7:  ("Device Not Available",      "FPU/SSE instruction, FPU not available.",
         "Enable the FPU or add TS-bit handling."),
    8:  ("Double Fault",              "Second exception while handling first.",
         "Likely stack overflow or bad IDT/GDT entry."),
    9:  ("Coproc Overrun",            "Legacy FPU segment overrun.",
         "Rare; usually FPU memory boundary issue."),
    10: ("Invalid TSS",               "Task switch to invalid TSS.",
         "GDT/TSS descriptor corrupt or selector wrong."),
    11: ("Segment Not Present",       "Segment descriptor not present.",
         "Loading a selector with P=0; GDT corruption."),
    12: ("Stack-Segment Fault",       "Stack segment access fault.",
         "Stack pointer out of bounds or SS not present."),
    13: ("General Protection Fault",  "Ring-violation or bad selector access.",
         "User code accessing kernel memory, or misaligned instruction."),
    14: ("Page Fault",                "Virtual address not mapped or permissions violated.",
         "Check CR2 for faulting address and page table mappings."),
    16: ("x87 FP Exception",          "Floating-point arithmetic error.",
         "Check MXCSR/SW for the specific FP error flag."),
    17: ("Alignment Check",           "Unaligned memory access with AC flag set.",
         "Align data structures to their natural size."),
    18: ("Machine Check",             "Internal CPU / bus error.",
         "Hardware failure; check MCi_STATUS MSRs."),
    19: ("SIMD FP Exception",         "SSE/AVX floating-point error.",
         "Check MXCSR exception flags."),
    20: ("Virtualization",            "EPT violation or VMX error.",
         "Check VM-exit reason registers."),
    30: ("Security Exception",        "SGX or security-sensitive access.",
         "Check if SEV/SGX is involved."),
    255:("Spurious / Corrupt Stack",  "Unregistered or spurious interrupt.",
         "Likely stack smash or bad IDT handler pointer."),
}

# ── Syscall Number Map (from syscall.h) ───────────────────────────────────────
SYSCALLS = {
    1:"SYS_PRINT", 2:"SYS_OPEN", 3:"SYS_READ", 4:"SYS_WRITE",
    5:"SYS_CLOSE", 6:"SYS_MALLOC", 7:"SYS_FREE", 8:"SYS_GET_TICKS",
    9:"SYS_YIELD", 10:"SYS_EXIT", 11:"SYS_DRAW_RECT", 12:"SYS_DRAW_TEXT",
    13:"SYS_GET_KEY", 14:"SYS_GET_MOUSE", 15:"SYS_CREATE_WINDOW",
    16:"SYS_GET_EVENT", 17:"SYS_UPDATE_WINDOW", 18:"SYS_THREAD_CREATE",
    19:"SYS_SLEEP", 20:"SYS_GET_PID", 21:"SYS_SET_PRIORITY",
    22:"SYS_GET_PRIORITY", 23:"SYS_IPC_CREATE", 24:"SYS_IPC_SEND",
    25:"SYS_IPC_RECV", 26:"SYS_IPC_DESTROY", 27:"SYS_IPC_TRY_SEND",
    28:"SYS_IPC_TRY_RECV", 29:"SYS_VMM_MAP", 30:"SYS_VMM_ALLOC",
    31:"SYS_VMM_FREE", 32:"SYS_PIPE", 33:"SYS_GET_TIME",
    34:"SYS_PLAY_SOUND", 35:"SYS_GET_SYSINFO", 36:"SYS_GET_PCI_INFO",
    37:"SYS_LIST_DIR", 38:"SYS_STAT_FILE", 39:"SYS_DNS_RESOLVE",
    40:"SYS_TCP_CONNECT", 41:"SYS_TCP_SEND", 42:"SYS_TCP_RECV",
    43:"SYS_NET_STATUS", 44:"SYS_SET_STDOUT_IPC", 45:"SYS_EXEC_CMD",
    46:"SYS_GET_TASKS", 47:"SYS_GET_WINDOWS", 48:"SYS_KILL_TASK",
    49:"SYS_GET_LAUNCH_ARG", 50:"SYS_CREATE_FILE", 51:"SYS_LOAD_LIBRARY",
    93:"SYS_MMAP_FILE", 94:"SYS_MSYNC",
    95:"SYS_LSEEK", 96:"SYS_FSTAT",
}

# ══════════════════════════════════════════════════════════════════════════════
#  LIVE OS STATE TRACKER
# ══════════════════════════════════════════════════════════════════════════════
class OSState:
    """Maintains a live picture of what's happening inside Mectov OS."""

    def __init__(self):
        # { tid: {"name": str, "state": str, "ring": int, "wins": [wid] } }
        self.tasks  = {}
        # { wid: {"title": str, "tid": int, "state": str } }
        self.windows = {}
        # Recent event timeline  (deque of str)
        self.timeline = collections.deque(maxlen=200)
        # Syscall rate (count per second bucket)
        self.syscall_counts = collections.defaultdict(int)
        self._last_syscall_ts = time.time()
        # VMM / memory events
        self.vmm_oom_count = 0
        self.cow_count     = 0
        self.page_faults   = 0
        # Net events
        self.dns_queries   = []
        self.tcp_connects  = []
        # Crash history
        self.crashes       = []
        # Boot phase
        self.booted        = False
        self.boot_ts       = None
        # Loader events  { tid: [event_str] }
        self.loader_events = collections.defaultdict(list)
        # Pending loader context (current app being loaded)
        self._loading_app  = None
        # IPC queues  { key: qid }
        self.ipc_queues    = {}
        # Exit events
        self.exits         = []

    def event(self, msg, ts=None):
        ts = ts or time.strftime("%H:%M:%S")
        self.timeline.append(f"[{ts}] {msg}")

    def on_task_create(self, tid, ring=3):
        if tid not in self.tasks:
            self.tasks[tid] = {"name": f"task_{tid}", "state": "running",
                               "ring": ring, "wins": [], "loaded_at": time.time()}
        self.event(f"TASK CREATE  tid={tid} ring={ring}")

    def on_task_exit(self, tid):
        name = self.tasks.get(tid, {}).get("name", f"task_{tid}")
        if tid in self.tasks:
            self.tasks[tid]["state"] = "dead"
        self.exits.append({"tid": tid, "name": name, "ts": time.time()})
        self.event(f"TASK EXIT    tid={tid} ({name})")

    def on_task_kill(self, tid):
        name = self.tasks.get(tid, {}).get("name", f"task_{tid}")
        if tid in self.tasks:
            self.tasks[tid]["state"] = "killed"
        self.event(f"TASK KILL    tid={tid} ({name})")

    def on_window_open(self, wid, tid=None):
        self.windows[wid] = {"title": "?", "tid": tid, "state": "open"}
        if tid and tid in self.tasks:
            self.tasks[tid]["wins"].append(wid)
        self.event(f"WIN OPEN     wid={wid} owner={tid}")

    def on_window_close(self, wid):
        title = self.windows.get(wid, {}).get("title", "?")
        if wid in self.windows:
            self.windows[wid]["state"] = "closed"
        self.event(f"WIN CLOSE    wid={wid} ({title})")

    def on_loader_start(self, app=None):
        self._loading_app = app
        self.event(f"LOADER START app={app or '?'}")

    def on_loader_ok(self):
        self.event(f"LOADER OK    app={self._loading_app or '?'}")
        self._loading_app = None

    def on_loader_fail(self, reason):
        self.event(f"LOADER FAIL  app={self._loading_app or '?'} reason={reason}")
        self._loading_app = None

    def on_vmm_oom(self):
        self.vmm_oom_count += 1
        self.event(f"VMM OOM      #{self.vmm_oom_count}")

    def on_cow(self, addr):
        self.cow_count += 1
        self.event(f"COW COPY     addr={addr}")

    def on_page_fault(self):
        self.page_faults += 1

    def on_crash(self, info):
        self.crashes.append({**info, "ts": time.time()})
        self.event(f"CRASH        int={info.get('int_no','?')} eip={info.get('EIP','?')}")

    def on_dns(self, domain):
        self.dns_queries.append(domain)
        self.event(f"DNS QUERY    {domain}")

    def on_tcp(self, dst):
        self.tcp_connects.append(dst)
        self.event(f"TCP CONNECT  {dst}")

    def on_ipc_create(self, key, qid):
        self.ipc_queues[key] = qid
        self.event(f"IPC CREATE   key={key} qid={qid}")

    def summary(self):
        alive  = [t for t, v in self.tasks.items()  if v["state"] == "running"]
        dead   = [t for t, v in self.tasks.items()  if v["state"] in ("dead","killed")]
        open_w = [w for w, v in self.windows.items() if v["state"] == "open"]
        up = f"{int(time.time()-self.boot_ts)}s" if self.boot_ts else "?"
        return {
            "uptime": up,
            "tasks_alive": len(alive),
            "tasks_dead":  len(dead),
            "windows_open": len(open_w),
            "page_faults": self.page_faults,
            "cow_copies":  self.cow_count,
            "vmm_oom":     self.vmm_oom_count,
            "crashes":     len(self.crashes),
            "dns_queries": len(self.dns_queries),
            "tcp_connects":len(self.tcp_connects),
        }

# ══════════════════════════════════════════════════════════════════════════════
#  SYMBOLICATION
# ══════════════════════════════════════════════════════════════════════════════
_elf_cache = None

def find_all_elfs():
    global _elf_cache
    if _elf_cache is None:
        _elf_cache = glob.glob("**/*.elf", recursive=True)
    return _elf_cache

def get_source_line(fpath, lineno):
    if not fpath or not os.path.exists(fpath): return None
    try:
        with open(fpath, "r", encoding="utf-8", errors="ignore") as f:
            lines = f.readlines()
            if 1 <= lineno <= len(lines):
                return lines[lineno-1].strip()
    except Exception: pass
    return None

def addr2line(elf, addr_hex):
    try:
        out = subprocess.check_output(
            ["addr2line", "-e", elf, "-f", "-p", addr_hex],
            stderr=subprocess.DEVNULL, timeout=3
        ).decode().strip()
        if out.startswith("??"): return None
        m = re.match(r"(.*) at (.+):(\d+|\?)", out)
        if m:
            func = m.group(1).strip()
            fpath = m.group(2).strip()
            lineno = int(m.group(3)) if m.group(3).isdigit() else 0
            short = os.path.relpath(fpath) if os.path.isabs(fpath) else fpath
            code  = get_source_line(fpath, lineno) if lineno > 0 else None
            return {"elf": elf, "func": func, "file": short,
                    "line": lineno or "?", "code": code}
    except Exception: pass
    return None

def symbolicate(eip_hex, is_user=False):
    try: val = int(eip_hex, 16)
    except ValueError: return None

    is_user_space = is_user or val >= 0x08000000

    # Executing in heap/stack region (not code segment) = stack exec
    if is_user_space and val < 0x08000000:
        return {"elf":"USER_STACK", "func":"<Stack Execution>",
                "file":"Corrupt return address / stack smash",
                "line":0, "code":None, "is_user":True, "stack_exec":True}

    if is_user_space:
        for elf in find_all_elfs():
            if elf == KERNEL_ELF: continue
            r = addr2line(elf, eip_hex)
            if r: r["is_user"] = True; return r
    else:
        if os.path.exists(KERNEL_ELF):
            r = addr2line(KERNEL_ELF, eip_hex)
            if r: r["is_user"] = False; return r
    return None

# ══════════════════════════════════════════════════════════════════════════════
#  DIAGNOSIS ENGINE  — root-cause hints per crash
# ══════════════════════════════════════════════════════════════════════════════
def diagnose(crash, sym, state: OSState):
    """Return a list of human-readable root-cause hypothesis strings."""
    hints = []
    int_no  = crash.get("int_no")
    cr2     = crash.get("cr2", "")
    err_val = int(crash.get("err_code", "0x0"), 16) if crash.get("err_code") else 0
    is_user = crash.get("is_user", False)
    eip     = crash.get("EIP", "")
    esp     = crash.get("ESP", "")
    tid     = crash.get("tid")

    exc_entry = EXCEPTIONS.get(int_no, ("Unknown", "", ""))

    # ── Page Fault (14) ──────────────────────────────────────────────────────
    if int_no == 14:
        p      = err_val & 1   # present
        write  = err_val & 2
        user   = err_val & 4
        fetch  = err_val & 16

        if not p:
            hints.append("Page not present — pointer to unmapped/freed memory.")
        if write and not p:
            hints.append("Write to unmapped page — likely NULL/wild pointer write.")
        if fetch:
            hints.append("Instruction fetch fault — EIP jumped to unmapped memory.")
        if cr2:
            try:
                cr2v = int(cr2, 16)
                if cr2v < 0x1000:
                    hints.append(f"CR2={cr2} is near NULL → classic NULL-pointer dereference.")
                elif cr2v >= 0x08000000 and cr2v < 0x10000000:
                    hints.append(f"CR2={cr2} is in user heap/stack — possible heap overflow or use-after-free.")
                elif cr2v > 0xC0000000:
                    hints.append(f"CR2={cr2} in kernel-virtual range — user code tried to access kernel memory (missing boundary check).")
                if is_user and not user:
                    hints.append("User-mode code hit a supervisor-only page — missing user-bit in page table entry.")
            except ValueError: pass

        if state.vmm_oom_count > 0:
            hints.append(f"VMM ran out of frames {state.vmm_oom_count}× before crash — memory exhaustion likely contributed.")

    # ── General Protection Fault (13) ────────────────────────────────────────
    elif int_no == 13:
        hints.append("GPF: Ring 3 code accessed a privileged resource or used bad selector.")
        if is_user:
            hints.append("User task likely executed a privileged instruction (IN/OUT/CLI/etc.).")

    # ── Invalid Opcode (6) ───────────────────────────────────────────────────
    elif int_no == 6:
        hints.append("Invalid opcode — EIP may point to data, not code (code corruption or bad function pointer).")
        if sym and sym.get("stack_exec"):
            hints.append("EIP is in stack memory → return address was overwritten (buffer overflow).")

    # ── Double Fault (8) ─────────────────────────────────────────────────────
    elif int_no == 8:
        hints.append("Double Fault — almost always a stack overflow in kernel mode, or bad IDT/GDT descriptor.")

    # ── Stack-Segment Fault (12) ─────────────────────────────────────────────
    elif int_no == 12:
        hints.append("Stack-segment fault — kernel or user stack overflowed its allocated region.")
        if esp:
            try:
                espv = int(esp, 16)
                if espv < 0x1000:
                    hints.append(f"ESP={esp} is near zero → stack completely exhausted.")
            except ValueError: pass

    # ── Context from OS state ─────────────────────────────────────────────────
    if tid:
        task_info = state.tasks.get(int(tid, 16) if isinstance(tid, str) and tid.startswith("0x") else int(tid, 10) if isinstance(tid, str) else tid)
        if task_info:
            name = task_info.get("name","?")
            hints.append(f"Crashed task: TID={tid} name='{name}' ring={task_info.get('ring','?')}.")
            wins = task_info.get("wins",[])
            if wins:
                hints.append(f"Task had {len(wins)} open window(s) — check if window callbacks are valid after task death.")

    recent_ooms = state.vmm_oom_count
    if recent_ooms:
        hints.append(f"⚠ VMM OOM happened {recent_ooms}× — physical frame exhaustion may have corrupted heap.")

    recent_cow = state.cow_count
    if recent_cow and int_no == 14:
        hints.append(f"COW was active ({recent_cow} copies made) — check COW reference counts.")

    if not hints:
        exc_tip = exc_entry[2] if len(exc_entry) > 2 else ""
        if exc_tip:
            hints.append(exc_tip)

    return hints

# ══════════════════════════════════════════════════════════════════════════════
#  CRASH REPORT PRINTER
# ══════════════════════════════════════════════════════════════════════════════
def print_crash_report(crash, state: OSState):
    int_no   = crash.get("int_no")
    eip      = crash.get("EIP")
    cr2      = crash.get("cr2")
    err_code = crash.get("err_code")
    tid      = crash.get("tid")
    is_user  = crash.get("is_user", False)

    exc_entry = EXCEPTIONS.get(int_no, ("Unknown Exception", "No description.", ""))
    exc_name, exc_desc, _ = (exc_entry + ("","",""))[:3]
    sym = symbolicate(eip, is_user) if eip else None

    w = 66
    print()
    print(clr(B, RED, "╔" + "═"*w + "╗"))
    print(clr(B, RED, "║", WHT, f"{'  💥  MECTOV OS — CRASH REPORT  💥':^{w}}", RED, "║"))
    print(clr(B, RED, "╚" + "═"*w + "╝"))

    # ── Exception type ──────────────────────────────────────────────────────
    int_str = f"int #{int_no}" if int_no is not None else "?"
    print(clr(B, WHT, f"\n  Exception : ") + clr(B, RED, f"{int_str}  {exc_name}"))
    print(clr(DIM, f"  Detail    : {exc_desc}"))

    # ── Task context ─────────────────────────────────────────────────────────
    if tid is not None:
        mode = clr(BGR, WHT, " RING 3 ") if is_user else clr(BGC, WHT, " KERNEL ")
        print(clr(B, WHT, f"  Task      : ") + f"TID {clr(CYN, str(tid))}  {mode}")

    # ── Page Fault address ───────────────────────────────────────────────────
    if cr2:
        print(clr(B, WHT, f"  CR2 (PF)  : ") + clr(B, RED, cr2))
    if err_code:
        ev = int(err_code, 16)
        flags = " | ".join(filter(None, [
            "Present"         if ev & 1  else "Not-Present",
            "Write"           if ev & 2  else "Read",
            "User"            if ev & 4  else "Supervisor",
            "ReservedBitViol" if ev & 8  else "",
            "InstrFetch"      if ev & 16 else "",
        ]))
        print(clr(B, WHT, f"  PF Flags  : ") + clr(YLW, f"{err_code}  ({flags})"))

    # ── EIP ──────────────────────────────────────────────────────────────────
    print(clr(B, WHT, f"  EIP       : ") + clr(GRN, B, eip or "?"))

    # ── Symbolication ────────────────────────────────────────────────────────
    if sym:
        if sym.get("stack_exec"):
            print(clr(B, BGR, WHT, "\n  🚨 STACK EXECUTION DETECTED — return address smashed! "))
        else:
            kind = "Ring-3 App" if sym["is_user"] else "Kernel"
            print(clr(B, GRN, f"\n  ✓ Symbolicated ({kind}):"))
            print(f"    Binary   : {clr(MAG, sym['elf'])}")
            print(f"    Function : {clr(CYN, B, sym['func']+'()')}")
            print(f"    Source   : {clr(WHT, sym['file'])} : {clr(YLW, B, 'Line '+str(sym['line']))}")
            if sym["code"]:
                print(f"\n    {clr(B,'Code →')}  {clr(BGY, WHT, ' '+sym['code']+' ')}")
    else:
        print(clr(YLW, f"\n  ⚠ Symbolication failed for EIP {eip}."))
        print(clr(DIM,  "    Compile with debug info and ensure ELFs are present."))

    # ── Registers ────────────────────────────────────────────────────────────
    print(clr(B, WHT, "\n  Registers:"))
    regs = [("EAX",crash.get("EAX","?")),("EBX",crash.get("EBX","?")),
            ("ECX",crash.get("ECX","?")),("EDX",crash.get("EDX","?")),
            ("ESP",crash.get("ESP","?")),("EBP",crash.get("EBP","?")),
            ("CR3",crash.get("CR3","?"))]
    row = "    "
    for i,(name,val) in enumerate(regs):
        row += clr(DIM, name+"=") + clr(YLW, val) + "  "
        if (i+1) % 4 == 0: print(row); row = "    "
    if row.strip(): print(row)

    # ── Diagnosis ────────────────────────────────────────────────────────────
    hints = diagnose(crash, sym, state)
    if hints:
        print(clr(B, CYN, "\n  🔍 Root-Cause Diagnosis:"))
        for h in hints:
            print(f"    {clr(YLW,'▸')} {h}")

    # ── Recent timeline context ───────────────────────────────────────────────
    recent = list(state.timeline)[-8:]
    if recent:
        print(clr(B, BLU, "\n  📋 Last 8 OS Events Before Crash:"))
        for ev in recent:
            print(clr(DIM, "    " + ev))

    print(clr(B, RED, "\n" + "─"*68 + "\n"))

# ══════════════════════════════════════════════════════════════════════════════
#  STATUS BAR  (printed periodically)
# ══════════════════════════════════════════════════════════════════════════════
_last_status = 0
def maybe_print_status(state: OSState, force=False):
    global _last_status
    now = time.time()
    if not force and now - _last_status < 10:
        return
    _last_status = now
    s = state.summary()
    alive  = [f"{t}({v['name']})" for t,v in state.tasks.items() if v["state"]=="running"]
    open_w = [f"{w}({v['title']})" for w,v in state.windows.items() if v["state"]=="open"]

    print(clr(DIM, "\n┌─── OS Status ─────────────────────────────────────────────────"))
    print(clr(DIM, f"│ Uptime: {s['uptime']}  Tasks: {s['tasks_alive']} alive / {s['tasks_dead']} exited"
              f"  Windows: {s['windows_open']}  Crashes: {s['crashes']}"))
    if alive:
        print(clr(DIM, f"│ Running: {', '.join(alive[:6])}{'…' if len(alive)>6 else ''}"))
    if open_w:
        print(clr(DIM, f"│ Windows: {', '.join(open_w[:6])}{'…' if len(open_w)>6 else ''}"))
    if s['page_faults'] or s['vmm_oom'] or s['cow_copies']:
        print(clr(DIM, f"│ Memory : PF={s['page_faults']}  COW={s['cow_copies']}  OOM={s['vmm_oom']}"))
    if s['dns_queries'] or s['tcp_connects']:
        print(clr(DIM, f"│ Network: DNS={s['dns_queries']}  TCP={s['tcp_connects']}"))
    print(clr(DIM, "└───────────────────────────────────────────────────────────────\n"))

# ══════════════════════════════════════════════════════════════════════════════
#  LINE COLORIZER
# ══════════════════════════════════════════════════════════════════════════════
def colorize(line):
    l = line.rstrip("\n")
    # Critical
    if "[EXCEPTION]" in l or "[KERNEL PANIC]" in l:
        return clr(B, BGR, WHT, f" {l} ")
    if "[CRASH]" in l:
        return clr(B, RED, l)
    if "OUT OF PHYSICAL FRAMES" in l or "VMM OOM" in l or "OOM" in l:
        return clr(B, BGR, WHT, f" {l} ")
    if "FAIL" in l and not l.startswith("[NET]"):
        return clr(RED, l)
    # Subsystems
    if l.startswith("[LOADER]"):  return clr(MAG, l)
    if l.startswith("[TASK]"):    return clr(CYN, l)
    if l.startswith("[SYSCALL]"): return clr(BLU, l)
    if l.startswith("[SYS]"):     return clr(BLU, l)
    if l.startswith("[VMM]"):     return clr(YLW, l)
    if l.startswith("[COW]"):     return clr(MAG, l)
    if l.startswith("[NET]"):     return clr(GRN, l)
    if l.startswith("[TCP]"):     return clr(GRN, l)
    if l.startswith("[VFS]"):     return clr(CYN, l)
    if l.startswith("[IPC]"):     return clr(MAG, l)
    if l.startswith("[WM]") or l.startswith("WM_"): return clr(BLU, l)
    if l.startswith("[EXT2]"):    return clr(DIM, l)
    if l.startswith("[K]") or l.startswith("[KERNEL]"): return clr(GRN, l)
    if l.startswith("[SB16]"):    return clr(DIM, l)
    if l.startswith("[*]"):       return clr(GRN, l)
    if l.startswith("[!]"):       return clr(YLW, l)
    if l.startswith("[-]"):       return clr(RED, l)
    # Register dumps
    if any(r in l for r in ("EIP:","ESP:","EBP:","EAX:","EBX:","CR3:","PF addr:")):
        return clr(YLW, l)
    return l

# ══════════════════════════════════════════════════════════════════════════════
#  LINE PARSER  — updates OSState from serial output
# ══════════════════════════════════════════════════════════════════════════════
def parse_line(line: str, state: OSState, crash: dict, in_exc: list):
    """
    Returns updated (crash_data, in_exception_block).
    in_exc is a 1-element list so we can mutate it inside the function.
    """

    # ── Boot detection ──────────────────────────────────────────────────────
    if "BOOTED KERNEL LOOP" in line or "[KERNEL] boot start" in line:
        if not state.booted:
            state.booted = True
            state.boot_ts = time.time()
            state.event("KERNEL BOOTED")

    # ── Loader events ───────────────────────────────────────────────────────
    if "[LOADER] start" in line:
        state.on_loader_start()
    elif "[LOADER] OK" in line or line.strip() == "OK":
        state.on_loader_ok()
    elif "[LOADER] NOT FOUND" in line:
        state.on_loader_fail("NOT FOUND")
    elif "[LOADER] invalid size" in line or "[LOADER] too small" in line:
        state.on_loader_fail("BAD SIZE")
    elif "[LOADER] Invalid magic" in line:
        state.on_loader_fail("INVALID MAGIC")
    elif "[LOADER] load lib:" in line:
        m = re.search(r"load lib:\s*(\S+)", line)
        state.event(f"LOAD LIB  {m.group(1) if m else '?'}")

    # ── Task events ─────────────────────────────────────────────────────────
    elif "[TASK] Ring 3 task created OK" in line:
        # We'll pick up tid from the next context; create placeholder
        state.event("TASK Ring3 created")
    elif "[TASK] create_user_task" in line:
        state.event("TASK create_user_task")
    elif "[TASK] No free slots!" in line:
        state.event("TASK SLOT EXHAUSTED — too many concurrent tasks!")
        print(clr(B, BGR, WHT, "  ⚠ TASK SLOT EXHAUSTION — no free task slots!  "))
    elif "[TASK] task_exit tid=" in line:
        m = re.search(r"tid=([0-9a-fA-Fx]+)", line)
        if m:
            try: tid = int(m.group(1), 16) if m.group(1).startswith("0x") else int(m.group(1))
            except: tid = 0
            state.on_task_exit(tid)
    elif "[TASK] task_kill:" in line:
        m = re.search(r"killed task\s+(\d+)", line)
        if m: state.on_task_kill(int(m.group(1)))
    elif "[SYSCALL] SYS_EXIT from TID=" in line:
        m = re.search(r"TID=(\d+)", line)
        if m: state.on_task_exit(int(m.group(1)))

    # ── Window events ───────────────────────────────────────────────────────
    elif "WM_OPEN id=" in line:
        m = re.search(r"id=(0x[0-9a-fA-F]+)", line)
        if m:
            wid = int(m.group(1), 16)
            state.on_window_open(wid)
    elif "WM_CLOSE called for id=" in line:
        m = re.search(r"id=(0x[0-9a-fA-F]+)", line)
        if m:
            state.on_window_close(int(m.group(1), 16))
    elif "[WM] closing zombie window:" in line:
        m = re.search(r"window:\s*(.+)", line)
        title = m.group(1).strip() if m else "?"
        state.event(f"WM ZOMBIE CLEANUP  '{title}'")

    # ── VMM / Memory events ─────────────────────────────────────────────────
    elif "OUT OF PHYSICAL FRAMES" in line or "VMM OOM" in line:
        state.on_vmm_oom()
    elif "[COW] Duplicated page at" in line:
        m = re.search(r"at\s+(0x[0-9a-fA-F]+)", line)
        state.on_cow(m.group(1) if m else "?")
    elif "[COW] OUT OF PHYSICAL FRAMES" in line:
        state.on_vmm_oom()
        print(clr(B, BGR, WHT, "  ⚠ COW: OUT OF FRAMES — crash imminent!  "))
    elif "[VMM] alloc_page_at: NO FRAME" in line:
        state.on_vmm_oom()

    # ── IPC events ──────────────────────────────────────────────────────────
    elif "[IPC] Created queue key=" in line or "[SYS] ipc_create" in line:
        mk = re.search(r"key=(0x[0-9a-fA-F]+)", line)
        mq = re.search(r"qid=(\d+)", line)
        if mk and mq:
            state.on_ipc_create(mk.group(1), int(mq.group(1)))

    # ── Network events ──────────────────────────────────────────────────────
    elif "[SYSCALL] SYS_DNS_RESOLVE" in line or "Sending DNS query for domain:" in line:
        m = re.search(r"domain[:\s]+(\S+)", line)
        state.on_dns(m.group(1) if m else "?")
    elif "[NET] DNS A-record resolved" in line:
        state.event("DNS RESOLVED")
    elif "net_tcp_connect" in line or "[SYSCALL] SYS_TCP_CONNECT" in line:
        state.on_tcp("?")

    # ── Page Fault tracking (non-crash) ─────────────────────────────────────
    elif "[COW] Promoted sole-owned page" in line:
        state.page_faults += 1  # was handled as COW, not crash

    # ═══════════════════════════════════════════════════════════════════════
    #  CRASH DETECTION STATE MACHINE
    # ═══════════════════════════════════════════════════════════════════════
    if "[EXCEPTION]" in line:
        in_exc[0] = True
        crash.clear()
        crash["is_user"] = False

        m = re.search(r"int_no=(0x[0-9a-fA-F]+)", line)
        if m: crash["int_no"] = int(m.group(1), 16)

        m = re.search(r"CS=(0x[0-9a-fA-F]+)", line)
        if m:
            cs = int(m.group(1), 16)
            crash["is_user"] = (cs & 3) == 3

    elif "[KERNEL PANIC] Unhandled Exception:" in line:
        in_exc[0] = True
        crash.clear()
        crash["is_user"] = False
        m = re.search(r"Exception:\s*(0x[0-9a-fA-F]+|\d+)", line)
        if m:
            v = m.group(1)
            crash["int_no"] = int(v, 16) if v.startswith("0x") else int(v)

    elif in_exc[0]:
        # Parse all crash fields
        for tag in ("EIP","ESP","EBP","EAX","EBX","ECX","EDX","CR3","CS"):
            pat = fr"{tag}:\s*(0x[0-9a-fA-F]+)"
            m = re.search(pat, line)
            if m: crash[tag] = m.group(1)

        if "Task ID:" in line or "[CRASH] Task ID:" in line:
            m = re.search(r"Task ID[:\s]+(0x[0-9a-fA-F]+|\d+)", line)
            if m: crash["tid"] = m.group(1)

        if "PF addr:" in line:
            m = re.search(r"PF addr:\s*(0x[0-9a-fA-F]+)", line)
            if m: crash["cr2"] = m.group(1)
            m = re.search(r"err=(0x[0-9a-fA-F]+)", line)
            if m: crash["err_code"] = m.group(1)
            state.on_page_fault()

        # Kernel panic uses "EIP=" format
        m = re.search(r"\bEIP=(0x[0-9a-fA-F]+|\d+)\b", line)
        if m and not crash.get("EIP"):
            v = m.group(1)
            crash["EIP"] = v if v.startswith("0x") else f"0x{int(v):x}"

        # Final line of crash block: CR3 (Ring3) or the EIP=/CS= line (kernel)
        triggered = False
        if "CR3:" in line and crash.get("EIP"):
            triggered = True
        elif "EIP=" in line and crash.get("EIP") and not crash.get("CR3"):
            triggered = True

        if triggered:
            state.on_crash(crash.copy())
            print_crash_report(crash.copy(), state)
            in_exc[0] = False
            crash.clear()

    return crash, in_exc

# ══════════════════════════════════════════════════════════════════════════════
#  MONITOR LOOP
# ══════════════════════════════════════════════════════════════════════════════
def monitor():
    state   = OSState()
    crash   = {}
    in_exc  = [False]
    f       = None
    use_sock = False

    print(clr(B, CYN, "╔════════════════════════════════════════════════════════════╗"))
    print(clr(B, CYN, "║      MECTOV-OS LIVE DEBUGGER v3.0  —  Smart Analyzer       ║"))
    print(clr(B, CYN, "╚════════════════════════════════════════════════════════════╝"))
    print(clr(DIM, f"  Kernel ELF : {KERNEL_ELF}"))
    print(clr(DIM, f"  ELF pool   : {len(find_all_elfs())} app ELFs found"))
    print(clr(DIM, f"  Log file   : {LOG_FILE}"))
    print()

    # ── Connect to QEMU TCP socket ───────────────────────────────────────────
    print(clr(WHT, f"[*] Trying QEMU serial socket {QEMU_HOST}:{QEMU_PORT}..."))
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(2.0)
    try:
        s.connect((QEMU_HOST, QEMU_PORT))
        s.settimeout(None)
        print(clr(GRN, B, f"[+] Connected to QEMU serial socket!"))
        f = s.makefile("r", encoding="utf-8", errors="replace")
        use_sock = True
    except Exception as e:
        print(clr(YLW, f"[!] Socket failed ({e}) — falling back to log file."))

    if not use_sock:
        while not os.path.exists(LOG_FILE):
            print(clr(DIM, f"[*] Waiting for {LOG_FILE}..."))
            time.sleep(1)
        print(clr(GRN, f"[+] Tailing {LOG_FILE}"))
        f = open(LOG_FILE, "r", encoding="utf-8", errors="replace")
        f.seek(0, os.SEEK_END)

    print(clr(GRN, "[+] Monitoring started. Press Ctrl-C to quit.\n"))

    try:
        while True:
            if use_sock:
                try:
                    line = f.readline()
                    if not line:
                        print(clr(RED, B, "[!] QEMU socket closed."))
                        break
                except socket.timeout: continue
                except Exception as e:
                    print(clr(RED, f"[!] Socket error: {e}"))
                    break
            else:
                pos  = f.tell()
                line = f.readline()
                if not line:
                    f.seek(0, os.SEEK_END)
                    if f.tell() < pos: f.seek(0); continue
                    f.seek(pos); time.sleep(0.04); continue

            print(colorize(line), end="", flush=True)
            parse_line(line, state, crash, in_exc)
            maybe_print_status(state)

    except KeyboardInterrupt:
        pass
    finally:
        if f: f.close()

    # ── Exit summary ─────────────────────────────────────────────────────────
    print()
    maybe_print_status(state, force=True)

    if state.crashes:
        print(clr(B, RED, f"\n  ⚠ {len(state.crashes)} crash(es) recorded during session:"))
        for i, c in enumerate(state.crashes, 1):
            int_no = c.get("int_no", "?")
            name   = EXCEPTIONS.get(int_no, ("?",))[0]
            print(f"    {i}. int#{int_no} ({name})  EIP={c.get('EIP','?')}  TID={c.get('tid','?')}")

    if state.timeline:
        print(clr(B, BLU, "\n  Last 12 OS events:"))
        for ev in list(state.timeline)[-12:]:
            print(clr(DIM, "    " + ev))

    print(clr(GRN, B, "\n[*] Debugger stopped. Happy hacking, bos alif!\n"))


if __name__ == "__main__":
    monitor()
