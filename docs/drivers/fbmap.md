# Ring 3 Scanout Takeover — `SYS_FB_MAP` / `SYS_FB_RELEASE`

A Ring 3 task can own the display's scanout directly: `SYS_FB_MAP` (118) maps the VBE linear framebuffer into the caller's address space as `PAGE_DEV` memory, the kernel desktop suppresses its composition, and `SYS_FB_RELEASE` (119) hands it back.

---

## 🎯 Scope (v38.53)

* **Syscalls**: `SYS_FB_MAP (118)` → `fb_info_t {base,width,height,pitch,bpp}`, `SYS_FB_RELEASE (119)` — both in `src/include/syscall.h:30`, `src/sys/syscall_gui.c:14`.
* **Memory type**: `PAGE_DEV` (available bit 11, `src/include/mem.h:4` beside `PAGE_COW`/`PAGE_SHARED`) — `vmm.c` clone and free walkers skip it; never inherited on fork, never refcounted.
* **Authorization**: logind-style — `uid 0` or the controlling terminal's foreground `pgrp` (the active console session); background `!TCGETPGRP` is refused (`src/sys/task.c`).
* **App**: `apps/fbmap.c:111` — gradient + bouncing box 120 frames, then mouse-chase until key, `MAX_FRAMES=2400` cap.

---

## 🔌 How a Ring 3 app owns pixels

```
+---------------------------------------------------------------+
|  fbmap.mct: gradient + box + crosshair via put_px()           |
+---------------------------------------------------------------+
|  SYS_FB_MAP → fb_info_t → direct writes to fb.base            |
+---------------------------------------------------------------+
|  PAGE_DEV PTEs (RW+USER+NX, eager map, never COWed)           |
+---------------------------------------------------------------+
|  Authorization: uid 0 or fg pgrp (task.c)                      |
+---------------------------------------------------------------+
```

### Map path

1. Caller `sys_fb_map(&fb)` — kernel checks `page_dir !=0` (no boot tasks), `uid`/`pgrp`, no existing owner.
2. `vmm` maps `fb_size` eagerly (`PAGE_PRESENT|PAGE_RW|PAGE_USER|PAGE_DEV|PAGE_NX` — W^X) into the caller's `cr3`; `fb_info_t` returned.
3. While owned, `kernel.c` `fb_active` branch stops `full_redraw`/cursor/WM; only `kbd pump`/`gdb`/`net`/`reap`/`autolock` run.

### Release path

* `sys_fb_release()` owner-only; `task_cleanup` (exit/kill) and `exec` discard the region table and clear `fb_owner` + `full_redraw`.

### Input routing

* Holder receives every key via the main-loop pump that also feeds `SYS_GET_KEY` (gate accepts owner); mouse via `SYS_GET_MOUSE` (global). No `full_redraw` means no cursor — app draws its own crosshair.

### Spawn race fix

`pgrp` + `fg` handoff moved inside `load_mct_app_fg`'s `cli` window — an app could otherwise run before `cmd_run` finished handing over the terminal and fail the `SYS_FB_MAP` authz check.

---

## 🔒 Locking & lifetime

No new lock — `task_lock` for owner, `vmm` for PTEs. `PAGE_DEV` skipped in `vmm_clone_address_space` and both `vmm_free` walks — closes an unbounded `frame_ref_count[]` OOB on MMIO above RAM.

---

## 🧪 Testing

`scripts/fbmap_test.py` (CI: "Ring 3 scanout takeover regression"):

1. Boot → login → launch `fbmap`
2. **Scenario A**: grant → 120-frame gradient, typed key routed via pump → `SYS_FB_RELEASE` → `[FBMAP] desktop restored`
3. **Scenario B**: relaunch → `Ctrl+C` mid-takeover → `task_cleanup` drops ownership → desktop restored — zero `[PANIC]`
