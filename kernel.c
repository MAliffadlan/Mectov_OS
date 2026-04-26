// --- MECTOV OS v6.0 (Persistent Storage Edition) ---
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

// --- ATA PIO (HARD DISK DRIVER) ---
void ata_wait_bsy() { while(inb(0x1F7) & 0x80); }
void ata_wait_drq() { while(!(inb(0x1F7) & 0x08)); }

void ata_read_sector(unsigned int lba, unsigned char* buffer) {
    ata_wait_bsy();
    outb(0x1F6, 0xE0 | ((lba >> 24) & 0x0F));
    outb(0x1F2, 1);
    outb(0x1F3, (unsigned char)lba);
    outb(0x1F4, (unsigned char)(lba >> 8));
    outb(0x1F5, (unsigned char)(lba >> 16));
    outb(0x1F7, 0x20); // Perintah Read
    ata_wait_bsy();
    ata_wait_drq();
    for (int i = 0; i < 256; i++) {
        unsigned short word = inw(0x1F0);
        buffer[i * 2] = (unsigned char)word;
        buffer[i * 2 + 1] = (unsigned char)(word >> 8);
    }
}

void ata_write_sector(unsigned int lba, unsigned char* buffer) {
    ata_wait_bsy();
    outb(0x1F6, 0xE0 | ((lba >> 24) & 0x0F));
    outb(0x1F2, 1);
    outb(0x1F3, (unsigned char)lba);
    outb(0x1F4, (unsigned char)(lba >> 8));
    outb(0x1F5, (unsigned char)(lba >> 16));
    outb(0x1F7, 0x30); // Perintah Write
    ata_wait_bsy();
    ata_wait_drq();
    for (int i = 0; i < 256; i++) {
        unsigned short word = buffer[i * 2] | (buffer[i * 2 + 1] << 8);
        outw(0x1F0, word);
    }
    outb(0x1F7, 0xE7); // Flush
    ata_wait_bsy();
}

// --- AUDIO DRIVER ---
void play_sound(unsigned int nFrequence) {
    unsigned int Div = 1193180 / nFrequence;
    outb(0x43, 0xb6); outb(0x42, (unsigned char)(Div)); outb(0x42, (unsigned char)(Div >> 8));
    unsigned char tmp = inb(0x61);
    if (tmp != (tmp | 3)) outb(0x61, tmp | 3);
}
void nosound() { outb(0x61, inb(0x61) & 0xFC); }

void delay(int ms) {
    for (volatile int i = 0; i < ms * 20000; i++) {
        __asm__ __volatile__ ("pause");
    } 
}

void beep() { play_sound(1000); delay(100); nosound(); }
void nada(int freq, int dur) { if (freq > 0) play_sound(freq); delay(dur); nosound(); }

int shift_pressed = 0;
int capslock_active = 0;

char scancode_to_char(unsigned char scancode) {
    unsigned char map_normal[58] = {
        0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',   
        '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',      
        0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',   
        0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,      
        '*', 0, ' ', 0
    };
    unsigned char map_shift[58] = {
        0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',   
        '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',      
        0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '\"', '~',   
        0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,      
        '*', 0, ' ', 0
    };
    if (scancode < 58) {
        char c = map_normal[scancode];
        char c_shift = map_shift[scancode];
        int is_letter = (c >= 'a' && c <= 'z');
        if (shift_pressed) return (is_letter && capslock_active) ? c : c_shift;
        else return (is_letter && capslock_active) ? c_shift : c;
    }
    return 0;
}

// --- WINDOW MANAGER TUI ---
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

int cursor_x = 0;
int cursor_y = 0;

void draw_char_at(int x, int y, char c, unsigned char color) {
    if (x >= 0 && x < 80 && y >= 0 && y < 25) {
        int index = (y * 80 + x) * 2;
        video_memory[index] = c; video_memory[index + 1] = color;
    }
}

void draw_desktop() {
    for (int y = 0; y < 24; y++) for (int x = 0; x < 80; x++) draw_char_at(x, y, 176, 0x09);
    for (int x = 0; x < 80; x++) draw_char_at(x, 24, ' ', 0x70); 
    const char* taskbar_text = " [ Mectov ]    Terminal Aktif    |    Mectov OS v6.0 (Persistent Edition) ";
    for (int i = 0; taskbar_text[i] != '\0' && i < 80; i++) draw_char_at(i, 24, taskbar_text[i], 0x70); 
}

void draw_window(int x, int y, int w, int h, const char* title) {
    for (int sy = y + 1; sy <= y + h; sy++) {
        draw_char_at(x + w, sy, ' ', 0x08); draw_char_at(x + w + 1, sy, ' ', 0x08);
    }
    for (int sx = x + 2; sx <= x + w + 1; sx++) draw_char_at(sx, y + h, ' ', 0x08);

    for (int i = x; i < x + w; i++) {
        for (int j = y; j < y + h; j++) {
            if (j == y) draw_char_at(i, j, ' ', 0x1F); 
            else if (i == x || i == x + w - 1 || j == y + h - 1) draw_char_at(i, j, 177, 0x1F); 
            else draw_char_at(i, j, ' ', 0x0F); 
        }
    }
    int title_len = 0; while(title[title_len]) title_len++;
    int title_x = x + (w - title_len) / 2;
    for(int i = 0; i < title_len; i++) draw_char_at(title_x + i, y, title[i], 0x1F);
    draw_char_at(x + w - 4, y, '[', 0x1F); draw_char_at(x + w - 3, y, 'X', 0x1C); draw_char_at(x + w - 2, y, ']', 0x1F);
}

void clear_workspace() {
    for (int y = CY; y < CY + CH - 1; y++) {
        for (int x = CX; x < CX + CW; x++) draw_char_at(x, y, ' ', 0x0F);
    }
    cursor_x = 0; cursor_y = 0;
}

void scroll_workspace() {
    for (int y = CY; y < CY + CH - 2; y++) {
        for (int x = CX; x < CX + CW; x++) {
            int src_idx = ((y + 1) * 80 + x) * 2, dst_idx = (y * 80 + x) * 2;
            video_memory[dst_idx] = video_memory[src_idx];
            video_memory[dst_idx + 1] = video_memory[src_idx + 1];
        }
    }
    int last_y = CY + CH - 2;
    for (int x = CX; x < CX + CW; x++) draw_char_at(x, last_y, ' ', 0x0F);
    cursor_y--;
}

void print(const char* str, unsigned char color) {
    int i = 0;
    while (str[i] != '\0') {
        if (str[i] == '\n') { cursor_x = 0; cursor_y++; } 
        else {
            draw_char_at(CX + cursor_x, CY + cursor_y, str[i], color);
            cursor_x++; if (cursor_x >= CW) { cursor_x = 0; cursor_y++; }
        }
        if (cursor_y >= CH - 1) scroll_workspace();
        i++;
    }
}

void print_char(char c, unsigned char color) {
    if (c == '\n') { cursor_x = 0; cursor_y++; } 
    else { draw_char_at(CX + cursor_x, CY + cursor_y, c, color); cursor_x++; if (cursor_x >= CW) { cursor_x = 0; cursor_y++; } }
    if (cursor_y >= CH - 1) scroll_workspace();
}

int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

int strncmp(const char* s1, const char* s2, int n) {
    while (n && *s1 && (*s1 == *s2)) { ++s1; ++s2; --n; }
    if (n == 0) return 0;
    else return (*(unsigned char *)s1 - *(unsigned char *)s2);
}

void strcpy(char* dest, const char* src) {
    while ((*dest++ = *src++));
}

void print_int(int num, unsigned char color) {
    if (num < 0) { print("-", color); num = -num; }
    if (num == 0) { print("0", color); return; }
    char buf[10]; int i = 0;
    while (num > 0) { buf[i++] = (num % 10) + '0'; num /= 10; }
    for (int j = 0; j < i / 2; j++) { char temp = buf[j]; buf[j] = buf[i - j - 1]; buf[i - j - 1] = temp; }
    buf[i] = '\0';
    if (i == 1 && color == 0x0E) print("0", color); 
    print(buf, color);
}

int atoi(const char* str) {
    int res = 0, sign = 1, i = 0;
    if (str[0] == '-') { sign = -1; i++; }
    for (; str[i] != '\0'; ++i) {
        if (str[i] >= '0' && str[i] <= '9') res = res * 10 + str[i] - '0';
        else break; 
    }
    return sign * res;
}

// --- FILE SYSTEM YANG DISIMPAN KE HARDDISK (PERSISTENT VFS) ---
#define MAX_FILES 16
#define MAX_FILENAME 16
#define MAX_FILE_SIZE 1024

// Struktur File diubah jadi 1536 Bytes (tepat 3 sektor disk) supaya penyimpanannya rapi
typedef struct {
    char name[MAX_FILENAME];
    char data[MAX_FILE_SIZE];
    int size;
    int in_use;
    char padding[488]; 
} File;

File filesystem[MAX_FILES];

void vfs_init() {
    for (int i = 0; i < MAX_FILES; i++) { filesystem[i].in_use = 0; filesystem[i].size = 0; }
}

// SIMPAN KE DISK FISIK
void vfs_save_to_disk() {
    unsigned char* fs_ptr = (unsigned char*)filesystem;
    // Ukuran FileSystem = 16 * 1536 = 24576 bytes = 48 Sector (512 bytes)
    for (int i = 0; i < 48; i++) {
        ata_write_sector(i + 1, fs_ptr + (i * 512));
    }
    // Tulis Signature MECTOV di Sektor ke-0 sebagai tanda Disk sudah di-Format
    unsigned char sig[512] = {0};
    sig[0] = 'M'; sig[1] = 'E'; sig[2] = 'C'; sig[3] = 'T'; sig[4] = 'O'; sig[5] = 'V';
    ata_write_sector(0, sig);
}

// BACA DARI DISK FISIK
int vfs_load_from_disk() {
    unsigned char sig[512];
    ata_read_sector(0, sig);
    if (sig[0] == 'M' && sig[1] == 'E' && sig[2] == 'C' && sig[3] == 'T' && sig[4] == 'O' && sig[5] == 'V') {
        unsigned char* fs_ptr = (unsigned char*)filesystem;
        for (int i = 0; i < 48; i++) {
            ata_read_sector(i + 1, fs_ptr + (i * 512));
        }
        return 1; // Sukses baca disk
    } else {
        vfs_init();
        vfs_save_to_disk(); // Format Disk kosong baru
        return 0; // Disk baru
    }
}

int vfs_find_file(const char* name) {
    for (int i = 0; i < MAX_FILES; i++) if (filesystem[i].in_use && strcmp(filesystem[i].name, name) == 0) return i;
    return -1;
}

int vfs_create_file(const char* name) {
    if (vfs_find_file(name) != -1) return -2; 
    for (int i = 0; i < MAX_FILES; i++) {
        if (!filesystem[i].in_use) {
            strcpy(filesystem[i].name, name);
            filesystem[i].in_use = 1; filesystem[i].size = 0; filesystem[i].data[0] = '\0';
            return i;
        }
    }
    return -1; 
}

// --- TEXT EDITOR (Mectov Nano) ---
int editor_active = 0;
char editor_filename[MAX_FILENAME];
char editor_buffer[MAX_FILE_SIZE];
int editor_cursor = 0;

void start_editor(const char* filename) {
    strcpy(editor_filename, filename);
    int idx = vfs_find_file(filename);
    if (idx != -1) {
        strcpy(editor_buffer, filesystem[idx].data);
        editor_cursor = filesystem[idx].size;
    } else {
        editor_buffer[0] = '\0'; editor_cursor = 0;
    }
    editor_active = 1;
    draw_desktop(); draw_window(WIN_X, WIN_Y, WIN_W, WIN_H, " Mectov Nano Editor (Tekan ESC untuk Save & Exit) "); clear_workspace();
    print(editor_buffer, 0x0F);
}

void save_and_exit_editor() {
    int idx = vfs_find_file(editor_filename);
    if (idx == -1) idx = vfs_create_file(editor_filename);
    if (idx != -1) { strcpy(filesystem[idx].data, editor_buffer); filesystem[idx].size = editor_cursor; }
    
    // --- SAVE KE HARDDISK OTOMATIS SETIAP KALI EDIT ---
    vfs_save_to_disk();

    editor_active = 0;
    draw_desktop(); draw_window(WIN_X, WIN_Y, WIN_W, WIN_H, " Terminal - Mectov OS "); clear_workspace();
    print("File '", 0x0A); print(editor_filename, 0x0E); print("' disimpan permanen ke Harddisk!\n", 0x0B);
    print("root@mectov:~# ", 0x0A);
}

// --- SHELL COMMAND LOGIC & SCRIPT ENGINE ---
char command_buffer[256];
int buffer_index = 0;
int is_script_running = 0;

void execute_command();

void run_script(const char* filename) {
    int idx = vfs_find_file(filename);
    if (idx == -1) {
        print("Error: Script tidak ditemukan!\n", 0x0C); return;
    }
    print("--- Menjalankan Script ---\n", 0x0D);
    char* data = filesystem[idx].data;
    int len = filesystem[idx].size;
    int i = 0;
    is_script_running = 1;
    while (i < len) {
        buffer_index = 0;
        while (i < len && data[i] != '\n' && data[i] != '\0') {
            if (buffer_index < 255) command_buffer[buffer_index++] = data[i];
            i++;
        }
        command_buffer[buffer_index] = '\0';
        if (buffer_index > 0) execute_command(); 
        if (data[i] == '\n') i++;
    }
    is_script_running = 0;
    print("--- Script Selesai ---\n", 0x0D);
}

void execute_command() {
    if (!is_script_running) print("\n", 0x0F); 
    command_buffer[buffer_index] = '\0'; 

    if (buffer_index == 0) {
    } else if (strcmp(command_buffer, "mfetch") == 0) {
        print("       .---.        ", 0x0B); print("root", 0x0A); print("@", 0x0F); print("mectov-os\n", 0x0B);
        print("      /     \\       ", 0x0B); print("--------------\n", 0x0F);
        print("     | () () |      ", 0x0B); print("OS", 0x0E); print(": Bare-metal x86\n", 0x0F);
        print("      \\  ^  /       ", 0x0B); print("Kernel", 0x0E); print(": Custom C v6.0\n", 0x0F);
        print("       |||||        ", 0x0B); print("Disk", 0x0E); print(": Persistent ATA IDE\n", 0x0F);
        print("       |||||        ", 0x0B); print("Audio", 0x0E); print(": Synth Engine\n", 0x0F);
        print("                    ", 0x0B); print("Creator", 0x0E); print(": Bos Alif\n", 0x0F);
    } else if (strcmp(command_buffer, "help") == 0) {
        print("Sistem : mfetch, waktu, warna, clear, beep\n", 0x0E);
        print("File   : ls, edit [nama], hapus [nama], baca [nama], buat, tulis\n", 0x0E);
        print("Script : echo [teks], tunggu [ms], nada [Hz] [ms], jalankan [file]\n", 0x0A);
    } else if (strcmp(command_buffer, "clear") == 0) {
        clear_workspace();
    } else if (strcmp(command_buffer, "beep") == 0) {
        beep();
    } else if (strncmp(command_buffer, "echo ", 5) == 0) {
        print(&command_buffer[5], 0x0F); print("\n", 0x0F);
    } else if (strncmp(command_buffer, "tunggu ", 7) == 0) {
        int ms = atoi(&command_buffer[7]);
        if (ms > 0) delay(ms);
    } else if (strncmp(command_buffer, "nada ", 5) == 0) {
        int i = 5; while(command_buffer[i] == ' ') i++; int freq = atoi(&command_buffer[i]);
        while(command_buffer[i] >= '0' && command_buffer[i] <= '9') i++; while(command_buffer[i] == ' ') i++;
        int dur = atoi(&command_buffer[i]); if (freq > 0 && dur > 0) nada(freq, dur);
    } else if (strncmp(command_buffer, "jalankan ", 9) == 0) {
        if (!is_script_running) { run_script(&command_buffer[9]); buffer_index = 0; if (!is_script_running) print("root@mectov:~# ", 0x0A); return; } 
        else print("Error: Tidak bisa jalankan script dari dalam script!\n", 0x0C);
    } else if (strcmp(command_buffer, "ls") == 0) {
        print("Daftar File di Harddisk (ATA IDE):\n", 0x0D);
        int found = 0;
        for (int i = 0; i < MAX_FILES; i++) {
            if (filesystem[i].in_use) {
                print("- ", 0x0F); print(filesystem[i].name, 0x0B);
                print(" (", 0x08); print_int(filesystem[i].size, 0x08); print(" bytes)\n", 0x08);
                found++;
            }
        }
        if (found == 0) print("(Disk kosong)\n", 0x08);
    } else if (strncmp(command_buffer, "buat ", 5) == 0) {
        char* filename = &command_buffer[5]; int res = vfs_create_file(filename);
        if (res == -2) print("Error: File sudah ada!\n", 0x0C);
        else if (res == -1) print("Error: Disk penuh!\n", 0x0C);
        else { vfs_save_to_disk(); print("File dibuat di Harddisk.\n", 0x0B); }
    } else if (strncmp(command_buffer, "tulis ", 6) == 0) {
        int i = 6; while (command_buffer[i] == ' ') i++; int name_start = i;
        while (command_buffer[i] != ' ' && command_buffer[i] != '\0') i++; 
        if (command_buffer[i] == '\0') print("Error: tulis [nama] [teks]\n", 0x0C);
        else {
            command_buffer[i] = '\0'; char* filename = &command_buffer[name_start]; char* txt = &command_buffer[i + 1];
            int idx = vfs_find_file(filename); if (idx == -1) idx = vfs_create_file(filename);
            if (idx != -1) {
                int t = 0; while (txt[t] != '\0' && t < MAX_FILE_SIZE - 1) { filesystem[idx].data[t] = txt[t]; t++; }
                filesystem[idx].data[t] = '\0'; filesystem[idx].size = t;
                vfs_save_to_disk(); print("Disimpan ke Harddisk.\n", 0x0B);
            }
        }
    } else if (strncmp(command_buffer, "edit ", 5) == 0) {
        if (!is_script_running) { start_editor(&command_buffer[5]); buffer_index = 0; return; }
    } else if (strncmp(command_buffer, "baca ", 5) == 0) {
        int idx = vfs_find_file(&command_buffer[5]);
        if (idx == -1) print("Error: File tidak ditemukan!\n", 0x0C);
        else { print(filesystem[idx].data, 0x0F); print("\n", 0x0F); }
    } else if (strncmp(command_buffer, "hapus ", 6) == 0) {
        int idx = vfs_find_file(&command_buffer[6]);
        if (idx != -1) { filesystem[idx].in_use = 0; vfs_save_to_disk(); print("Dihapus permanen dari Disk.\n", 0x0C); }
    } else {
        print("bash: ", 0x0C); print(command_buffer, 0x0C); print(": command not found\n", 0x0C); 
    }

    buffer_index = 0;
    if (!is_script_running) print("root@mectov:~# ", 0x0A); 
}

void kernel_main(void) {
    draw_desktop(); draw_window(WIN_X, WIN_Y, WIN_W, WIN_H, " Terminal - Mectov OS "); clear_workspace();

    print("===================================================\n", 0x0A);
    print("  MECTOV OS v6.0 - HARD DISK ATA/IDE INITIALIZED!  \n", 0x0E);
    print("===================================================\n", 0x0A);
    
    // LOAD DISK (ATA PIO)
    int disk_status = vfs_load_from_disk();
    if (disk_status == 1) {
        print("[+] Disk Ditemukan! Memuat file lama dari MectovFS...\n\n", 0x0B);
    } else {
        print("[!] Disk Kosong. Mem-format MectovFS baru...\n\n", 0x0E);
    }
    
    print("root@mectov:~# ", 0x0A);

    unsigned char last_scancode = 0;

    while (1) {
        if (inb(0x64) & 1) {
            unsigned char scancode = inb(0x60); 

            if (scancode == 0x2A || scancode == 0x36) shift_pressed = 1; 
            else if (scancode == 0xAA || scancode == 0xB6) shift_pressed = 0; 
            else if (scancode == 0x3A) capslock_active = !capslock_active; 

            if (scancode == 0x01 && last_scancode != 0x01) { 
                if (editor_active) save_and_exit_editor();
                last_scancode = scancode;
                continue;
            }

            if (scancode < 0x80 && scancode != last_scancode) { 
                char c = scancode_to_char(scancode);
                
                if (editor_active) {
                    if (c == '\b') {
                        if (editor_cursor > 0) {
                            editor_cursor--; editor_buffer[editor_cursor] = '\0';
                            clear_workspace(); print(editor_buffer, 0x0F);
                        }
                    } else if (c != 0) {
                        if (editor_cursor < MAX_FILE_SIZE - 1) {
                            editor_buffer[editor_cursor++] = c; editor_buffer[editor_cursor] = '\0';
                            print_char(c, 0x0F);
                        }
                    }
                } else {
                    if (c != 0) {
                        if (c == '\n') execute_command(); 
                        else if (c == '\b') {
                            if (buffer_index > 0) {
                                buffer_index--; 
                                if (cursor_x > 0) { cursor_x--; draw_char_at(CX + cursor_x, CY + cursor_y, ' ', 0x0F); }
                            }
                        } else {
                            draw_char_at(CX + cursor_x, CY + cursor_y, c, current_color);
                            cursor_x++; if (cursor_x >= CW) { cursor_x = 0; cursor_y++; }
                            if (cursor_y >= CH - 1) scroll_workspace();
                            if (buffer_index < 255) { command_buffer[buffer_index] = c; buffer_index++; }
                        }
                    }
                }
                last_scancode = scancode; 
            } else if (scancode >= 0x80) last_scancode = 0; 
        }
    }
}