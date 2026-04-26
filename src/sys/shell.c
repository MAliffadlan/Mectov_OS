#include "../include/shell.h"
#include "../include/vga.h"
#include "../include/keyboard.h"
#include "../include/speaker.h"
#include "../include/ata.h"
#include "../include/vfs.h"
#include "../include/utils.h"
#include "../include/apps.h"
#include "../include/security.h"
#include "../include/mem.h"

char cmd_b[256], hist_b[256]; 
int b_idx = 0, is_script = 0;
const char* cmd_list[] = {"mfetch", "waktu", "warna", "clear", "beep", "matikan", "mulaiulang", "ular", "ls", "buat", "tulis", "edit", "baca", "hapus", "echo", "tunggu", "nada", "jalankan", "kunci", "man", "mem"};

void run_script(const char* f) { 
    int i = vfs_find(f); 
    if (i < 0) return; 
    is_script = 1; 
    abort_ex = 0; 
    char* d = fs[i].data; 
    int l = fs[i].size, p = 0; 
    while (p < l && !abort_ex) { 
        b_idx = 0; 
        while (p < l && d[p] != '\n' && d[p] != '\0') { 
            if (b_idx < 255) cmd_b[b_idx++] = d[p]; 
            p++; 
        } 
        cmd_b[b_idx] = '\0'; 
        if (b_idx > 0) ex_cmd(); 
        if (d[p] == '\n') p++; 
    } 
    is_script = 0; 
    abort_ex = 0; 
}

void ex_cmd() { 
    if (!is_script) { 
        print("\n", 0x0F); 
        cmd_b[b_idx] = '\0'; 
        if (b_idx > 0) strcpy(hist_b, cmd_b); 
    } else {
        cmd_b[b_idx] = '\0';
    }

    if (b_idx == 0) {} 
    else if (strcmp(cmd_b, "matikan") == 0) shutdown();
    else if (strcmp(cmd_b, "mulaiulang") == 0) reboot();
    else if (strcmp(cmd_b, "kunci") == 0) lock_screen();
    else if (strcmp(cmd_b, "ular") == 0) { start_ular(); b_idx = 0; return; }
    else if (strcmp(cmd_b, "clear") == 0) { c_work(); }
    else if (strcmp(cmd_b, "waktu") == 0) { 
        unsigned char j = bcd_to_bin(read_cmos(0x04)), m = bcd_to_bin(read_cmos(0x02)), d = bcd_to_bin(read_cmos(0x00)); 
        int wj = (j + 7) % 24; 
        print("WIB: ", 0x0B); p_int(wj, 0x0E); print(":", 0x0F); p_int(m, 0x0E); print(":", 0x0F); p_int(d, 0x0E); print("\n", 0x0F); 
    }
    else if (strcmp(cmd_b, "mfetch") == 0) {
        unsigned char ch = bcd_to_bin(read_cmos(0x04)), cm = bcd_to_bin(read_cmos(0x02));
        int tb = boot_hour * 60 + boot_min, tc = ch * 60 + cm, diff = tc - tb; if (diff < 0) diff += 1440;
        int uf = 0; for(int i=0; i<MAX_FILES; i++) if(fs[i].in_use) uf++;
        
        print("       .---.        root", 0x0A); print("@", 0x0F); print("mectov-os\n", 0x0B);
        print("      /     \\       --------------\n", 0x0B);
        print("     | () () |      CPU: ", 0x0B); print(cpu_brand, 0x0A);
        print("\n      \\  ^  /       OS : MectovOS v11.0\n", 0x0B);
        print("       |||||        Uptime: ", 0x0B); p_int(diff/60, 0x0F); print("h ", 0x0F); p_int(diff%60, 0x0F); print("m\n", 0x0F);
        print("       |||||        Memory: ", 0x0B); p_int(get_used_memory() / 1024, 0x0F); print(" KB / ", 0x0F); p_int(get_total_memory() / 1024 / 1024, 0x0F); print(" MB\n", 0x0F);
        print("                    Storage: ", 0x0B); p_int(uf, 0x0F); print("/16 Files\n", 0x0B);
        print("                    MMM Core : Active (Paging)\n\n", 0x0F);
        for(int i=0; i<8; i++) { d_char(CX+20+i, CY+8, ' ', (i << 4)); } print("\n", 0x0F);
    }
    else if (strcmp(cmd_b, "mem") == 0) {
        print("--- Mectov Memory Manager ---\n", 0x0D);
        print("Total RAM: ", 0x0F); p_int(get_total_memory() / 1024 / 1024, 0x0A); print(" MB\n", 0x0F);
        print("Used RAM : ", 0x0F); p_int(get_used_memory() / 1024, 0x0A); print(" KB\n", 0x0F);
        print("Free RAM : ", 0x0F); p_int(get_free_memory() / 1024, 0x0A); print(" KB\n", 0x0F);
    }
    else if (strcmp(cmd_b, "warna") == 0) { 
        if (cur_col == 0x0F) cur_col = 0x0A; else if (cur_col == 0x0A) cur_col = 0x0B; else if (cur_col == 0x0B) cur_col = 0x0C; else cur_col = 0x0F; 
        print("Warna terminal diubah!\n", cur_col); 
    }
    else if (strcmp(cmd_b, "beep") == 0) { beep(); }
    else if (strcmp(cmd_b, "help") == 0) {
        print(" +------------------ HELP MENU - MECTOV OS v11.0 ------------------+\n", 0x0B);
        print(" | ", 0x0B); print("SISTEM      ", 0x0E); print("| mfetch, waktu, warna, clear, beep, ular, kunci, mem |\n", 0x0F);
        print(" | ", 0x0B); print("POWER       ", 0x0E); print("| matikan, mulaiulang                            |\n", 0x0F);
        print(" | ", 0x0B); print("FILESYSTEM  ", 0x0E); print("| ls, buat, tulis, baca, edit, hapus             |\n", 0x0F);
        print(" | ", 0x0B); print("SCRIPTING   ", 0x0E); print("| echo, tunggu, nada, jalankan                   |\n", 0x0F);
        print(" | ", 0x0B); print("MANUAL BOOK ", 0x0E); print("| man [perintah]                                 |\n", 0x0F);
        print(" +-----------------------------------------------------------------+\n", 0x0B);
    }
    else if (strncmp(cmd_b, "man ", 4) == 0) {
        char* t = &cmd_b[4];
        if (strcmp(t, "edit") == 0) print("Mectov Nano v1.0. Editor teks sakti. Tekan ESC untuk save & exit.\n", 0x0E);
        else if (strcmp(t, "ular") == 0) print("Game Snake Bare-metal. Kendali: W,A,S,D atau Panah. ESC keluar.\n", 0x0E);
        else if (strcmp(t, "kunci") == 0) print("Kunci layar dadakan. Password default: mectov123.\n", 0x0E);
        else if (strcmp(t, "mem") == 0) print("Menampilkan statistik Mectov Memory Manager (PMM).\n", 0x0E);
        else print("Manual tidak ditemukan.\n", 0x0C);
    }
    else if (strcmp(cmd_b, "ls") == 0) { for (int i = 0; i < MAX_FILES; i++) if (fs[i].in_use) { print("- ", 0x0F); print(fs[i].name, 0x0B); print("\n", 0x0F); } }
    else if (strncmp(cmd_b, "buat ", 5) == 0) { if (vfs_create(&cmd_b[5]) >= 0) { vfs_save(); print("Created.\n", 0x0B); } }
    else if (strncmp(cmd_b, "edit ", 5) == 0) { if (!is_script) { st_ed(&cmd_b[5]); b_idx = 0; return; } }
    else if (strncmp(cmd_b, "baca ", 5) == 0) { int i = vfs_find(&cmd_b[5]); if (i >= 0) { print(fs[i].data, 0x0F); print("\n", 0x0F); } }
    else if (strncmp(cmd_b, "hapus ", 6) == 0) { int i = vfs_find(&cmd_b[6]); if (i >= 0) { fs[i].in_use = 0; vfs_save(); print("Deleted.\n", 0x0C); } }
    else if (strncmp(cmd_b, "echo ", 5) == 0) { print(&cmd_b[5], 0x0F); print("\n", 0x0F); }
    else if (strncmp(cmd_b, "tunggu ", 7) == 0) { int ms = atoi(&cmd_b[7]); if (ms > 0) delay(ms); }
    else if (strncmp(cmd_b, "nada ", 5) == 0) { 
        int i = 5; while(cmd_b[i] == ' ') i++; int f = atoi(&cmd_b[i]);
        while(cmd_b[i] >= '0' && cmd_b[i] <= '9') i++; while(cmd_b[i] == ' ') i++;
        int d = atoi(&cmd_b[i]); if (f > 0 && d > 0) nada(f, d); 
    }
    else if (strncmp(cmd_b, "jalankan ", 9) == 0) { if (!is_script) { run_script(&cmd_b[9]); b_idx = 0; if (!is_script) print("root@mectov:~# ", 0x0A); return; } }
    else if (strncmp(cmd_b, "tulis ", 6) == 0) {
        int i = 6; while (cmd_b[i] == ' ') i++; int ns = i; while (cmd_b[i] != ' ' && cmd_b[i] != '\0') i++; 
        if (cmd_b[i] != '\0') { cmd_b[i] = '\0'; char* fn = &cmd_b[ns]; char* tx = &cmd_b[i+1]; int idx = vfs_find(fn); if (idx == -1) idx = vfs_create(fn); if (idx >= 0) { strcpy(fs[idx].data, tx); fs[idx].size = 0; while(tx[fs[idx].size]) fs[idx].size++; vfs_save(); print("Stored.\n", 0x0B); } }
    } else if (cmd_b[0] != '\0') { print("Command not found\n", 0x0C); }
    
    b_idx = 0; 
    if (!is_script) print("root@mectov:~# ", 0x0A); 
}
