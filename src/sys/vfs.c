#include "../include/vfs.h"
#include "../include/ata.h"
#include "../include/utils.h"

File fs[MAX_FILES];

void vfs_save() { unsigned char* p = (unsigned char*)fs; for (int i = 0; i < 48; i++) ata_write_sector(i + 1, p + (i * 512)); unsigned char s[512] = {0}; s[0] = 'M'; s[1] = 'E'; s[2] = 'C'; s[3] = 'T'; s[4] = 'O'; s[5] = 'V'; ata_write_sector(0, s); }
int vfs_load() { unsigned char s[512]; ata_read_sector(0, s); if (s[0] == 'M' && s[1] == 'E' && s[2] == 'C' && s[3] == 'T' && s[4] == 'O' && s[5] == 'V') { unsigned char* p = (unsigned char*)fs; for (int i = 0; i < 48; i++) ata_read_sector(i + 1, p + (i * 512)); return 1; } return 0; }
int vfs_find(const char* n) { for (int i = 0; i < MAX_FILES; i++) if (fs[i].in_use && strcmp(fs[i].name, n) == 0) return i; return -1; }
int vfs_create(const char* n) { if (vfs_find(n) != -1) return -2; for (int i = 0; i < MAX_FILES; i++) if (!fs[i].in_use) { strcpy(fs[i].name, n); fs[i].in_use = 1; fs[i].size = 0; fs[i].data[0] = '\0'; vfs_save(); return i; } return -1; }
