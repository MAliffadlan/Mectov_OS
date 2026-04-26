// --- MECTOV OS v6.5 (Emergency Stop Edition) ---
static inline unsigned char inb(unsigned short port) {
    unsigned char ret;
    __asm__ __volatile__ ( "inb %1, %0" : "=a"(ret) : "Nd"(port) );
    return ret;
}

static inline void outb(unsigned short port, unsigned char val) {
    __asm__ __volatile__ ( "outb %0, %1" : : "a"(val), "Nd"(port) );
}

static inline unsigned short inw(unsigned short port) {
    unsigned short ret;
    __asm__ __volatile__ ( "inw %1, %0" : "=a"(ret) : "Nd"(port) );
    return ret;
}

static inline void outw(unsigned short port, unsigned short val) {
    __asm__ __volatile__ ( "outw %0, %1" : : "a"(val), "Nd"(port) );
}

unsigned char read_cmos(unsigned char reg) {
    outb(0x70, reg); return inb(0x71);
}

unsigned char bcd_to_bin(unsigned char bcd) {
    return ((bcd & 0xF0) >> 1) + ((bcd & 0xF0) >> 3) + (bcd & 0x0F);
}

// --- HARD DISK DRIVER ---
void ata_wait_bsy() { while(inb(0x1F7) & 0x80); }
void ata_wait_drq() { while(!(inb(0x1F7) & 0x08)); }
void ata_read_sector(unsigned int lba, unsigned char* buffer) {
    ata_wait_bsy(); outb(0x1F6, 0xE0 | ((lba >> 24) & 0x0F)); outb(0x1F2, 1); outb(0x1F3, (unsigned char)lba);
    outb(0x1F4, (unsigned char)(lba >> 8)); outb(0x1F5, (unsigned char)(lba >> 16)); outb(0x1F7, 0x20);
    ata_wait_bsy(); ata_wait_drq();
    for (int i = 0; i < 256; i++) { unsigned short word = inw(0x1F0); buffer[i * 2] = (unsigned char)word; buffer[i * 2 + 1] = (unsigned char)(word >> 8); }
}
void ata_write_sector(unsigned int lba, unsigned char* buffer) {
    ata_wait_bsy(); outb(0x1F6, 0xE0 | ((lba >> 24) & 0x0F)); outb(0x1F2, 1); outb(0x1F3, (unsigned char)lba);
    outb(0x1F4, (unsigned char)(lba >> 8)); outb(0x1F5, (unsigned char)(lba >> 16)); outb(0x1F7, 0x30);
    ata_wait_bsy(); ata_wait_drq();
    for (int i = 0; i < 256; i++) { unsigned short word = buffer[i * 2] | (buffer[i * 2 + 1] << 8); outw(0x1F0, word); }
    outb(0x1F7, 0xE7); ata_wait_bsy();
}

// --- AUDIO & DELAY (DENGAN EMERGENCY STOP) ---
void play_sound(unsigned int nFrequence) {
    unsigned int Div = 1193180 / nFrequence;
    outb(0x43, 0xb6); outb(0x42, (unsigned char)(Div)); outb(0x42, (unsigned char)(Div >> 8));
    unsigned char tmp = inb(0x61); if (tmp != (tmp | 3)) outb(0x61, tmp | 3);
}
void nosound() { outb(0x61, inb(0x61) & 0xFC); }

int abort_execution = 0;

void check_abort() {
    if (inb(0x64) & 1) {
        unsigned char sc = inb(0x60);
        if (sc == 0x01) { // ESC Key
            abort_execution = 1;
            nosound();
        }
    }
}

void delay(int ms) {
    for (volatile int i = 0; i < ms; i++) {
        check_abort(); if (abort_execution) return;
        for (volatile int j = 0; j < 4000; j++) __asm__ __volatile__ ("pause");
    }
}

void beep() { play_sound(1000); delay(100); nosound(); }
void nada(int freq, int dur) { if (abort_execution) return; if (freq > 0) play_sound(freq); delay(dur); nosound(); }

// --- KEYBOARD ---
int shift_pressed = 0, capslock_active = 0;
char scancode_to_char(unsigned char scancode) {
    unsigned char map_n[58] = { 0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ', 0 };
    unsigned char map_s[58] = { 0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '\"', '~', 0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ', 0 };
    if (scancode < 58) {
        char c = map_n[scancode], cs = map_s[scancode]; int isl = (c >= 'a' && c <= 'z');
        if (shift_pressed) return (isl && capslock_active) ? c : cs;
        else return (isl && capslock_active) ? cs : c;
    }
    return 0;
}

// --- GUI ---
volatile char* video_memory = (volatile char*) 0xb8000;
unsigned char current_color = 0x0F; 
#define WIN_X 2
#define WIN_Y 2
#define WIN_W 76
#define WIN_H 20
#define CX (WIN_X + 1)
#define CY (WIN_Y + 1)
#define CW (WIN_W - 2)
#define CH (WIN_H - 2)
int cursor_x = 0, cursor_y = 0;
void draw_char_at(int x, int y, char c, unsigned char color) { if (x >= 0 && x < 80 && y >= 0 && y < 25) { int i = (y * 80 + x) * 2; video_memory[i] = c; video_memory[i + 1] = color; } }
void draw_desktop() { for (int y = 0; y < 24; y++) for (int x = 0; x < 80; x++) draw_char_at(x, y, 176, 0x09); for (int x = 0; x < 80; x++) draw_char_at(x, 24, ' ', 0x70); const char* t = " [ Mectov ]    Terminal Aktif    |    Mectov OS v6.5 (ESC to STOP) "; for (int i = 0; t[i] != '\0' && i < 80; i++) draw_char_at(i, 24, t[i], 0x70); }
void draw_window(int x, int y, int w, int h, const char* title) { for (int sy = y + 1; sy <= y + h; sy++) { draw_char_at(x + w, sy, ' ', 0x08); draw_char_at(x + w + 1, sy, ' ', 0x08); } for (int sx = x + 2; sx <= x + w + 1; sx++) draw_char_at(sx, y + h, ' ', 0x08); for (int i = x; i < x + w; i++) for (int j = y; j < y + h; j++) { if (j == y) draw_char_at(i, j, ' ', 0x1F); else if (i == x || i == x + w - 1 || j == y + h - 1) draw_char_at(i, j, 177, 0x1F); else draw_char_at(i, j, ' ', 0x0F); } int tl = 0; while(title[tl]) tl++; int tx = x + (w - tl) / 2; for(int i = 0; i < tl; i++) draw_char_at(tx + i, y, title[i], 0x1F); draw_char_at(x + w - 4, y, '[', 0x1F); draw_char_at(x + w - 3, y, 'X', 0x1C); draw_char_at(x + w - 2, y, ']', 0x1F); }
void clear_workspace() { for (int y = CY; y < CY + CH - 1; y++) for (int x = CX; x < CX + CW; x++) draw_char_at(x, y, ' ', 0x0F); cursor_x = 0; cursor_y = 0; }
void scroll_workspace() { for (int y = CY; y < CY + CH - 2; y++) for (int x = CX; x < CX + CW; x++) { int s = ((y + 1) * 80 + x) * 2, d = (y * 80 + x) * 2; video_memory[d] = video_memory[s]; video_memory[d + 1] = video_memory[s + 1]; } int ly = CY + CH - 2; for (int x = CX; x < CX + CW; x++) draw_char_at(x, ly, ' ', 0x0F); cursor_y--; }
void print(const char* str, unsigned char color) { int i = 0; while (str[i] != '\0') { if (str[i] == '\n') { cursor_x = 0; cursor_y++; } else { draw_char_at(CX + cursor_x, CY + cursor_y, str[i], color); cursor_x++; if (cursor_x >= CW) { cursor_x = 0; cursor_y++; } } if (cursor_y >= CH - 1) scroll_workspace(); i++; } }
void print_char(char c, unsigned char color) { if (c == '\n') { cursor_x = 0; cursor_y++; } else { draw_char_at(CX + cursor_x, CY + cursor_y, c, color); cursor_x++; if (cursor_x >= CW) { cursor_x = 0; cursor_y++; } } if (cursor_y >= CH - 1) scroll_workspace(); }

// --- MATH & STRING ---
int strcmp(const char* s1, const char* s2) { while (*s1 && (*s1 == *s2)) { s1++; s2++; } return *(const unsigned char*)s1 - *(const unsigned char*)s2; }
int strncmp(const char* s1, const char* s2, int n) { while (n && *s1 && (*s1 == *s2)) { ++s1; ++s2; --n; } if (n == 0) return 0; return (*(unsigned char *)s1 - *(unsigned char *)s2); }
void strcpy(char* d, const char* s) { while ((*d++ = *s++)); }
void print_int(int n, unsigned char c) { if (n < 0) { print("-", c); n = -n; } if (n == 0) { print("0", c); return; } char buf[10]; int i = 0; while (n > 0) { buf[i++] = (n % 10) + '0'; n /= 10; } for (int j = 0; j < i / 2; j++) { char t = buf[j]; buf[j] = buf[i - j - 1]; buf[i - j - 1] = t; } buf[i] = '\0'; if (i == 1 && c == 0x0E) print("0", c); print(buf, c); }
int atoi(const char* s) { int r = 0, si = 1, i = 0; if (s[0] == '-') { si = -1; i++; } for (; s[i] != '\0'; ++i) { if (s[i] >= '0' && s[i] <= '9') r = r * 10 + s[i] - '0'; else break; } return si * r; }

// --- VFS ---
#define MAX_FILES 16
#define MAX_FILENAME 16
#define MAX_FILE_SIZE 1024
typedef struct { char name[MAX_FILENAME]; char data[MAX_FILE_SIZE]; int size; int in_use; char padding[488]; } File;
File filesystem[MAX_FILES];
void vfs_init() { for (int i = 0; i < MAX_FILES; i++) { filesystem[i].in_use = 0; filesystem[i].size = 0; } }
void vfs_save_to_disk() { unsigned char* p = (unsigned char*)filesystem; for (int i = 0; i < 48; i++) ata_write_sector(i + 1, p + (i * 512)); unsigned char s[512] = {0}; s[0] = 'M'; s[1] = 'E'; s[2] = 'C'; s[3] = 'T'; s[4] = 'O'; s[5] = 'V'; ata_write_sector(0, s); }
int vfs_load_from_disk() { unsigned char s[512]; ata_read_sector(0, s); if (s[0] == 'M' && s[1] == 'E' && s[2] == 'C' && s[3] == 'T' && s[4] == 'O' && s[5] == 'V') { unsigned char* p = (unsigned char*)filesystem; for (int i = 0; i < 48; i++) ata_read_sector(i + 1, p + (i * 512)); return 1; } vfs_init(); vfs_save_to_disk(); return 0; }
int vfs_find_file(const char* n) { for (int i = 0; i < MAX_FILES; i++) if (filesystem[i].in_use && strcmp(filesystem[i].name, n) == 0) return i; return -1; }
int vfs_create_file(const char* n) { if (vfs_find_file(n) != -1) return -2; for (int i = 0; i < MAX_FILES; i++) if (!filesystem[i].in_use) { strcpy(filesystem[i].name, n); filesystem[i].in_use = 1; filesystem[i].size = 0; filesystem[i].data[0] = '\0'; return i; } return -1; }

// --- EDITOR ---
int editor_active = 0; char editor_filename[MAX_FILENAME], editor_buffer[MAX_FILE_SIZE]; int editor_cursor = 0;
void start_editor(const char* f) { strcpy(editor_filename, f); int i = vfs_find_file(f); if (i != -1) { strcpy(editor_buffer, filesystem[i].data); editor_cursor = filesystem[i].size; } else { editor_buffer[0] = '\0'; editor_cursor = 0; } editor_active = 1; draw_desktop(); draw_window(WIN_X, WIN_Y, WIN_W, WIN_H, " Mectov Nano Editor (ESC to Save/Exit) "); clear_workspace(); print(editor_buffer, 0x0F); }
void save_and_exit_editor() { int i = vfs_find_file(editor_filename); if (i == -1) i = vfs_create_file(editor_filename); if (i != -1) { strcpy(filesystem[i].data, editor_buffer); filesystem[i].size = editor_cursor; } vfs_save_to_disk(); editor_active = 0; draw_desktop(); draw_window(WIN_X, WIN_Y, WIN_W, WIN_H, " Terminal - Mectov OS "); clear_workspace(); print("File saved to disk.\n", 0x0B); print("root@mectov:~# ", 0x0A); }

// --- SHELL ---
char command_buffer[256]; int buffer_index = 0, is_script_running = 0;
void execute_command();
void run_script(const char* f) { int i = vfs_find_file(f); if (i == -1) { print("Error: Script not found!\n", 0x0C); return; } print("--- Running Script (ESC to STOP) ---\n", 0x0D); char* d = filesystem[i].data; int l = filesystem[i].size; int p = 0; is_script_running = 1; abort_execution = 0; while (p < l && !abort_execution) { buffer_index = 0; while (p < l && d[p] != '\n' && d[p] != '\0') { if (buffer_index < 255) command_buffer[buffer_index++] = d[p]; p++; } command_buffer[buffer_index] = '\0'; if (buffer_index > 0) execute_command(); if (d[p] == '\n') p++; } if (abort_execution) print("\n[!] Script aborted by User.\n", 0x0C); else print("--- Script Done ---\n", 0x0D); is_script_running = 0; abort_execution = 0; }
void execute_command() { if (!is_script_running) print("\n", 0x0F); command_buffer[buffer_index] = '\0'; if (buffer_index == 0) {} else if (strcmp(command_buffer, "mfetch") == 0) { print("       .---.        root@mectov-os\n      /     \\       --------------\n     | () () |      OS: x86 Bare-metal\n      \\  ^  /       Kernel: Custom C v6.5\n       |||||        Shell: Mectovsh\n       |||||        Creator: Bos Alif\n", 0x0B); } else if (strcmp(command_buffer, "help") == 0) { print("System: mfetch, waktu, warna, clear, beep\nFile  : ls, edit, hapus, baca, buat, tulis\nScript: echo, tunggu, nada, jalankan\n", 0x0E); } else if (strcmp(command_buffer, "clear") == 0) { clear_workspace(); } else if (strcmp(command_buffer, "beep") == 0) { beep(); } else if (strncmp(command_buffer, "echo ", 5) == 0) { print(&command_buffer[5], 0x0F); print("\n", 0x0F); } else if (strncmp(command_buffer, "tunggu ", 7) == 0) { int ms = atoi(&command_buffer[7]); if (ms > 0) delay(ms); } else if (strncmp(command_buffer, "nada ", 5) == 0) { int i = 5; while(command_buffer[i] == ' ') i++; int f = atoi(&command_buffer[i]); while(command_buffer[i] >= '0' && command_buffer[i] <= '9') i++; while(command_buffer[i] == ' ') i++; int d = atoi(&command_buffer[i]); if (f > 0 && d > 0) nada(f, d); } else if (strncmp(command_buffer, "jalankan ", 9) == 0) { if (!is_script_running) { run_script(&command_buffer[9]); buffer_index = 0; if (!is_script_running) print("root@mectov:~# ", 0x0A); return; } } else if (strcmp(command_buffer, "ls") == 0) { print("Disk Files (ATA):\n", 0x0D); for (int i = 0; i < MAX_FILES; i++) if (filesystem[i].in_use) { print("- ", 0x0F); print(filesystem[i].name, 0x0B); print(" (", 0x08); print_int(filesystem[i].size, 0x08); print(" bytes)\n", 0x08); } } else if (strncmp(command_buffer, "buat ", 5) == 0) { if (vfs_create_file(&command_buffer[5]) >= 0) { vfs_save_to_disk(); print("Created.\n", 0x0B); } } else if (strncmp(command_buffer, "edit ", 5) == 0) { if (!is_script_running) { start_editor(&command_buffer[5]); buffer_index = 0; return; } } else if (strncmp(command_buffer, "baca ", 5) == 0) { int i = vfs_find_file(&command_buffer[5]); if (i >= 0) { print(filesystem[i].data, 0x0F); print("\n", 0x0F); } } else if (strncmp(command_buffer, "hapus ", 6) == 0) { int i = vfs_find_file(&command_buffer[6]); if (i >= 0) { filesystem[i].in_use = 0; vfs_save_to_disk(); print("Deleted.\n", 0x0C); } } else { print("bash: command not found\n", 0x0C); } buffer_index = 0; if (!is_script_running) print("root@mectov:~# ", 0x0A); }

void kernel_main(void) {
    vfs_init(); draw_desktop(); draw_window(WIN_X, WIN_Y, WIN_W, WIN_H, " Terminal - Mectov OS "); clear_workspace();
    print("MECTOV OS v6.5 - PRESS ESC TO ABORT SCRIPTS\n", 0x0E);
    if (vfs_load_from_disk()) print("[+] HDD Ready.\n\n", 0x0B); else print("[!] Formatting HDD...\n\n", 0x0E);
    print("root@mectov:~# ", 0x0A);
    unsigned char ls = 0;
    while (1) {
        if (inb(0x64) & 1) {
            unsigned char sc = inb(0x60);
            if (sc == 0x2A || sc == 0x36) shift_pressed = 1; else if (sc == 0xAA || sc == 0xB6) shift_pressed = 0; else if (sc == 0x3A) capslock_active = !capslock_active;
            if (sc == 0x01 && ls != 0x01) { if (editor_active) save_and_exit_editor(); ls = sc; continue; }
            if (sc < 0x80 && sc != ls) {
                char c = scancode_to_char(sc);
                if (editor_active) {
                    if (c == '\b') { if (editor_cursor > 0) { editor_cursor--; editor_buffer[editor_cursor] = '\0'; clear_workspace(); print(editor_buffer, 0x0F); } }
                    else if (c != 0 && editor_cursor < MAX_FILE_SIZE - 1) { editor_buffer[editor_cursor++] = c; editor_buffer[editor_cursor] = '\0'; print_char(c, 0x0F); }
                } else {
                    if (c != 0) {
                        if (c == '\n') execute_command();
                        else if (c == '\b') { if (buffer_index > 0) { buffer_index--; if (cursor_x > 0) { cursor_x--; draw_char_at(CX + cursor_x, CY + cursor_y, ' ', 0x0F); } } }
                        else { draw_char_at(CX + cursor_x, CY + cursor_y, c, current_color); cursor_x++; if (cursor_x >= CW) { cursor_x = 0; cursor_y++; } if (cursor_y >= CH - 1) scroll_workspace(); if (buffer_index < 255) { command_buffer[buffer_index] = c; buffer_index++; } }
                    }
                }
                ls = sc;
            } else if (sc >= 0x80) ls = 0;
        }
    }
}