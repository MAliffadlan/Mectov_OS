#include "../include/apps.h"
#include "../include/vga.h"
#include "../include/keyboard.h"
#include "../include/vfs.h"
#include "../include/utils.h"

int ed_a = 0; 
char ed_b[MAX_FILE_SIZE]; 
char ed_fn[MAX_FILENAME]; 
int ed_c = 0;

void st_ed(const char* f) { 
    strcpy(ed_fn, f); 
    int i = vfs_find(f); 
    if (i >= 0) { 
        strcpy(ed_b, fs[i].data); 
        ed_c = fs[i].size; 
    } else { 
        ed_b[0] = '\0'; 
        ed_c = 0; 
    } 
    ed_a = 1; 
    c_work(); 
    print(ed_b, 0x0F); 
    update_hw_cursor(CX + (ed_c % CW), CY + (ed_c / CW)); 
}

void sa_ex_ed() { 
    int i = vfs_find(ed_fn); 
    if (i == -1) i = vfs_create(ed_fn); 
    if (i >= 0) { 
        strcpy(fs[i].data, ed_b); 
        fs[i].size = ed_c; 
    } 
    vfs_save(); 
    ed_a = 0; 
    c_work(); 
    print("root@mectov:~# ", 0x0A); 
}
