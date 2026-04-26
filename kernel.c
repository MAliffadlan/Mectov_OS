// --- MECTOV OS v10.0 (The Intelligence & Security Update) ---
static inline unsigned char inb(unsigned short port) {
    unsigned char ret; __asm__ __volatile__ ( "inb %1, %0" : "=a"(ret) : "Nd"(port) ); return ret;
}
static inline void outb(unsigned short port, unsigned char val) {
    __asm__ __volatile__ ( "outb %0, %1" : : "a"(val), "Nd"(port) );
}
static inline unsigned short inw(unsigned short port) {
    unsigned short ret; __asm__ __volatile__ ( "inw %1, %0" : "=a"(ret) : "Nd"(port) ); return ret;
}
static inline void outw(unsigned short port, unsigned short val) {
    __asm__ __volatile__ ( "outw %0, %1" : : "a"(val), "Nd"(port) );
}

unsigned char read_cmos(unsigned char reg) { outb(0x70, reg); return inb(0x71); }
unsigned char bcd_to_bin(unsigned char bcd) { return ((bcd & 0xF0) >> 1) + ((bcd & 0xF0) >> 3) + (bcd & 0x0F); }

// --- CPUID & UPTIME ---
char cpu_brand[49];
void detect_cpu() {
    unsigned int eax, ebx, ecx, edx;
    unsigned int *ptr = (unsigned int *)cpu_brand;
    for (unsigned int i = 0; i < 3; i++) {
        __asm__ __volatile__ ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000002 + i));
        ptr[i * 4 + 0] = eax; ptr[i * 4 + 1] = ebx; ptr[i * 4 + 2] = ecx; ptr[i * 4 + 3] = edx;
    }
    cpu_brand[48] = '\0';
}
unsigned char boot_sec, boot_min, boot_hour;
void init_uptime() { boot_sec = bcd_to_bin(read_cmos(0x00)); boot_min = bcd_to_bin(read_cmos(0x02)); boot_hour = bcd_to_bin(read_cmos(0x04)); }

// --- POWER ---
void shutdown() { outw(0x604, 0x2000); }
void reboot() { unsigned char g = 0x02; while (g & 0x02) g = inb(0x64); outb(0x64, 0xFE); }

// --- HARDWARE CURSOR ---
void update_hw_cursor(int x, int y) {
    unsigned short pos = y * 80 + x;
    outb(0x3D4, 0x0F); outb(0x3D5, (unsigned char) (pos & 0xFF));
    outb(0x3D4, 0x0E); outb(0x3D5, (unsigned char) ((pos >> 8) & 0xFF));
}

// --- HDD ATA ---
void ata_wait_bsy() { while(inb(0x1F7) & 0x80); }
void ata_wait_drq() { while(!(inb(0x1F7) & 0x08)); }
void ata_read_sector(unsigned int lba, unsigned char* b) {
    ata_wait_bsy(); outb(0x1F6, 0xE0 | ((lba >> 24) & 0x0F)); outb(0x1F2, 1); outb(0x1F3, (unsigned char)lba);
    outb(0x1F4, (unsigned char)(lba >> 8)); outb(0x1F5, (unsigned char)(lba >> 16)); outb(0x1F7, 0x20);
    ata_wait_bsy(); ata_wait_drq();
    for (int i = 0; i < 256; i++) { unsigned short word = inw(0x1F0); b[i * 2] = (unsigned char)word; b[i * 2 + 1] = (unsigned char)(word >> 8); }
}
void ata_write_sector(unsigned int lba, unsigned char* b) {
    ata_wait_bsy(); outb(0x1F6, 0xE0 | ((lba >> 24) & 0x0F)); outb(0x1F2, 1); outb(0x1F3, (unsigned char)lba);
    outb(0x1F4, (unsigned char)(lba >> 8)); outb(0x1F5, (unsigned char)(lba >> 16)); outb(0x1F7, 0x30);
    ata_wait_bsy(); ata_wait_drq();
    for (int i = 0; i < 256; i++) { unsigned short word = b[i * 2] | (b[i * 2 + 1] << 8); outw(0x1F0, word); }
    outb(0x1F7, 0xE7); ata_wait_bsy();
}

// --- AUDIO & DELAY ---
void play_sound(unsigned int n) { unsigned int d = 1193180/n; outb(0x43,0xb6); outb(0x42,(unsigned char)d); outb(0x42,(unsigned char)(d>>8)); unsigned char t = inb(0x61); if(t!=(t|3)) outb(0x61,t|3); }
void nosound() { outb(0x61, inb(0x61) & 0xFC); }
int abort_ex = 0;
void delay(int ms) { for (volatile int i = 0; i < ms; i++) { if (inb(0x64) & 1) if (inb(0x60) == 0x01) { abort_ex = 1; nosound(); } if (abort_ex) return; for (volatile int j = 0; j < 4000; j++) __asm__ __volatile__ ("pause"); } }
void beep() { play_sound(1000); delay(100); nosound(); }
void nada(int f, int d) { if (abort_ex) return; if (f > 0) play_sound(f); delay(d); nosound(); }

// --- KEYBOARD ---
int shift_p = 0, caps_a = 0;
char scancode_to_char(unsigned char s) {
    static unsigned char m_n[] = { 0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ', 0 };
    static unsigned char m_s[] = { 0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '\"', '~', 0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ', 0 };
    if (s < 58) {
        char c = m_n[s], cs = m_s[s]; int isl = (c >= 'a' && c <= 'z');
        if (shift_p) return (isl && caps_a) ? c : cs;
        else return (isl && caps_a) ? cs : c;
    }
    return 0;
}

// --- GUI ---
volatile char* video_m = (volatile char*) 0xb8000;
unsigned char cur_col = 0x0F; 
int cx = 0, cy = 0;
#define WIN_X 2
#define WIN_Y 2
#define WIN_W 76
#define WIN_H 20
#define CX (WIN_X + 1)
#define CY (WIN_Y + 1)
#define CW (WIN_W - 2)
#define CH (WIN_H - 2)

void d_char(int x, int y, char c, unsigned char col) { if (x >= 0 && x < 80 && y >= 0 && y < 25) { int i = (y * 80 + x) * 2; video_m[i] = c; video_m[i + 1] = col; } }
void d_desktop() { for (int y = 0; y < 24; y++) for (int x = 0; x < 80; x++) d_char(x, y, 176, 0x09); for (int x = 0; x < 80; x++) d_char(x, 24, ' ', 0x70); }
void d_win(int x, int y, int w, int h, const char* t) { for (int i = x; i < x + w; i++) for (int j = y; j < y + h; j++) { if (j == y) d_char(i, j, ' ', 0x1F); else if (i == x || i == x + w - 1 || j == y + h - 1) d_char(i, j, 177, 0x1F); else d_char(i, j, ' ', 0x0F); } int tl = 0; while(t[tl]) tl++; int tx = x + (w - tl) / 2; for(int i = 0; i < tl; i++) d_char(tx + i, y, t[i], 0x1F); }
void c_work() { for (int y = CY; y < CY + CH - 1; y++) for (int x = CX; x < CX + CW; x++) d_char(x, y, ' ', 0x0F); cx = 0; cy = 0; update_hw_cursor(CX, CY); }
void s_work() { for (int y = CY; y < CY + CH - 2; y++) for (int x = CX; x < CX + CW; x++) { int s = ((y + 1) * 80 + x) * 2, d = (y * 80 + x) * 2; video_m[d] = video_m[s]; video_m[d + 1] = video_m[s + 1]; } int ly = CY + CH - 2; for (int x = CX; x < CX + CW; x++) d_char(x, ly, ' ', 0x0F); cy--; }
void print(const char* s, unsigned char col) { int i = 0; while (s[i] != '\0') { if (s[i] == '\n') { cx = 0; cy++; } else { d_char(CX + cx, CY + cy, s[i], col); cx++; if (cx >= CW) { cx = 0; cy++; } } if (cy >= CH - 1) s_work(); i++; } update_hw_cursor(CX + cx, CY + cy); }
void p_char(char c, unsigned char col) { if (c == '\n') { cx = 0; cy++; } else { d_char(CX + cx, CY + cy, c, col); cx++; if (cx >= CW) { cx = 0; cy++; } } if (cy >= CH - 1) s_work(); update_hw_cursor(CX + cx, CY + cy); }

const char* marquee_text = ">>> Mectov OS v10.0 - Tab Auto-Complete Enabled - Secure Screen Lock Active - Indonesia Raya <<<         ";
int marquee_pos = 0; int marquee_counter = 0;
void wait_retrace() { while (inb(0x3DA) & 0x08); while (!(inb(0x3DA) & 0x08)); }
void update_marquee() {
    marquee_counter++; if (marquee_counter < 350000) return; marquee_counter = 0;
    int text_len = 0; while(marquee_text[text_len]) text_len++;
    wait_retrace();
    for (int i = 0; i < 80; i++) { int c_idx = (marquee_pos + i) % text_len; int v_idx = (24 * 80 + i) * 2; video_m[v_idx] = marquee_text[c_idx]; video_m[v_idx + 1] = 0x70; }
    marquee_pos = (marquee_pos + 1) % text_len;
}

// --- UTILS ---
int strcmp(const char* s1, const char* s2) { while (*s1 && (*s1 == *s2)) { s1++; s2++; } return *(const unsigned char*)s1 - *(const unsigned char*)s2; }
int strncmp(const char* s1, const char* s2, int n) { while (n && *s1 && (*s1 == *s2)) { ++s1; ++s2; --n; } if (n == 0) return 0; return (*(unsigned char *)s1 - *(unsigned char *)s2); }
void strcpy(char* d, const char* s) { while ((*d++ = *s++)); }
void p_int(int n, unsigned char c) { if (n < 0) { print("-", c); n = -n; } if (n == 0) { print("0", c); return; } char buf[10]; int i = 0; while (n > 0) { buf[i++] = (n % 10) + '0'; n /= 10; } for (int j = 0; j < i / 2; j++) { char t = buf[j]; buf[j] = buf[i - j - 1]; buf[i - j - 1] = t; } buf[i] = '\0'; print(buf, c); }
int atoi(const char* s) { int r = 0, si = 1, i = 0; if (s[0] == '-') { si = -1; i++; } for (; s[i] != '\0'; ++i) { if (s[i] >= '0' && s[i] <= '9') r = r * 10 + s[i] - '0'; else break; } return si * r; }
unsigned int rand_seed = 0;
unsigned int rand() { rand_seed = rand_seed * 1103515245 + 12345; return (unsigned int)(rand_seed / 65536) % 32768; }

// --- VFS ---
#define MAX_FILES 16
#define MAX_FILENAME 16
#define MAX_FILE_SIZE 1024
typedef struct { char name[MAX_FILENAME]; char data[MAX_FILE_SIZE]; int size; int in_use; char padding[488]; } File;
File fs[MAX_FILES];
void vfs_save() { unsigned char* p = (unsigned char*)fs; for (int i = 0; i < 48; i++) ata_write_sector(i + 1, p + (i * 512)); unsigned char s[512] = {0}; s[0] = 'M'; s[1] = 'E'; s[2] = 'C'; s[3] = 'T'; s[4] = 'O'; s[5] = 'V'; ata_write_sector(0, s); }
int vfs_load() { unsigned char s[512]; ata_read_sector(0, s); if (s[0] == 'M' && s[1] == 'E' && s[2] == 'C' && s[3] == 'T' && s[4] == 'O' && s[5] == 'V') { unsigned char* p = (unsigned char*)fs; for (int i = 0; i < 48; i++) ata_read_sector(i + 1, p + (i * 512)); return 1; } return 0; }
int vfs_find(const char* n) { for (int i = 0; i < MAX_FILES; i++) if (fs[i].in_use && strcmp(fs[i].name, n) == 0) return i; return -1; }
int vfs_create(const char* n) { if (vfs_find(n) != -1) return -2; for (int i = 0; i < MAX_FILES; i++) if (!fs[i].in_use) { strcpy(fs[i].name, n); fs[i].in_use = 1; fs[i].size = 0; fs[i].data[0] = '\0'; return i; } return -1; }

// --- EDITOR ---
int ed_a = 0; char ed_b[MAX_FILE_SIZE], ed_fn[MAX_FILENAME]; int ed_c = 0;
void st_ed(const char* f) { strcpy(ed_fn, f); int i = vfs_find(f); if (i >= 0) { strcpy(ed_b, fs[i].data); ed_c = fs[i].size; } else { ed_b[0] = '\0'; ed_c = 0; } ed_a = 1; c_work(); print(ed_b, 0x0F); update_hw_cursor(CX + (ed_c % CW), CY + (ed_c / CW)); }
void sa_ex_ed() { int i = vfs_find(ed_fn); if (i == -1) i = vfs_create(ed_fn); if (i >= 0) { strcpy(fs[i].data, ed_b); fs[i].size = ed_c; } vfs_save(); ed_a = 0; c_work(); print("root@mectov:~# ", 0x0A); }

// --- SNAKE GAME ---
void start_ular() {
    c_work(); d_win(15, 5, 50, 15, " Mectov Ular v1.2 - WASD / Arrows ");
    int sx[100], sy[100], len = 3, fx, fy, dir = 1, score = 0;
    for(int i=0; i<len; i++) { sx[i] = 25 - i; sy[i] = 12; } fx = 30; fy = 10;
    while(1) {
        rand_seed++; update_marquee();
        if (inb(0x64) & 1) {
            unsigned char sc = inb(0x60);
            if (sc == 0x01) break; 
            if ((sc == 0x11 || sc == 0x48) && dir != 2) dir = 0;
            if ((sc == 0x1F || sc == 0x50) && dir != 0) dir = 2;
            if ((sc == 0x1E || sc == 0x4B) && dir != 1) dir = 3;
            if ((sc == 0x20 || sc == 0x4D) && dir != 3) dir = 1;
        }
        for(int i=len-1; i>0; i--) { sx[i] = sx[i-1]; sy[i] = sy[i-1]; }
        if(dir==0) sy[0]--; else if(dir==1) sx[0]++; else if(dir==2) sy[0]++; else if(dir==3) sx[0]--;
        if(sx[0] < 16 || sx[0] > 63 || sy[0] < 6 || sy[0] > 18) break;
        if(sx[0] == fx && sy[0] == fy) { score += 10; len++; fx = 16 + (rand()%47); fy = 6 + (rand()%12); beep(); }
        for(int y=6; y<19; y++) for(int x=16; x<64; x++) d_char(x,y,' ',0x0F);
        d_char(fx, fy, '*', 0x0E);
        for(int i=0; i<len; i++) d_char(sx[i], sy[i], (i==0?'0':'o'), 0x0A);
        delay(80 - (len > 30 ? 60 : len));
    }
    beep(); c_work(); print("Game Over! Skor: ", 0x0E); p_int(score, 0x0A); print("\n", 0x0F);
}

// --- SCREEN LOCK ---
int is_locked = 0;
void lock_screen() {
    is_locked = 1;
    beep(); c_work();
    d_win(WIN_X, WIN_Y, WIN_W, WIN_H, " Mectov Secure Lock ");
    print("\nSystem Locked. Please enter password.\nPassword: ", 0x0E);
}

// --- SHELL ---
char cmd_b[256], hist_b[256]; int b_idx = 0, is_script = 0;
const char* cmd_list[] = {"mfetch", "waktu", "warna", "clear", "beep", "matikan", "mulaiulang", "ular", "ls", "buat", "tulis", "edit", "baca", "hapus", "echo", "tunggu", "nada", "jalankan", "kunci"};

void ex_cmd();
void run_script(const char* f) { int i = vfs_find(f); if (i < 0) return; is_script = 1; abort_ex = 0; char* d = fs[i].data; int l = fs[i].size, p = 0; while (p < l && !abort_ex) { b_idx = 0; while (p < l && d[p] != '\n' && d[p] != '\0') { if (b_idx < 255) cmd_b[b_idx++] = d[p]; p++; } cmd_b[b_idx] = '\0'; if (b_idx > 0) ex_cmd(); if (d[p] == '\n') p++; } is_script = 0; abort_ex = 0; }

void ex_cmd() {
    if (!is_script) { print("\n", 0x0F); cmd_b[b_idx] = '\0'; if (b_idx > 0) strcpy(hist_b, cmd_b); }
    else cmd_b[b_idx] = '\0';

    if (b_idx == 0) {} 
    else if (strcmp(cmd_b, "matikan") == 0) shutdown();
    else if (strcmp(cmd_b, "mulaiulang") == 0) reboot();
    else if (strcmp(cmd_b, "kunci") == 0) lock_screen();
    else if (strcmp(cmd_b, "ular") == 0) { start_ular(); b_idx = 0; return; }
    else if (strcmp(cmd_b, "clear") == 0) { c_work(); }
    else if (strcmp(cmd_b, "waktu") == 0) { unsigned char j = bcd_to_bin(read_cmos(0x04)), m = bcd_to_bin(read_cmos(0x02)), d = bcd_to_bin(read_cmos(0x00)); int wj = (j + 7) % 24; print("WIB: ", 0x0B); p_int(wj, 0x0E); print(":", 0x0F); p_int(m, 0x0E); print(":", 0x0F); p_int(d, 0x0E); print("\n", 0x0F); }
    else if (strcmp(cmd_b, "mfetch") == 0) {
        unsigned char ch = bcd_to_bin(read_cmos(0x04)), cm = bcd_to_bin(read_cmos(0x02));
        int tb = boot_hour * 60 + boot_min, tc = ch * 60 + cm, diff = tc - tb; if (diff < 0) diff += 1440;
        int uf = 0; for(int i=0; i<MAX_FILES; i++) if(fs[i].in_use) uf++;
        print("       .---.        root@mectov-os\n      /     \\       --------------\n     | () () |      CPU: ", 0x0B); print(cpu_brand, 0x0A);
        print("\n      \\  ^  /       OS : MectovOS v10.0\n       |||||        Uptime: ", 0x0B); p_int(diff/60, 0x0F); print("h ", 0x0F); p_int(diff%60, 0x0F); print("m\n", 0x0F);
        print("       |||||        Storage: ", 0x0B); p_int(uf, 0x0F); print("/16 Files\n", 0x0B);
        print("                    Tab Auto-Complete : OK\n\n", 0x0F);
        for(int i=0; i<8; i++) { d_char(CX+20+i, CY+8, ' ', (i << 4)); } print("\n", 0x0F);
    }
    else if (strcmp(cmd_b, "warna") == 0) { if (cur_col == 0x0F) cur_col = 0x0A; else if (cur_col == 0x0A) cur_col = 0x0B; else if (cur_col == 0x0B) cur_col = 0x0C; else cur_col = 0x0F; print("Warna diubah!\n", cur_col); }
    else if (strcmp(cmd_b, "beep") == 0) { beep(); }
    else if (strcmp(cmd_b, "help") == 0) {
        print(" +------------------ HELP MENU - MECTOV OS v10.0 ------------------+\n", 0x0B);
        print(" | ", 0x0B); print("SISTEM      ", 0x0E); print("| mfetch, waktu, warna, clear, beep, ular, kunci |\n", 0x0F);
        print(" | ", 0x0B); print("POWER       ", 0x0E); print("| matikan, mulaiulang                            |\n", 0x0F);
        print(" | ", 0x0B); print("FILESYSTEM  ", 0x0E); print("| ls, buat, tulis, baca, edit, hapus             |\n", 0x0F);
        print(" | ", 0x0B); print("SCRIPTING   ", 0x0E); print("| echo, tunggu, nada, jalankan                   |\n", 0x0F);
        print(" +-----------------------------------------------------------------+\n", 0x0B);
    }
    else if (strcmp(cmd_b, "ls") == 0) { for (int i = 0; i < MAX_FILES; i++) if (fs[i].in_use) { print("- ", 0x0F); print(fs[i].name, 0x0B); print("\n", 0x0F); } }
    else if (strncmp(cmd_b, "buat ", 5) == 0) { if (vfs_create(&cmd_b[5]) >= 0) { vfs_save(); print("Created.\n", 0x0B); } }
    else if (strncmp(cmd_b, "edit ", 5) == 0) { if (!is_script) { st_ed(&cmd_b[5]); b_idx = 0; return; } }
    else if (strncmp(cmd_b, "baca ", 5) == 0) { int i = vfs_find(&cmd_b[5]); if (i >= 0) { print(fs[i].data, 0x0F); print("\n", 0x0F); } }
    else if (strncmp(cmd_b, "hapus ", 6) == 0) { int i = vfs_find(&cmd_b[6]); if (i >= 0) { fs[i].in_use = 0; vfs_save(); print("Deleted.\n", 0x0C); } }
    else if (strncmp(cmd_b, "echo ", 5) == 0) { print(&cmd_b[5], 0x0F); print("\n", 0x0F); }
    else if (strncmp(cmd_b, "tunggu ", 7) == 0) { int ms = atoi(&cmd_b[7]); if (ms > 0) delay(ms); }
    else if (strncmp(cmd_b, "nada ", 5) == 0) { int i = 5; while(cmd_b[i] == ' ') i++; int f = atoi(&cmd_b[i]); while(cmd_b[i] >= '0' && cmd_b[i] <= '9') i++; while(cmd_b[i] == ' ') i++; int d = atoi(&cmd_b[i]); if (f > 0 && d > 0) nada(f, d); }
    else if (strncmp(cmd_b, "jalankan ", 9) == 0) { if (!is_script) { run_script(&cmd_b[9]); b_idx = 0; if (!is_script) print("root@mectov:~# ", 0x0A); return; } }
    else if (strncmp(cmd_b, "tulis ", 6) == 0) {
        int i = 6; while (cmd_b[i] == ' ') i++; int ns = i; while (cmd_b[i] != ' ' && cmd_b[i] != '\0') i++; 
        if (cmd_b[i] != '\0') { cmd_b[i] = '\0'; char* fn = &cmd_b[ns]; char* tx = &cmd_b[i+1]; int idx = vfs_find(fn); if (idx == -1) idx = vfs_create(fn); if (idx >= 0) { strcpy(fs[idx].data, tx); fs[idx].size = 0; while(tx[fs[idx].size]) fs[idx].size++; vfs_save(); print("Stored.\n", 0x0B); } }
    } else if (cmd_b[0] != '\0') { print("Command not found\n", 0x0C); }
    b_idx = 0; if (!is_script) print("root@mectov:~# ", 0x0A);
}

void kernel_main(void) {
    vfs_load(); detect_cpu(); init_uptime();
    d_desktop(); d_win(WIN_X, WIN_Y, WIN_W, WIN_H, " Mectov Security Login "); c_work();
    const char* pass = "mectov123"; char in[32]; int in_idx = 0;
    print("Welcome to Mectov OS v10.0\nPassword: ", 0x0E);
    int log = 0; unsigned char ls = 0;
    while (1) {
        if (!log || is_locked) {
            rand_seed++; update_marquee();
            if (inb(0x64) & 1) {
                unsigned char sc = inb(0x60);
                if (sc < 0x80 && sc != ls) {
                    char c = scancode_to_char(sc);
                    if (c == '\n') { in[in_idx] = '\0'; if (strcmp(in, pass) == 0) { log = 1; is_locked = 0; beep(); c_work(); d_win(WIN_X, WIN_Y, WIN_W, WIN_H, " Terminal - Mectov OS "); print("Login Success! Welcome Bos Alif Fadlan.\nroot@mectov:~# ", 0x0A); } else { print("\nDenied!\nPassword: ", 0x0C); in_idx = 0; } }
                    else if (c == '\b' && in_idx > 0) { in_idx--; d_char(CX + 10 + in_idx, CY + (is_locked?2:1), ' ', 0x0F); update_hw_cursor(CX + 10 + in_idx, CY + (is_locked?2:1)); }
                    else if (c != 0 && in_idx < 31) { in[in_idx++] = c; p_char('*', 0x0F); }
                    ls = sc;
                } else if (sc >= 0x80) ls = 0;
            }
            continue;
        }

        rand_seed++; update_marquee();
        if (inb(0x64) & 1) {
            unsigned char sc = inb(0x60);
            if (sc == 0x2A || sc == 0x36) shift_p = 1; else if (sc == 0xAA || sc == 0xB6) shift_p = 0; else if (sc == 0x3A) caps_a = !caps_a;
            
            if (sc == 0x48 && ls != 0x48 && !ed_a) {
                while (cx > 15) { cx--; d_char(CX + cx, CY + cy, ' ', 0x0F); }
                strcpy(cmd_b, hist_b); b_idx = 0; while(cmd_b[b_idx]) { d_char(CX + cx, CY + cy, cmd_b[b_idx], cur_col); cx++; b_idx++; }
                update_hw_cursor(CX + cx, CY + cy); ls = sc; continue;
            }

            if (sc == 0x0F && ls != 0x0F && !ed_a && b_idx > 0) {
                cmd_b[b_idx] = '\0';
                for (int i = 0; i < 19; i++) {
                    if (strncmp(cmd_b, cmd_list[i], b_idx) == 0) {
                        const char* match = cmd_list[i];
                        while (match[b_idx]) { d_char(CX + cx, CY + cy, match[b_idx], cur_col); cmd_b[b_idx] = match[b_idx]; cx++; b_idx++; }
                        cmd_b[b_idx] = '\0'; update_hw_cursor(CX + cx, CY + cy); break;
                    }
                }
                ls = sc; continue;
            }

            if (sc == 0x01 && ls != 0x01) { if (ed_a) sa_ex_ed(); ls = sc; continue; }
            if (sc < 0x80 && sc != ls) {
                char c = scancode_to_char(sc);
                if (ed_a) { if (c == '\b' && ed_c > 0) { ed_c--; ed_b[ed_c] = '\0'; c_work(); print(ed_b, 0x0F); } else if (c != 0 && ed_c < MAX_FILE_SIZE-1) { ed_b[ed_c++] = c; p_char(c, 0x0F); } }
                else {
                    if (c == '\n') { ex_cmd(); in_idx = 0; }
                    else if (c == '\b') { if (b_idx > 0) { b_idx--; if (cx > 15) { cx--; d_char(CX + cx, CY + cy, ' ', 0x0F); update_hw_cursor(CX + cx, CY + cy); } } } 
                    else if (c != 0) { d_char(CX + cx, CY + cy, c, cur_col); cx++; if (cx >= CW) { cx = 0; cy++; } if (cy >= CH-1) s_work(); if (b_idx < 255) { cmd_b[b_idx] = c; b_idx++; } update_hw_cursor(CX + cx, CY + cy); } 
                }
                ls = sc;
            } else if (sc >= 0x80) ls = 0;
        }
    }
}