// ============================================================
// vfs.c — Mectov OS Virtual File System with Directory Support
// ============================================================
// Layout pada ATA disk:
//   Sector 0      : Magic signature "MECTOVFS" + metadata
//   Sector 1-64   : Node table (64 nodes × 512 bytes = 32768)
//   Sector 65+    : File data blocks (per file, kontigu)
// ============================================================

#include "../include/vfs.h"
#include "../include/ata.h"
#include "../include/utils.h"
#include "../include/mem.h"

// Embedded binary for gcalc.mct
extern uint8_t _binary_gcalc_mct_start[];
extern uint8_t _binary_gcalc_mct_end[];
static uint32_t gcalc_mct_size() { return (uint32_t)(_binary_gcalc_mct_end - _binary_gcalc_mct_start); }

fs_node_t fs_nodes[MAX_NODES];
int current_dir = 0;  // Root directory

// --- Internal helpers ---

static void strtolower(char* dst, const char* src) {
    while (*src) {
        char c = *src++;
        if (c >= 'A' && c <= 'Z') c += 32;
        *dst++ = c;
    }
    *dst = '\0';
}

// Split path into components. Return number of components.
// Contoh: "/home/user/file.txt" → {"home","user","file.txt"}
static int split_path(const char* path, char components[MAX_PATH/2][MAX_FILENAME]) {
    int count = 0;
    int i = 0, j = 0;
    
    if (!path || path[0] == '\0') return 0;
    
    // Skip leading slash
    if (path[0] == '/') i = 1;
    
    while (path[i]) {
        if (path[i] == '/') {
            if (j > 0) {
                components[count][j] = '\0';
                count++;
                j = 0;
            }
        } else if (j < MAX_FILENAME - 1) {
            components[count][j++] = path[i];
        }
        i++;
    }
    if (j > 0) {
        components[count][j] = '\0';
        count++;
    }
    return count;
}

// --- Inisialisasi ---

void vfs_init() {
    // Coba load dari disk
    if (vfs_load()) return;
    
    // Tidak ada filesystem — buat root directory
    memset(fs_nodes, 0, sizeof(fs_nodes));
    fs_nodes[0].in_use = 1;
    fs_nodes[0].parent = -1;
    fs_nodes[0].type = FS_DIR;
    strcpy(fs_nodes[0].name, "/");
    fs_nodes[0].size = 0;
    current_dir = 0;
    
    // Buat home directory default
    vfs_create_node("home", FS_DIR, 0);
    vfs_create_node("home/user", FS_DIR, 1);
    
    // Populate dengan beberapa file demo
    vfs_create_file("readme.txt");
    vfs_write_file("readme.txt", "Welcome to Mectov OS v15.0!\n\nThis is a hobby operating system.\nUse 'help' for commands.\n", 95);
    
    vfs_create_file("hello.txt");
    vfs_write_file("hello.txt", "Hello, World!\n", 14);
    
    // Buat directory apps untuk .mct files
    vfs_create_node("apps", FS_DIR, 0);
    
    // Inject gcalc.mct dari embedded binary ke VFS
    vfs_create_file("apps/gcalc.mct");
    vfs_write_file("apps/gcalc.mct", (const char*)_binary_gcalc_mct_start, gcalc_mct_size());
    
    vfs_save();
}

// --- Simpan / Load dari ATA ---

#define VFS_MAGIC_SECTOR  0
#define VFS_NODE_START    1
#define VFS_NODE_SECTORS  64   // 64 nodes * 512 bytes = 32KB on disk
#define VFS_DATA_START    (VFS_NODE_START + VFS_NODE_SECTORS)

// Magic signature: 8 bytes + 2 bytes version + 6 bytes reserved = 16 bytes in sector 0
void vfs_save() {
    unsigned char meta[512];
    memset(meta, 0, 512);
    
    // Write magic + metadata
    meta[0] = 'M'; meta[1] = 'E'; meta[2] = 'C'; meta[3] = 'T';
    meta[4] = 'O'; meta[5] = 'V'; meta[6] = 'F'; meta[7] = 'S';  // "MECTOVFS"
    meta[8] = 0x01;  // Version major
    meta[9] = 0x00;  // Version minor
    
    // Current dir index
    meta[16] = (unsigned char)(current_dir & 0xFF);
    meta[17] = (unsigned char)((current_dir >> 8) & 0xFF);
    
    // Node count (for quick validation)
    int count = 0;
    for (int i = 0; i < MAX_NODES; i++) if (fs_nodes[i].in_use) count++;
    meta[18] = (unsigned char)(count & 0xFF);
    meta[19] = (unsigned char)((count >> 8) & 0xFF);
    
    ata_write_sector(VFS_MAGIC_SECTOR, meta);
    
    // Write node table
    unsigned char* p = (unsigned char*)fs_nodes;
    for (int i = 0; i < VFS_NODE_SECTORS; i++) {
        ata_write_sector(VFS_NODE_START + i, p + (i * 512));
    }
}

int vfs_load() {
    unsigned char meta[512];
    ata_read_sector(VFS_MAGIC_SECTOR, meta);
    
    // Check magic
    if (meta[0] != 'M' || meta[1] != 'E' || meta[2] != 'C' ||
        meta[3] != 'T' || meta[4] != 'O' || meta[5] != 'V' ||
        meta[6] != 'F' || meta[7] != 'S') {
        return 0;  // Not a valid VFS disk
    }
    
    // Read node table
    unsigned char* p = (unsigned char*)fs_nodes;
    for (int i = 0; i < VFS_NODE_SECTORS; i++) {
        ata_read_sector(VFS_NODE_START + i, p + (i * 512));
    }
    
    // Restore current_dir
    current_dir = meta[16] | (meta[17] << 8);
    if (current_dir < 0 || current_dir >= MAX_NODES || !fs_nodes[current_dir].in_use)
        current_dir = 0;
    
    return 1;
}

// --- Resolusi Path ---

// Resolve relative/absolute path menjadi absolute path string
void vfs_resolve_path(const char* path, char* resolved, int buf_size) {
    if (!path || path[0] == '\0') {
        // Default: current directory
        vfs_get_abs_path(current_dir, resolved, buf_size);
        return;
    }
    
    if (path[0] == '/') {
        // Absolute path — copy langsung
        int i = 0;
        while (path[i] && i < buf_size - 1) {
            resolved[i] = path[i];
            i++;
        }
        resolved[i] = '\0';
        return;
    }
    
    // Relative path — resolve against current_dir
    char cur_path[MAX_PATH];
    vfs_get_abs_path(current_dir, cur_path, MAX_PATH);
    
    int cur_len = strlen(cur_path);
    int path_len = strlen(path);
    
    // Handle "." and ".." in relative path
    if (strcmp(path, ".") == 0) {
        strcpy(resolved, cur_path);
        return;
    }
    
    if (strcmp(path, "..") == 0) {
        // Go up from current directory
        if (current_dir == 0) {
            strcpy(resolved, "/");
            return;
        }
        vfs_get_abs_path(fs_nodes[current_dir].parent, resolved, buf_size);
        return;
    }
    
    // If current path is "/", just append
    if (cur_len == 1 && cur_path[0] == '/') {
        int i = 0;
        resolved[i++] = '/';
        int j = 0;
        while (path[j] && i < buf_size - 1) {
            resolved[i++] = path[j++];
        }
        resolved[i] = '\0';
        return;
    }
    
    // Otherwise: current_path + "/" + relative_path
    int i = 0;
    while (cur_path[i] && i < buf_size - 1) {
        resolved[i] = cur_path[i];
        i++;
    }
    if (i > 0 && resolved[i-1] != '/' && i < buf_size - 1) {
        resolved[i++] = '/';
    }
    int j = 0;
    while (path[j] && i < buf_size - 1) {
        resolved[i++] = path[j++];
    }
    resolved[i] = '\0';
}

// Dapatkan absolute path dari node index
int vfs_get_abs_path(int node_idx, char* buf, int buf_size) {
    if (node_idx < 0 || node_idx >= MAX_NODES || !fs_nodes[node_idx].in_use) {
        strcpy(buf, "?");
        return -1;
    }
    
    // Build path from node up to root
    char stack[16][MAX_FILENAME];
    int sp = 0;
    int cur = node_idx;
    
    while (cur >= 0 && fs_nodes[cur].in_use) {
        if (strlen(fs_nodes[cur].name) > 0) {
            strcpy(stack[sp++], fs_nodes[cur].name);
        }
        cur = fs_nodes[cur].parent;
        if (sp >= 16) break;
    }
    
    // Build absolute path
    int i = 0;
    buf[i++] = '/';
    for (int s = sp - 1; s >= 0; s--) {
        int slen = strlen(stack[s]);
        if (i + slen >= buf_size - 1) break;
        if (i > 1) buf[i++] = '/';
        int j = 0;
        while (stack[s][j]) buf[i++] = stack[s][j++];
    }
    buf[i] = '\0';
    return i;
}

// Cari node berdasarkan path. Return node index atau -1.
int vfs_get_node(const char* path) {
    if (!path || path[0] == '\0') return current_dir;
    
    char resolved[MAX_PATH];
    vfs_resolve_path(path, resolved, MAX_PATH);
    
    // Root case
    if (strcmp(resolved, "/") == 0) return 0;
    
    // Parse resolved path into components
    char comps[MAX_PATH/2][MAX_FILENAME];
    int ncomp = split_path(resolved, comps);
    if (ncomp == 0) return 0;
    
    // Walk from root
    int cur = 0;
    for (int i = 0; i < ncomp; i++) {
        // Normalize component name (lowercase comparison)
        char lc_name[MAX_FILENAME];
        strtolower(lc_name, comps[i]);
        
        int found = -1;
        for (int j = 0; j < MAX_NODES; j++) {
            if (!fs_nodes[j].in_use) continue;
            if (fs_nodes[j].parent != cur) continue;
            
            char lc_node[MAX_FILENAME];
            strtolower(lc_node, fs_nodes[j].name);
            if (strcmp(lc_node, lc_name) == 0) {
                found = j;
                break;
            }
        }
        
        if (found < 0) return -1;
        cur = found;
    }
    
    return cur;
}

// Cari di dalam satu directory
int vfs_find_in_dir(const char* name, int dir_node) {
    if (dir_node < 0 || dir_node >= MAX_NODES) return -1;
    if (!fs_nodes[dir_node].in_use || fs_nodes[dir_node].type != FS_DIR) return -1;
    
    char lc_name[MAX_FILENAME];
    strtolower(lc_name, name);
    
    for (int i = 0; i < MAX_NODES; i++) {
        if (!fs_nodes[i].in_use) continue;
        if (fs_nodes[i].parent != dir_node) continue;
        
        char lc_node[MAX_FILENAME];
        strtolower(lc_node, fs_nodes[i].name);
        if (strcmp(lc_node, lc_name) == 0) return i;
    }
    return -1;
}

// --- Create / Delete Nodes ---

int vfs_create_node(const char* name, fs_type_t type, int parent) {
    // Validate parent
    if (parent < 0 || parent >= MAX_NODES) return -1;
    if (!fs_nodes[parent].in_use || fs_nodes[parent].type != FS_DIR) return -1;
    
    // Check name exists in parent
    if (vfs_find_in_dir(name, parent) >= 0) return -2;
    
    // Find free slot
    for (int i = 0; i < MAX_NODES; i++) {
        if (!fs_nodes[i].in_use) {
            strcpy(fs_nodes[i].name, name);
            fs_nodes[i].type = type;
            fs_nodes[i].parent = parent;
            fs_nodes[i].size = 0;
            fs_nodes[i].data_sector = 0;
            fs_nodes[i].in_use = 1;
            vfs_save();
            return i;
        }
    }
    return -1; // Full
}

int vfs_mkdir(const char* path) {
    // Find parent directory
    char parent_path[MAX_PATH];
    char dirname[MAX_FILENAME];
    
    if (vfs_get_parent(path, parent_path, MAX_PATH) < 0) {
        return -1;
    }
    
    // Extract directory name (last component of path)
    int len = strlen(path);
    int i = len - 1;
    while (i >= 0 && path[i] == '/') i--;
    int end = i;
    while (i >= 0 && path[i] != '/') i--;
    int start = i + 1;
    int j;
    for (j = 0; j < end - start + 1 && j < MAX_FILENAME - 1; j++) {
        dirname[j] = path[start + j];
    }
    dirname[j] = '\0';
    
    int parent = vfs_get_node(parent_path);
    if (parent < 0) return -1;
    
    return vfs_create_node(dirname, FS_DIR, parent);
}

int vfs_create_file(const char* path) {
    char parent_path[MAX_PATH];
    char filename[MAX_FILENAME];
    
    if (vfs_get_parent(path, parent_path, MAX_PATH) < 0) return -1;
    
    int len = strlen(path);
    int i = len - 1;
    while (i >= 0 && path[i] == '/') i--;
    int end = i;
    while (i >= 0 && path[i] != '/') i--;
    int start = i + 1;
    int j;
    for (j = 0; j < end - start + 1 && j < MAX_FILENAME - 1; j++) {
        filename[j] = path[start + j];
    }
    filename[j] = '\0';
    
    int parent = vfs_get_node(parent_path);
    if (parent < 0) return -1;
    
    return vfs_create_node(filename, FS_FILE, parent);
}

int vfs_delete_node(const char* path) {
    int node = vfs_get_node(path);
    if (node < 0) return -1;
    if (node == 0) return -3; // Cannot delete root
    
    // Recursively delete children if directory (handle nested dirs)
    if (fs_nodes[node].type == FS_DIR) {
        // Delete deepest children first (multiple passes needed for nesting)
        int deleted;
        do {
            deleted = 0;
            for (int i = 0; i < MAX_NODES; i++) {
                if (!fs_nodes[i].in_use) continue;
                // Check if this node is a descendant of target
                int p = fs_nodes[i].parent;
                int is_descendant = 0;
                while (p >= 0) {
                    if (p == node) { is_descendant = 1; break; }
                    if (!fs_nodes[p].in_use) break;
                    p = fs_nodes[p].parent;
                }
                if (is_descendant) {
                    // Only delete if this node has no children itself
                    int has_children = 0;
                    for (int j = 0; j < MAX_NODES; j++) {
                        if (fs_nodes[j].in_use && fs_nodes[j].parent == i) {
                            has_children = 1;
                            break;
                        }
                    }
                    if (!has_children) {
                        fs_nodes[i].in_use = 0;
                        deleted = 1;
                    }
                }
            }
        } while (deleted);
    }
    
    fs_nodes[node].in_use = 0;
    vfs_save();
    return 0;
}

// --- Read / Write File Data ---

// Data layout per file di ATA:
//   data_sector menyimpan start sector untuk data file ini.
//   Data disimpan sebagai array sektor kontigu.
//   Untuk file kecil (< 512 bytes), cukup 1 sektor.
//   Sektor terakhir berisi data file (tidak harus full 512 bytes).

int vfs_read_file(const char* path, char* buf, int max_size) {
    int node = vfs_get_node(path);
    if (node < 0) return -1;
    if (fs_nodes[node].type != FS_FILE) return -2;
    
    int size = fs_nodes[node].size;
    if (size > max_size) size = max_size;
    if (size <= 0) { buf[0] = '\0'; return 0; }
    
    int sector = fs_nodes[node].data_sector;
    if (sector <= 0) { buf[0] = '\0'; return 0; }
    
    // Read sectors
    int remaining = size;
    int offset = 0;
    while (remaining > 0) {
        unsigned char tmp[512];
        ata_read_sector(sector++, tmp);
        
        int chunk = remaining > 512 ? 512 : remaining;
        memcpy(buf + offset, tmp, chunk);
        offset += chunk;
        remaining -= chunk;
    }
    
    buf[size] = '\0';
    return size;
}

int vfs_write_file(const char* path, const char* data, int size) {
    int node = vfs_get_node(path);
    if (node < 0) return -1;
    if (fs_nodes[node].type != FS_FILE) return -2;
    
    // Calculate how many sectors needed
    int sectors_needed = (size + 511) / 512;
    if (sectors_needed < 1) sectors_needed = 1;
    
    // Find contiguous free sectors in data area
    // Simple strategy: just use the first available slot
    // For now, allocate from end of node table
    static int next_data_sector = 0;
    if (next_data_sector == 0) {
        // Find highest used data sector
        int max_sector = VFS_DATA_START;
        for (int i = 0; i < MAX_NODES; i++) {
            if (fs_nodes[i].in_use && fs_nodes[i].data_sector > max_sector)
                max_sector = fs_nodes[i].data_sector;
        }
        // Also check for freed sectors by scanning
        // For simplicity, start from after highest known sector
        next_data_sector = max_sector + 1;
        if (next_data_sector < VFS_DATA_START) next_data_sector = VFS_DATA_START;
    }
    
    // If node already has data sectors, reuse or expand
    int start_sector = fs_nodes[node].data_sector;
    if (start_sector == 0) {
        start_sector = next_data_sector;
        next_data_sector += sectors_needed;
        fs_nodes[node].data_sector = start_sector;
    }
    
    // Write data sectors
    int remaining = size;
    int offset = 0;
    int sector = start_sector;
    while (remaining > 0 || (offset == 0 && remaining == 0)) {
        unsigned char tmp[512];
        memset(tmp, 0, 512);
        
        int chunk = remaining > 512 ? 512 : remaining;
        if (chunk > 0) memcpy(tmp, data + offset, chunk);
        
        ata_write_sector(sector++, tmp);
        offset += 512;
        remaining -= 512;
        
        if (offset >= size) break;
    }
    
    fs_nodes[node].size = size;
    vfs_save();
    return size;
}

// --- Find path with parent resolution ---

int vfs_find_path(const char* path, int* parent_dir) {
    char resolved[MAX_PATH];
    vfs_resolve_path(path, resolved, MAX_PATH);
    
    if (strcmp(resolved, "/") == 0) {
        if (parent_dir) *parent_dir = -1;
        return 0;
    }
    
    // Parse into components, find parent
    char comps[MAX_PATH/2][MAX_FILENAME];
    int ncomp = split_path(resolved, comps);
    if (ncomp == 0) {
        if (parent_dir) *parent_dir = -1;
        return 0;
    }
    
    // Walk to parent directory (all components except last)
    int cur = 0;
    for (int i = 0; i < ncomp - 1; i++) {
        char lc_name[MAX_FILENAME];
        strtolower(lc_name, comps[i]);
        
        int found = -1;
        for (int j = 0; j < MAX_NODES; j++) {
            if (!fs_nodes[j].in_use) continue;
            if (fs_nodes[j].parent != cur) continue;
            
            char lc_node[MAX_FILENAME];
            strtolower(lc_node, fs_nodes[j].name);
            if (strcmp(lc_node, lc_name) == 0) {
                found = j;
                break;
            }
        }
        if (found < 0) return -1;
        cur = found;
    }
    
    if (parent_dir) *parent_dir = cur;
    
    // Find the last component in parent directory
    char lc_name[MAX_FILENAME];
    strtolower(lc_name, comps[ncomp - 1]);
    
    for (int j = 0; j < MAX_NODES; j++) {
        if (!fs_nodes[j].in_use) continue;
        if (fs_nodes[j].parent != cur) continue;
        
        char lc_node[MAX_FILENAME];
        strtolower(lc_node, fs_nodes[j].name);
        if (strcmp(lc_node, lc_name) == 0) {
            return j;
        }
    }
    
    return -1; // Last component not found
}

// Get parent path from a path string
int vfs_get_parent(const char* path, char* parent_path, int buf_size) {
    if (!path || path[0] == '\0') return -1;
    
    int len = strlen(path);
    int end = len - 1;
    
    // Strip trailing slashes
    while (end > 0 && path[end] == '/') end--;
    
    // Find last slash
    int last_slash = -1;
    for (int i = end - 1; i >= 0; i--) {
        if (path[i] == '/') {
            last_slash = i;
            break;
        }
    }
    
    if (last_slash < 0) {
        // Just a filename relative to current dir
        // Actually this means there's no parent in the path string
        // Return current dir path as parent
        return vfs_get_abs_path(current_dir, parent_path, buf_size);
    }
    
    if (last_slash == 0) {
        strcpy(parent_path, "/");
    } else {
        int i;
        for (i = 0; i < last_slash && i < buf_size - 1; i++) {
            parent_path[i] = path[i];
        }
        parent_path[i] = '\0';
    }
    
    return strlen(parent_path);
}

// --- Listing ---

void vfs_list_dir(int dir_node, void (*print_fn)(const char*, unsigned char)) {
    int count = 0;
    for (int i = 0; i < MAX_NODES; i++) {
        if (!fs_nodes[i].in_use) continue;
        if (fs_nodes[i].parent != dir_node) continue;
        
        count++;
        
        // Print file/dir icon
        if (fs_nodes[i].type == FS_DIR) {
            print_fn("[DIR]  ", 0x0B);
            print_fn(fs_nodes[i].name, 0x0B);
            print_fn("/\n", 0x0B);
        } else {
            print_fn("[FILE] ", 0x0F);
            print_fn(fs_nodes[i].name, 0x0F);
            
            // Print size
            print_fn("  (", 0x07);
            char size_str[12];
            int n = fs_nodes[i].size;
            int si = 0;
            if (n == 0) { size_str[si++] = '0'; }
            while (n > 0) { size_str[si++] = '0' + (n % 10); n /= 10; }
            size_str[si] = '\0';
            for (int j = 0; j < si/2; j++) { char t = size_str[j]; size_str[j] = size_str[si-1-j]; size_str[si-1-j] = t; }
            size_str[si] = ' '; size_str[si+1] = 'B'; size_str[si+2] = ')'; size_str[si+3] = '\n'; size_str[si+4] = '\0';
            print_fn(size_str, 0x07);
        }
    }
    
    if (count == 0) {
        print_fn("  (empty)\n", 0x07);
    }
}

void vfs_tree(int dir_node, int depth, void (*print_fn)(const char*, unsigned char)) {
    for (int i = 0; i < MAX_NODES; i++) {
        if (!fs_nodes[i].in_use) continue;
        if (fs_nodes[i].parent != dir_node) continue;
        
        // Print indent
        for (int d = 0; d < depth; d++) print_fn("  ", 0x0F);
        
        if (fs_nodes[i].type == FS_DIR) {
            print_fn("[", 0x0B); print_fn(fs_nodes[i].name, 0x0B); print_fn("]\n", 0x0B);
            vfs_tree(i, depth + 1, print_fn);
        } else {
            print_fn(fs_nodes[i].name, 0x0F);
            print_fn("\n", 0x0F);
        }
    }
}

// --- Helper Functions ---

int vfs_is_dir(int node) {
    if (node < 0 || node >= MAX_NODES) return 0;
    return fs_nodes[node].in_use && fs_nodes[node].type == FS_DIR;
}

int vfs_is_file(int node) {
    if (node < 0 || node >= MAX_NODES) return 0;
    return fs_nodes[node].in_use && fs_nodes[node].type == FS_FILE;
}

int vfs_get_node_count() {
    int count = 0;
    for (int i = 0; i < MAX_NODES; i++) {
        if (fs_nodes[i].in_use) count++;
    }
    return count;
}