import sys
try:
    from PIL import Image
except ImportError:
    print("Pillow not installed")
    sys.exit(1)

input_img = sys.argv[1]
output_bin = sys.argv[2]

img = Image.open(input_img)
img = img.resize((800, 600), Image.Resampling.LANCZOS)
img = img.convert('RGB')

with open(output_bin, 'wb') as f:
    for y in range(600):
        for x in range(800):
            r, g, b = img.getpixel((x, y))
            # Little endian: B, G, R, A(0x00)
            f.write(bytes([b, g, r, 0]))

print(f"Successfully converted {input_img} to {output_bin} (800x600, 32-bit BGRA)")
