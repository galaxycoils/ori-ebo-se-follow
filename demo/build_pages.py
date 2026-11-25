"""Package only the simulator assets for GitHub Pages."""

import json
import shutil
import sys
from pathlib import Path

from fixtures import HEIGHT, MOVEMENT, REFERENCE, WIDTH, texture

source = Path(__file__).parent
out = Path(sys.argv[1])
html = (source / "index.html").read_text().replace('<html lang="en">', '<html lang="en" data-preview="pages">')
(out / "index.html").write_text(html.replace("Desktop replay demo", "GitHub Pages simulator"))
shutil.copyfile(source / "simulator.mjs", out / "simulator.mjs")

sizes = [box[2] for box, _ in MOVEMENT]
smallest, largest = min(sizes), max(sizes)
(out / "textures.bin").write_bytes(
    b"".join(texture(size, size, 0) for size in range(smallest, largest + 1))
)
config = dict(width=WIDTH, height=HEIGHT, reference=REFERENCE, legs=MOVEMENT,
              smallest=smallest, largest=largest)
(out / "fixtures.mjs").write_text(f"export default {json.dumps(config)};\n")
