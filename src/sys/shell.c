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

char cmd_b[256]; int b_idx = 0;
char hist_b[256];
const char* cmd_list[] = {"mfetch","waktu","warna","clear","beep","matikan","mulaiulang","ular","ls","buat","tulis","edit","baca","hapus","echo","tunggu","nada","jalankan","kunci","man","mem","help"};

void ex_cmd() {
    print("\n", 0x0F);
    cmd_b[b_idx] = '\0';
    if (b_idx > 0) strcpy(hist_b, cmd_b);

    if (strcmp(cmd_b, "help") == 0) {
        print("--- MECTOV OS v13.5 HELP MENU ---\n", 0x0B);
        print("SISTEM: mfetch, waktu, warna, clear, mem, kunci\n", 0x0E);
        print("FILES : ls, buat, tulis, baca, hapus, edit\n", 0x0E);
        print("APPS  : ular, nada, beep, echo\n", 0x0E);
        print("POWER : matikan, mulaiulang\n", 0x0E);
        print("HW    : lspci\n", 0x0E);
    }
    else if (strcmp(cmd_b, "clear") == 0) { 
        if (get_use_term_buf()) term_clear();
        else c_work(); 
    }
    else if (strcmp(cmd_b, "mfetch") == 0) {
        print("    .---.      ", 0x0A); print("root@mectov-os\n", 0x0B);
        print("   /     \\     ", 0x0A); print("--------------\n", 0x0F);
        print("  | () () |    ", 0x0B); print("OS  : Mectov OS v13.5\n", 0x0F);
        print("   \\  ^  /     ", 0x0B); print("MODE: VBE High-Res 32-bit\n", 0x0F);
        print("    |||||      ", 0x0B); print("MEM : ", 0x0F); p_int(get_used_memory()/1024, 0x0F); print(" KB used\n", 0x0F);
    }
    else if (strcmp(cmd_b, "mem") == 0) {
        print("RAM Status:\n", 0x0B);
        print("Total: ", 0x0F); p_int(get_total_memory()/1024, 0x0A); print(" KB\n", 0x0F);
        print("Free : ", 0x0F); p_int(get_free_memory()/1024, 0x0A); print(" KB\n", 0x0F);
    }
    else if (strcmp(cmd_b, "ls") == 0) { 
        for (int i = 0; i < MAX_FILES; i++) if (fs[i].in_use) { print("- ", 0x0F); print(fs[i].name, 0x0B); print("\n", 0x0F); } 
    }
    else if (strcmp(cmd_b, "matikan") == 0) shutdown();
    else if (strcmp(cmd_b, "mulaiulang") == 0) reboot();
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
    else if (strcmp(cmd_b, "ular") == 0) start_ular();
    else if (strcmp(cmd_b, "kunci") == 0) lock_screen();
    else if (strcmp(cmd_b, "waktu") == 0) {
        unsigned char h = bcd_to_bin(read_cmos(0x04));
        unsigned char m = bcd_to_bin(read_cmos(0x02));
        unsigned char s = bcd_to_bin(read_cmos(0x00));
        h = (h + 7) % 24; // Convert UTC to WIB (UTC+7)
        print("Waktu sekarang (WIB): ", 0x0B);
        p_int(h, 0x0F); print(":", 0x0F);
        if (m < 10) print("0", 0x0F); p_int(m, 0x0F); print(":", 0x0F);
        if (s < 10) print("0", 0x0F); p_int(s, 0x0F); print("\n", 0x0F);
    }
    else if (strcmp(cmd_b, "warna") == 0) {
        print("Fitur warna hanya untuk TTY (Text Mode).\n", 0x0E);
    }
    else if (strncmp(cmd_b, "jalankan ", 9) == 0) {
        char* fname = cmd_b + 9;
        int fid = vfs_find(fname);
        if (fid == -1) {
            print("Script tidak ditemukan: ", 0x0C); print(fname, 0x0C); print("\n", 0x0C);
        } else {
            print("Menjalankan script: ", 0x0A); print(fname, 0x0A); print("\n", 0x0A);
            // Sangat sederhana: print isi file karena engine script asli belum ada
            print(fs[fid].data, 0x0F);
            print("\n[Script selesai]\n", 0x0A);
        }
    }
    else if (strncmp(cmd_b, "buat ", 5) == 0) {
        char* fname = cmd_b + 5;
        int res = vfs_create(fname);
        if (res >= 0) print("File berhasil dibuat.\n", 0x0A);
        else if (res == -2) print("File sudah ada.\n", 0x0C);
        else print("Disk penuh!\n", 0x0C);
    }
    else if (strncmp(cmd_b, "baca ", 5) == 0) {
        char* fname = cmd_b + 5;
        int fid = vfs_find(fname);
        if (fid == -1) print("File tidak ditemukan.\n", 0x0C);
        else { print(fs[fid].data, 0x0F); print("\n", 0x0F); }
    }
    else if (strncmp(cmd_b, "edit ", 5) == 0 || strncmp(cmd_b, "tulis ", 6) == 0) {
        char* fname = (cmd_b[0] == 'e') ? (cmd_b + 5) : (cmd_b + 6);
        st_ed(fname);
    }
    else if (strncmp(cmd_b, "hapus ", 6) == 0) {
        char* fname = cmd_b + 6;
        int fid = vfs_find(fname);
        if (fid == -1) print("File tidak ditemukan.\n", 0x0C);
        else { fs[fid].in_use = 0; print("File dihapus.\n", 0x0A); vfs_save(); }
    }
    else if (cmd_b[0] != '\0') { print("Command not found\n", 0x0C); }
    
    b_idx = 0; 
    print("root@mectov:~# ", 0x0A); 
}
