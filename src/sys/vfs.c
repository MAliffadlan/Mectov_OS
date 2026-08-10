// ============================================================
// vfs.c — Mectov OS Virtual File System with Directory Support
// ============================================================
// Layout pada ATA disk (konstanta di vfs.h):
//   Sector 0      : Magic signature "MECTOVFS" + metadata
//   Sector 1..256 : Node table (256 nodes × 512 bytes = 128KB)
//   Sector 257+   : File data blocks (per file, kontigu)
// ============================================================

#include "../include/vfs.h"
#include "../include/ata.h"
#include "../include/utils.h"
#include "../include/mem.h"
#include "../include/ext2.h"
#include "../include/fd.h"
#include "../include/task.h"
#include "../include/spinlock.h"

// ---- Reentrant irqsave lock (kernel locking audit v38.4) ----
//
// Protects the node table (fs_nodes[]), sector allocation, and the on-disk
// image (vfs_save/load) from concurrent syscalls on different cores. Public
// VFS functions call each other (e.g. vfs_write_file -> vfs_get_node), so the
// lock tracks owner (cpu,tid) + depth: a nested call on the same task is a
// no-op. Process context only — nothing touches VFS from an IRQ. Ordering:
// task_lock > fd_lock > vfs_lock > ata_lock.
static spinlock_t vfs_lock = SPINLOCK_INIT;
static uint32_t vfs_eflags;
static int vfs_lock_owner = -1;
static int vfs_lock_depth = 0;

static void vfs_lock_acquire(void) {
    int tid = get_current_task();
    int key = (task_get_cid() << 16) | (tid & 0xFFFF);
    if (vfs_lock_owner == key) { vfs_lock_depth++; return; }
    vfs_eflags = spin_lock_irqsave(&vfs_lock);
    vfs_lock_owner = key;
    vfs_lock_depth = 1;
}

static void vfs_lock_release(void) {
    if (vfs_lock_depth > 1) { vfs_lock_depth--; return; }
    vfs_lock_depth = 0;
    vfs_lock_owner = -1;
    spin_unlock_irqrestore(&vfs_lock, vfs_eflags);
}

extern global_fd_t global_fds[MAX_GLOBAL_FDS];

// /proc generation reads these kernel-wide counters.
extern uint32_t smp_cpu_count;
extern uint32_t get_uptime_seconds(void);

extern void write_serial_string(const char*);
extern void write_serial_hex(uint32_t);

// Embedded binary for gcalc.mct
extern uint8_t _binary_gcalc_mct_start[];
extern uint8_t _binary_gcalc_mct_end[];
static uint32_t gcalc_mct_size() { return (uint32_t)(_binary_gcalc_mct_end - _binary_gcalc_mct_start); }

// Embedded binary for terminal.mct
extern uint8_t _binary_terminal_mct_start[];
extern uint8_t _binary_terminal_mct_end[];

extern uint8_t _binary_taskmgr_mct_start[];
extern uint8_t _binary_taskmgr_mct_end[];

extern uint8_t _binary_notepad_mct_start[];
extern uint8_t _binary_notepad_mct_end[];

extern uint8_t _binary_flappy_mct_start[];
extern uint8_t _binary_flappy_mct_end[];

// Embedded binary for hello.mct
extern uint8_t _binary_hello_mct_start[];
extern uint8_t _binary_hello_mct_end[];
static uint32_t hello_mct_size() { return (uint32_t)(_binary_hello_mct_end - _binary_hello_mct_start); }

fs_node_t fs_nodes[MAX_NODES];

// --- Internal helpers ---

static void strtolower(char* dst, const char* src) {
    while (*src) {
        char c = *src++;
        if (c >= 'A' && c <= 'Z') c += 32;
        *dst++ = c;
    }
    *dst = '\0';
}

static int copy_node_name(char* dst, const char* src) {
    int i = 0;
    if (!src || src[0] == '\0') return 0;
    while (src[i]) {
        if (src[i] == '/' || i >= MAX_FILENAME - 1) return 0;
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
    return 1;
}

// Split path into components. Return number of components.
// Contoh: "/home/user/file.txt" → {"home","user","file.txt"}
static int split_path(const char* path, char components[MAX_PATH/2][MAX_FILENAME]) {
    int count = 0;
    int i = 0, j = 0;
    
    if (!path || path[0] == '\0') return 0;
    
    // Skip leading slash
    if (path[0] == '/') i = 1;
    
    while (path[i]) {
        if (path[i] == '/') {
            if (j > 0) {
                components[count][j] = '\0';
                count++;
                if (count >= MAX_PATH / 2) return -1;
                j = 0;
            }
        } else {
            if (j >= MAX_FILENAME - 1) return -1;
            components[count][j++] = path[i];
        }
        i++;
    }
    if (j > 0) {
        components[count][j] = '\0';
        count++;
        if (count >= MAX_PATH / 2) return -1;
    }
    return count;
}

// --- Inisialisasi ---

static int vfs_update_file_if_needed(const char* path, const char* data, int size) {
    int node = vfs_get_node(path);
    if (node < 0) {
        vfs_create_file(path);
        vfs_write_file(path, data, size);
        return 1;
    } else {
        if (fs_nodes[node].size != size) {
            vfs_write_file(path, data, size);
            return 1;
        }
        // If it is an MCT file, check if disk has the correct magic
        if (size >= 4 && data[0] == '1' && data[1] == 'T' && data[2] == 'C' && data[3] == 'M') {
            extern int vfs_read_file(const char* path, char* buf, int max_size);
            char disk_magic[4];
            int rd = vfs_read_file(path, disk_magic, 4);
            if (rd < 4 || disk_magic[0] != '1' || disk_magic[1] != 'T' || disk_magic[2] != 'C' || disk_magic[3] != 'M') {
                write_serial_string("[VFS] magic mismatch, rewriting: ");
                write_serial_string(path);
                write_serial_string("\n");
                vfs_write_file(path, data, size);
                return 1;
            }
        }
    }
    return 0;
}

// Set while vfs_init() seeds the built-in apps: vfs_write_file() skips its
// per-file vfs_save() (256 node-table sectors each) so a fresh-disk rebuild
// is ONE write at the end of init instead of ~30 — the old behavior took
// minutes at first boot after a layout change. Seeding is boot-time only;
// runtime writes (shell echo > file, etc.) still save immediately.
static int vfs_seeding = 0;

void vfs_init() {
    write_serial_string("[VFS] init start\n");
    vfs_seeding = 1;
    // Coba load dari disk
    if (vfs_load()) {
        write_serial_string("[VFS] loaded ok\n");
        write_serial_string("[VFS] calling vfs_get_node(/dev)\n");
        int dev_node_check = vfs_get_node("/dev");
        write_serial_string("[VFS] vfs_get_node(/dev) returned: ");
        write_serial_hex(dev_node_check);
        write_serial_string("\n");
        if (dev_node_check < 0) {
            write_serial_string("[VFS] creating /dev\n");
            int dev_node = vfs_create_node("dev", FS_DIR, 0);
            write_serial_string("[VFS] vfs_create_node dev returned: ");
            write_serial_hex(dev_node);
            write_serial_string("\n");
            if (dev_node >= 0) {
                vfs_create_node("null", FS_DEV, dev_node);
                vfs_create_node("zero", FS_DEV, dev_node);
                vfs_create_node("random", FS_DEV, dev_node);
                write_serial_string("[VFS] saving /dev nodes...\n");
                vfs_save();
                write_serial_string("[VFS] saved /dev nodes\n");
            }
        }
        
        // Virtual /proc filesystem: FS_PROC nodes whose content is generated
        // on the fly at read time (tasks, meminfo, cpuinfo, uptime, version).
        if (vfs_get_node("/proc") < 0) {
            int proc_node = vfs_create_node("proc", FS_DIR, 0);
            if (proc_node >= 0) {
                vfs_create_node("tasks", FS_PROC, proc_node);
                vfs_create_node("meminfo", FS_PROC, proc_node);
                vfs_create_node("cpuinfo", FS_PROC, proc_node);
                vfs_create_node("uptime", FS_PROC, proc_node);
                vfs_create_node("version", FS_PROC, proc_node);
                write_serial_string("[VFS] created /proc nodes\n");
            }
        }
        
        // CRITICAL: Reset current_dir to root during init.
        // vfs_load() restores current_dir from disk, but if user previously
        // cd'd to a non-root dir (e.g., /home), all relative paths below
        // would resolve against that dir instead of root, causing NOT FOUND.
        set_current_dir(0);
        
        write_serial_string("[VFS] checking apps dir\n");
        // Ensure apps directory exists
        int apps_node = vfs_get_node("apps");
        write_serial_string("[VFS] apps dir node is: ");
        write_serial_hex(apps_node);
        write_serial_string("\n");
        if (apps_node < 0) {
            write_serial_string("[VFS] creating apps dir\n");
            apps_node = vfs_create_node("apps", FS_DIR, 0);
            write_serial_string("[VFS] apps dir created at: ");
            write_serial_hex(apps_node);
            write_serial_string("\n");
        }
        
        int changed = 0;
        // Always update core apps to latest kernel version for dev if needed
        extern uint8_t _binary_snake_mct_start[];
        extern uint8_t _binary_snake_mct_end[];
        changed += vfs_update_file_if_needed("apps/snake.mct", (const char*)_binary_snake_mct_start, _binary_snake_mct_end - _binary_snake_mct_start);

        extern uint8_t _binary_libc_mct_start[];
        extern uint8_t _binary_libc_mct_end[];
        changed += vfs_update_file_if_needed("apps/libc.mct", (const char*)_binary_libc_mct_start, _binary_libc_mct_end - _binary_libc_mct_start);

        extern uint8_t _binary_calc_mct_start[];
        extern uint8_t _binary_calc_mct_end[];
        changed += vfs_update_file_if_needed("apps/calc.mct", (const char*)_binary_calc_mct_start, _binary_calc_mct_end - _binary_calc_mct_start);

        extern uint8_t _binary_gcalc_mct_start[];
        extern uint8_t _binary_gcalc_mct_end[];
        changed += vfs_update_file_if_needed("apps/gcalc.mct", (const char*)_binary_gcalc_mct_start, _binary_gcalc_mct_end - _binary_gcalc_mct_start);

        extern uint8_t _binary_clock_mct_start[];
        extern uint8_t _binary_clock_mct_end[];
        changed += vfs_update_file_if_needed("apps/clock.mct", (const char*)_binary_clock_mct_start, _binary_clock_mct_end - _binary_clock_mct_start);

        extern uint8_t _binary_volume_mct_start[];
        extern uint8_t _binary_volume_mct_end[];
        changed += vfs_update_file_if_needed("apps/volume.mct", (const char*)_binary_volume_mct_start, _binary_volume_mct_end - _binary_volume_mct_start);

        extern uint8_t _binary_mplayer_mct_start[];
        extern uint8_t _binary_mplayer_mct_end[];
        changed += vfs_update_file_if_needed("apps/mplayer.mct", (const char*)_binary_mplayer_mct_start, _binary_mplayer_mct_end - _binary_mplayer_mct_start);

        extern uint8_t _binary_apps_music_wav_start[];
        extern uint8_t _binary_apps_music_wav_end[];
        changed += vfs_update_file_if_needed("apps/music.wav", (const char*)_binary_apps_music_wav_start, _binary_apps_music_wav_end - _binary_apps_music_wav_start);

        extern uint8_t _binary_hello_mct_start[];
        extern uint8_t _binary_hello_mct_end[];
        changed += vfs_update_file_if_needed("apps/hello.mct", (const char*)_binary_hello_mct_start, _binary_hello_mct_end - _binary_hello_mct_start);

        extern uint8_t _binary_sysinfo_mct_start[];
        extern uint8_t _binary_sysinfo_mct_end[];
        changed += vfs_update_file_if_needed("apps/sysinfo.mct", (const char*)_binary_sysinfo_mct_start, _binary_sysinfo_mct_end - _binary_sysinfo_mct_start);

        extern uint8_t _binary_pci_mct_start[];
        extern uint8_t _binary_pci_mct_end[];
        changed += vfs_update_file_if_needed("apps/pci.mct", (const char*)_binary_pci_mct_start, _binary_pci_mct_end - _binary_pci_mct_start);

        extern uint8_t _binary_explorer_mct_start[];
        extern uint8_t _binary_explorer_mct_end[];
        changed += vfs_update_file_if_needed("apps/explorer.mct", (const char*)_binary_explorer_mct_start, _binary_explorer_mct_end - _binary_explorer_mct_start);

        extern uint8_t _binary_browser_mct_start[];
        extern uint8_t _binary_browser_mct_end[];
        changed += vfs_update_file_if_needed("apps/browser.mct", (const char*)_binary_browser_mct_start, _binary_browser_mct_end - _binary_browser_mct_start);

        extern uint8_t _binary_terminal_mct_start[];
        extern uint8_t _binary_terminal_mct_end[];
        changed += vfs_update_file_if_needed("apps/terminal.mct", (const char*)_binary_terminal_mct_start, _binary_terminal_mct_end - _binary_terminal_mct_start);

        extern uint8_t _binary_taskmgr_mct_start[];
        extern uint8_t _binary_taskmgr_mct_end[];
        changed += vfs_update_file_if_needed("apps/taskmgr.mct", (const char*)_binary_taskmgr_mct_start, _binary_taskmgr_mct_end - _binary_taskmgr_mct_start);

        extern uint8_t _binary_notepad_mct_start[];
        extern uint8_t _binary_notepad_mct_end[];
        changed += vfs_update_file_if_needed("apps/notepad.mct", (const char*)_binary_notepad_mct_start, _binary_notepad_mct_end - _binary_notepad_mct_start);

        extern uint8_t _binary_flappy_mct_start[];
        extern uint8_t _binary_flappy_mct_end[];
        changed += vfs_update_file_if_needed("apps/flappy.mct", (const char*)_binary_flappy_mct_start, _binary_flappy_mct_end - _binary_flappy_mct_start);

        // Fork demo (process model: fork/waitpid/signals)
        extern uint8_t _binary_forkdemo_mct_start[];
        extern uint8_t _binary_forkdemo_mct_end[];
        changed += vfs_update_file_if_needed("apps/forkdemo.mct", (const char*)_binary_forkdemo_mct_start, _binary_forkdemo_mct_end - _binary_forkdemo_mct_start);

        // Exec demo (fork + exec + waitpid)
        extern uint8_t _binary_execdemo_mct_start[];
        extern uint8_t _binary_execdemo_mct_end[];
        changed += vfs_update_file_if_needed("apps/execdemo.mct", (const char*)_binary_execdemo_mct_start, _binary_execdemo_mct_end - _binary_execdemo_mct_start);
        extern uint8_t _binary_execchild_mct_start[];
        extern uint8_t _binary_execchild_mct_end[];
        changed += vfs_update_file_if_needed("apps/execchild.mct", (const char*)_binary_execchild_mct_start, _binary_execchild_mct_end - _binary_execchild_mct_start);

        // Shared memory demo
        extern uint8_t _binary_shmdemo_mct_start[];
        extern uint8_t _binary_shmdemo_mct_end[];
        changed += vfs_update_file_if_needed("apps/shmdemo.mct", (const char*)_binary_shmdemo_mct_start, _binary_shmdemo_mct_end - _binary_shmdemo_mct_start);

        // mmap demand-paging demo
        extern uint8_t _binary_mmapdemo_mct_start[];
        extern uint8_t _binary_mmapdemo_mct_end[];
        changed += vfs_update_file_if_needed("apps/mmapdemo.mct", (const char*)_binary_mmapdemo_mct_start, _binary_mmapdemo_mct_end - _binary_mmapdemo_mct_start);

        // lazy zero page + heap/stack demand-paging demo
        extern uint8_t _binary_demandtest_mct_start[];
        extern uint8_t _binary_demandtest_mct_end[];
        changed += vfs_update_file_if_needed("apps/demandtest.mct", (const char*)_binary_demandtest_mct_start, _binary_demandtest_mct_end - _binary_demandtest_mct_start);

        // Ctrl+C interrupt demo (infinite loop)
        extern uint8_t _binary_looper_mct_start[];
        extern uint8_t _binary_looper_mct_end[];
        changed += vfs_update_file_if_needed("apps/looper.mct", (const char*)_binary_looper_mct_start, _binary_looper_mct_end - _binary_looper_mct_start);
        extern uint8_t _binary_tcpserver_mct_start[];
        extern uint8_t _binary_tcpserver_mct_end[];
        changed += vfs_update_file_if_needed("apps/tcpserver.mct", (const char*)_binary_tcpserver_mct_start, _binary_tcpserver_mct_end - _binary_tcpserver_mct_start);

        // #UD exception-handler test (executes ud2, must be killed cleanly)
        extern uint8_t _binary_crashme_mct_start[];
        extern uint8_t _binary_crashme_mct_end[];
        changed += vfs_update_file_if_needed("apps/crashme.mct", (const char*)_binary_crashme_mct_start, _binary_crashme_mct_end - _binary_crashme_mct_start);

        // Window-manager capacity test (opens 12 windows)
        extern uint8_t _binary_winman_mct_start[];
        extern uint8_t _binary_winman_mct_end[];
        changed += vfs_update_file_if_needed("apps/winman.mct", (const char*)_binary_winman_mct_start, _binary_winman_mct_end - _binary_winman_mct_start);
        extern uint8_t _binary_sigdemo_mct_start[];
        extern uint8_t _binary_sigdemo_mct_end[];
        changed += vfs_update_file_if_needed("apps/sigdemo.mct", (const char*)_binary_sigdemo_mct_start, _binary_sigdemo_mct_end - _binary_sigdemo_mct_start);
        extern uint8_t _binary_smpstress_mct_start[];
        extern uint8_t _binary_smpstress_mct_end[];
        changed += vfs_update_file_if_needed("apps/smpstress.mct", (const char*)_binary_smpstress_mct_start, _binary_smpstress_mct_end - _binary_smpstress_mct_start);
        extern uint8_t _binary_bgread_mct_start[];
        extern uint8_t _binary_bgread_mct_end[];
        changed += vfs_update_file_if_needed("apps/bgread.mct", (const char*)_binary_bgread_mct_start, _binary_bgread_mct_end - _binary_bgread_mct_start);

        // Pipeline demo apps
        extern uint8_t _binary_pipegen_mct_start[];
        extern uint8_t _binary_pipegen_mct_end[];
        changed += vfs_update_file_if_needed("apps/pipegen.mct", (const char*)_binary_pipegen_mct_start, _binary_pipegen_mct_end - _binary_pipegen_mct_start);
        extern uint8_t _binary_piperead_mct_start[];
        extern uint8_t _binary_piperead_mct_end[];
        changed += vfs_update_file_if_needed("apps/piperead.mct", (const char*)_binary_piperead_mct_start, _binary_piperead_mct_end - _binary_piperead_mct_start);

        // ELF demo app (real ELF32 binary, not MCT)
        extern uint8_t _binary_elfdemo_elf_start[];
        extern uint8_t _binary_elfdemo_elf_end[];
        changed += vfs_update_file_if_needed("apps/elfdemo.elf", (const char*)_binary_elfdemo_elf_start, _binary_elfdemo_elf_end - _binary_elfdemo_elf_start);

        // Sync demo app (semaphore + futex test, ELF)
        extern uint8_t _binary_syncdemo_elf_start[];
        extern uint8_t _binary_syncdemo_elf_end[];
        changed += vfs_update_file_if_needed("apps/syncdemo.elf", (const char*)_binary_syncdemo_elf_start, _binary_syncdemo_elf_end - _binary_syncdemo_elf_start);

        // UDP test app (validates UDP syscall API)
        extern uint8_t _binary_udptest_elf_start[];
        extern uint8_t _binary_udptest_elf_end[];
        changed += vfs_update_file_if_needed("apps/udptest.elf", (const char*)_binary_udptest_elf_start, _binary_udptest_elf_end - _binary_udptest_elf_start);

        if (changed > 0) {
            write_serial_string("[VFS] saving...\n");
            vfs_save();
            write_serial_string("[VFS] saved ok\n");
        }
        
        // Phase 2: Ext2 Filesystem on Drive 1
        extern int ext2_init(int drive);
        extern void ext2_populate_vfs(uint32_t inode_num, int vfs_parent_node);
        write_serial_string("[VFS] ext2_init...\n");
        if (ext2_init(1) == 0) {
            write_serial_string("[VFS] ext2 ok\n");
            int ext2_node = vfs_get_node("ext2");
            if (ext2_node < 0) {
                ext2_node = vfs_create_node("ext2", FS_EXT2_DIR, 0);
            } else {
                // The mount point must be ext2-backed: create/write/delete
                // hooks only fire under an FS_EXT2_DIR parent, and older
                // versions saved the node as a plain FS_DIR.
                fs_nodes[ext2_node].type = FS_EXT2_DIR;
            }
            if (ext2_node >= 0) {
                // Mount point must point at the ext2 root directory inode (2),
                // otherwise create/write hooks would pass inode 0.
                fs_nodes[ext2_node].ext2_inode = 2;
                ext2_populate_vfs(2, ext2_node); // Inode 2 is the root directory
            }
        }
        
        // We always start at root (dir 0) when booting
        set_current_dir(0);
        
        // Flush the seeded node table + app files in one write. The per-file
        // saves were suppressed above (vfs_seeding) so a fresh-disk rebuild
        // costs a single 256-sector write, not ~30.
        vfs_seeding = 0;
        vfs_save();
        write_serial_string("[VFS] init done\n");
        return;
    }
    
    // No filesystem on disk — build the tree from scratch (fresh disk).
    vfs_seeding = 1;
    
    // Tidak ada filesystem — buat root directory
    memset(fs_nodes, 0, sizeof(fs_nodes));
    fs_nodes[0].in_use = 1;
    fs_nodes[0].parent = -1;
    fs_nodes[0].type = FS_DIR;
    strcpy(fs_nodes[0].name, "/");
    fs_nodes[0].size = 0;
    set_current_dir(0);
    
    // Buat home directory default
    int home_node = vfs_create_node("home", FS_DIR, 0);
    if (home_node >= 0) {
        vfs_create_node("user", FS_DIR, home_node);
    }
    
    // Populate dengan beberapa file demo
    vfs_create_file("readme.txt");
    vfs_write_file("readme.txt", "Welcome to Mectov OS v15.0!\n\nThis is a hobby operating system.\nUse 'help' for commands.\n", 95);
    
    vfs_create_file("hello.txt");
    vfs_write_file("hello.txt", "Hello, World!\n", 14);
    
    // Buat directory apps untuk .mct files
    vfs_create_node("apps", FS_DIR, 0);
    
    // Inject gcalc.mct dari embedded binary ke VFS
    vfs_create_file("apps/gcalc.mct");
    vfs_write_file("apps/gcalc.mct", (const char*)_binary_gcalc_mct_start, gcalc_mct_size());

    // Exec demo apps
    extern uint8_t _binary_execdemo_mct_start[];
    extern uint8_t _binary_execdemo_mct_end[];
    vfs_create_file("apps/execdemo.mct");
    vfs_write_file("apps/execdemo.mct", (const char*)_binary_execdemo_mct_start, _binary_execdemo_mct_end - _binary_execdemo_mct_start);
    extern uint8_t _binary_execchild_mct_start[];
    extern uint8_t _binary_execchild_mct_end[];
    vfs_create_file("apps/execchild.mct");
    vfs_write_file("apps/execchild.mct", (const char*)_binary_execchild_mct_start, _binary_execchild_mct_end - _binary_execchild_mct_start);

    // Shared memory demo
    extern uint8_t _binary_shmdemo_mct_start[];
    extern uint8_t _binary_shmdemo_mct_end[];
    vfs_create_file("apps/shmdemo.mct");
    vfs_write_file("apps/shmdemo.mct", (const char*)_binary_shmdemo_mct_start, _binary_shmdemo_mct_end - _binary_shmdemo_mct_start);

    // mmap demand-paging demo
    extern uint8_t _binary_mmapdemo_mct_start[];
    extern uint8_t _binary_mmapdemo_mct_end[];
    vfs_create_file("apps/mmapdemo.mct");
    vfs_write_file("apps/mmapdemo.mct", (const char*)_binary_mmapdemo_mct_start, _binary_mmapdemo_mct_end - _binary_mmapdemo_mct_start);

    // lazy zero page + heap/stack demand-paging demo
    extern uint8_t _binary_demandtest_mct_start[];
    extern uint8_t _binary_demandtest_mct_end[];
    vfs_create_file("apps/demandtest.mct");
    vfs_write_file("apps/demandtest.mct", (const char*)_binary_demandtest_mct_start, _binary_demandtest_mct_end - _binary_demandtest_mct_start);

    // Ctrl+C interrupt demo (infinite loop)
    extern uint8_t _binary_looper_mct_start[];
    extern uint8_t _binary_looper_mct_end[];
    vfs_create_file("apps/looper.mct");
    vfs_write_file("apps/looper.mct", (const char*)_binary_looper_mct_start, _binary_looper_mct_end - _binary_looper_mct_start);
    extern uint8_t _binary_tcpserver_mct_start[];
    extern uint8_t _binary_tcpserver_mct_end[];
    vfs_create_file("apps/tcpserver.mct");
    vfs_write_file("apps/tcpserver.mct", (const char*)_binary_tcpserver_mct_start, _binary_tcpserver_mct_end - _binary_tcpserver_mct_start);
    // #UD exception-handler test
    extern uint8_t _binary_crashme_mct_start[];
    extern uint8_t _binary_crashme_mct_end[];
    vfs_create_file("apps/crashme.mct");
    vfs_write_file("apps/crashme.mct", (const char*)_binary_crashme_mct_start, _binary_crashme_mct_end - _binary_crashme_mct_start);
    // Window-manager capacity test
    extern uint8_t _binary_winman_mct_start[];
    extern uint8_t _binary_winman_mct_end[];
    vfs_create_file("apps/winman.mct");
    vfs_write_file("apps/winman.mct", (const char*)_binary_winman_mct_start, _binary_winman_mct_end - _binary_winman_mct_start);
    extern uint8_t _binary_sigdemo_mct_start[];
    extern uint8_t _binary_sigdemo_mct_end[];
    vfs_create_file("apps/sigdemo.mct");
    vfs_write_file("apps/sigdemo.mct", (const char*)_binary_sigdemo_mct_start, _binary_sigdemo_mct_end - _binary_sigdemo_mct_start);
    extern uint8_t _binary_smpstress_mct_start[];
    extern uint8_t _binary_smpstress_mct_end[];
    vfs_create_file("apps/smpstress.mct");
    vfs_write_file("apps/smpstress.mct", (const char*)_binary_smpstress_mct_start, _binary_smpstress_mct_end - _binary_smpstress_mct_start);
    extern uint8_t _binary_bgread_mct_start[];
    extern uint8_t _binary_bgread_mct_end[];
    vfs_create_file("apps/bgread.mct");
    vfs_write_file("apps/bgread.mct", (const char*)_binary_bgread_mct_start, _binary_bgread_mct_end - _binary_bgread_mct_start);

    // Pipeline demo apps
    extern uint8_t _binary_pipegen_mct_start[];
    extern uint8_t _binary_pipegen_mct_end[];
    vfs_create_file("apps/pipegen.mct");
    vfs_write_file("apps/pipegen.mct", (const char*)_binary_pipegen_mct_start, _binary_pipegen_mct_end - _binary_pipegen_mct_start);
    extern uint8_t _binary_piperead_mct_start[];
    extern uint8_t _binary_piperead_mct_end[];
    vfs_create_file("apps/piperead.mct");
    vfs_write_file("apps/piperead.mct", (const char*)_binary_piperead_mct_start, _binary_piperead_mct_end - _binary_piperead_mct_start);

    // Inject hello.mct
    vfs_create_file("apps/hello.mct");
    vfs_write_file("apps/hello.mct", (const char*)_binary_hello_mct_start, hello_mct_size());

    // Inject clock.mct
    extern uint8_t _binary_clock_mct_start[];
    extern uint8_t _binary_clock_mct_end[];
    vfs_create_file("apps/clock.mct");
    vfs_write_file("apps/clock.mct", (const char*)_binary_clock_mct_start, _binary_clock_mct_end - _binary_clock_mct_start);

    // Inject snake.mct
    extern uint8_t _binary_snake_mct_start[];
    extern uint8_t _binary_snake_mct_end[];
    vfs_create_file("apps/snake.mct");
    vfs_write_file("apps/snake.mct", (const char*)_binary_snake_mct_start, _binary_snake_mct_end - _binary_snake_mct_start);

    // Inject sysinfo.mct
    extern uint8_t _binary_sysinfo_mct_start[];
    extern uint8_t _binary_sysinfo_mct_end[];
    vfs_create_file("apps/sysinfo.mct");
    vfs_write_file("apps/sysinfo.mct", (const char*)_binary_sysinfo_mct_start, _binary_sysinfo_mct_end - _binary_sysinfo_mct_start);

    // Inject pci.mct
    extern uint8_t _binary_pci_mct_start[];
    extern uint8_t _binary_pci_mct_end[];
    vfs_create_file("apps/pci.mct");
    vfs_write_file("apps/pci.mct", (const char*)_binary_pci_mct_start, _binary_pci_mct_end - _binary_pci_mct_start);

    // Inject explorer.mct
    extern uint8_t _binary_explorer_mct_start[];
    extern uint8_t _binary_explorer_mct_end[];
    vfs_create_file("apps/explorer.mct");
    vfs_write_file("apps/explorer.mct", (const char*)_binary_explorer_mct_start, _binary_explorer_mct_end - _binary_explorer_mct_start);

    // Inject browser.mct
    extern uint8_t _binary_browser_mct_start[];
    extern uint8_t _binary_browser_mct_end[];
    vfs_create_file("apps/browser.mct");
    vfs_write_file("apps/browser.mct", (const char*)_binary_browser_mct_start, _binary_browser_mct_end - _binary_browser_mct_start);

    // Inject terminal.mct
    vfs_create_file("apps/terminal.mct");
    vfs_write_file("apps/terminal.mct", (const char*)_binary_terminal_mct_start, _binary_terminal_mct_end - _binary_terminal_mct_start);
    
    // Task Manager & Editor (New Ring 3 apps)
    vfs_create_file("apps/taskmgr.mct");
    vfs_write_file("apps/taskmgr.mct", (const char*)_binary_taskmgr_mct_start, _binary_taskmgr_mct_end - _binary_taskmgr_mct_start);
    vfs_create_file("apps/notepad.mct");
    vfs_write_file("apps/notepad.mct", (const char*)_binary_notepad_mct_start, _binary_notepad_mct_end - _binary_notepad_mct_start);
    
    // Flappy Bird Game
    vfs_create_file("apps/flappy.mct");
    vfs_write_file("apps/flappy.mct", (const char*)_binary_flappy_mct_start, _binary_flappy_mct_end - _binary_flappy_mct_start);
    
    // Shared Library
    extern uint8_t _binary_libc_mct_start[];
    extern uint8_t _binary_libc_mct_end[];
    vfs_create_file("apps/libc.mct");
    vfs_write_file("apps/libc.mct", (const char*)_binary_libc_mct_start, _binary_libc_mct_end - _binary_libc_mct_start);

    // ELF demo app (real ELF32 binary)
    extern uint8_t _binary_elfdemo_elf_start[];
    extern uint8_t _binary_elfdemo_elf_end[];
    vfs_create_file("apps/elfdemo.elf");
    vfs_write_file("apps/elfdemo.elf", (const char*)_binary_elfdemo_elf_start, _binary_elfdemo_elf_end - _binary_elfdemo_elf_start);

    // Sync demo app (semaphore + futex test, ELF)
    extern uint8_t _binary_syncdemo_elf_start[];
    extern uint8_t _binary_syncdemo_elf_end[];
    vfs_create_file("apps/syncdemo.elf");
    vfs_write_file("apps/syncdemo.elf", (const char*)_binary_syncdemo_elf_start, _binary_syncdemo_elf_end - _binary_syncdemo_elf_start);

    // UDP test app (validates UDP syscall API)
    extern uint8_t _binary_udptest_elf_start[];
    extern uint8_t _binary_udptest_elf_end[];
    vfs_create_file("apps/udptest.elf");
    vfs_write_file("apps/udptest.elf", (const char*)_binary_udptest_elf_start, _binary_udptest_elf_end - _binary_udptest_elf_start);
    
    // Calc
    extern uint8_t _binary_calc_mct_start[];
    extern uint8_t _binary_calc_mct_end[];
    vfs_create_file("apps/calc.mct");
    vfs_write_file("apps/calc.mct", (const char*)_binary_calc_mct_start, _binary_calc_mct_end - _binary_calc_mct_start);
    
    vfs_save();

    // Phase 1: UNIX-like /dev filesystem
    int dev_node = vfs_create_node("dev", FS_DIR, 0);
    if (dev_node >= 0) {
        vfs_create_node("null", FS_DEV, dev_node);
        vfs_create_node("zero", FS_DEV, dev_node);
        vfs_create_node("random", FS_DEV, dev_node);
    }
    
    // Phase 1b: virtual /proc filesystem (dynamic content on read)
    int proc_node = vfs_create_node("proc", FS_DIR, 0);
    if (proc_node >= 0) {
        vfs_create_node("tasks", FS_PROC, proc_node);
        vfs_create_node("meminfo", FS_PROC, proc_node);
        vfs_create_node("cpuinfo", FS_PROC, proc_node);
        vfs_create_node("uptime", FS_PROC, proc_node);
        vfs_create_node("version", FS_PROC, proc_node);
        write_serial_string("[VFS] created /proc nodes\n");
    }
    
    // Phase 2: Ext2 Filesystem on Drive 1
    extern int ext2_init(int drive);
    extern void ext2_populate_vfs(uint32_t inode_num, int vfs_parent_node);
    if (ext2_init(1) == 0) {
        int ext2_node = vfs_get_node("ext2");
        if (ext2_node < 0) {
            ext2_node = vfs_create_node("ext2", FS_EXT2_DIR, 0);
        } else {
            fs_nodes[ext2_node].type = FS_EXT2_DIR;
        }
        if (ext2_node >= 0) {
            fs_nodes[ext2_node].ext2_inode = 2; // mount point = root dir inode
            ext2_populate_vfs(2, ext2_node);    // Inode 2 is the root directory
        }
    }
    
    // Flush the seeded tree in one write (per-file saves were suppressed by
    // vfs_seeding); runtime writes save immediately again from here on.
    vfs_seeding = 0;
    vfs_save();
}

// --- Simpan / Load dari ATA ---
// Layout constants live in vfs.h (single source of truth: shell's `df` and
// the allocator below both depend on VFS_DATA_START / VFS_DISK_SECTORS).

// Magic signature: 8 bytes + 2 bytes version + 6 bytes reserved = 16 bytes in sector 0
static void vfs_save_unlocked() {
    unsigned char meta[512];
    memset(meta, 0, 512);
    
    // Write magic + metadata
    meta[0] = 'M'; meta[1] = 'E'; meta[2] = 'C'; meta[3] = 'T';
    meta[4] = 'O'; meta[5] = 'V'; meta[6] = 'F'; meta[7] = 'S';  // "MECTOVFS"
    meta[8] = VFS_LAYOUT_VERSION;  // Version major (2 = 256-node table)
    meta[9] = 0x00;                // Version minor
    
    // Current dir index
    int cur_dir = get_current_dir();
    meta[16] = (unsigned char)(cur_dir & 0xFF);
    meta[17] = (unsigned char)((cur_dir >> 8) & 0xFF);
    
    // Node count (for quick validation)
    int count = 0;
    for (int i = 0; i < MAX_NODES; i++) if (fs_nodes[i].in_use) count++;
    meta[18] = (unsigned char)(count & 0xFF);
    meta[19] = (unsigned char)((count >> 8) & 0xFF);
    
    ata_write_sector(VFS_MAGIC_SECTOR, meta);
    
    // Write node table
    unsigned char* p = (unsigned char*)fs_nodes;
    for (int i = 0; i < VFS_NODE_SECTORS; i++) {
        ata_write_sector(VFS_NODE_START + i, p + (i * 512));
    }
}
void vfs_save() {
    vfs_lock_acquire();
    vfs_save_unlocked();
    vfs_lock_release();
}

static int vfs_load_unlocked() {
    unsigned char meta[512];
    ata_read_sector(VFS_MAGIC_SECTOR, meta);
    
    // Check magic
    if (meta[0] != 'M' || meta[1] != 'E' || meta[2] != 'C' ||
        meta[3] != 'T' || meta[4] != 'O' || meta[5] != 'V' ||
        meta[6] != 'F' || meta[7] != 'S') {
        return 0;  // Not a valid VFS disk
    }
    // Layout mismatch: an image written with the old 64-node table must be
    // rebuilt, not half-loaded. Without this, nodes 64..255 read file data
    // as garbage and vfs_update_file_if_needed() finds phantom "files".
    if (meta[8] != VFS_LAYOUT_VERSION) {
        write_serial_string("[VFS] on-disk layout v");
        write_serial_hex(meta[8]);
        write_serial_string(" != expected v");
        write_serial_hex(VFS_LAYOUT_VERSION);
        write_serial_string(", rebuilding\n");
        return 0;
    }
    
    // Read node table
    unsigned char* p = (unsigned char*)fs_nodes;
    for (int i = 0; i < VFS_NODE_SECTORS; i++) {
        ata_read_sector(VFS_NODE_START + i, p + (i * 512));
    }
    
    // Sanitize the on-disk node table. Names and parent links are attacker
    // controlled: a name with no NUL byte in its 32 bytes makes strtolower()/
    // strlen() walk off the node buffer (stack corruption), and an out-of-range
    // parent makes vfs_get_abs_path() index fs_nodes[] out of bounds.
    for (int i = 0; i < MAX_NODES; i++) {
        fs_nodes[i].name[MAX_FILENAME - 1] = '\0';
        if (fs_nodes[i].in_use &&
            (fs_nodes[i].parent < -1 || fs_nodes[i].parent >= MAX_NODES)) {
            fs_nodes[i].parent = 0;
        }
    }
    
    // Restore current_dir
    int loaded_cd = meta[16] | (meta[17] << 8);
    if (loaded_cd < 0 || loaded_cd >= MAX_NODES || !fs_nodes[loaded_cd].in_use)
        loaded_cd = 0;
    set_current_dir(loaded_cd);

    // Check Root node
    if (!fs_nodes[0].in_use) {
        return 0; // Root missing, invalid disk state
    }
    
    return 1;
}
int vfs_load() {
    vfs_lock_acquire();
    int r = vfs_load_unlocked();
    vfs_lock_release();
    return r;
}

// --- Resolusi Path ---

// Resolve relative/absolute path menjadi absolute path string
static void vfs_resolve_path_unlocked(const char* path, char* resolved, int buf_size) {
    if (!path || path[0] == '\0') {
        // Default: current directory
        vfs_get_abs_path(get_current_dir(), resolved, buf_size);
        return;
    }
    
    if (path[0] == '/') {
        // Absolute path — copy langsung
        int i = 0;
        while (path[i] && i < buf_size - 1) {
            resolved[i] = path[i];
            i++;
        }
        resolved[i] = '\0';
        return;
    }
    
    // Relative path — resolve against current_dir
    char cur_path[MAX_PATH];
    vfs_get_abs_path(get_current_dir(), cur_path, MAX_PATH);
    
    int cur_len = strlen(cur_path);
    
    // Handle "." and ".." in relative path
    if (strcmp(path, ".") == 0) {
        strcpy(resolved, cur_path);
        return;
    }
    
    if (strcmp(path, "..") == 0) {
        // Go up from current directory
        int cur_cd = get_current_dir();
        if (cur_cd == 0) {
            strcpy(resolved, "/");
            return;
        }
        vfs_get_abs_path(fs_nodes[cur_cd].parent, resolved, buf_size);
        return;
    }
    
    // If current path is "/", just append
    if (cur_len == 1 && cur_path[0] == '/') {
        int i = 0;
        resolved[i++] = '/';
        int j = 0;
        while (path[j] && i < buf_size - 1) {
            resolved[i++] = path[j++];
        }
        resolved[i] = '\0';
        return;
    }
    
    // Otherwise: current_path + "/" + relative_path
    int i = 0;
    while (cur_path[i] && i < buf_size - 1) {
        resolved[i] = cur_path[i];
        i++;
    }
    if (i > 0 && resolved[i-1] != '/' && i < buf_size - 1) {
        resolved[i++] = '/';
    }
    int j = 0;
    while (path[j] && i < buf_size - 1) {
        resolved[i++] = path[j++];
    }
    resolved[i] = '\0';
}
void vfs_resolve_path(const char* path, char* resolved, int buf_size) {
    vfs_lock_acquire();
    vfs_resolve_path_unlocked(path,  resolved,  buf_size);
    vfs_lock_release();
}

// Dapatkan absolute path dari node index
static int vfs_get_abs_path_unlocked(int node_idx, char* buf, int buf_size) {
    if (node_idx < 0 || node_idx >= MAX_NODES || !fs_nodes[node_idx].in_use) {
        strcpy(buf, "?");
        return -1;
    }
    
    // Build path from node up to root
    char stack[16][MAX_FILENAME];
    int sp = 0;
    int cur = node_idx;
    
    while (cur >= 0 && fs_nodes[cur].in_use) {
        // Skip root name in stack as we start with /
        if (cur != 0 && strlen(fs_nodes[cur].name) > 0) {
            strcpy(stack[sp++], fs_nodes[cur].name);
        }
        cur = fs_nodes[cur].parent;
        if (sp >= 16) break;
    }
    
    // Build absolute path
    int i = 0;
    buf[i++] = '/';
    for (int s = sp - 1; s >= 0; s--) {
        int slen = strlen(stack[s]);
        if (i + slen >= buf_size - 1) break;
        if (i > 1) buf[i++] = '/';
        int j = 0;
        while (stack[s][j]) buf[i++] = stack[s][j++];
    }
    buf[i] = '\0';
    return i;
}
int vfs_get_abs_path(int node_idx, char* buf, int buf_size) {
    vfs_lock_acquire();
    int r = vfs_get_abs_path_unlocked(node_idx,  buf,  buf_size);
    vfs_lock_release();
    return r;
}

// Cari node berdasarkan path. Return node index atau -1.
static int vfs_get_node_unlocked(const char* path) {
    if (!path || path[0] == '\0') return get_current_dir();
    
    char resolved[MAX_PATH];
    vfs_resolve_path(path, resolved, MAX_PATH);
    
    // Root case
    if (strcmp(resolved, "/") == 0) return 0;
    
    // Parse resolved path into components
    char comps[MAX_PATH/2][MAX_FILENAME];
    int ncomp = split_path(resolved, comps);
    if (ncomp < 0) return -1;
    if (ncomp == 0) return 0;
    
    // Walk from root
    int cur = 0;
    for (int i = 0; i < ncomp; i++) {
        // Normalize component name (lowercase comparison)
        char lc_name[MAX_FILENAME];
        strtolower(lc_name, comps[i]);
        
        int found = -1;
        for (int j = 0; j < MAX_NODES; j++) {
            if (!fs_nodes[j].in_use) continue;
            if (fs_nodes[j].parent != cur) continue;
            
            char lc_node[MAX_FILENAME];
            strtolower(lc_node, fs_nodes[j].name);
            if (strcmp(lc_node, lc_name) == 0) {
                found = j;
                break;
            }
        }
        
        if (found < 0) return -1;
        cur = found;
    }
    
    return cur;
}
int vfs_get_node(const char* path) {
    vfs_lock_acquire();
    int r = vfs_get_node_unlocked(path);
    vfs_lock_release();
    return r;
}

// Cari di dalam satu directory
static int vfs_find_in_dir_unlocked(const char* name, int dir_node) {
    if (dir_node < 0 || dir_node >= MAX_NODES) return -1;
    if (!fs_nodes[dir_node].in_use || (fs_nodes[dir_node].type != FS_DIR && fs_nodes[dir_node].type != FS_EXT2_DIR)) return -1;
    
    char lc_name[MAX_FILENAME];
    strtolower(lc_name, name);
    
    for (int i = 0; i < MAX_NODES; i++) {
        if (!fs_nodes[i].in_use) continue;
        if (fs_nodes[i].parent != dir_node) continue;
        
        char lc_node[MAX_FILENAME];
        strtolower(lc_node, fs_nodes[i].name);
        if (strcmp(lc_node, lc_name) == 0) return i;
    }
    return -1;
}
int vfs_find_in_dir(const char* name, int dir_node) {
    vfs_lock_acquire();
    int r = vfs_find_in_dir_unlocked(name,  dir_node);
    vfs_lock_release();
    return r;
}

// --- Create / Delete Nodes ---

static int vfs_create_node_unlocked(const char* name, fs_type_t type, int parent) {
    // Validate parent
    if (parent < 0 || parent >= MAX_NODES) return -1;
    if (!fs_nodes[parent].in_use || (fs_nodes[parent].type != FS_DIR && fs_nodes[parent].type != FS_EXT2_DIR)) return -1;
    if (!name || name[0] == '\0') return -1;
    
    // Check name exists in parent
    if (vfs_find_in_dir(name, parent) >= 0) return -2;
    
    // Find free slot
    for (int i = 0; i < MAX_NODES; i++) {
        if (!fs_nodes[i].in_use) {
            if (!copy_node_name(fs_nodes[i].name, name)) return -1;
            fs_nodes[i].type = type;
            fs_nodes[i].parent = parent;
            fs_nodes[i].size = 0;
            fs_nodes[i].data_sector = 0;
            fs_nodes[i].in_use = 1;
            // New object under an ext2 directory: create it on the real
            // filesystem. Populate passes FS_EXT2_* types for objects that
            // already exist on disk, so those bypass this hook.
            if (fs_nodes[parent].type == FS_EXT2_DIR &&
                (type == FS_DIR || type == FS_FILE)) {
                uint8_t ft = (type == FS_DIR) ? EXT2_FT_DIR : EXT2_FT_REG_FILE;
                uint32_t einode = ext2_create_entry(fs_nodes[parent].ext2_inode, name, ft);
                if (!einode) {
                    write_serial_string("VFS: ext2 create entry failed\n");
                    fs_nodes[i].in_use = 0;
                    return -1;
                }
                fs_nodes[i].type = (type == FS_DIR) ? FS_EXT2_DIR : FS_EXT2_FILE;
                fs_nodes[i].ext2_inode = einode;
                fs_nodes[i].size = (int)ext2_inode_size(einode);
            }
            // Runtime creates persist immediately; seeding-time creates are
            // flushed once by vfs_init()'s final vfs_save() (see vfs_seeding).
            if (!vfs_seeding) vfs_save();
            return i;
        }
    }
    
    // Debug info to serial port
    extern void write_serial_string(const char*);
    write_serial_string("VFS: FAILED to create node. Node table full or Root missing.\n");
    
    return -1; // Full
}
int vfs_create_node(const char* name, fs_type_t type, int parent) {
    vfs_lock_acquire();
    int r = vfs_create_node_unlocked(name,  type,  parent);
    vfs_lock_release();
    return r;
}

static int vfs_mkdir_unlocked(const char* path) {
    // Find parent directory
    char parent_path[MAX_PATH];
    char dirname[MAX_FILENAME];
    
    if (vfs_get_parent(path, parent_path, MAX_PATH) < 0) {
        return -1;
    }
    
    // Extract directory name (last component of path)
    int len = strlen(path);
    int i = len - 1;
    while (i >= 0 && path[i] == '/') i--;
    int end = i;
    while (i >= 0 && path[i] != '/') i--;
    int start = i + 1;
    int j;
    for (j = 0; j < end - start + 1 && j < MAX_FILENAME - 1; j++) {
        dirname[j] = path[start + j];
    }
    dirname[j] = '\0';
    
    int parent = vfs_get_node(parent_path);
    if (parent < 0) return -1;
    
    return vfs_create_node(dirname, FS_DIR, parent);
}
int vfs_mkdir(const char* path) {
    vfs_lock_acquire();
    int r = vfs_mkdir_unlocked(path);
    vfs_lock_release();
    return r;
}

static int vfs_create_file_unlocked(const char* path) {
    char parent_path[MAX_PATH];
    char filename[MAX_FILENAME];
    
    if (vfs_get_parent(path, parent_path, MAX_PATH) < 0) return -1;
    
    int len = strlen(path);
    int i = len - 1;
    while (i >= 0 && path[i] == '/') i--;
    int end = i;
    while (i >= 0 && path[i] != '/') i--;
    int start = i + 1;
    int j;
    for (j = 0; j < end - start + 1 && j < MAX_FILENAME - 1; j++) {
        filename[j] = path[start + j];
    }
    filename[j] = '\0';
    
    int parent = vfs_get_node(parent_path);
    if (parent < 0) return -1;
    
    return vfs_create_node(filename, FS_FILE, parent);
}
int vfs_create_file(const char* path) {
    vfs_lock_acquire();
    int r = vfs_create_file_unlocked(path);
    vfs_lock_release();
    return r;
}

// Remove the on-disk ext2 object behind a VFS node (if any). Must run while
// the node is still in_use so parent/name lookups stay valid.
static void vfs_remove_ext2_entry(int node) {
    if (node < 0 || node >= MAX_NODES) return;
    if (fs_nodes[node].type != FS_EXT2_FILE && fs_nodes[node].type != FS_EXT2_DIR) return;
    if (!fs_nodes[node].ext2_inode) return;
    int p = fs_nodes[node].parent;
    if (p < 0 || p >= MAX_NODES) return;
    if (fs_nodes[p].type != FS_EXT2_DIR) return;
    ext2_remove_entry(fs_nodes[p].ext2_inode, fs_nodes[node].name);
}

static int vfs_delete_node_unlocked(const char* path) {
    int node = vfs_get_node(path);
    if (node < 0) return -1;
    if (node == 0) return -3; // Cannot delete root
    if (fs_nodes[node].type == FS_PROC) return -7; // virtual, cannot delete
    // The /proc mount point itself is virtual too: deleting it would take the
    // whole tree down until the next boot recreates it.
    if (fs_nodes[node].type == FS_DIR && fs_nodes[node].parent == 0 &&
        strcmp(fs_nodes[node].name, "proc") == 0) return -7;

    // Refuse to delete while any open fd references the node: its slot would
    // be reused by the next create, and reads/writes on the stale fd would
    // silently hit an unrelated file.
    for (int i = 0; i < MAX_GLOBAL_FDS; i++) {
        if (global_fds[i].in_use && global_fds[i].type == FD_TYPE_FILE &&
            global_fds[i].vfs_node == node) {
            return -6;
        }
    }
    
    // Recursively delete children if directory (handle nested dirs)
    if (fs_nodes[node].type == FS_DIR || fs_nodes[node].type == FS_EXT2_DIR) {
        // Delete deepest children first (multiple passes needed for nesting)
        int deleted;
        do {
            deleted = 0;
            for (int i = 0; i < MAX_NODES; i++) {
                if (!fs_nodes[i].in_use) continue;
                // Check if this node is a descendant of target
                int p = fs_nodes[i].parent;
                int is_descendant = 0;
                while (p >= 0) {
                    if (p == node) { is_descendant = 1; break; }
                    if (!fs_nodes[p].in_use) break;
                    p = fs_nodes[p].parent;
                }
                if (is_descendant) {
                    // Only delete if this node has no children itself
                    int has_children = 0;
                    for (int j = 0; j < MAX_NODES; j++) {
                        if (fs_nodes[j].in_use && fs_nodes[j].parent == i) {
                            has_children = 1;
                            break;
                        }
                    }
                    if (!has_children) {
                        vfs_remove_ext2_entry(i);
                        fs_nodes[i].in_use = 0;
                        fs_nodes[i].size = 0;
                        fs_nodes[i].data_sector = 0;
                        deleted = 1;
                    }
                }
            }
        } while (deleted);
    }
    
    vfs_remove_ext2_entry(node);
    fs_nodes[node].in_use = 0;
    fs_nodes[node].size = 0;
    fs_nodes[node].data_sector = 0;
    vfs_save();
    return 0;
}
int vfs_delete_node(const char* path) {
    vfs_lock_acquire();
    int r = vfs_delete_node_unlocked(path);
    vfs_lock_release();
    return r;
}

// --- Read / Write File Data ---

// Data layout per file di ATA:
//   data_sector menyimpan start sector untuk data file ini.
//   Data disimpan sebagai array sektor kontigu.
//   Untuk file kecil (< 512 bytes), cukup 1 sektor.
//   Sektor terakhir berisi data file (tidak harus full 512 bytes).

static int vfs_rename_unlocked(const char* old_path, const char* new_path) {
    int node = vfs_get_node(old_path);
    if (node < 0) return -1;
    if (node == 0) return -3; // Cannot rename root
    
    char new_parent_path[MAX_PATH];
    char new_filename[MAX_FILENAME];
    
    if (vfs_get_parent(new_path, new_parent_path, MAX_PATH) < 0) return -1;
    
    int len = strlen(new_path);
    int i = len - 1;
    while (i >= 0 && new_path[i] == '/') i--;
    int end = i;
    while (i >= 0 && new_path[i] != '/') i--;
    int start = i + 1;
    int j;
    for (j = 0; j < end - start + 1 && j < MAX_FILENAME - 1; j++) {
        new_filename[j] = new_path[start + j];
    }
    new_filename[j] = '\0';
    
    int new_parent = vfs_get_node(new_parent_path);
    if (new_parent < 0) return -1;
    
    // Prevent directory cycles: moving a node under itself or under one of its
    // own descendants would make it unreachable and duplicate the whole subtree
    // in listings. Walk the parent chain of new_parent.
    if (new_parent == node) return -4;
    for (int p = new_parent; p > 0; p = fs_nodes[p].parent) {
        if (p == node) return -4;
    }
    
    // Delete existing target if any
    int existing = vfs_find_in_dir(new_filename, new_parent);
    if (existing >= 0) {
        vfs_delete_node(new_path);
    }
    
    // Ext2-backed node: rename the on-disk entry too. Only same-directory
    // renames are supported (ext2_rename_entry unlinks + re-adds in one dir);
    // cross-dir moves or ext2<->native moves are rejected.
    int is_ext2 = (fs_nodes[node].type == FS_EXT2_FILE || fs_nodes[node].type == FS_EXT2_DIR);
    if (is_ext2) {
        int old_p = fs_nodes[node].parent;
        if (old_p != new_parent || fs_nodes[old_p].type != FS_EXT2_DIR) {
            return -5; // cross-directory / cross-filesystem rename not supported
        }
        if (ext2_rename_entry(fs_nodes[old_p].ext2_inode, fs_nodes[node].name,
                              new_filename, fs_nodes[node].ext2_inode) != 0) {
            return -5;
        }
    }
    
    // Move node
    fs_nodes[node].parent = new_parent;
    strncpy(fs_nodes[node].name, new_filename, MAX_FILENAME - 1);
    fs_nodes[node].name[MAX_FILENAME - 1] = '\0';
    
    vfs_save();
    return 0;
}
int vfs_rename(const char* old_path, const char* new_path) {
    vfs_lock_acquire();
    int r = vfs_rename_unlocked(old_path,  new_path);
    vfs_lock_release();
    return r;
}

// ============================================================
// Virtual /proc filesystem: content generated on the fly at read time.
// ============================================================

static void proc_itoa(char* out, int val) {
    char tmp[16];
    int i = 0;
    if (val == 0) { out[0] = '0'; out[1] = '\0'; return; }
    while (val > 0 && i < 15) { tmp[i++] = '0' + (val % 10); val /= 10; }
    int j = 0;
    while (i > 0) out[j++] = tmp[--i];
    out[j] = '\0';
}

static void proc_add(char* buf, int* len, int cap, const char* s) {
    while (*s && *len < cap - 1) buf[(*len)++] = *s++;
}

// Right-align a number into a fixed-width column.
static void proc_field(char* buf, int* len, int cap, int val, int width) {
    char num[16];
    proc_itoa(num, val);
    int pad = width - (int)strlen(num);
    for (int i = 0; i < pad; i++) proc_add(buf, len, cap, " ");
    proc_add(buf, len, cap, num);
}

// Fixed-width (8 chars, space-padded) so the tasks columns stay aligned.
static const char* proc_state_str(int s) {
    switch (s) {
        case TASK_STATE_RUNNING: return "RUNNING";
        case TASK_STATE_READY:   return "READY   ";
        case TASK_STATE_SLEEP:   return "SLEEP   ";
        case TASK_STATE_BLOCKED: return "BLOCKED ";
        case TASK_STATE_ZOMBIE:  return "ZOMBIE  ";
        case TASK_STATE_STOPPED: return "STOPPED ";
        default: return "UNKNOWN ";
    }
}

// Generate the content of a /proc file into buf. Returns the byte count, with
// a NUL appended when there is room (same contract as vfs_read_file).
static int vfs_proc_read(const char* name, char* buf, int max_size) {
    int len = 0;

    if (strcmp(name, "tasks") == 0) {
        // STK% = peak kernel-stack bytes used / TASK_KSTACK_SIZE. 100% means
        // the task has come within a push of its guard page (overflow = panic).
        proc_add(buf, &len, max_size, "  PID STATE    RING PRI  PGRP SESS STK% NAME\n");
        task_info_t info;
        int tid = -1;
        while ((tid = task_enum(tid, &info)) >= 0) {
            proc_field(buf, &len, max_size, info.id, 4);
            proc_add(buf, &len, max_size, "  ");
            proc_add(buf, &len, max_size, proc_state_str(info.state));
            proc_add(buf, &len, max_size, "  ");
            proc_field(buf, &len, max_size, info.ring, 3);
            proc_add(buf, &len, max_size, "  ");
            proc_field(buf, &len, max_size, info.priority, 3);
            proc_add(buf, &len, max_size, "  ");
            proc_field(buf, &len, max_size, task_get_pgrp(info.id), 4);
            proc_add(buf, &len, max_size, "  ");
            proc_field(buf, &len, max_size, task_get_session(info.id), 4);
            proc_add(buf, &len, max_size, "  ");
            proc_field(buf, &len, max_size,
                       (info.stack_watermark * 100) / TASK_KSTACK_SIZE, 4);
            proc_add(buf, &len, max_size, "  ");
            const char* nm = task_get_launch_arg(info.id);
            if (nm[0] == '\0') proc_add(buf, &len, max_size, "kernel");
            else proc_add(buf, &len, max_size, nm);
            proc_add(buf, &len, max_size, "\n");
        }
        if (len == 0) proc_add(buf, &len, max_size, "  (no tasks)\n");
    } else if (strcmp(name, "meminfo") == 0) {
        // Numbers must go through the capped proc_add too: max_size comes
        // straight from the caller (an app can read() with a small buffer),
        // so writing proc_itoa directly into buf would overflow it.
        char num[16];
        proc_add(buf, &len, max_size, "MemTotal: ");
        proc_itoa(num, get_total_memory() / 1024);
        proc_add(buf, &len, max_size, num);
        proc_add(buf, &len, max_size, " KB\nMemFree:  ");
        proc_itoa(num, get_free_memory() / 1024);
        proc_add(buf, &len, max_size, num);
        proc_add(buf, &len, max_size, " KB\nMemUsed:  ");
        proc_itoa(num, get_used_memory() / 1024);
        proc_add(buf, &len, max_size, num);
        proc_add(buf, &len, max_size, " KB\n");
        // Kernel heap allocator state (heap hardening v38.4).
        extern void kmalloc_get_stats(kmalloc_stats_t* s);
        kmalloc_stats_t hs;
        kmalloc_get_stats(&hs);
        proc_add(buf, &len, max_size, "HeapUsed: ");
        proc_itoa(num, hs.heap_used);
        proc_add(buf, &len, max_size, num);
        proc_add(buf, &len, max_size, " B\nHeapAlloc: ");
        proc_itoa(num, hs.allocated);
        proc_add(buf, &len, max_size, num);
        proc_add(buf, &len, max_size, " B\nHeapFree: ");
        proc_itoa(num, hs.free_bytes);
        proc_add(buf, &len, max_size, num);
        proc_add(buf, &len, max_size, " B (largest ");
        proc_itoa(num, hs.largest_free);
        proc_add(buf, &len, max_size, num);
        proc_add(buf, &len, max_size, ", ");
        proc_itoa(num, hs.free_blocks);
        proc_add(buf, &len, max_size, num);
        proc_add(buf, &len, max_size, " free)\nHeapBlocks: ");
        proc_itoa(num, hs.blocks);
        proc_add(buf, &len, max_size, num);
        proc_add(buf, &len, max_size, " live, ");
        proc_itoa(num, hs.allocs);
        proc_add(buf, &len, max_size, num);
        proc_add(buf, &len, max_size, " allocs, ");
        proc_itoa(num, hs.frees);
        proc_add(buf, &len, max_size, num);
        proc_add(buf, &len, max_size, " frees\nHeapOOM: ");
        proc_itoa(num, hs.oom_count);
        proc_add(buf, &len, max_size, num);
        proc_add(buf, &len, max_size, "\nHeapCorrupt: ");
        proc_itoa(num, hs.canary_failures);
        proc_add(buf, &len, max_size, num);
        proc_add(buf, &len, max_size, " canary, ");
        proc_itoa(num, hs.magic_failures);
        proc_add(buf, &len, max_size, num);
        proc_add(buf, &len, max_size, " magic\n");
    } else if (strcmp(name, "cpuinfo") == 0) {
        char num[16];
        proc_add(buf, &len, max_size, "processor\t: 0\n");
        proc_add(buf, &len, max_size, "vendor_id\t: ");
        proc_add(buf, &len, max_size, cpu_brand);
        proc_add(buf, &len, max_size, "\ncores\t\t: ");
        proc_itoa(num, (int)smp_cpu_count);
        proc_add(buf, &len, max_size, num);
        proc_add(buf, &len, max_size, "\n");
    } else if (strcmp(name, "uptime") == 0) {
        char num[16];
        proc_add(buf, &len, max_size, "up ");
        proc_itoa(num, (int)get_uptime_seconds());
        proc_add(buf, &len, max_size, num);
        proc_add(buf, &len, max_size, " seconds\n");
    } else if (strcmp(name, "version") == 0) {
        proc_add(buf, &len, max_size, "MectovOS version ");
        proc_add(buf, &len, max_size, OS_VERSION);
        proc_add(buf, &len, max_size, " (i686, SMP)\n");
    } else {
        proc_add(buf, &len, max_size, "no such /proc file\n");
    }

    if (len < max_size) buf[len] = '\0';
    return len;
}

// Reads up to max_size bytes and returns the byte count. A NUL terminator is
// appended ONLY when there is room left over — callers such as load_mct_app()
// pass max_size == the exact file size and need every one of those bytes, so
// the data must never be clamped to max_size - 1 to make space for it.
static int vfs_read_file_unlocked(const char* path, char* buf, int max_size) {
    int node = vfs_get_node(path);
    if (node < 0) return -1;
    if (max_size <= 0) return -1;

    if (fs_nodes[node].type == FS_PROC) {
        return vfs_proc_read(fs_nodes[node].name, buf, max_size);
    }

    if (fs_nodes[node].type == FS_DEV) {
        if (strcmp(fs_nodes[node].name, "zero") == 0) {
            memset(buf, 0, max_size);
            return max_size;
        } else if (strcmp(fs_nodes[node].name, "null") == 0) {
            return 0; // EOF immediately
        } else if (strcmp(fs_nodes[node].name, "random") == 0) {
            // Very simple PRNG for /dev/random (since we don't have a real entropy pool)
            static uint32_t seed = 0x12345678;
            for (int i = 0; i < max_size; i++) {
                seed = (1103515245 * seed + 12345);
                buf[i] = (char)(seed & 0xFF);
            }
            return max_size;
        }
        return -2; // Unknown device
    }
    
    if (fs_nodes[node].type == FS_EXT2_FILE) {
        extern int ext2_read_file_data(uint32_t inode_num, char* buf, int max_size);
        int bytes = ext2_read_file_data(fs_nodes[node].ext2_inode, buf, max_size);
        if (bytes >= 0 && bytes < max_size) buf[bytes] = '\0';
        return bytes;
    }
    
    if (fs_nodes[node].type != FS_FILE) return -2;
    
    int size = fs_nodes[node].size;
    if (size > max_size) size = max_size;
    if (size <= 0) { buf[0] = '\0'; return 0; }
    
    int sector = fs_nodes[node].data_sector;
    if (sector <= 0 || sector >= VFS_DISK_SECTORS) { buf[0] = '\0'; return 0; }
    
    // A corrupt node can claim sectors past the end of the disk;
    // clamp so ata_read_sector never walks off the image.
    int max_readable = (VFS_DISK_SECTORS - sector) * 512;
    if (size > max_readable) size = max_readable;
    
    // Read sectors
    int remaining = size;
    int offset = 0;
    while (remaining > 0) {
        unsigned char tmp[512];
        ata_read_sector(sector++, tmp);
        
        int chunk = remaining > 512 ? 512 : remaining;
        memcpy(buf + offset, tmp, chunk);
        offset += chunk;
        remaining -= chunk;
    }
    
    // Only terminate if the data left room — size == max_size means the file
    // exactly filled the caller's buffer and buf[size] is one past the end.
    if (size < max_size) buf[size] = '\0';
    return size;
}
int vfs_read_file(const char* path, char* buf, int max_size) {
    vfs_lock_acquire();
    int r = vfs_read_file_unlocked(path,  buf,  max_size);
    vfs_lock_release();
    return r;
}

static int vfs_alloc_sectors(int sectors_needed, int exclude_node) {
    // VFS_DATA_START begins at 257 (1 magic + 256 node sectors with the
    // 256-node table). sector_map is VFS_DISK_SECTORS bytes so marking
    // [0, VFS_DATA_START) as metadata is in-bounds by construction.
    uint8_t sector_map[VFS_DISK_SECTORS];
    memset(sector_map, 0, sizeof(sector_map));
    
    // Mark VFS metadata and node sectors (0 to VFS_DATA_START-1) as allocated.
    for (int i = 0; i < VFS_DATA_START; i++) {
        sector_map[i] = 1;
    }
    
    // Scan all active files to populate the allocation map.
    for (int i = 0; i < MAX_NODES; i++) {
        if (fs_nodes[i].in_use && fs_nodes[i].type == FS_FILE && fs_nodes[i].data_sector > 0 && i != exclude_node) {
            int node_sectors = (fs_nodes[i].size + 511) / 512;
            if (node_sectors < 1) node_sectors = 1; // min 1 sector allocated
            for (int s = 0; s < node_sectors; s++) {
                int sector = fs_nodes[i].data_sector + s;
                if (sector < VFS_DISK_SECTORS) {
                    sector_map[sector] = 1;
                }
            }
        }
    }
    
    // Find first contiguous block of free sectors
    for (int i = VFS_DATA_START; i <= VFS_DISK_SECTORS - sectors_needed; i++) {
        int found = 1;
        for (int s = 0; s < sectors_needed; s++) {
            if (sector_map[i + s]) {
                found = 0;
                break;
            }
        }
        if (found) {
            return i;
        }
    }
    
    return -1; // Disk Full / Out of contiguous space
}

static int vfs_write_file_unlocked(const char* path, const char* data, int size) {
    extern void write_serial_string(const char*);
    write_serial_string("[VFS] write: ");
    write_serial_string(path);
    write_serial_string("\n");
    int node = vfs_get_node(path);
    if (node < 0) return -1;

    if (fs_nodes[node].type == FS_DEV) {
        if (strcmp(fs_nodes[node].name, "null") == 0 || strcmp(fs_nodes[node].name, "zero") == 0) {
            return size; // Discard data successfully
        } else if (strcmp(fs_nodes[node].name, "random") == 0) {
            return size; // Writes to /dev/random are ignored (or could add entropy)
        }
        return -2;
    }

    if (fs_nodes[node].type == FS_EXT2_FILE) {
        int r = ext2_write_file_data(fs_nodes[node].ext2_inode, data, size);
        if (r >= 0) {
            fs_nodes[node].size = r;
            if (!vfs_seeding) vfs_save();
        }
        return r;
    }

    if (fs_nodes[node].type != FS_FILE) return -2;
    
    // Calculate how many sectors needed
    int sectors_needed = (size + 511) / 512;
    if (sectors_needed < 1) sectors_needed = 1;
    
    // Reuse existing data sector if it fits, otherwise allocate new contiguous space
    int start_sector = fs_nodes[node].data_sector;
    int old_sectors = (fs_nodes[node].size + 511) / 512;
    if (old_sectors < 1) old_sectors = 1;
    
    if (start_sector > 0 && sectors_needed > old_sectors) {
        start_sector = 0; // relocation needed
    }
    
    if (start_sector == 0) {
        start_sector = vfs_alloc_sectors(sectors_needed, node);
        if (start_sector < 0) {
            write_serial_string("[VFS] write failed: Disk Full / Out of contiguous space\n");
            return -3; // Disk Full
        }
        fs_nodes[node].data_sector = start_sector;
    }
    
    // Write data sectors
    int remaining = size;
    int offset = 0;
    int sector = start_sector;
    while (remaining > 0 || (offset == 0 && remaining == 0)) {
        unsigned char tmp[512];
        memset(tmp, 0, 512);
        
        int chunk = remaining > 512 ? 512 : remaining;
        if (chunk > 0) memcpy(tmp, data + offset, chunk);
        
        ata_write_sector(sector++, tmp);
        offset += 512;
        remaining -= 512;
        
        if (offset >= size) break;
    }
    
    fs_nodes[node].size = size;
    if (!vfs_seeding) vfs_save();
    return size;
}
int vfs_write_file(const char* path, const char* data, int size) {
    vfs_lock_acquire();
    int r = vfs_write_file_unlocked(path,  data,  size);
    vfs_lock_release();
    return r;
}

// --- Find path with parent resolution ---

static int vfs_find_path_unlocked(const char* path, int* parent_dir) {
    char resolved[MAX_PATH];
    vfs_resolve_path(path, resolved, MAX_PATH);
    
    if (strcmp(resolved, "/") == 0) {
        if (parent_dir) *parent_dir = -1;
        return 0;
    }
    
    // Parse into components, find parent
    char comps[MAX_PATH/2][MAX_FILENAME];
    int ncomp = split_path(resolved, comps);
    if (ncomp < 0) {
        if (parent_dir) *parent_dir = -1;
        return -1;
    }
    if (ncomp == 0) {
        if (parent_dir) *parent_dir = -1;
        return 0;
    }
    
    // Walk to parent directory (all components except last)
    int cur = 0;
    for (int i = 0; i < ncomp - 1; i++) {
        char lc_name[MAX_FILENAME];
        strtolower(lc_name, comps[i]);
        
        int found = -1;
        for (int j = 0; j < MAX_NODES; j++) {
            if (!fs_nodes[j].in_use) continue;
            if (fs_nodes[j].parent != cur) continue;
            
            char lc_node[MAX_FILENAME];
            strtolower(lc_node, fs_nodes[j].name);
            if (strcmp(lc_node, lc_name) == 0) {
                found = j;
                break;
            }
        }
        if (found < 0) return -1;
        cur = found;
    }
    
    if (parent_dir) *parent_dir = cur;
    
    // Find the last component in parent directory
    char lc_name[MAX_FILENAME];
    strtolower(lc_name, comps[ncomp - 1]);
    
    for (int j = 0; j < MAX_NODES; j++) {
        if (!fs_nodes[j].in_use) continue;
        if (fs_nodes[j].parent != cur) continue;
        
        char lc_node[MAX_FILENAME];
        strtolower(lc_node, fs_nodes[j].name);
        if (strcmp(lc_node, lc_name) == 0) {
            return j;
        }
    }
    
    return -1; // Last component not found
}
int vfs_find_path(const char* path, int* parent_dir) {
    vfs_lock_acquire();
    int r = vfs_find_path_unlocked(path,  parent_dir);
    vfs_lock_release();
    return r;
}

// Get parent path from a path string
static int vfs_get_parent_unlocked(const char* path, char* parent_path, int buf_size) {
    if (!path || path[0] == '\0') return -1;
    
    int len = strlen(path);
    int end = len - 1;
    
    // Strip trailing slashes
    while (end > 0 && path[end] == '/') end--;
    
    // Find last slash
    int last_slash = -1;
    for (int i = end - 1; i >= 0; i--) {
        if (path[i] == '/') {
            last_slash = i;
            break;
        }
    }
    
    if (last_slash < 0) {
        // Just a filename relative to current dir
        // Actually this means there's no parent in the path string
        // Return current dir path as parent
        return vfs_get_abs_path(get_current_dir(), parent_path, buf_size);
    }
    
    if (last_slash == 0) {
        strcpy(parent_path, "/");
    } else {
        int i;
        for (i = 0; i < last_slash && i < buf_size - 1; i++) {
            parent_path[i] = path[i];
        }
        parent_path[i] = '\0';
    }
    
    return strlen(parent_path);
}
int vfs_get_parent(const char* path, char* parent_path, int buf_size) {
    vfs_lock_acquire();
    int r = vfs_get_parent_unlocked(path,  parent_path,  buf_size);
    vfs_lock_release();
    return r;
}

// --- Listing ---

static void vfs_list_dir_unlocked(int dir_node, void (*print_fn)(const char*, unsigned char)) {
    int count = 0;
    for (int i = 0; i < MAX_NODES; i++) {
        if (!fs_nodes[i].in_use) continue;
        if (fs_nodes[i].parent != dir_node) continue;
        
        count++;
        
        // Print file/dir icon
        if (fs_nodes[i].type == FS_DIR || fs_nodes[i].type == FS_EXT2_DIR) {
            print_fn("[DIR]  ", 0x0B);
            print_fn(fs_nodes[i].name, 0x0B);
            print_fn("/\n", 0x0B);
        } else if (fs_nodes[i].type == FS_PROC) {
            // Virtual /proc file: content generated on read, no disk size.
            print_fn("[SYS]  ", 0x0D);
            print_fn(fs_nodes[i].name, 0x0D);
            print_fn("\n", 0x0D);
        } else {
            print_fn("[FILE] ", 0x0F);
            print_fn(fs_nodes[i].name, 0x0F);
            
            // Print size
            print_fn("  (", 0x07);
            char size_str[24];
            int n = fs_nodes[i].size;
            int si = 0;
            if (n == 0) { size_str[si++] = '0'; }
            while (n > 0) { size_str[si++] = '0' + (n % 10); n /= 10; }
            for (int j = 0; j < si/2; j++) { char t = size_str[j]; size_str[j] = size_str[si-1-j]; size_str[si-1-j] = t; }
            // Tail: up to 10 digits + " B)\n\0" needs ≥ 15 bytes
            size_str[si++] = ' ';
            size_str[si++] = 'B';
            size_str[si++] = ')';
            size_str[si++] = '\n';
            size_str[si] = '\0';
            print_fn(size_str, 0x07);
        }
    }
    
    if (count == 0) {
        print_fn("  (empty)\n", 0x07);
    }
}
void vfs_list_dir(int dir_node, void (*print_fn)(const char*, unsigned char)) {
    vfs_lock_acquire();
    vfs_list_dir_unlocked(dir_node, print_fn);
    vfs_lock_release();
}

static void vfs_tree_unlocked(int dir_node, int depth, void (*print_fn)(const char*, unsigned char)) {
    for (int i = 0; i < MAX_NODES; i++) {
        if (!fs_nodes[i].in_use) continue;
        if (fs_nodes[i].parent != dir_node) continue;
        
        // Print indent
        for (int d = 0; d < depth; d++) print_fn("  ", 0x0F);
        
        if (fs_nodes[i].type == FS_DIR) {
            print_fn("[", 0x0B); print_fn(fs_nodes[i].name, 0x0B); print_fn("]\n", 0x0B);
            vfs_tree(i, depth + 1, print_fn);
        } else {
            print_fn(fs_nodes[i].name, 0x0F);
            print_fn("\n", 0x0F);
        }
    }
}
void vfs_tree(int dir_node, int depth, void (*print_fn)(const char*, unsigned char)) {
    vfs_lock_acquire();
    vfs_tree_unlocked(dir_node, depth, print_fn);
    vfs_lock_release();
}

// --- Helper Functions ---

static int vfs_is_dir_unlocked(int node) {
    if (node < 0 || node >= MAX_NODES) return 0;
    return fs_nodes[node].in_use && (fs_nodes[node].type == FS_DIR || fs_nodes[node].type == FS_EXT2_DIR);
}
int vfs_is_dir(int node) {
    vfs_lock_acquire();
    int r = vfs_is_dir_unlocked(node);
    vfs_lock_release();
    return r;
}

static int vfs_is_file_unlocked(int node) {
    if (node < 0 || node >= MAX_NODES) return 0;
    return fs_nodes[node].in_use && (fs_nodes[node].type == FS_FILE || fs_nodes[node].type == FS_EXT2_FILE);
}
int vfs_is_file(int node) {
    vfs_lock_acquire();
    int r = vfs_is_file_unlocked(node);
    vfs_lock_release();
    return r;
}

static int vfs_get_node_count_unlocked() {
    int count = 0;
    for (int i = 0; i < MAX_NODES; i++) {
        if (fs_nodes[i].in_use) count++;
    }
    return count;
}
int vfs_get_node_count() {
    vfs_lock_acquire();
    int r = vfs_get_node_count_unlocked();
    vfs_lock_release();
    return r;
}
