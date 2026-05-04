from PIL import Image
import sys

def convert_png_to_ico(png_path, ico_path):
    img = Image.open(png_path)
    # Resize to common icon sizes
    icon_sizes = [(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]
    img.save(ico_path, sizes=icon_sizes)

if __name__ == "__main__":
    convert_png_to_ico(sys.argv[1], sys.argv[2])
