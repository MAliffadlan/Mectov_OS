// ============================================================
// shell.c — Mectov OS Shell dengan Tab Completion & History
// ============================================================

#include "../include/shell.h"
#include "../include/vga.h"
#include "../include/keyboard.h"
#include "../include/utils.h"
#include "../include/vfs.h"
#include "../include/security.h"
#include "../include/speaker.h"
#include "../include/mem.h"
#include "../include/apps.h"
#include "../include/pci.h"
#include "../include/net.h"
#include "../include/rtl8139.h"
#include "../include/timer.h"
#include "../include/loader.h"
#include "../include/rtc.h"

// --- Command buffer & state ---
char cmd_b[CMD_BUF_SIZE]; int b_idx = 0;
char hist_b[256];
int is_script = 0;

const char* cmd_list[] = {
    "mfetch","waktu","warna","clear","beep","matikan","mulaiulang","ular",
    "ls","buat","tulis","edit","baca","hapus","echo","tunggu","nada",
    "jalankan","kunci","man","mem","help","cd","pwd","mkdir","rm","tree",
    "kmemstats","touch","cat","reboot","shutdown", NULL
};

// --- History circular buffer ---
char history[HIST_MAX][CMD_BUF_SIZE];
int hist_count = 0;
int hist_pos = -1;

static int hist_next_slot = 0; // next slot to overwrite (oldest)

static void history_add(const char* cmd) {
    if (!cmd || cmd[0] == '\0') return;
    // Don't add duplicate of last entry
    if (hist_count > 0) {
        int last = (hist_next_slot == 0) ? HIST_MAX - 1 : hist_next_slot - 1;
        if (strcmp(history[last], cmd) == 0) return;
    }
    strcpy(history[hist_next_slot], cmd);
    hist_next_slot = (hist_next_slot + 1) % HIST_MAX;
    if (hist_count < HIST_MAX) hist_count++;
    hist_pos = -1;
}

// --- Tab completion ---
int tab_match_count = 0;
char tab_matches[TAB_MAX][CMD_BUF_SIZE];

static int tab_match_prefix(const char* prefix, const char* str) {
    while (*prefix && *str && *prefix == *str) { prefix++; str++; }
    return (*prefix == '\0'); // prefix fully matched
}

int shell_try_complete() {
    tab_match_count = 0;
    if (b_idx == 0) return 0;
    
    int spc = 0;
    cmd_b[b_idx] = '\0';
    for (int i = 0; cmd_b[i]; i++) if (cmd_b[i] == ' ') spc++;
    
    // If there's a space, we're completing a filename arg
    if (spc > 0) {
        // Find start of last word
        int last = b_idx - 1;
        while (last >= 0 && cmd_b[last] != ' ') last--;
        int word_start = last + 1;
        
        // Extract prefix into temp buffer (don't corrupt cmd_b)
        char prefix[CMD_BUF_SIZE];
        strcpy(prefix, &cmd_b[word_start]);
        
        // Scan VFS for matching files/dirs
        for (int i = 0; i < MAX_NODES && tab_match_count < TAB_MAX; i++) {
            if (!fs_nodes[i].in_use) continue;
            int p = fs_nodes[i].parent;
            // Only match if parent is current_dir, or if path is absolute
            if (p == current_dir || current_dir == 0) {
                if (tab_match_prefix(prefix, fs_nodes[i].name)) {
                    strcpy(tab_matches[tab_match_count], fs_nodes[i].name);
                    tab_match_count++;
                }
            }
        }
    } else {
        // Completing built-in commands
        for (int i = 0; cmd_list[i] != NULL && tab_match_count < TAB_MAX; i++) {
            if (tab_match_prefix(cmd_b, cmd_list[i])) {
                strcpy(tab_matches[tab_match_count], cmd_list[i]);
                tab_match_count++;
            }
        }
    }
    return tab_match_count;
}

// --- History navigation ---
int shell_history_up() {
    if (hist_count == 0) return 0;
    if (hist_pos == -1) hist_pos = hist_next_slot; // start from newest
    // Go back one
    int new_pos = (hist_pos == 0) ? HIST_MAX - 1 : hist_pos - 1;
    // If we wrapped around past oldest, stop
    if (new_pos == hist_next_slot) return 0;
    
    // Check if there's actually a command there
    if (history[new_pos][0] == '\0') return 0;
    
    hist_pos = new_pos;
    strcpy(cmd_b, history[hist_pos]);
    b_idx = strlen(cmd_b);
    return 1;
}

int shell_history_down() {
    if (hist_count == 0 || hist_pos == -1) return 0;
    
    int new_pos = (hist_pos + 1) % HIST_MAX;
    
    // If we've gone back to baseline
    if (new_pos == hist_next_slot) {
        hist_pos = -1;
        cmd_b[0] = '\0';
        b_idx = 0;
        return 1;
    }
    
    hist_pos = new_pos;
    strcpy(cmd_b, history[hist_pos]);
    b_idx = strlen(cmd_b);
    return 1;
}

void shell_reset_history_nav() { hist_pos = -1; }

// --- Print a range of cmd_b ---
static void shell_redisplay() {
    cmd_b[b_idx] = '\0';
    if (get_use_term_buf()) {
        // Clear line by printing spaces, then reprint
        term_print("\r", 0x00);
        for (int i = 0; i < 79; i++) term_putchar(' ', 0x00);
        term_print("\rroot@mectov:~# ", 0x0A);
        term_print(cmd_b, 0x0F);
    } else {
        print("\rroot@mectov:~# ", 0x0A);
        print(cmd_b, 0x0F);
        // Clear rest of line
        int row_len = b_idx + 14;
        for (int i = row_len; i < 80; i++) print(" ", 0x00);
        print("\rroot@mectov:~# ", 0x0A);
        print(cmd_b, 0x0F);
    }
}

// --- Tab completion handler (call from keyboard handler) ---
// Apply a tab completion: replace cmd_b with common prefix
void shell_apply_tab() {
    int n = shell_try_complete();
    if (n == 0) return;
    
    if (n == 1) {
        // Single match — complete immediately
        // Find prefix length
        int old_len = b_idx;
        // Find start of last word
        int last = b_idx - 1;
        while (last >= 0 && cmd_b[last] != ' ') last--;
        int word_start = last + 1;
        
        // Replace from word_start with the match
        int j = word_start;
        int k = 0;
        while (tab_matches[0][k]) {
            cmd_b[j++] = tab_matches[0][k++];
        }
        // If directory, add trailing /
        int node = vfs_get_node(tab_matches[0]);
        if (node >= 0 && vfs_is_dir(node)) cmd_b[j++] = '/';
        cmd_b[j] = '\0';
        b_idx = j;
    } else {
        // Multiple matches — show them and redisplay
        if (get_use_term_buf()) {
            term_putchar('\n', 0x00);
            for (int i = 0; i < n; i++) {
                term_print(tab_matches[i], 0x0F);
                term_print("  ", 0x07);
                if ((i + 1) % 5 == 0) term_putchar('\n', 0x00);
            }
            term_putchar('\n', 0x00);
        } else {
            print("\n", 0x00);
            for (int i = 0; i < n; i++) {
                print(tab_matches[i], 0x0F);
                print("  ", 0x07);
                if ((i + 1) % 5 == 0) print("\n", 0x00);
            }
            print("\n", 0x00);
        }
    }
    shell_redisplay();
}

// ============================================================
// Main command execution
// ============================================================

void ex_cmd() {
    print("\n", 0x0F);
    cmd_b[b_idx] = '\0';
    
    // Save to history (skip empty)
    if (b_idx > 0) {
        history_add(cmd_b);
        strcpy(hist_b, cmd_b);
    }
    
    shell_reset_history_nav();
    
    // --- HELP ---
    if (strcmp(cmd_b, "help") == 0) {
        print("--- MECTOV OS v15.0 HELP MENU ---\n", 0x0B);
        print("SISTEM: mfetch, waktu, warna, clear, mem, kmemstats, kunci\n", 0x0E);
        print("FILES : ls, cd, pwd, mkdir, touch, cat, tree, rm\n", 0x0E);
        print("        buat, tulis, baca, edit, hapus (legacy)\n", 0x0E);
        print("APPS  : ular, nada, beep, echo, tunggu\n", 0x0E);
        print("POWER : matikan, mulaiulang, shutdown, reboot\n", 0x0E);
        print("HW    : lspci\n", 0x0E);
        print("NET   : ping [ip], host [domain]\n", 0x0E);
        print("NAV   : Tab=complete, Up/Down=history\n", 0x0E);
    }
    // --- CLEAR ---
    else if (strcmp(cmd_b, "clear") == 0) { 
        if (get_use_term_buf()) term_clear();
        else c_work(); 
    }
    // --- MFETCH ---
    else if (strcmp(cmd_b, "mfetch") == 0) {
        print("    .---.      ", 0x0A); print("root@mectov-os\n", 0x0B);
        print("   /     \\     ", 0x0A); print("--------------\n", 0x0F);
        print("  | () () |    ", 0x0B); print("OS  : Mectov OS v15.0\n", 0x0F);
        print("   \\  ^  /     ", 0x0B); print("MODE: VBE High-Res 32-bit\n", 0x0F);
        print("    |||||      ", 0x0B); print("MEM : ", 0x0F); p_int(get_used_memory()/1024, 0x0F); print(" KB used\n", 0x0F);
    }
    // --- MEM / KMEMSTATS ---
    else if (strcmp(cmd_b, "mem") == 0) {
        print("RAM Status:\n", 0x0B);
        print("Total: ", 0x0F); p_int(get_total_memory()/1024, 0x0A); print(" KB\n", 0x0F);
        print("Free : ", 0x0F); p_int(get_free_memory()/1024, 0x0A); print(" KB\n", 0x0F);
    }
    else if (strcmp(cmd_b, "kmemstats") == 0) {
        kmalloc_stats(print);
    }
    // --- CD ---
    else if (strncmp(cmd_b, "cd ", 3) == 0 || strcmp(cmd_b, "cd") == 0) {
        if (strcmp(cmd_b, "cd") == 0) {
            current_dir = 0; // Go to root
        } else {
            char* dirpath = cmd_b + 3;
            int node = vfs_get_node(dirpath);
            if (node < 0 || !vfs_is_dir(node)) {
                print("cd: directory not found: ", 0x0C);
                print(dirpath, 0x0C);
                print("\n", 0x0C);
            } else {
                current_dir = node;
            }
        }
    }
    // --- PWD ---
    else if (strcmp(cmd_b, "pwd") == 0) {
        char cwd[MAX_PATH];
        vfs_get_abs_path(current_dir, cwd, MAX_PATH);
        print(cwd, 0x0F);
        print("\n", 0x0F);
    }
    // --- LS (new VFS version) ---
    else if (strcmp(cmd_b, "ls") == 0) {
        vfs_list_dir(current_dir, print);
    }
    else if (strncmp(cmd_b, "ls ", 3) == 0) {
        char* dirpath = cmd_b + 3;
        int node = vfs_get_node(dirpath);
        if (node < 0 || !vfs_is_dir(node)) {
            print("ls: directory not found: ", 0x0C);
            print(dirpath, 0x0C);
            print("\n", 0x0C);
        } else {
            vfs_list_dir(node, print);
        }
    }
    // --- TREE ---
    else if (strcmp(cmd_b, "tree") == 0) {
        vfs_tree(current_dir, 0, print);
    }
    else if (strncmp(cmd_b, "tree ", 5) == 0) {
        char* dirpath = cmd_b + 5;
        int node = vfs_get_node(dirpath);
        if (node < 0 || !vfs_is_dir(node)) {
            print("tree: directory not found\n", 0x0C);
        } else {
            vfs_tree(node, 0, print);
        }
    }
    // --- MKDIR ---
    else if (strncmp(cmd_b, "mkdir ", 6) == 0) {
        char* dirpath = cmd_b + 6;
        int res = vfs_mkdir(dirpath);
        if (res < 0) {
            print("mkdir: failed (", 0x0C);
            p_int(res, 0x0C);
            print(")\n", 0x0C);
        }
    }
    // --- TOUCH (create empty file) ---
    else if (strncmp(cmd_b, "touch ", 6) == 0) {
        char* fpath = cmd_b + 6;
        int res = vfs_create_file(fpath);
        if (res < 0) {
            print("touch: failed\n", 0x0C);
        }
    }
    // --- CAT (read file) ---
    else if (strncmp(cmd_b, "cat ", 4) == 0) {
        char* fpath = cmd_b + 4;
        char buf[2048];
        int sz = vfs_read_file(fpath, buf, 2047);
        if (sz < 0) {
            print("cat: file not found\n", 0x0C);
        } else {
            buf[sz] = '\0';
            print(buf, 0x0F);
            print("\n", 0x0F);
        }
    }
    // --- RM (delete) ---
    else if (strncmp(cmd_b, "rm ", 3) == 0) {
        char* fpath = cmd_b + 3;
        int res = vfs_delete_node(fpath);
        if (res < 0) {
            print("rm: failed\n", 0x0C);
        }
    }
    // --- SHUTDOWN / REBOOT ---
    else if (strcmp(cmd_b, "matikan") == 0 || strcmp(cmd_b, "shutdown") == 0) shutdown();
    else if (strcmp(cmd_b, "mulaiulang") == 0 || strcmp(cmd_b, "reboot") == 0) reboot();
    // --- LSPCI ---
    else if (strcmp(cmd_b, "lspci") == 0) {
        print("--- PCI Bus Devices ---\n", 0x0B);
        for (int i = 0; i < pci_device_count; i++) {
            pci_device_t *d = &pci_devices[i];
            print(" ", 0x0F);
            print(pci_vendor_name(d->vendor_id), 0x0A);
            print(" | ", 0x07);
            print(pci_class_name(d->class_code, d->subclass), 0x0E);
            print("\n", 0x0F);
        }
    }
    // --- ULAR ---
    else if (strcmp(cmd_b, "ular") == 0) start_ular();
    // --- KUNCI ---
    else if (strcmp(cmd_b, "kunci") == 0) lock_screen();
    // --- WAKTU ---
    else if (strcmp(cmd_b, "waktu") == 0) {
        rtc_time_t tm = rtc_read_time();
        unsigned char h = (tm.hour + 7) % 24;
        print("Waktu sekarang (WIB): ", 0x0B);
        p_int(h, 0x0F); print(":", 0x0F);
        if (tm.minute < 10) { print("0", 0x0F); }
        p_int(tm.minute, 0x0F); print(":", 0x0F);
        if (tm.second < 10) { print("0", 0x0F); }
        p_int(tm.second, 0x0F); print("\n", 0x0F);
    }
    // --- WARNA ---
    else if (strcmp(cmd_b, "warna") == 0) {
        print("Fitur warna hanya untuk TTY (Text Mode).\n", 0x0E);
    }
    // --- JALANKAN (run .mct app) ---
    else if (strncmp(cmd_b, "jalankan ", 9) == 0) {
        char* fname = cmd_b + 9;
        // Use vfs_find from old API or the new vfs_get_node
        // Backward compat: try old vfs_find
        int fid = -1;
        // Check if old vfs_find exists (might be in vfs.h old API)
        // Use new VFS: read file data
        int node = vfs_get_node(fname);
        if (node < 0) {
            print("File tidak ditemukan: ", 0x0C); print(fname, 0x0C); print("\n", 0x0C);
        } else {
            print("Membuka aplikasi MCT: ", 0x0A); print(fname, 0x0A); print("\n", 0x0A);
            int res = load_mct_app(fname);
            if (res >= 0) {
                print("[+] Task User Mode Dibuat! (Task ID: ", 0x0A); p_int(res, 0x0A); print(")\n", 0x0A);
                
                extern int term_app_running;
                extern int term_app_task_id;
                term_app_running = 1;
                term_app_task_id = res;
                return; // DO NOT PRINT PROMPT
            } else {
                print("[-] Gagal menjalankan! Error: ", 0x0C); p_int(res, 0x0C); print("\n", 0x0C);
            }
        }
    }
    // --- Legacy: BUAT ---
    else if (strncmp(cmd_b, "buat ", 5) == 0) {
        char* fname = cmd_b + 5;
        // Use new VFS
        int res = vfs_create_file(fname);
        if (res >= 0) print("File berhasil dibuat.\n", 0x0A);
        else if (res == -2) print("File sudah ada.\n", 0x0C);
        else print("Disk penuh!\n", 0x0C);
    }
    // --- Legacy: BACA ---
    else if (strncmp(cmd_b, "baca ", 5) == 0) {
        char* fname = cmd_b + 5;
        char buf[2048];
        int sz = vfs_read_file(fname, buf, 2047);
        if (sz < 0) print("File tidak ditemukan.\n", 0x0C);
        else { buf[sz] = '\0'; print(buf, 0x0F); print("\n", 0x0F); }
    }
    // --- Legacy: EDIT / TULIS ---
    else if (strncmp(cmd_b, "edit ", 5) == 0 || strncmp(cmd_b, "tulis ", 6) == 0) {
        char* fname = (cmd_b[0] == 'e') ? (cmd_b + 5) : (cmd_b + 6);
        st_ed(fname);
    }
    // --- Legacy: HAPUS ---
    else if (strncmp(cmd_b, "hapus ", 6) == 0) {
        char* fname = cmd_b + 6;
        int res = vfs_delete_node(fname);
        if (res < 0) print("File tidak ditemukan.\n", 0x0C);
        else print("File dihapus.\n", 0x0A);
    }
    // --- PING ---
    else if (strncmp(cmd_b, "ping ", 5) == 0) {
        char* ip_str = cmd_b + 5;
        uint8_t tip[4] = {0, 0, 0, 0};
        int octet = 0, val = 0;
        for (int i = 0; ip_str[i] && octet < 4; i++) {
            if (ip_str[i] >= '0' && ip_str[i] <= '9') val = val * 10 + (ip_str[i] - '0');
            else if (ip_str[i] == '.') { tip[octet++] = (uint8_t)val; val = 0; }
        }
        if (octet < 4) tip[octet] = (uint8_t)val;
        
        if (!rtl_present) {
            print("No network card detected.\n", 0x0C);
        } else {
            print("PING ", 0x0B);
            p_int(tip[0], 0x0F); print(".", 0x0F);
            p_int(tip[1], 0x0F); print(".", 0x0F);
            p_int(tip[2], 0x0F); print(".", 0x0F);
            p_int(tip[3], 0x0F); print(" ...\n", 0x0F);
            
            if (!net_ready) {
                print("Resolving gateway (ARP)...\n", 0x07);
                net_send_arp_request(gateway_ip);
                uint32_t arp_start = get_ticks();
                while (!net_ready && (get_ticks() - arp_start) < 2000) net_poll();
                if (!net_ready) {
                    print("ARP timeout: gateway not found.\n", 0x0C);
                    goto ping_done;
                }
                print("Gateway resolved!\n", 0x0A);
            }
            
            net_send_ping(tip);
            uint32_t start = get_ticks();
            while (!ping_replied && (get_ticks() - start) < 3000) net_poll();
            if (ping_replied) {
                uint32_t ms = (ping_rtt * 1000) / 60;
                print("Reply from ", 0x0A);
                p_int(tip[0], 0x0F); print(".", 0x0F);
                p_int(tip[1], 0x0F); print(".", 0x0F);
                p_int(tip[2], 0x0F); print(".", 0x0F);
                p_int(tip[3], 0x0F);
                print(" time=", 0x0F); p_int(ms, 0x0A); print("ms\n", 0x0F);
            } else {
                print("Request timed out.\n", 0x0C);
            }
        }
        ping_done: ;
    }
    // --- HOST ---
    else if (strncmp(cmd_b, "host ", 5) == 0) {
        char* domain = cmd_b + 5;
        if (!rtl_present) {
            print("No network card detected.\n", 0x0C);
        } else {
            print("Resolving ", 0x0B); print(domain, 0x0F); print(" ...\n", 0x0F);
            
            if (!net_ready) {
                net_send_arp_request(gateway_ip);
                uint32_t arp_start = get_ticks();
                while (!net_ready && (get_ticks() - arp_start) < 2000) net_poll();
                if (!net_ready) {
                    print("  [!] Gateway ARP timeout!\n", 0x0C);
                    goto host_done;
                }
            }
            
            net_send_dns_query(domain);
            uint32_t start = get_ticks();
            while (!dns_resolved && (get_ticks() - start) < 3000) net_poll();
            
            if (dns_resolved) {
                print(domain, 0x0A); print(" has address ", 0x0F);
                p_int(dns_resolved_ip[0], 0x0A); print(".", 0x0A);
                p_int(dns_resolved_ip[1], 0x0A); print(".", 0x0A);
                p_int(dns_resolved_ip[2], 0x0A); print(".", 0x0A);
                p_int(dns_resolved_ip[3], 0x0A); print("\n", 0x0A);
            } else {
                print("Host not found (timeout).\n", 0x0C);
            }
        }
        host_done: ;
    }
    // --- ECHO ---
    else if (strncmp(cmd_b, "echo ", 5) == 0) {
        print(cmd_b + 5, 0x0F);
        print("\n", 0x0F);
    }
    // --- TUNGGU (sleep) ---
    else if (strncmp(cmd_b, "tunggu ", 7) == 0) {
        int ms = atoi(cmd_b + 7);
        if (ms > 0 && ms < 60000) {
            uint32_t start = get_ticks();
            while ((get_ticks() - start) < (uint32_t)ms) { /* wait */ }
        }
    }
    // --- NADA (beep frequency) ---
    else if (strncmp(cmd_b, "nada ", 5) == 0) {
        int freq = atoi(cmd_b + 5);
        if (freq > 20 && freq < 20000) beep(freq, 300);
    }
    // --- BEEP ---
    else if (strcmp(cmd_b, "beep") == 0) {
        beep(880, 200);
    }
    // --- MAN ---
    else if (strncmp(cmd_b, "man ", 4) == 0) {
        char* topic = cmd_b + 4;
        if (strcmp(topic, "cd") == 0) {
            print("cd [dir]  — Change directory\n", 0x0B);
            print("  cd /   = go to root\n", 0x0F);
            print("  cd ..  = go up one level\n", 0x0F);
            print("  cd home/user = relative path\n", 0x0F);
        } else if (strcmp(topic, "ls") == 0) {
            print("ls [dir]  — List directory contents\n", 0x0B);
        } else if (strcmp(topic, "mkdir") == 0) {
            print("mkdir [path] — Create directory\n", 0x0B);
        } else if (strcmp(topic, "rm") == 0) {
            print("rm [path] — Remove file or directory\n", 0x0B);
        } else if (strcmp(topic, "touch") == 0) {
            print("touch [path] — Create empty file\n", 0x0B);
        } else if (strcmp(topic, "cat") == 0) {
            print("cat [path] — Display file contents\n", 0x0B);
        } else {
            print("man: no manual entry for '", 0x0C);
            print(topic, 0x0C);
            print("'\n", 0x0C);
        }
    }
    // --- UNKNOWN ---
    else if (cmd_b[0] != '\0') {
        print("Command not found: ", 0x0C);
        print(cmd_b, 0x0C);
        print("\n", 0x0C);
    }
    
    b_idx = 0;
    
    extern int term_app_running;
    if (!term_app_running) {
        print("root@mectov:~# ", 0x0A); 
    }
}

void run_script(const char* f) {
    is_script = 1;
    // Find file in new VFS
    char buf[2048];
    int sz = vfs_read_file(f, buf, 2047);
    if (sz < 0) {
        print("run_script: file not found\n", 0x0C);
        is_script = 0;
        return;
    }
    buf[sz] = '\0';
    
    // Parse line by line
    int i = 0, j = 0;
    while (buf[i]) {
        if (buf[i] == '\n' || buf[i] == '\r') {
            if (j > 0) {
                cmd_b[j] = '\0';
                b_idx = j;
                print("> ", 0x0A);
                print(cmd_b, 0x0F);
                ex_cmd();
                // Don't print prompt (ex_cmd does it)
                j = 0;
            }
        } else if (j < CMD_BUF_SIZE - 1) {
            cmd_b[j++] = buf[i];
        }
        i++;
    }
    if (j > 0) {
        cmd_b[j] = '\0';
        b_idx = j;
        print("> ", 0x0A);
        print(cmd_b, 0x0F);
        ex_cmd();
    }
    is_script = 0;
}