import sys
import struct
import os

MAX_FILES = 16
FILE_STRUCT_SIZE = 4608 # 16 + 4096 + 4 + 4 + 488
VFS_SECTOR_START = 1

def inject_file(disk_img, mct_file, vfs_name):
    # Read MCT
    with open(mct_file, "rb") as f:
        mct_data = f.read()
        
    if len(mct_data) > 4096:
        print(f"[!] Error: {mct_file} is {len(mct_data)} bytes. Max VFS size is 4096 bytes!")
        return

    # Read disk
    with open(disk_img, "rb+") as f:
        f.seek(VFS_SECTOR_START * 512)
        vfs_raw = bytearray(f.read(144 * 512)) # 16 * 4608 = 73728 = 144 sectors
        
        # Find existing file or free slot
        target_idx = -1
        free_idx = -1
        
        for i in range(MAX_FILES):
            offset = i * FILE_STRUCT_SIZE
            in_use = struct.unpack_from("<i", vfs_raw, offset + 16 + 4096 + 4)[0]
            
            if in_use:
                # Read name
                name_bytes = vfs_raw[offset:offset+15]
                name_str = name_bytes.decode('ascii').split('\x00')[0]
                if name_str == vfs_name:
                    target_idx = i
                    break
            elif free_idx == -1:
                free_idx = i
                
        if target_idx != -1:
            print(f"[*] Found existing file at index {target_idx}, overwriting...")
            i = target_idx
        elif free_idx != -1:
            print(f"[*] Found free slot at index {free_idx}")
            i = free_idx
        else:
            print("[!] VFS is full!")
            return
            
        if True:
            offset = i * FILE_STRUCT_SIZE
            
            # Zero out the struct first
            for j in range(FILE_STRUCT_SIZE):
                vfs_raw[offset + j] = 0
            
            # Name
            name_bytes = vfs_name.encode('ascii')[:15]
            for j, b in enumerate(name_bytes):
                vfs_raw[offset + j] = b
            
            # Data
            for j, b in enumerate(mct_data):
                vfs_raw[offset + 16 + j] = b
                
            # Size & in_use
            struct.pack_into("<ii", vfs_raw, offset + 16 + 4096, len(mct_data), 1)
            
            # Write back
            f.seek(VFS_SECTOR_START * 512)
            f.write(vfs_raw)
            
            # Magic sector 0
            f.seek(0)
            magic = bytearray(f.read(512))
            magic[0:6] = b'MECTOV'
            f.seek(0)
            f.write(magic)
            
            print(f"[+] Injected {vfs_name} ({len(mct_data)} bytes) into disk.img!")
            return
                
        print("[!] No free slots in VFS!")

if __name__ == "__main__":
    if len(sys.argv) < 4:
        print("Usage: python3 inject_vfs.py <disk.img> <app.mct> <vfs_filename>")
        sys.exit(1)
    
    inject_file(sys.argv[1], sys.argv[2], sys.argv[3])
