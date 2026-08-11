import sys
try:
    from PIL import Image
except ImportError:
    print("Pillow not installed")
    sys.exit(1)

input_img = sys.argv[1]
output_bin = sys.argv[2]

# Target resolution (matches grub.cfg gfxmode)
TARGET_W = 1024
TARGET_H = 768

img = Image.open(input_img)
img = img.resize((TARGET_W, TARGET_H), Image.Resampling.LANCZOS)
img = img.convert('RGB')

with open(output_bin, 'wb') as f:
    for y in range(TARGET_H):
        for x in range(TARGET_W):
            r, g, b = img.getpixel((x, y))
            # Little endian: B, G, R, A(0x00)
            f.write(bytes([b, g, r, 0]))

print(f"Successfully converted {input_img} to {output_bin} ({TARGET_W}x{TARGET_H}, 32-bit BGRA)")
