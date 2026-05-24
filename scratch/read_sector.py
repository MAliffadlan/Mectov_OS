import sys

def read_sector(disk_path, sector_num, size):
    with open(disk_path, "rb") as f:
        f.seek(sector_num * 512)
        data = f.read(size)
        print("Raw bytes:", data)
        print("ASCII:", data.decode('ascii', errors='replace'))

if __name__ == "__main__":
    disk = "disk.img"
    # Notepad tes has sector 290, size 15
    read_sector(disk, 290, 15)
