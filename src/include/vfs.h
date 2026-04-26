#ifndef VFS_H
#define VFS_H

#define MAX_FILES 16
#define MAX_FILENAME 16
#define MAX_FILE_SIZE 1024

typedef struct {
    char name[MAX_FILENAME];
    char data[MAX_FILE_SIZE];
    int size;
    int in_use;
    char padding[488];
} File;

extern File fs[MAX_FILES];

void vfs_save();
int vfs_load();
int vfs_find(const char* n);
int vfs_create(const char* n);

#endif
