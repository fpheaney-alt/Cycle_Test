"""Turn the .ppm screen captures written by the simulation into .png files (2x size, easier to read)."""
import os
import sys

from PIL import Image

folder = sys.argv[1] if len(sys.argv) > 1 else "out"
count = 0
for name in sorted(os.listdir(folder)):
    if name.endswith(".ppm"):
        path = os.path.join(folder, name)
        image = Image.open(path)
        image = image.resize((image.width * 2, image.height * 2), Image.NEAREST)
        image.save(path[:-4] + ".png")
        count += 1
print("wrote %d PNG files to %s/" % (count, folder))
