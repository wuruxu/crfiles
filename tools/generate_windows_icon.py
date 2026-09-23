#!/usr/bin/env python3
"""Regenerate the Windows ICO from the application SVG (Inkscape + Pillow)."""

from pathlib import Path
import subprocess
import tempfile

from PIL import Image


def main():
    project = Path(__file__).resolve().parent.parent
    destination = project / "resources/win/crfiles_app_icon.ico"
    with tempfile.TemporaryDirectory(prefix="crfiles-icon-") as directory:
        png = Path(directory) / "icon.png"
        subprocess.run([
            "inkscape", str(project / "crFiles.svg"),
            "--export-type=png", "--export-area-page",
            "--export-width=512", "--export-height=512",
            f"--export-filename={png}",
        ], check=True)
        with Image.open(png) as image:
            image.convert("RGBA").save(
                destination, format="ICO",
                sizes=[(size, size) for size in (16, 24, 32, 48, 64, 128, 256)])
    print(destination)


if __name__ == "__main__":
    main()
