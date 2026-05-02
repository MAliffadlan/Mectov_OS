#ifndef VFS_H
#define VFS_H

#include "types.h"

#define MAX_NODES     64
#define MAX_PATH      256
#define MAX_FILENAME  32

typedef enum { FS_FILE, FS_DIR, FS_DEV } fs_type_t;

typedef struct {
    char name[MAX_FILENAME];
    fs_type_t type;
    int parent;          // Index parent directory (-1 = root)
    int size;            // Untuk FILE: size data
    int data_sector;     // Untuk FILE: ATA sector start data
    int in_use;
    char pad[460];       // Total 512 bytes per node
} __attribute__((packed)) fs_node_t;

extern fs_node_t fs_nodes[MAX_NODES];
extern int current_dir;  // Index node dari current directory

// Inisialisasi VFS
void vfs_init();

// Simpan/load dari ATA disk
void vfs_save();
int vfs_load();

// Operasi node
int vfs_find_in_dir(const char* name, int dir_node);
int vfs_find_path(const char* path, int* parent_dir);
int vfs_create_node(const char* name, fs_type_t type, int parent);
int vfs_mkdir(const char* path);
int vfs_create_file(const char* path);
int vfs_delete_node(const char* path);
int vfs_write_file(const char* path, const char* data, int size);
int vfs_read_file(const char* path, char* buf, int max_size);

// Resolusi path
void vfs_resolve_path(const char* path, char* resolved, int buf_size);
int vfs_get_abs_path(int node_idx, char* buf, int buf_size);

// List directory
void vfs_list_dir(int dir_node, void (*print_fn)(const char*, unsigned char));
void vfs_tree(int dir_node, int depth, void (*print_fn)(const char*, unsigned char));

// Helpers
int vfs_is_dir(int node);
int vfs_is_file(int node);
int vfs_get_node(const char* path);
int vfs_get_node_count();
int vfs_get_parent(const char* path, char* parent_path, int buf_size);

#endif