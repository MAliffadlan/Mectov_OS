import struct

MAX_NODES = 64
NODE_SIZE = 512

def read_vfs(disk_path):
    with open(disk_path, "rb") as f:
        # Read magic
        f.seek(0)
        magic = f.read(8)
        print("Magic:", magic)
        
        # Read node table (Sectors 1 to 64)
        f.seek(512)
        nodes_raw = f.read(MAX_NODES * NODE_SIZE)
        
        print(f"{'Index':<5} | {'Name':<24} | {'Type':<8} | {'Parent':<6} | {'Size':<8} | {'Sector':<8} | {'InUse':<5}")
        print("-" * 80)
        for i in range(MAX_NODES):
            offset = i * NODE_SIZE
            node_bytes = nodes_raw[offset:offset+NODE_SIZE]
            
            # Struct:
            # char name[32]; (32 bytes)
            # fs_type_t type; (4 bytes int)
            # int parent; (4 bytes int)
            # int size; (4 bytes int)
            # int data_sector; (4 bytes int)
            # int in_use; (4 bytes int)
            # uint32_t ext2_inode; (4 bytes int)
            # char pad[456]; (456 bytes)
            
            name = node_bytes[0:32].decode('ascii', errors='replace').split('\x00')[0]
            type_val, parent, size, data_sector, in_use, ext2_inode = struct.unpack_from("<iiiiii", node_bytes, 32)
            
            if in_use:
                type_str = ["FILE", "DIR", "DEV", "EXT2_FILE", "EXT2_DIR"][type_val]
                print(f"{i:<5} | {name:<24} | {type_str:<8} | {parent:<6} | {size:<8} | {data_sector:<8} | {in_use:<5}")

if __name__ == "__main__":
    import sys
    disk = "disk.img" if len(sys.argv) < 2 else sys.argv[1]
    read_vfs(disk)
