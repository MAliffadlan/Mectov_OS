import sys
import struct
import os

MAX_FILES = 16
FILE_STRUCT_SIZE = 1536 # 16 + 1024 + 4 + 4 + 488
VFS_SECTOR_START = 1

def inject_file(disk_img, mct_file, vfs_name):
    # Read MCT
    with open(mct_file, "rb") as f:
        mct_data = f.read()
        
    if len(mct_data) > 1024:
        print(f"[!] Error: {mct_file} is {len(mct_data)} bytes. Max VFS size is 1024 bytes!")
        return

    # Read disk
    with open(disk_img, "rb+") as f:
        f.seek(VFS_SECTOR_START * 512)
        vfs_raw = bytearray(f.read(48 * 512))
        
        # Find free slot
        for i in range(MAX_FILES):
            offset = i * FILE_STRUCT_SIZE
            in_use = struct.unpack_from("<i", vfs_raw, offset + 16 + 1024 + 4)[0]
            
            if in_use == 0:
                print(f"[*] Found free slot at index {i}")
                # Pack new file
                # name (16), data (1024), size (4), in_use (4), padding (488)
                
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
                struct.pack_into("<ii", vfs_raw, offset + 16 + 1024, len(mct_data), 1)
                
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
