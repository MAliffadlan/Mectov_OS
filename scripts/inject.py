import struct
import os

disk_path = '/home/mectov/my-os/disk.img'

# Cek apakah disk.img ada, kalau tidak, buat baru
if not os.path.exists(disk_path) or os.path.getsize(disk_path) == 0:
    with open(disk_path, 'wb') as f:
        f.write(b'\0' * (1024 * 1024)) # 1MB

with open(disk_path, 'r+b') as f:
    # Pastikan disk terformat dengan signature MECTOV
    f.seek(0)
    sig = f.read(6)
    if sig != b'MECTOV':
        f.seek(0)
        f.write(b'MECTOV' + b'\0'*506)

    # Ini adalah teks script lagu "Bintang Kecil"
    content = (
        b"echo --- Bintang Kecil ---\n"
        b"nada 261 500\n"
        b"tunggu 100\n"
        b"nada 261 500\n"
        b"tunggu 100\n"
        b"nada 392 500\n"
        b"tunggu 100\n"
        b"nada 392 500\n"
        b"tunggu 100\n"
        b"nada 440 500\n"
        b"tunggu 100\n"
        b"nada 440 500\n"
        b"tunggu 100\n"
        b"nada 392 1000\n"
        b"tunggu 200\n"
        b"nada 349 500\n"
        b"tunggu 100\n"
        b"nada 349 500\n"
        b"tunggu 100\n"
        b"nada 329 500\n"
        b"tunggu 100\n"
        b"nada 329 500\n"
        b"tunggu 100\n"
        b"nada 293 500\n"
        b"tunggu 100\n"
        b"nada 293 500\n"
        b"tunggu 100\n"
        b"nada 261 1000\n"
        b"echo --- Selesai! ---\n"
    )
    
    # Format ke dalam struct VFS milik Mectov OS (1536 bytes per file)
    filename = b"lagu.ms".ljust(16, b'\0')
    data = content.ljust(1024, b'\0')
    size = len(content)
    in_use = 1
    
    # <16s (name) 1024s (data) i (size) i (in_use) 488s (padding)
    file_struct = struct.pack('<16s1024sii488s', filename, data, size, in_use, b'\0'*488)

    # Cari slot kosong di VFS ATA IDE
    for i in range(16):
        f.seek(512 + i * 1536)
        slot_data = f.read(1536)
        slot_name = slot_data[0:16].strip(b'\0')
        slot_in_use = struct.unpack('<i', slot_data[1044:1048])[0]
        
        # Kalau slotnya kosong atau namanya sama-sama lagu.ms (tiban aja)
        if slot_in_use == 0 or slot_name == b"lagu.ms":
            f.seek(512 + i * 1536)
            f.write(file_struct)
            print("File 'lagu.ms' berhasil di-inject ke dalam disk.img!")
            break