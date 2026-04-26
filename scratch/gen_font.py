import sys

with open('/tmp/terminus_bold.psf', 'rb') as f:
    magic1 = f.read(1)[0]
    magic2 = f.read(1)[0]
    mode = f.read(1)[0]
    charsize = f.read(1)[0]
    
    if magic1 != 0x36 or magic2 != 0x04:
        print("Not a valid PSF1 file")
        sys.exit(1)
        
    num_chars = 512 if (mode & 0x01) else 256
    
    font_data = []
    for i in range(num_chars):
        char_data = f.read(charsize)
        font_data.append(char_data)

with open('/home/mectov/my-os/src/drivers/font8x16.c', 'w') as f:
    f.write('#include "../include/font8x16.h"\n')
    f.write('unsigned char font8x16_data[256][16] = {\n')
    for i in range(256):
        data = font_data[i]
        hex_data = ','.join([f"0x{b:02X}" for b in data])
        f.write(f'    [0x{i:02X}] = {{{hex_data}}},\n')
    f.write('};\n')

print("font8x16.c generated successfully.")
