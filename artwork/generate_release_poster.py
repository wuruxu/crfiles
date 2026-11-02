#!/usr/bin/env python3
"""Generate a self-contained, 1080 x 2400 crFiles release poster.

Run with Python 3, without building Chromium:
  python3 crFiles/artwork/generate_release_poster.py
  python3 crFiles/artwork/generate_release_poster.py --version 1.6.9

The default version comes from crFiles/VERSION. Each version has a repeatable
particle field and data traces. All app icons are embedded as SVG vectors.
"""

import argparse
import hashlib
from pathlib import Path
import random
import re
import xml.etree.ElementTree as ET


APP = Path(__file__).resolve().parents[1]
ROOT = APP.parent
SVG = "http://www.w3.org/2000/svg"
XLINK = "http://www.w3.org/1999/xlink"
ET.register_namespace("", SVG)
ET.register_namespace("xlink", XLINK)


def icon(relative_path, prefix, x, y, size):
    root = ET.parse(ROOT / relative_path).getroot()
    # Expand SVG 2 shadows for SVG 1.1 renderers (including older Inkscape).
    for parent in list(root.iter()):
        for shadow in list(parent):
            if shadow.tag != "{" + SVG + "}feDropShadow":
                continue
            position = list(parent).index(shadow)
            source = shadow.get("in", "SourceGraphic")
            primitives = [
                ET.Element("{" + SVG + "}feGaussianBlur", {
                    "in": source, "stdDeviation": shadow.get("stdDeviation", "2"),
                    "result": "shadow-blur"}),
                ET.Element("{" + SVG + "}feOffset", {
                    "in": "shadow-blur", "dx": shadow.get("dx", "2"),
                    "dy": shadow.get("dy", "2"), "result": "shadow-offset"}),
                ET.Element("{" + SVG + "}feFlood", {
                    "flood-color": shadow.get("flood-color", "black"),
                    "flood-opacity": shadow.get("flood-opacity", "1"),
                    "result": "shadow-color"}),
                ET.Element("{" + SVG + "}feComposite", {
                    "in": "shadow-color", "in2": "shadow-offset", "operator": "in",
                    "result": "shadow"}),
            ]
            merge = ET.Element("{" + SVG + "}feMerge")
            ET.SubElement(merge, "{" + SVG + "}feMergeNode", {"in": "shadow"})
            ET.SubElement(merge, "{" + SVG + "}feMergeNode", {"in": source})
            primitives.append(merge)
            parent.remove(shadow)
            for offset, primitive in enumerate(primitives):
                parent.insert(position + offset, primitive)
    ids = {node.get("id"): prefix + "-" + node.get("id")
           for node in root.iter() if node.get("id")}
    for node in root.iter():
        for key, value in list(node.attrib.items()):
            if key == "id":
                node.set(key, ids[value])
            elif key in ("aria-labelledby", "aria-describedby"):
                node.set(key, " ".join(ids.get(part, part) for part in value.split()))
            else:
                value = re.sub(r"url\(#([^)]*)\)",
                               lambda m: "url(#" + ids[m[1]] + ")", value)
                if key in ("href", "{" + XLINK + "}href") and value.startswith("#"):
                    value = "#" + ids[value[1:]]
                node.set(key, value)
    root.attrib.update(x=str(x), y=str(y), width=str(size), height=str(size))
    return ET.tostring(root, encoding="unicode")


def make_poster(version):
    rng = random.Random(int.from_bytes(hashlib.sha256(version.encode()).digest(), "big"))
    particles = []
    for _ in range(145):
        x, y = rng.randrange(36, 1044), rng.randrange(170, 1950)
        radius = rng.choice((0.8, 1, 1.3, 2))
        particles.append(f'<circle cx="{x}" cy="{y}" r="{radius}" '
                         f'fill="#93d9ef" opacity="{rng.uniform(.1, .48):.2f}"/>')
    for side in (0, 1):
        for i in range(8):
            x = 24 + i * 22 if side == 0 else 1056 - i * 22
            y = rng.randrange(1050, 1540)
            end = x + (100 if side == 0 else -100)
            particles.append(f'<path d="M{x} 930 V{y} L{end} {y + 100} V1810" '
                             'fill="none" stroke="#5ce0e6" stroke-opacity=".09"/>')
            particles.append(f'<circle cx="{x}" cy="{y}" r="2.5" fill="#6df0e1" opacity=".55"/>')

    family = [
        ("crScrcpy", "crScrcpy/crScrcpy.svg"),
        ("crShare", "crShare/crShare.svg"),
        ("crPDF", "crpdf/pdf.svg"),
        ("crMarkdown", "crMarkdown/md.svg"),
        ("crRequest", "crRequest/crRequest.svg"),
        ("crPhotos", "crPhotos/crPhotos-logo.svg"),
        ("crDesk", "crDesk/crDesk.svg"),
        ("crPlayer", "crPlayer/crPlayer.svg"),
        ("crYGet", "crYGet/crYGet.svg"),
    ]
    family_svg = []
    for i, (name, path) in enumerate(family):
        row, column = divmod(i, 5)
        row_count = min(5, len(family) - row * 5)
        x = 540 + (column - (row_count - 1) / 2) * 180
        y = 2072 + row * 137
        family_svg.append(icon(path, name, x - 38, y, 76))
        family_svg.append(f'<text x="{x}" y="{y + 102}" class="family">{name}</text>')

    # Reuse the bundled browser mark geometry with Chrome's color palette.
    chrome_badge = icon("chrome/app/theme/chromium/product_logo.svg",
                        "chrome-badge", 681, 248, 80)
    for chromium_color, chrome_color in (("#669DF6", "#34A853"),
                                         ("#AECBFA", "#FBBC05"),
                                         ("#1967D2", "#EA4335"),
                                         ("#1A73E8", "#4285F4")):
        chrome_badge = chrome_badge.replace(chromium_color, chrome_color)

    return TEMPLATE.replace("{{VERSION}}", version).replace(
        "{{PARTICLES}}", "\n".join(particles)).replace(
        "{{ICON}}", icon("crFiles/crFiles.svg", "crfiles", 350, 257, 380)).replace(
        "{{CHROME_BADGE}}", chrome_badge).replace(
        "{{FAMILY}}", "\n".join(family_svg))


TEMPLATE = '''<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"
     width="1080" height="2400" viewBox="0 0 1080 2400" role="img" aria-labelledby="poster-title poster-desc">
  <title id="poster-title">crFiles {{VERSION}} released</title>
  <desc id="poster-desc">A crFiles release poster with the original app icon, a luminous file constellation representing search, previews, archives and QuickShare with an Android icon, and nine libcr family app icons.</desc>
  <defs>
    <linearGradient id="background" x2=".7" y2="1">
      <stop stop-color="#091423"/><stop offset=".55" stop-color="#050c18"/><stop offset="1" stop-color="#080e1b"/>
    </linearGradient>
    <radialGradient id="halo">
      <stop stop-color="#197e9a" stop-opacity=".37"/><stop offset=".48" stop-color="#145773" stop-opacity=".16"/><stop offset="1" stop-color="#10263e" stop-opacity="0"/>
    </radialGradient>
    <radialGradient id="core-glow">
      <stop stop-color="#2eeac6" stop-opacity=".22"/><stop offset="1" stop-color="#15bdc2" stop-opacity="0"/>
    </radialGradient>
    <linearGradient id="folder-back" x2=".4" y2="1">
      <stop stop-color="#164b60"/><stop offset="1" stop-color="#0c2238"/>
    </linearGradient>
    <linearGradient id="folder-front" x2=".85" y2="1">
      <stop stop-color="#226a7a" stop-opacity=".96"/><stop offset=".45" stop-color="#123d52" stop-opacity=".97"/><stop offset="1" stop-color="#0b2038"/>
    </linearGradient>
    <linearGradient id="edge" x2="1" y2=".6">
      <stop stop-color="#b0fff2"/><stop offset=".45" stop-color="#50ded5"/><stop offset="1" stop-color="#4588e2" stop-opacity=".3"/>
    </linearGradient>
    <linearGradient id="card" x2="1" y2="1">
      <stop stop-color="#1c394e"/><stop offset="1" stop-color="#0b192c"/>
    </linearGradient>
    <linearGradient id="fade-line">
      <stop stop-color="#65d6dc" stop-opacity="0"/><stop offset=".5" stop-color="#65d6dc" stop-opacity=".6"/><stop offset="1" stop-color="#65d6dc" stop-opacity="0"/>
    </linearGradient>
    <pattern id="grid" width="60" height="60" patternUnits="userSpaceOnUse">
      <path d="M60 0H0V60" fill="none" stroke="#62afc6" stroke-opacity=".045"/>
    </pattern>
    <filter id="glow" x="-70%" y="-70%" width="240%" height="240%">
      <feGaussianBlur stdDeviation="5"/>
    </filter>
    <style>
      text {font-family:'DejaVu Sans',Arial,sans-serif;fill:#eff8ff}
      .mono {font-family:'DejaVu Sans Mono',monospace}
      .label {font-size:18px;letter-spacing:4px;fill:#88a8ba}
      .caption {font-size:19px;letter-spacing:2px;fill:#9cb6c8}
      .family {font-size:18px;fill:#9eb1c6;text-anchor:middle}
      .small {font-size:16px;letter-spacing:2px;fill:#78a4b6}
    </style>
  </defs>

  <rect width="1080" height="2400" fill="url(#background)"/>
  <rect width="1080" height="1955" fill="url(#grid)"/>
  <ellipse cx="540" cy="440" rx="650" ry="620" fill="url(#halo)"/>
  <ellipse cx="540" cy="1420" rx="580" ry="510" fill="url(#core-glow)"/>
  {{PARTICLES}}

  <!-- Release identity -->
  <path d="M76 102h25m-25 0v25M1004 102h-25m25 0v25" fill="none" stroke="#4c798e"/>
  <text x="112" y="120" font-size="26" font-weight="700" letter-spacing="1">libcr<tspan font-weight="400" fill="#7794a9"> / </tspan></text>
  <text x="217" y="119" class="label" font-size="16">APP COLLECTION</text>
  <circle cx="802" cy="112" r="4" fill="#78e8ca"/>
  <text x="969" y="119" text-anchor="end" class="mono small">RELEASE {{VERSION}}</text>

  <!-- Orbital geometry frames the original vector app icon. -->
  <g fill="none" stroke="#74c9df">
    <circle cx="540" cy="445" r="241" stroke-opacity=".10"/>
    <circle cx="540" cy="445" r="273" stroke-opacity=".08" stroke-dasharray="2 13"/>
    <path d="M320 347A241 241 0 0 1 454 220M761 542A241 241 0 0 1 626 670" stroke-opacity=".5"/>
    <ellipse cx="540" cy="453" rx="375" ry="102" transform="rotate(-23 540 453)" stroke-opacity=".16"/>
  </g>
  <circle cx="224" cy="588" r="4" fill="#86f8da"/>
  <circle cx="856" cy="318" r="3" fill="#88b7ff"/>
  {{ICON}}
  <g id="chrome-superscript" opacity=".24" aria-label="Chrome">
    {{CHROME_BADGE}}
  </g>
  <text x="540" y="778" text-anchor="middle" font-size="132" font-weight="700" letter-spacing="-7">crFiles</text>
  <text id="release-version" x="540" y="868" text-anchor="middle" font-size="54" letter-spacing="-1"><tspan fill="#7cf4dc">{{VERSION}}</tspan><tspan dx="16" fill="#c8d8e7">released</tspan></text>
  <text x="540" y="938" text-anchor="middle" font-size="27" fill="#94afc4">Every file. In its place.</text>

  <!-- File constellation: search, preview, archiving and QuickShare. -->
  <g fill="none" stroke-linejoin="round">
    <path d="M100 1550L540 1320L980 1550L540 1780Z" stroke="#5da5be" stroke-opacity=".12"/>
    <path d="M155 1550L540 1350L925 1550L540 1750Z" stroke="#5da5be" stroke-opacity=".12"/>
    <path d="M210 1550L540 1380L870 1550L540 1720Z" stroke="#69d6d7" stroke-opacity=".2"/>
    <path d="M100 1550H980M540 1320V1780M320 1435L760 1665M320 1665L760 1435" stroke="#67a7c0" stroke-opacity=".11"/>
    <ellipse cx="540" cy="1522" rx="413" ry="154" stroke="#559cbd" stroke-opacity=".15" stroke-dasharray="4 10"/>
    <path d="M219 1190H300L380 1270M852 1265H800L730 1335M236 1578H310L372 1516M848 1590H768L708 1530" stroke="#64e7d4" stroke-opacity=".45"/>
  </g>

  <g transform="translate(112 1096)">
    <rect width="188" height="136" rx="18" fill="url(#card)" stroke="#466f88" stroke-opacity=".5"/>
    <circle cx="43" cy="43" r="14" fill="none" stroke="#7ee9da" stroke-width="2.5"/>
    <path d="M54 54l12 12M83 35h73M83 49h49" fill="none" stroke="#7ee9da" stroke-width="2.5"/>
    <text x="23" y="103" class="caption">SEARCH</text>
  </g>
  <g transform="translate(792 1187)">
    <rect width="176" height="136" rx="18" fill="url(#card)" stroke="#466f88" stroke-opacity=".5"/>
    <rect x="24" y="24" width="48" height="40" rx="5" fill="none" stroke="#87b6ff" stroke-width="2"/>
    <circle cx="58" cy="35" r="4" fill="#87b6ff"/>
    <path d="M26 57l14-16 16 18 8-9 8 9" fill="none" stroke="#87b6ff" stroke-width="2"/>
    <text x="23" y="103" class="caption">PREVIEW</text>
  </g>

  <!-- Layered document sheets emerge from the luminous folder. -->
  <g transform="rotate(-13 480 1370)">
    <path d="M354 1215v-100q0-16 16-16h102l48 48v188H354Z" fill="url(#card)" stroke="#618ea8" stroke-width="1.5"/>
    <path d="M472 1099v48h48M380 1180h94M380 1196h72M380 1212h86" fill="none" stroke="#89b8cb" stroke-opacity=".65" stroke-width="2"/>
  </g>
  <g transform="rotate(9 606 1260)">
    <path d="M542 1086h103l48 48v224H526v-256q0-16 16-16Z" fill="url(#card)" stroke="#75adb7" stroke-width="1.5"/>
    <path d="M645 1086v48h48" fill="none" stroke="#75adb7" stroke-width="1.5"/>
    <rect x="550" y="1160" width="119" height="80" rx="6" fill="#214459"/>
    <path d="M552 1222l35-36 32 30 20-17 28 30" fill="none" stroke="#75d8d3" stroke-width="2"/>
    <circle cx="643" cy="1178" r="8" fill="#79dbc8"/>
    <path d="M550 1260h113M550 1277h79" stroke="#75adb7" stroke-width="2"/>
  </g>
  <path d="M325 1281v-39q0-22 22-22h108l35 38h227q25 0 25 25v236H325Z" fill="url(#folder-back)" stroke="#6bcbbf" stroke-width="2"/>
  <path d="M298 1312q-4-22 21-22h435q24 0 19 25l-37 239q-3 21-26 21H357q-23 0-26-22Z" fill="url(#folder-front)" stroke="url(#edge)" stroke-width="2"/>
  <path d="M319 1294h435" stroke="#a5fff1" stroke-width="2"/>
  <path d="M345 1520h376" stroke="#69ffe0" stroke-width="2" opacity=".5" filter="url(#glow)"/>
  <path d="M345 1520h376" stroke="#69ffe0" stroke-width="1" opacity=".5"/>
  <g transform="translate(489 1360)" fill="none" stroke="#a3ffea" stroke-width="2.5" stroke-linecap="round">
    <rect width="103" height="91" rx="16" stroke-opacity=".3"/>
    <path d="M28 29h47M28 46h47M28 63h30"/>
    <circle cx="77" cy="64" r="3" fill="#a3ffea" stroke="none"/>
  </g>
  <text x="540" y="1500" text-anchor="middle" class="mono small" fill="#99ccda">YOUR FILES, CONNECTED</text>

  <g transform="translate(102 1530)">
    <rect width="184" height="130" rx="18" fill="url(#card)" stroke="#466f88" stroke-opacity=".5"/>
    <path d="M24 34l24-13 24 13v29L48 76 24 63ZM24 34l24 13 24-13M48 47v29M36 27l24 14v15" fill="none" stroke="#eac576" stroke-width="2" stroke-linejoin="round"/>
    <text x="23" y="105" class="caption">ARCHIVE</text>
  </g>
  <g transform="translate(783 1528)">
    <rect width="195" height="148" rx="18" fill="url(#card)" stroke="#466f88" stroke-opacity=".5"/>
    <g aria-label="Android">
      <path d="M36 38L28 24M78 38L86 24" fill="none" stroke="#3ddc84" stroke-width="3.5" stroke-linecap="round"/>
      <path d="M23 69C23 20 91 20 91 69Z" fill="#3ddc84"/>
      <circle cx="41" cy="53" r="3" fill="#102438"/>
      <circle cx="73" cy="53" r="3" fill="#102438"/>
    </g>
    <text x="23" y="112" font-size="22" fill="#9cb6c8">QuickShare</text>
  </g>

  <text x="540" y="1818" text-anchor="middle" font-size="33" font-weight="500" letter-spacing="-1">Find. Preview. Organize. Share.</text>
  <g class="mono" font-size="17" letter-spacing="1.5" fill="#829bad">
    <text x="540" y="1872" text-anchor="middle" xml:space="preserve">Windows<tspan fill="#476175">  /  </tspan>macOS<tspan fill="#476175">  /  </tspan>Linux</text>
  </g>

  <!-- Embedded family icons: no raster images or external dependencies. -->
  <path d="M90 1920H990" stroke="url(#fade-line)"/>
  <text x="540" y="1972" text-anchor="middle" font-size="24" letter-spacing="1" style="fill:#8ab4f8">https://libcr.github.io/</text>
  <text x="540" y="2022" text-anchor="middle" font-size="18" letter-spacing="2" style="fill:#4285F4">THE libcr FAMILY BUILT WITH C++ and chromium</text>
  {{FAMILY}}
  <path d="M76 2342h25m-25 0v-25M1004 2342h-25m25 0v-25" fill="none" stroke="#4c798e"/>
</svg>
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", help="Release version; defaults to crFiles/VERSION")
    parser.add_argument("--output", type=Path, help="SVG destination")
    args = parser.parse_args()
    version = args.version
    if version is None:
        values = dict(line.strip().split("=", 1) for line in
                      (APP / "VERSION").read_text().splitlines() if "=" in line)
        version = ".".join(values[key] for key in ("MAJOR", "MINOR", "PATCH"))
    if not re.fullmatch(r"\d{1,3}\.\d{1,3}\.\d{1,3}(?:\.\d{1,3})?", version):
        parser.error("Version must contain three or four dot-separated numbers (1–3 digits each).")
    output = args.output or APP / "artwork" / f"crFiles-{version}-poster.svg"
    artwork = make_poster(version)
    ET.fromstring(artwork)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(artwork, encoding="utf-8")
    print(output)


if __name__ == "__main__":
    main()
