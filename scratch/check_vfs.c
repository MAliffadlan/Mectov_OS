
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define MAX_NODES 64
#define MAX_FILENAME 32

typedef enum { FS_FILE, FS_DIR } fs_type_t;

typedef struct {
    char name[MAX_FILENAME];
    fs_type_t type;
    int parent;
    int size;
    int data_sector;
    int in_use;
    char pad[504];
} fs_node_t;

int main() {
    FILE* f = fopen("disk.img", "rb");
    if (!f) {
        printf("Disk not found\n");
        return 1;
    }

    char magic[9];
    fread(magic, 1, 8, f);
    magic[8] = '\0';
    if (strcmp(magic, "MECTOVFS") != 0) {
        printf("Invalid magic: %s\n", magic);
    }

    fseek(f, 512, SEEK_SET); // Skip sector 0
    fs_node_t nodes[MAX_NODES];
    fread(nodes, sizeof(fs_node_t), MAX_NODES, f);

    int count = 0;
    for (int i = 0; i < MAX_NODES; i++) {
        if (nodes[i].in_use) {
            printf("Node %d: %s (Type: %s, Parent: %d, Size: %d, Sector: %d)\n", 
                i, nodes[i].name, nodes[i].type == FS_DIR ? "DIR" : "FILE", 
                nodes[i].parent, nodes[i].size, nodes[i].data_sector);
            count++;
        }
    }
    printf("Total nodes: %d/%d\n", count, MAX_NODES);

    fclose(f);
    return 0;
}
